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
#include "EditorUIPrimitives.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
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

// Forwards to the shared implementation (EditorUIPrimitives.h, Defect #53).
bool ActionButton(const EditorModuleHostAPI& host, const char* icon, const char* tooltip) {
    return EditorUIPrimitives::ActionButton(icon, tooltip, host.SetTooltip);
}

// Defect #20 — an unchecked Collapse/Clear on Play/Error Pause "reads as bare text label", per
// the audit. Live pixel-sampled the running build to settle what that actually meant: raw OS-level
// screenshots (System.Drawing.Bitmap.GetPixel, not a rescaled/compressed screenshot-tool crop)
// prove ImGui::Checkbox's frame WAS painting the configured rgb(97,97,97) FrameBg exactly where
// expected — it isn't invisible, it's just a 3:1-contrast mid-grey square with no border (this
// theme runs FrameBorderSize 0 everywhere, see EditorLayer.cpp's #34 comment) on a near-black
// #121212 toolbar, small enough and low-contrast enough to disappear at a glance and in any
// compressed/rescaled screenshot — which is exactly the "no checkbox or button frame" the audit
// (reasonably) reported. Auto-scroll/Timestamps read fine only because they default checked, and
// a checked box's CheckboxSelectedBg fill + CheckMark tick are both far more saturated. The same
// pattern turned up editor-wide (#73), so the fix now lives as EditorUIPrimitives::Checkbox — a
// thin wrapper that keeps real ImGui::Checkbox for all of the actual behaviour and just adds the
// outline this theme otherwise omits.

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
};

