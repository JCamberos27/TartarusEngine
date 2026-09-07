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

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
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

// The editor's flat icon-button treatment (#160), matching EditorInternal::ActionButton.
bool ActionButton(const EditorModuleHostAPI& host, const char* icon, const char* tooltip) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
    // Button() folds its label into its ID, so scope the ID to the (unique) tooltip string.
    ImGui::PushID(tooltip);
    bool clicked = ImGui::Button(icon);
    ImGui::PopID();
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered() && host.SetTooltip) host.SetTooltip(tooltip);
    return clicked;
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
};

bool FetchEntry(const EditorModuleHostAPI& host, int index, Entry& out) {
    const char* message = nullptr;
    const char* time = nullptr;
    if (!host.LogGetEntry || !host.LogGetEntry(index, &out.Level, &message, &time, &out.Count)) return false;
    out.Message = message ? message : "";
    out.Time = time ? time : "";
    return true;
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

    // The dock tab bar renders synchronously inside Begin(); on a light-chrome theme (Windows XP)
    // its text — tab labels, per-tab ×, the ▼ list button, node × — wants to stay white against
    // the coloured tabs. Detected from WindowBg luminance so it needs no theme knowledge across
    // the module boundary and is a no-op on the dark themes; the body below is unaffected.
    const ImVec4 consoleBg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const bool consoleLightChrome =
        (0.299f * consoleBg.x + 0.587f * consoleBg.y + 0.114f * consoleBg.z) > 0.5f;
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
    ImGui::Checkbox("Auto-scroll", &state.AutoScroll);
    if (ImGui::IsItemHovered()) Tooltip(host, "Automatically jump to the newest message as it arrives");
    ImGui::SameLine();
    ImGui::Checkbox("Timestamps", &state.ShowTimestamps);
    if (ImGui::IsItemHovered()) Tooltip(host, "Show the HH:MM:SS each message first arrived");
    ImGui::SameLine();
    ImGui::Checkbox("Collapse", &state.Collapse);
    if (ImGui::IsItemHovered()) Tooltip(host, "Show each identical message once, with a total count - not just consecutive repeats");
    ImGui::SameLine();
    ImGui::Checkbox("Clear on Play", &state.ClearOnPlay);
    if (ImGui::IsItemHovered()) Tooltip(host, "Wipe the console every time you enter Play mode");
    ImGui::SameLine();
    ImGui::Checkbox("Error Pause", &state.ErrorPause);
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
    ImGui::Checkbox(infoLabel, &state.ShowInfo);
    if (ImGui::IsItemHovered()) Tooltip(host, "Show/hide informational messages");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.30f, 1.0f));
    ImGui::Checkbox(warnLabel, &state.ShowWarning);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) Tooltip(host, "Show/hide warnings");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.38f, 1.0f));
    ImGui::Checkbox(errorLabel, &state.ShowError);
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
                if (entry.Level == EditorModuleLogLevel_Warning) { color = ImVec4(1.0f, 0.80f, 0.30f, 1.0f); icon = ICON_FA_TRIANGLE_EXCLAMATION; }
                else if (entry.Level == EditorModuleLogLevel_Error) { color = ImVec4(1.0f, 0.42f, 0.38f, 1.0f); icon = ICON_FA_CIRCLE_EXCLAMATION; }

                const int rowCount = (row >= 0 && (size_t)row < g_RowCounts.size()) ? g_RowCounts[(size_t)row] : entry.Count;
                std::string tsPrefix = (state.ShowTimestamps && !entry.Time.empty()) ? ("[" + entry.Time + "]  ") : "";
                std::string rowLabel = tsPrefix + icon + "  " + entry.Message;
                if (rowCount > 1) rowLabel += "  (x" + std::to_string(rowCount) + ")";

                // PushID on the entry's stable log index rather than baking a pointer into the
                // label text: a message long enough to fill a fixed label buffer used to truncate
                // away the "##r<ptr>" ID suffix entirely, silently colliding ImGui IDs between
                // rows. PushID keeps identity independent of label content/length altogether.
                ImGui::PushID(entryIndex);
                // A full-width Selectable (rather than a bare Text) so the whole row is a real
                // item with a hover rect — needed for a reliable right-click context menu.
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::Selectable(rowLabel.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
                ImGui::PopStyleColor();

                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy message")) ImGui::SetClipboardText(entry.Message.c_str());
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy all shown")) {
                        std::string all = BuildShownText(host, state);
                        ImGui::SetClipboardText(all.c_str());
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(ICON_FA_TRASH "  Clear console")) {
                        if (host.LogClear) host.LogClear();
                    }
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered()) Tooltip(host, "Right-click for copy / clear");
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
