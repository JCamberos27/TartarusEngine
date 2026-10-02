// The editor's Console panel, living inside TartarusEditor.dll so its code hot-reloads while the
// editor stays open with its scene loaded. Ported verbatim in behaviour from
// EditorLayer::DrawConsole (EditorLayer_Toolbar.cpp) — the #219 ImGuiListClipper virtualization
// over a filtered index list rebuilt only on a Log::Revision()/filter/level-toggle change, the
// level toggle counters, the right-click copy/clear context menu, Clear, and Save...
//
// The one structural change is where data comes from: the module never links Core/Log.cpp, since
// a second copy of that translation unit inside this DLL would carry its own private (and
// permanently empty) entry vector — Windows shares no statics across a module boundary. Every log
// read/write goes through the host callback table instead. Same reasoning for EditorSettings
// (behind host.SetTooltip) and for the panel's own persistent UI state, which the host owns so a
// reload doesn't clear the user's filter.

#include "EditorModuleAPI.h"
#include "EditorPanels.h"
#include "EditorTheme.h"
#include "EditorUIPrimitives.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EditorModuleConsole {

namespace {

// Case-insensitive substring match — the same rule EditorInternal::MatchesFilter applies host-side
// (copied rather than included: EditorLayerInternal.h pulls in World/EnTT/the renderer, none of
// which belong in this DLL).
bool MatchesFilter(const std::string& filter, const std::string& text) {
    if (filter.empty()) return true;
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    return toLower(text).find(toLower(filter)) != std::string::npos;
}

// EditorUI::VSeparator: a 1px rule in ImGuiCol_Separator spanning the frame height, with
// ItemSpacing.x of breathing room either side. Advances the cursor itself.
void VSeparator() {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SameLine(0.0f, style.ItemSpacing.x);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x, p.y + h),
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(1.0f, h));
    ImGui::SameLine(0.0f, style.ItemSpacing.x);
}

void Tooltip(const EditorModuleHostAPI& host, const char* text) {
    if (host.SetTooltip) host.SetTooltip(text);
}

// One log entry, copied out of the host across the boundary. Deliberately a copy: the const char*
// the host hands back points into its own std::string storage, which anything that logs (including
// this panel's own Save... action) can invalidate.
struct Entry {
    int Level = EditorModuleLogLevel_Info;
    std::string Message;
    std::string Time;
    int Count = 1;
    int EntityOrder = -1;  // #146 structured context; -1 / "" when the entry wasn't tagged
    std::string AssetPath;
};

bool FetchEntry(const EditorModuleHostAPI& host, int index, Entry& out) {
    const char* message = nullptr;
    const char* time = nullptr;
    if (!host.LogGetEntry || !host.LogGetEntry(index, &out.Level, &message, &time, &out.Count)) return false;
    out.Message = message ? message : "";
    out.Time = time ? time : "";
    const char* assetPath = nullptr;
    out.EntityOrder = -1;
    if (host.LogGetEntryContext) host.LogGetEntryContext(index, &out.EntityOrder, &assetPath);
    out.AssetPath = assetPath ? assetPath : "";
    return true;
}