bool FetchEntry(const EditorModuleHostAPI& host, int index, Entry& out) {
    const char* message = nullptr;
    const char* time = nullptr;
    if (!host.LogGetEntry || !host.LogGetEntry(index, &out.Level, &message, &time, &out.Count)) return false;
    out.Message = message ? message : "";
    out.Time = time ? time : "";
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

// Matches PhysX/Scene log lines' own "entity 1234" phrasing (see e.g. PhysicsWorld.cpp,
// EditorLayer_Gizmos.cpp's "Joint broke on entity %d.").
bool ParseEntityRef(const std::string& msg, unsigned int& outId) {
    size_t pos = msg.find("entity ");
    while (pos != std::string::npos) {
        size_t start = pos + 7; // strlen("entity ")
        size_t end = start;
        while (end < msg.size() && msg[end] >= '0' && msg[end] <= '9') ++end;
        if (end > start) {
            outId = (unsigned int)std::strtoul(msg.substr(start, end - start).c_str(), nullptr, 10);
            return true;
        }
        pos = msg.find("entity ", pos + 1);
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
    const bool consoleOpen = ImGui::Begin(ICON_FA_TERMINAL "  Console", &state.Visible, flags);
    if (consoleLightChrome) ImGui::PopStyleColor();
    if (!consoleOpen) { ImGui::End(); return; }

    if (ActionButton(host, ICON_FA_TRASH "  Clear", "Remove every message from the console")) {
        if (host.LogClear) host.LogClear();
    }
    ImGui::SameLine();
    if (ActionButton(host, ICON_FA_FLOPPY_DISK "  Save...", "Write the messages currently shown to a text file")) {
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
    ImGui::SameLine();
    EditorUIPrimitives::Checkbox("Auto-scroll", &state.AutoScroll);
    if (ImGui::IsItemHovered()) Tooltip(host, "Automatically jump to the newest message as it arrives");
    ImGui::SameLine();
    EditorUIPrimitives::Checkbox("Timestamps", &state.ShowTimestamps);
    if (ImGui::IsItemHovered()) Tooltip(host, "Show the HH:MM:SS each message first arrived");
    ImGui::SameLine();
    EditorUIPrimitives::Checkbox("Collapse", &state.Collapse);
    if (ImGui::IsItemHovered()) Tooltip(host, "Show each identical message once, with a total count - not just consecutive repeats");
    ImGui::SameLine();
    EditorUIPrimitives::Checkbox("Clear on Play", &state.ClearOnPlay);
    if (ImGui::IsItemHovered()) Tooltip(host, "Wipe the console every time you enter Play mode");
    ImGui::SameLine();
    EditorUIPrimitives::Checkbox("Error Pause", &state.ErrorPause);
    if (ImGui::IsItemHovered()) Tooltip(host, "Freeze the running simulation the moment a new error is logged");

    // Per-level toggles double as counters, the way Unity's console header does.
    VSeparator();
    char infoLabel[32], warnLabel[32], errorLabel[32];
    const int infoCount  = host.LogCountOf ? host.LogCountOf(EditorModuleLogLevel_Info) : 0;
    const int warnCount  = host.LogCountOf ? host.LogCountOf(EditorModuleLogLevel_Warning) : 0;
    const int errorCount = host.LogCountOf ? host.LogCountOf(EditorModuleLogLevel_Error) : 0;
    snprintf(infoLabel, sizeof(infoLabel), ICON_FA_CIRCLE_INFO " %d", infoCount);
    snprintf(warnLabel, sizeof(warnLabel), ICON_FA_TRIANGLE_EXCLAMATION " %d", warnCount);
    snprintf(errorLabel, sizeof(errorLabel), ICON_FA_CIRCLE_EXCLAMATION " %d", errorCount);
    EditorUIPrimitives::Checkbox(infoLabel, &state.ShowInfo);
    if (ImGui::IsItemHovered()) Tooltip(host, "Show/hide informational messages");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::WarningColor());
    EditorUIPrimitives::Checkbox(warnLabel, &state.ShowWarning);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) Tooltip(host, "Show/hide warnings");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::DangerColor());
    EditorUIPrimitives::Checkbox(errorLabel, &state.ShowError);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) Tooltip(host, "Show/hide errors");

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ConsoleFilter", ICON_FA_MAGNIFYING_GLASS "  Filter messages...",
                             state.Filter, sizeof(state.Filter));
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) Tooltip(host, "Only show messages containing this text");

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
    (void)entryCount;

    ImGui::Separator();
    if (ImGui::BeginChild("##ConsoleScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clipper;
        clipper.Begin((int)g_FilteredIndices.size(), ImGui::GetTextLineHeightWithSpacing());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const int entryIndex = g_FilteredIndices[(size_t)row];
                Entry entry;
                if (!FetchEntry(host, entryIndex, entry)) continue;

                ImVec4 color(0.82f, 0.84f, 0.86f, 1.0f);
                const char* icon = ICON_FA_CIRCLE_INFO;
                if (entry.Level == EditorModuleLogLevel_Warning) { color = EditorUIPrimitives::WarningColor(); icon = ICON_FA_TRIANGLE_EXCLAMATION; }
                else if (entry.Level == EditorModuleLogLevel_Error) { color = EditorUIPrimitives::DangerColor(); icon = ICON_FA_CIRCLE_EXCLAMATION; }

                const int rowCount = (row >= 0 && (size_t)row < g_RowCounts.size()) ? g_RowCounts[(size_t)row] : entry.Count;
                std::string tsPrefix = (state.ShowTimestamps && !entry.Time.empty()) ? ("[" + entry.Time + "]  ") : "";
                // Phase 6 item 4 — category column. Padded (monospace body font) rather than a
                // separate ImGui column so it stays part of the one full-width Selectable the
                // right-click menu and double-click both depend on.
                char categoryField[16];
                snprintf(categoryField, sizeof(categoryField), "%-10s", DeriveCategory(entry.Message).c_str());
                std::string rowLabel = tsPrefix + categoryField + icon + "  " + entry.Message;
                if (rowCount > 1) rowLabel += "  (x" + std::to_string(rowCount) + ")";

                // Phase 6 item 4 — a 3px severity band down the row's left edge, in addition to
                // the icon+colour: colour alone fails at a glance for anyone who can't rely on
                // hue (the icon already covers that; this is a second, position-based cue matching
                // Console's leftmost real estate rather than adding a fourth ImGui column).
                const ImVec2 rowTop = ImGui::GetCursorScreenPos();
                const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
                ImGui::Indent(6.0f);

                // PushID on the entry's stable log index rather than baking a pointer into the
                // label text: a message long enough to fill a fixed label buffer used to truncate
                // away the "##r<ptr>" ID suffix entirely, silently colliding ImGui IDs between
                // rows. PushID keeps identity independent of label content/length altogether.
                ImGui::PushID(entryIndex);
                // A full-width Selectable (rather than a bare Text) so the whole row is a real
                // item with a hover rect — needed for a reliable right-click context menu.
                // Phase 1 item 5: the Console body is mono so a timestamp column and repeated-
                // count suffix actually line up; GetMonoFont has the severity icon range merged
                // onto it too (EditorLayer.cpp), so the inline FA glyph in rowLabel still renders.
                ImGui::PushFont(host.GetMonoFont ? host.GetMonoFont() : nullptr, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                const bool rowClicked = ImGui::Selectable(rowLabel.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
                ImGui::PopStyleColor();
                ImGui::PopFont();

                ImDrawList* rowDl = ImGui::GetWindowDrawList();
                rowDl->AddRectFilled(rowTop, ImVec2(rowTop.x + 3.0f, rowTop.y + rowHeight),
                                     ImGui::ColorConvertFloat4ToU32(color));
                ImGui::Unindent(6.0f);

                // Phase 6 item 4 — click-to-navigate. An entity reference wins over an asset one
                // when a message happens to parse as both (hasn't come up in practice, but PhysX
                // lines already quote asset-ish text around an entity id in a couple of cases).
                unsigned int entityRef = 0;
                std::string assetRef;
                const bool hasEntityRef = host.SelectEntityRaw && ParseEntityRef(entry.Message, entityRef);
                const bool hasAssetRef = !hasEntityRef && host.PingAssetPath && ParsePathRef(entry.Message, assetRef);

                if (rowClicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (hasEntityRef) host.SelectEntityRaw(entityRef);
                    else if (hasAssetRef) host.PingAssetPath(assetRef.c_str());
                }

                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy message")) ImGui::SetClipboardText(entry.Message.c_str());
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy all shown")) {
                        std::string all = BuildShownText(host, state);
                        ImGui::SetClipboardText(all.c_str());
                    }
                    if (hasEntityRef || hasAssetRef) {
                        ImGui::Separator();
                        if (hasEntityRef) {
                            std::string label = ICON_FA_LOCATION_CROSSHAIRS "  Select entity #" + std::to_string(entityRef);
                            if (ImGui::MenuItem(label.c_str())) host.SelectEntityRaw(entityRef);
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

    ImGui::End();
}

} // namespace EditorModuleConsole