// Phase 6 item 4 — a category column, derived from the message's own "Prefix: " convention
// (already used editor- and engine-wide, e.g. "PhysX: ...", "Scene: ...") rather than a second
// tag every call site would have to remember to pass. Variant prefixes that mean the same thing
// ("Hot reload" / "Editor hot reload") collapse onto one bucket; anything unrecognised falls back
// to its own raw prefix so it still sits in a stable column instead of "General" swallowing it.
const char* CanonicalCategory(const std::string& prefix) {
    static const std::pair<const char*, const char*> kBuckets[] = {
        {"PhysX", "PhysX"},
        {"Hot reload", "HotReload"}, {"Editor hot reload", "HotReload"},
        {"Scene", "Scene"}, {"New Scene", "Scene"}, {"Revert Scene", "Scene"},
        {"Prefab", "Scene"}, {"World", "Scene"},
        {"Screenshot", "Capture"}, {"Copy image", "Capture"}, {"Copied path", "Capture"},
        {"Sent screenshot", "Capture"}, {"Drop to surface", "Capture"},
        {"Audio", "Audio"},
        {"AssetDatabase", "Assets"}, {"ThumbnailCache", "Assets"}, {"Model", "Assets"},
        {"Texture", "Assets"}, {"Cubemap", "Assets"}, {"ShaderLibrary", "Assets"},
        {"ShaderAsset", "Assets"}, {"AtomicFile", "Assets"},
        {"Shortcuts", "Editor"}, {"EditorSettings", "Editor"}, {"ProjectSettings", "Editor"},
        {"LayerRegistry", "Editor"}, {"Layout preset", "Editor"}, {"Saved layout preset", "Editor"},
        {"Undo history", "Editor"}, {"Paste failed", "Editor"},
        {"GLDebug", "Renderer"}, {"SpotShadowMap", "Renderer"}, {"PointShadowMap", "Renderer"},
        {"ModelPreviewRenderer", "Renderer"}, {"Framebuffer", "Renderer"},
        {"CascadedShadowMap", "Renderer"}, {"ChannelPreviewRenderer", "Renderer"},
    };
    for (const auto& [p, cat] : kBuckets)
        if (prefix.rfind(p, 0) == 0) return cat;
    return nullptr;
}

// Static storage for the fallback case (an unrecognised prefix used verbatim) — the caller needs
// a stable const char* to hand to snprintf, not a temporary.
std::string DeriveCategory(const std::string& message) {
    const size_t colon = message.find(": ");
    if (colon == std::string::npos || colon > 24) return "General";
    const std::string prefix = message.substr(0, colon);
    if (prefix.empty()) return "General";
    if (const char* canon = CanonicalCategory(prefix)) return canon;
    return prefix.size() <= 12 ? prefix : prefix.substr(0, 12);
}

// Phase 6 item 4 — click-to-navigate. Both parsers are deliberately loose: a false-positive
// candidate is harmless (the host API v27 callbacks reject anything that doesn't resolve to a
// live entity / an existing file), and messages were never written with this in mind, so there's
// no delimiter convention to lean on beyond what already reads naturally to a human.

// Matches "entity #12", the stable form every engine log line uses (EntityLogRef, World.h):
// 12 is the entity's OrderComponent value, which survives undo / Play-Stop / reload (#182).
// The old raw "entity 1234" form is deliberately NOT matched: raw entt ids are recycled, so an
// old line would select whichever entity happens to hold that id now.
bool ParseEntityRef(const std::string& msg, int& outOrder) {
    for (size_t pos = msg.find("entity #"); pos != std::string::npos; pos = msg.find("entity #", pos + 1)) {
        size_t start = pos + 8; // strlen("entity #")
        size_t end = start;
        while (end < msg.size() && msg[end] >= '0' && msg[end] <= '9') ++end;
        if (end > start && end - start < 10) {
            outOrder = (int)std::strtol(msg.substr(start, end - start).c_str(), nullptr, 10);
            return true;
        }
    }
    return false;
}

// Most path-bearing messages quote the path in single quotes ("failed to load sound '...'",
// "failed to open '...'"). PingAssetPath's existence check is what actually decides whether it
// was a real path. #182 — an apostrophe inside a word ("couldn't load '...'") is not a quote:
// a quoted run must open after a non-word character and close before one, and among those the
// last one that looks like a path (has a separator or an extension) wins.
bool ParsePathRef(const std::string& msg, std::string& outPath) {
    auto isWord = [](char c) { return std::isalnum((unsigned char)c) != 0; };
    bool found = false;
    for (size_t start = msg.find('\''); start != std::string::npos; start = msg.find('\'', start + 1)) {
        if (start > 0 && isWord(msg[start - 1])) continue;         // "couldn't" - not an opening quote
        size_t end = start + 1;
        for (; (end = msg.find('\'', end)) != std::string::npos; ++end)
            if (end + 1 >= msg.size() || !isWord(msg[end + 1])) break; // closing quote
        if (end == std::string::npos) break;
        const std::string cand = msg.substr(start + 1, end - start - 1);
        if (!cand.empty() && cand.find_first_of("/\\.") != std::string::npos) {
            outPath = cand;
            found = true;
        }
        start = end;
    }
    return found;
}

bool LevelVisible(const EditorConsoleState& state, int level) {
    return (level == EditorModuleLogLevel_Info && state.ShowInfo) ||
           (level == EditorModuleLogLevel_Warning && state.ShowWarning) ||
           (level == EditorModuleLogLevel_Error && state.ShowError);
}

// #219's cached filtered index list. Module-side because it's pure derived data: after a reload
// the sentinel revision below forces one rebuild and the panel is back where it was.
// g_RowCounts is parallel: the (xN) shown per row — entry.Count normally, or the summed count
// of every identical message when Collapse is on (#236 A5).
std::vector<int> g_FilteredIndices;
std::vector<int> g_RowCounts;
unsigned int g_FilterCacheRevision = (unsigned int)-1; // forces a rebuild on first draw
int g_SelectedEntry = -1; // log index of the row picked for the detail pane; -1 = none
std::string g_FilterCacheFilter;
bool g_FilterCacheShowInfo = true;
bool g_FilterCacheShowWarning = true;
bool g_FilterCacheShowError = true;
bool g_FilterCacheCollapse = false;

// Build the visible-row list (respecting level + text filter, and Collapse). Shared by the
// panel's clipper loop and BuildShownText so "Save..." / "Copy all shown" match what's on screen.
void BuildFilteredRows(const EditorModuleHostAPI& host, const EditorConsoleState& state,
                       std::vector<int>& outIndices, std::vector<int>& outCounts) {
    outIndices.clear();
    outCounts.clear();
    const int entryCount = host.LogEntryCount ? host.LogEntryCount() : 0;
    const std::string filter = state.Filter;
    outIndices.reserve((size_t)entryCount);
    outCounts.reserve((size_t)entryCount);
    std::unordered_map<std::string, size_t> collapsedRow; // key -> position in outIndices
    for (int i = 0; i < entryCount; ++i) {
        Entry e;
        if (!FetchEntry(host, i, e)) continue;
        if (!LevelVisible(state, e.Level) || !MatchesFilter(filter, e.Message)) continue;
        if (state.Collapse) {
            std::string key; key.reserve(e.Message.size() + 2);
            key += (char)('0' + e.Level); key += '\x01'; key += e.Message;
            auto it = collapsedRow.find(key);
            if (it == collapsedRow.end()) {
                collapsedRow.emplace(std::move(key), outIndices.size());
                outIndices.push_back(i);
                outCounts.push_back(e.Count);
            } else {
                outCounts[it->second] += e.Count;
            }
        } else {
            outIndices.push_back(i);
            outCounts.push_back(e.Count);
        }
    }
}

// The plain-text dump of everything currently shown (respects the level + text filters), used by
// "Save..." and the right-click "Copy all shown".
std::string BuildShownText(const EditorModuleHostAPI& host, const EditorConsoleState& state) {
    std::vector<int> idx, cnt;
    BuildFilteredRows(host, state, idx, cnt);
    std::string out;
    for (size_t r = 0; r < idx.size(); ++r) {
        Entry e;
        if (!FetchEntry(host, idx[r], e)) continue;
        const char* tag = e.Level == EditorModuleLogLevel_Error ? "ERROR"
                        : (e.Level == EditorModuleLogLevel_Warning ? "WARN " : "INFO ");
        out += "[" + e.Time + "] " + tag + "  " + e.Message;
        if (cnt[r] > 1) out += "  (x" + std::to_string(cnt[r]) + ")";
        out += "\n";
    }
    return out;
}

} // namespace

void Draw(const EditorModuleHostAPI& host) {
    if (!host.ConsoleState) return;
    EditorConsoleState& state = *host.ConsoleState();
    if (!state.Visible) return;

    // This used to flip the dock tab bar's text to white against Windows XP's saturated-green
    // tab chrome (detected from WindowBg luminance, no theme knowledge needed across the module
    // boundary). Phase 1 item 9 removed that theme; Light's tabs are neutral and already pair
    // with its own normal text colour, so the luminance check would be a false positive there
    // now (see EditorLayerInternal.h's PanelChromeIsLight) — permanently disabled instead.
    const bool consoleLightChrome = false;
    if (consoleLightChrome) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.98f, 1.00f, 1.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    const bool consoleOpen = ImGui::Begin(EditorPanels::Console, &state.Visible, flags);
    if (consoleLightChrome) ImGui::PopStyleColor();
    if (!consoleOpen) { ImGui::End(); return; }

    // One toolbar row: clear / save, the three level toggles (each with its count), the search box
    // filling the middle, and an options popup for the less frequent switches.
    const float iconW = ImGui::GetFrameHeight();
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(EditorTheme::Px(2.0f), st.ItemSpacing.y));
    if (EditorUIPrimitives::ActionButton(ICON_FA_TRASH_CAN, "Clear the console", host.SetTooltip, false, ImVec2(iconW, iconW))) {
        if (host.LogClear) host.LogClear();
    }
    ImGui::SameLine();
    if (EditorUIPrimitives::ActionButton(ICON_FA_FLOPPY_DISK, "Save the messages shown to a text file", host.SetTooltip, false,
                                         ImVec2(iconW, iconW))) {
        char pathBuf[1024] = {};
        if (host.SaveFileDialog &&
            host.SaveFileDialog("Log Files\0*.log;*.txt\0All Files\0*.*\0", "log", pathBuf, (int)sizeof(pathBuf))) {
            const std::string path = pathBuf;
            std::ofstream f(path, std::ios::binary);
            if (f) {
                f << BuildShownText(host, state);
                if (host.LogInfo) host.LogInfo(("Console saved to " + path).c_str());
            } else if (host.LogError) {
                host.LogError(("Couldn't write " + path).c_str());
            }
        }
    }
    ImGui::PopStyleVar();
    VSeparator();

    // Per-level toggles double as counters, the way Unity's console header does: the level's
    // colour while shown, dim while hidden.
    const int infoCount  = host.LogCountOf ? host.LogCountOf(EditorModuleLogLevel_Info) : 0;
    const int warnCount  = host.LogCountOf ? host.LogCountOf(EditorModuleLogLevel_Warning) : 0;
    const int errorCount = host.LogCountOf ? host.LogCountOf(EditorModuleLogLevel_Error) : 0;
    auto levelToggle = [&](const char* icon, int count, bool& on, ImVec4 col, const char* tip) {
        char label[48];
        snprintf(label, sizeof(label), "%s %d##%s", icon, count, tip);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? EditorTheme::WithAlpha(col, 0.12f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? EditorTheme::WithAlpha(col, 0.20f) : EditorUIPrimitives::FlatHover());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::WithAlpha(col, 0.28f));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? col : EditorTheme::Dim);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        if (ImGui::Button(label)) on = !on;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered()) Tooltip(host, tip);
    };
    levelToggle(ICON_FA_CIRCLE_INFO, infoCount, state.ShowInfo, EditorTheme::Secondary, "Show / hide messages");
    ImGui::SameLine(0.0f, EditorTheme::Px(2.0f));
    levelToggle(ICON_FA_TRIANGLE_EXCLAMATION, warnCount, state.ShowWarning, EditorUIPrimitives::WarningColor(), "Show / hide warnings");
    ImGui::SameLine(0.0f, EditorTheme::Px(2.0f));
    levelToggle(ICON_FA_CIRCLE_EXCLAMATION, errorCount, state.ShowError, EditorUIPrimitives::DangerColor(), "Show / hide errors");
    VSeparator();

    EditorUIPrimitives::SearchField("##ConsoleFilter", state.Filter, sizeof(state.Filter), "Filter messages",
                                    -(iconW + st.ItemSpacing.x));
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) Tooltip(host, "Only show messages containing this text");
    ImGui::SameLine();
    const bool anyOption = state.Collapse || state.ClearOnPlay || state.ErrorPause;
    if (EditorUIPrimitives::ActionButton(ICON_FA_SLIDERS, "Console options", host.SetTooltip, anyOption, ImVec2(iconW, iconW)))
        ImGui::OpenPopup("##ConsoleOptions");
    if (ImGui::BeginPopup("##ConsoleOptions")) {
        EditorUIPrimitives::SectionHeader("CONSOLE");
        ImGui::MenuItem("Auto-scroll", nullptr, &state.AutoScroll);
        if (ImGui::IsItemHovered()) Tooltip(host, "Automatically jump to the newest message as it arrives");
        ImGui::MenuItem("Timestamps", nullptr, &state.ShowTimestamps);
        if (ImGui::IsItemHovered()) Tooltip(host, "Show the HH:MM:SS each message first arrived");
        ImGui::MenuItem("Collapse", nullptr, &state.Collapse);
        if (ImGui::IsItemHovered()) Tooltip(host, "Show each identical message once, with a total count - not just consecutive repeats");
        EditorUIPrimitives::SectionHeader("PLAY MODE");
        ImGui::MenuItem("Clear on Play", nullptr, &state.ClearOnPlay);
        if (ImGui::IsItemHovered()) Tooltip(host, "Wipe the console every time you enter Play mode");
        ImGui::MenuItem("Error Pause", nullptr, &state.ErrorPause);
        if (ImGui::IsItemHovered()) Tooltip(host, "Freeze the running simulation the moment a new error is logged");
        ImGui::EndPopup();
    }

    // #219: rebuild the filtered index list only when something that affects it actually changed
    // (the text filter, a level toggle, or the log gaining/losing entries via Log::Revision())
    // rather than re-running MatchesFilter over all 1000 possible entries every single frame the
    // panel happens to be open.
    const unsigned int revision = host.LogRevision ? host.LogRevision() : 0;
    const int entryCount = host.LogEntryCount ? host.LogEntryCount() : 0;
    const std::string filter = state.Filter;
    bool filterCacheStale =
        g_FilterCacheRevision != revision ||
        g_FilterCacheFilter != filter ||
        g_FilterCacheShowInfo != state.ShowInfo ||
        g_FilterCacheShowWarning != state.ShowWarning ||
        g_FilterCacheShowError != state.ShowError ||
        g_FilterCacheCollapse != state.Collapse;
    if (filterCacheStale) {
        BuildFilteredRows(host, state, g_FilteredIndices, g_RowCounts);
        g_FilterCacheRevision = revision;
        g_FilterCacheFilter = filter;
        g_FilterCacheShowInfo = state.ShowInfo;
        g_FilterCacheShowWarning = state.ShowWarning;
        g_FilterCacheShowError = state.ShowError;
        g_FilterCacheCollapse = state.Collapse;
    }

    {
        // A hairline under the toolbar, full width.
        const ImVec2 wp = ImGui::GetWindowPos();
        const float y = std::floor(ImGui::GetCursorScreenPos().y) + 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ImGui::GetWindowWidth(), y),
                                            EditorTheme::U32(EditorTheme::Hairline));
        ImGui::Dummy(ImVec2(0.0f, EditorTheme::Px(2.0f)));
    }
    if (g_FilteredIndices.empty()) {
        g_SelectedEntry = -1;
        EditorUIPrimitives::EmptyState(ICON_FA_TERMINAL, entryCount == 0 ? "No messages" : "Nothing matches the filter",
                                       entryCount == 0 ? "Logs, warnings and errors appear here." : "Clear the search or show more levels.");
        ImGui::End();
        return;
    }
    // The detail pane under the list: only while a row is selected (and still exists).
    Entry detailEntry;
    bool hasDetail = g_SelectedEntry >= 0 && g_SelectedEntry < entryCount && FetchEntry(host, g_SelectedEntry, detailEntry);
    if (!hasDetail) g_SelectedEntry = -1;
    if (hasDetail && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        g_SelectedEntry = -1;
        hasDetail = false;
    }
    float detailH = 0.0f;
    if (hasDetail) {
        const float availY = ImGui::GetContentRegionAvail().y;
        detailH = std::max(availY * 0.30f, ImGui::GetTextLineHeightWithSpacing() * 5.0f + EditorTheme::Px(8.0f));
        detailH = std::min(detailH, availY * 0.70f);
    }
    const ImVec2 listSize(0.0f, hasDetail ? ImGui::GetContentRegionAvail().y - detailH - ImGui::GetStyle().ItemSpacing.y : 0.0f);
    if (ImGui::BeginChild("##ConsoleScroll", listSize, false, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clipper;
        clipper.Begin((int)g_FilteredIndices.size(), ImGui::GetTextLineHeightWithSpacing());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const int entryIndex = g_FilteredIndices[(size_t)row];
                Entry entry;
                if (!FetchEntry(host, entryIndex, entry)) continue;

                ImVec4 color = EditorTheme::Secondary;
                const char* icon = ICON_FA_CIRCLE_INFO;
                if (entry.Level == EditorModuleLogLevel_Warning) { color = EditorUIPrimitives::WarningColor(); icon = ICON_FA_TRIANGLE_EXCLAMATION; }
                else if (entry.Level == EditorModuleLogLevel_Error) { color = EditorUIPrimitives::DangerColor(); icon = ICON_FA_CIRCLE_EXCLAMATION; }

                const int rowCount = (row >= 0 && (size_t)row < g_RowCounts.size()) ? g_RowCounts[(size_t)row] : entry.Count;

                // PushID on the entry's stable log index rather than baking a pointer into the
                // label text: a long message used to truncate the "##r<ptr>" suffix away and collide
                // rows' IDs.
                ImGui::PushID(entryIndex);
                // An empty Selectable owns the row's hover rect, click, double-click and right-click
                // menu; the row's parts are painted over it: timestamp and category dim, the level
                // icon and a 2px band in the level colour, the message, the repeat count. Phase 1
                // item 5: the body is monospace so the columns line up.
                ImGui::PushFont(host.GetMonoFont ? host.GetMonoFont() : nullptr, 0.0f);
                const ImVec2 rowTop = ImGui::GetCursorScreenPos();
                const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
                char categoryField[16]; // Phase 6 item 4 — the category column: 11 cells, cut to fit
                snprintf(categoryField, sizeof(categoryField), "%-11.10s", DeriveCategory(entry.Message).c_str());
                char cnt[24] = {};
                if (rowCount > 1) snprintf(cnt, sizeof(cnt), "%d", rowCount);
                const bool showTime = state.ShowTimestamps && !entry.Time.empty();
                const float gapS = EditorTheme::Px(8.0f), gapL = EditorTheme::Px(10.0f), pad = EditorTheme::Px(5.0f);
                const float rowW = gapS + (showTime ? ImGui::CalcTextSize(entry.Time.c_str()).x + gapL : 0.0f) +
                                   ImGui::CalcTextSize(categoryField).x + ImGui::CalcTextSize(icon).x + gapS +
                                   ImGui::CalcTextSize(entry.Message.c_str()).x +
                                   (cnt[0] ? gapL + ImGui::CalcTextSize(cnt).x + pad * 2.0f : 0.0f) + gapS;
                if (row % 2 == 1)
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(ImGui::GetWindowPos().x, rowTop.y),
                        ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), rowTop.y + rowHeight), EditorTheme::U32(EditorTheme::Stripe));
                const bool rowClicked = ImGui::Selectable("##row", entryIndex == g_SelectedEntry, ImGuiSelectableFlags_AllowDoubleClick,
                                                          ImVec2(std::max(ImGui::GetContentRegionAvail().x, rowW), 0.0f));
                {
                    ImDrawList* rowDl = ImGui::GetWindowDrawList();
                    rowDl->AddRectFilled(rowTop, ImVec2(rowTop.x + EditorTheme::Px(2.0f), rowTop.y + rowHeight - 1.0f),
                                         EditorTheme::U32(EditorTheme::WithAlpha(color, entry.Level == EditorModuleLogLevel_Info ? 0.35f : 1.0f)));
                    float x = rowTop.x + gapS;
                    const float y = rowTop.y;
                    auto put = [&](const char* s, ImVec4 c) {
                        rowDl->AddText(ImVec2(x, y), EditorTheme::U32(c), s);
                        x += ImGui::CalcTextSize(s).x;
                    };
                    if (showTime) { put(entry.Time.c_str(), EditorTheme::Dim); x += gapL; }
                    put(categoryField, EditorTheme::Dim);
                    put(icon, color);
                    x += gapS;
                    put(entry.Message.c_str(), entry.Level == EditorModuleLogLevel_Info ? EditorTheme::Text : color);
                    if (cnt[0]) {
                        x += gapL;
                        const ImVec2 cs = ImGui::CalcTextSize(cnt);
                        rowDl->AddRectFilled(ImVec2(x, y + 1.0f), ImVec2(x + cs.x + pad * 2.0f, y + cs.y - 1.0f),
                                             EditorTheme::U32(EditorTheme::Raised), cs.y * 0.5f);
                        rowDl->AddText(ImVec2(x + pad, y), EditorTheme::U32(EditorTheme::Secondary), cnt);
                    }
                }
                ImGui::PopFont();

                // Phase 6 item 4 — click-to-navigate. An entity reference wins over an asset one
                // when a message happens to parse as both (hasn't come up in practice, but PhysX
                // lines already quote asset-ish text around an entity id in a couple of cases).
                // #146: the logger's structured context first; parsing the text is the fallback
                // for the many call sites that don't tag their messages.
                int entityRef = entry.EntityOrder;
                std::string assetRef = entry.AssetPath;
                const bool hasEntityRef = host.SelectEntityByOrder &&
                    (entityRef >= 0 || ParseEntityRef(entry.Message, entityRef));
                const bool hasAssetRef = !hasEntityRef && host.PingAssetPath &&
                    (!assetRef.empty() || ParsePathRef(entry.Message, assetRef));

                if (rowClicked) g_SelectedEntry = entryIndex;
                if (rowClicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (hasEntityRef) host.SelectEntityByOrder(entityRef);
                    else if (hasAssetRef) host.PingAssetPath(assetRef.c_str());
                }

                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy message")) ImGui::SetClipboardText(entry.Message.c_str());
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy all shown")) {
                        std::string all = BuildShownText(host, state);
                        ImGui::SetClipboardText(all.c_str());
                    }
                    // #178 - Warnings and Errors carry the call stack from where they were
                    // logged. Resolved on demand (symbol lookup is slow), so this asks the host
                    // only when the menu is actually open.
                    {
                        const char* stack = nullptr;
                        if (host.LogGetEntryStack && host.LogGetEntryStack(entryIndex, &stack) && stack && *stack) {
                            ImGui::Separator();
                            if (ImGui::MenuItem(ICON_FA_LAYER_GROUP "  Copy stack trace"))
                                ImGui::SetClipboardText(stack);
                            if (ImGui::BeginMenu(ICON_FA_LAYER_GROUP "  Stack trace")) {
                                ImGui::TextUnformatted(stack);
                                ImGui::EndMenu();
                            }
                        }
                    }
                    if (hasEntityRef || hasAssetRef) {
                        ImGui::Separator();
                        if (hasEntityRef) {
                            std::string label = ICON_FA_LOCATION_CROSSHAIRS "  Select entity #" + std::to_string(entityRef);
                            if (ImGui::MenuItem(label.c_str())) host.SelectEntityByOrder(entityRef);
                        }
                        if (hasAssetRef) {
                            std::string label = ICON_FA_MAGNIFYING_GLASS_LOCATION "  Show in Asset Browser";
                            if (ImGui::MenuItem(label.c_str())) host.PingAssetPath(assetRef.c_str());
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(ICON_FA_TRASH "  Clear console")) {
                        if (host.LogClear) host.LogClear();
                    }
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered()) {
                    Tooltip(host, (hasEntityRef || hasAssetRef)
                        ? "Double-click to navigate. Right-click for copy / clear."
                        : "Right-click for copy / clear");
                }
                ImGui::PopID();
            }
        }
        clipper.End();

        // Only when something actually arrived, so scrolling back through history isn't yanked to
        // the bottom on every single frame.
        if (state.AutoScroll && revision != state.SeenRevision) {
            ImGui::SetScrollHereY(1.0f);
        }
        state.SeenRevision = revision;
    }
    ImGui::EndChild();

    if (hasDetail) {
        // A hairline splitter, then the full message of the selected row.
        {
            const ImVec2 wp = ImGui::GetWindowPos();
            const float y = std::floor(ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y * 0.5f) + 0.5f;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ImGui::GetWindowWidth(), y),
                                                EditorTheme::U32(EditorTheme::Hairline));
        }
        if (ImGui::BeginChild("##ConsoleDetail", ImVec2(0, 0), false)) {
            ImVec4 lvlCol = EditorTheme::Secondary;
            const char* lvlName = "INFO";
            if (detailEntry.Level == EditorModuleLogLevel_Warning) { lvlCol = EditorUIPrimitives::WarningColor(); lvlName = "WARNING"; }
            else if (detailEntry.Level == EditorModuleLogLevel_Error) { lvlCol = EditorUIPrimitives::DangerColor(); lvlName = "ERROR"; }
            const float btn = ImGui::GetFrameHeight();

            EditorTheme::PushSmall();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(lvlCol, "%s", lvlName);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Dim);
            ImGui::Text("%s%s%s", DeriveCategory(detailEntry.Message).c_str(),
                        detailEntry.Time.empty() ? "" : "   ", detailEntry.Time.c_str());
            ImGui::PopStyleColor();
            EditorTheme::PopFont();

            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - btn * 2.0f - ImGui::GetStyle().ItemSpacing.x);
            if (EditorUIPrimitives::ActionButton(ICON_FA_COPY, "Copy this message", host.SetTooltip, false, ImVec2(btn, btn)))
                ImGui::SetClipboardText(detailEntry.Message.c_str());
            ImGui::SameLine();
            const bool closeClicked = EditorUIPrimitives::ActionButton(ICON_FA_XMARK, "Close (Esc)", host.SetTooltip, false, ImVec2(btn, btn));

            if (ImGui::BeginChild("##ConsoleDetailText", ImVec2(0, 0), false)) {
                EditorTheme::PushMono();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, detailEntry.Level == EditorModuleLogLevel_Info ? EditorTheme::Text : lvlCol);
                ImGui::TextUnformatted(detailEntry.Message.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
                EditorTheme::PopFont();
            }
            ImGui::EndChild();
            if (closeClicked) g_SelectedEntry = -1;
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

} // namespace EditorModuleConsole
