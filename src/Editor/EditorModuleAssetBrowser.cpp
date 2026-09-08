// The Asset Browser's chrome — the dock window, the "+ Create / Import" toolbar row, the
// breadcrumb, the search box + Filters popup, the left folder-tree pane and the tree/grid
// splitter — living inside TartarusEditor.dll so its layout hot-reloads while the editor stays
// open. Ported in behaviour from EditorLayer::DrawAssetBrowser / DrawFolderTreeNode
// (EditorLayer_AssetBrowser.cpp).
//
// Issue #229: this DLL owns the panel's chrome AND (as of API v6) the asset grid's scaffold — the
// ##AssetList child, the ImGuiListClipper row loop with per-row SameLine wrapping, and the footer.
// Per-cell drawing (GL thumbnails, the five context menus, rename-in-place, drag sources,
// multi-select, delete/duplicate) and the background stay host code, reached through
// DrawAssetCell / HandleAssetGridBackground / AssetGridFrameEnd. AssetLibrary never crosses the
// boundary: the folder list is a per-frame string snapshot (GetFolderCount/GetFolder), the grid a
// per-frame cell count + DrawAssetCell(index) callback, every mutation an undoable command
// callback, and folder-tree expansion stays host-side (Left/Right-arrow shortcuts keep working,
// survives a reload) via Is/SetAssetFolderExpanded. Drag payloads are bare path strings.

#include "EditorModuleAPI.h"

#include <imgui.h>
#include <imgui_internal.h> // ImFloor
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace EditorModuleAssetBrowser {

namespace {

constexpr int kPathBuf = 512;

void Tooltip(const EditorModuleHostAPI& host, const char* text) {
    if (host.SetTooltip) host.SetTooltip(text);
}

// EditorInternal::ActionButton (flat, no "active" state needed here), copied module-side.
bool ActionButton(const EditorModuleHostAPI& host, const char* icon, const char* tooltip) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
    ImGui::PushID(tooltip);
    bool clicked = ImGui::Button(icon);
    ImGui::PopID();
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) Tooltip(host, tooltip);
    return clicked;
}

// EditorUI::VSeparator — a 1px rule spanning the frame height with ItemSpacing.x either side.
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

// EditorInternal::PushTabChromeText — keep the dock tab bar's text white on a light (XP) theme.
bool PanelChromeIsLight() {
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    return (0.299f * bg.x + 0.587f * bg.y + 0.114f * bg.z) > 0.5f;
}
void PushTabChromeText() {
    if (PanelChromeIsLight()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.98f, 1.00f, 1.0f));
}
void PopTabChromeText() {
    if (PanelChromeIsLight()) ImGui::PopStyleColor();
}

// EditorInternal::MatchesFilter / ParentFolderOf / LeafNameOf, copied module-side.
bool MatchesFilter(const std::string& filter, const std::string& text) {
    if (filter.empty()) return true;
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    return toLower(text).find(toLower(filter)) != std::string::npos;
}
std::string ParentFolderOf(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}
std::string LeafNameOf(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// Asset Browser search-token helpers (file-local in the host .cpp), copied module-side.
bool SearchHasToken(const std::string& filter, const std::string& token) {
    std::string t;
    for (size_t i = 0; i <= filter.size(); ++i) {
        if (i == filter.size() || filter[i] == ' ' || filter[i] == '\t') {
            if (t == token) return true;
            t.clear();
        } else {
            t += filter[i];
        }
    }
    return false;
}
void ToggleSearchToken(std::string& filter, const std::string& token) {
    std::vector<std::string> tokens;
    std::string t;
    bool removed = false;
    for (size_t i = 0; i <= filter.size(); ++i) {
        if (i == filter.size() || filter[i] == ' ' || filter[i] == '\t') {
            if (!t.empty()) {
                if (t == token) removed = true;
                else tokens.push_back(t);
            }
            t.clear();
        } else {
            t += filter[i];
        }
    }
    if (!removed) tokens.push_back(token);
    filter.clear();
    for (size_t i = 0; i < tokens.size(); ++i) { if (i) filter += " "; filter += tokens[i]; }
}

std::string HostString(void (*getter)(char*, int)) {
    if (!getter) return {};
    char buf[kPathBuf] = {};
    getter(buf, (int)sizeof(buf));
    return buf;
}

std::vector<std::string> Folders(const EditorModuleHostAPI& host) {
    std::vector<std::string> out;
    const int n = host.GetFolderCount ? host.GetFolderCount() : 0;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        char buf[kPathBuf] = {};
        if (host.GetFolder && host.GetFolder(i, buf, (int)sizeof(buf))) out.emplace_back(buf);
    }
    return out;
}

// A unique "New Folder" name under `parent`, matching the host's old makeNewFolder().
std::string UniqueNewFolder(const std::vector<std::string>& folders, const std::string& parent) {
    const std::string base = parent.empty() ? "New Folder" : (parent + "/New Folder");
    auto exists = [&](const std::string& p) {
        for (const auto& f : folders) if (f == p) return true;
        return false;
    };
    std::string candidate = base;
    int n = 1;
    while (exists(candidate)) candidate = base + " (" + std::to_string(n++) + ")";
    return candidate;
}

// Ported from EditorLayer::DrawFolderTreeNode. `folders` is this frame's snapshot; `curFolder`
// and `reveal` come from the host. Selection / expansion / drag-drop all route through host
// callbacks — nothing is mutated locally.
void DrawFolderNode(const EditorModuleHostAPI& host, const std::vector<std::string>& folders,
                    const std::string& folderPath, bool isRoot,
                    const std::string& curFolder, const std::string& reveal) {
    std::vector<std::string> children;
    for (const auto& f : folders)
        if (ParentFolderOf(f) == folderPath) children.push_back(f);
    std::sort(children.begin(), children.end(),
              [](const std::string& a, const std::string& b) { return LeafNameOf(a) < LeafNameOf(b); });

    const bool hasChildren = !children.empty();
    const bool wasExpanded = isRoot ||
        (host.IsAssetFolderExpanded && host.IsAssetFolderExpanded(folderPath.c_str()));
    if (!isRoot) ImGui::SetNextItemOpen(wasExpanded);

    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (curFolder == folderPath) nodeFlags |= ImGuiTreeNodeFlags_Selected;
    if (!hasChildren) nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (isRoot) nodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;

    const std::string label = std::string(isRoot ? ICON_FA_FOLDER_TREE : ICON_FA_FOLDER) + "  " +
                              (isRoot ? "Assets" : LeafNameOf(folderPath));

    ImGui::PushID(folderPath.c_str());
    const bool open = ImGui::TreeNodeEx("##node", nodeFlags, "%s", label.c_str());

    if (!isRoot && !reveal.empty() && folderPath == reveal) ImGui::SetScrollHereY(0.5f);

    if (!isRoot && open != wasExpanded && host.SetAssetFolderExpanded) {
        // The user just clicked this node's arrow (ImGui's toggle already ran, so `open` differs
        // from what we told it) — Alt makes it "and every descendant too".
        host.SetAssetFolderExpanded(folderPath.c_str(), open, ImGui::GetIO().KeyAlt);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen() &&
        host.SetCurrentAssetFolder) {
        host.SetCurrentAssetFolder(folderPath.c_str());
    }

    if (!isRoot && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("ASSET_FOLDER_PATH", folderPath.c_str(), folderPath.size() + 1);
        ImGui::TextUnformatted(LeafNameOf(folderPath).c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        for (const char* type : {"ASSET_MODEL_PATH", "ASSET_TEXTURE_PATH",
                                 "ASSET_SOUND_PATH", "ASSET_PREFAB_PATH"}) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type)) {
                if (host.MoveAssetToFolderUndoable)
                    host.MoveAssetToFolderUndoable((const char*)p->Data, folderPath.c_str());
            }
        }
        if (!isRoot) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_FOLDER_PATH")) {
                const std::string src((const char*)p->Data);
                if (src != folderPath && folderPath.rfind(src + "/", 0) != 0 && host.RenameFolderUndoable)
                    host.RenameFolderUndoable(src.c_str(), (folderPath + "/" + LeafNameOf(src)).c_str());
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemToggledOpen()) {
        Tooltip(host, isRoot
            ? "The root of every asset the editor knows about.\nAlt+click a folder's arrow to expand/collapse it and everything under it."
            : "Click to browse. Drag assets or other folders onto it to file them here.");
    }
    ImGui::PopID();

    if (open) {
        for (const auto& child : children)
            DrawFolderNode(host, folders, child, false, curFolder, reveal);
        // NoTreePushOnOpen is set whenever !hasChildren (root or not), so TreePop is gated on
        // hasChildren alone — see the host history at DrawFolderTreeNode.
        if (hasChildren) ImGui::TreePop();
    }
}

// The asset grid (API v6): the module owns the ##AssetList child, the ImGuiListClipper row loop
// with per-row SameLine wrapping, and the footer; every cell + the background + the delete-confirm
// popup are host code (DrawAssetCell / HandleAssetGridBackground / AssetGridFrameEnd). Cell size
// and cellsPerRow are pure math from GetAssetGridMetrics — ported from the host's old
// DrawAssetGridBody.
void DrawAssetGrid(const EditorModuleHostAPI& host, float contentHeight) {
    ImGui::BeginChild("##AssetList", ImVec2(0, contentHeight), ImGuiChildFlags_None);

    if (host.AssetGridFrameBegin) host.AssetGridFrameBegin();

    float iconSize = 64.0f, uiScale = 1.0f, listMinIcon = 24.0f;
    if (host.GetAssetGridMetrics) host.GetAssetGridMetrics(&iconSize, &uiScale, &listMinIcon);

    const bool gridMode = iconSize > listMinIcon * uiScale;
    const float cellPadding = 8.0f;
    const float cellWidth = iconSize + cellPadding * 2.0f;
    const float cellHeight = iconSize + cellPadding + ImGui::GetTextLineHeightWithSpacing();

    const int cellCount = host.AssetGridCellCount ? host.AssetGridCellCount() : 0;

    int cellsPerRow = 1;
    if (gridMode) {
        const float availW = ImGui::GetContentRegionAvail().x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        cellsPerRow = (int)((availW + spacing) / (cellWidth + spacing));
        if (cellsPerRow < 1) cellsPerRow = 1;
    }
    const int totalRows = cellCount == 0 ? 0 : (cellCount + cellsPerRow - 1) / cellsPerRow;
    const float clipRowHeight = gridMode ? cellHeight : ImGui::GetFrameHeightWithSpacing();

    ImGuiListClipper clipper;
    clipper.Begin(totalRows, clipRowHeight);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int col = 0; col < cellsPerRow; ++col) {
                const int idx = row * cellsPerRow + col;
                if (idx >= cellCount) break;
                if (host.DrawAssetCell) host.DrawAssetCell(idx, cellWidth, cellHeight, gridMode);
                // Wrap within the row: SameLine for every column but the last, and only when
                // there's another cell to draw (the final row may be partial).
                if (gridMode && col + 1 < cellsPerRow && idx + 1 < cellCount) ImGui::SameLine();
            }
        }
    }
    clipper.End();

    if (host.HandleAssetGridBackground) host.HandleAssetGridBackground();

    ImGui::EndChild(); // ##AssetList

    // Footer: selection summary on the left, the icon-size slider on the right (drag to the
    // minimum for the compact list view).
    ImGui::BeginChild("##AssetGridFooter", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    char summary[256] = {};
    if (host.GetAssetSelectionSummary) host.GetAssetSelectionSummary(summary, (int)sizeof(summary));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", summary);

    const float sliderWidth = 100.0f;
    const float sliderX = ImGui::GetWindowContentRegionMax().x - sliderWidth;
    if (sliderX > ImGui::GetCursorPosX()) ImGui::SameLine(sliderX);
    else ImGui::NewLine();
    ImGui::SetNextItemWidth(sliderWidth);
    float sliderVal = iconSize;
    if (ImGui::SliderFloat("##IconSize", &sliderVal, listMinIcon * uiScale, 128.0f * uiScale, "") &&
        host.SetAssetIconSize) {
        host.SetAssetIconSize(sliderVal, /*commit=*/false);
    }
    if (ImGui::IsItemHovered() && host.SetTooltip)
        host.SetTooltip("Icon size - drag all the way to the left for a compact list view.");
    if (ImGui::IsItemDeactivatedAfterEdit() && host.SetAssetIconSize)
        host.SetAssetIconSize(sliderVal, /*commit=*/true);
    ImGui::EndChild();

    if (host.AssetGridFrameEnd) host.AssetGridFrameEnd();
}

} // namespace

void Draw(const EditorModuleHostAPI& host) {
    if (host.GetShowAssetBrowser && !host.GetShowAssetBrowser()) return;

    bool visible = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    PushTabChromeText();
    const bool open = ImGui::Begin("Asset Browser", &visible, ImGuiWindowFlags_None);
    PopTabChromeText();
    ImGui::PopStyleVar();
    if (host.SetShowAssetBrowser) host.SetShowAssetBrowser(visible); // capture the title-bar X
    if (!open) { ImGui::End(); return; }

    if (host.SetAssetBrowserFocused)
        host.SetAssetBrowserFocused(ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows));

    char revealBuf[kPathBuf] = {};
    if (host.AssetTreeFrameSetup) host.AssetTreeFrameSetup(revealBuf, (int)sizeof(revealBuf));
    const std::string reveal = revealBuf;

    const std::vector<std::string> folders = Folders(host);
    const std::string curFolder = HostString(host.GetCurrentAssetFolder);
    std::string search = HostString(host.GetAssetSearch);
    const std::string searchBefore = search;

    // --- toolbar row ---------------------------------------------------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##AssetToolbar", ImVec2(0, ImGui::GetFrameHeight()), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    const float searchWidth = 200.0f;

    if (ActionButton(host, ICON_FA_PLUS, "Create / Import")) ImGui::OpenPopup("##AssetCreateMenu");
    if (ImGui::BeginPopup("##AssetCreateMenu")) {
        if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Folder")) {
            const std::string candidate = UniqueNewFolder(folders, curFolder);
            if (host.CreateFolderUndoable) host.CreateFolderUndoable(candidate.c_str());
            if (host.BeginRenameFolder) host.BeginRenameFolder(candidate.c_str());
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CUBE "  Import Model...") && host.ImportAssetViaDialog)
            host.ImportAssetViaDialog(0, curFolder.c_str());
        if (ImGui::MenuItem(ICON_FA_IMAGE "  Import Texture...") && host.ImportAssetViaDialog)
            host.ImportAssetViaDialog(1, curFolder.c_str());
        if (ImGui::MenuItem(ICON_FA_MUSIC "  Import Sound...") && host.ImportAssetViaDialog)
            host.ImportAssetViaDialog(2, curFolder.c_str());
        ImGui::EndPopup();
    }

    VSeparator();

    const float refreshFlash = host.GetAssetRefreshFlash ? host.GetAssetRefreshFlash() : 0.0f;

    // Breadcrumb — context only, non-interactive. The post-refresh confirmation (#236 G) rides
    // here on the toolbar itself rather than adding a row below it; it fades over its last second.
    {
        std::string crumb = "Assets";
        for (char c : curFolder) crumb += (c == '/') ? " / " : std::string(1, c);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", crumb.c_str());
        if (refreshFlash > 0.0f) {
            const float a = refreshFlash > 1.0f ? 1.0f : refreshFlash;
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.85f, 0.52f, a));
            ImGui::TextUnformatted(ICON_FA_CIRCLE_CHECK "  Assets refreshed");
            ImGui::PopStyleColor();
        }
    }

    // Search box + the trailing icon buttons (Filters, Sort, Refresh — #236 G), pinned to the
    // right edge (or a new line if the breadcrumb has crowded them out).
    const float iconBtnW = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const float trailingButtonsWidth = iconBtnW * 5.0f; // favourites | scope | filter | sort | refresh
    const float targetX = ImGui::GetWindowContentRegionMax().x - (searchWidth + trailingButtonsWidth);
    if (targetX > ImGui::GetCursorPosX()) ImGui::SameLine(targetX);
    else ImGui::NewLine();

    char searchBuf[128];
    std::snprintf(searchBuf, sizeof(searchBuf), "%s", search.c_str());
    ImGui::SetNextItemWidth(searchWidth);
    if (host.ConsumeAssetSearchFocus && host.ConsumeAssetSearchFocus()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputTextWithHint("##AssetFilter", ICON_FA_MAGNIFYING_GLASS "  Search...",
            searchBuf, sizeof(searchBuf))) {
        search = searchBuf;
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
        Tooltip(host,
            "Search every asset by name, across all folders.\n"
            "Type multiple words to match all of them (AND).\n"
            "t:Model / t:Texture / t:Sound / t:Scene / t:Prefab / t:Folder\n"
            "  restricts by type - listing several ORs them together.\n"
            "l:label restricts by label (set in an asset's right-click\n"
            "  menu) - listing several ANDs them, requiring every one.");
    }

    std::vector<std::string> knownLabels;
    {
        const int n = host.GetKnownLabelCount ? host.GetKnownLabelCount() : 0;
        for (int i = 0; i < n; ++i) {
            char buf[kPathBuf] = {};
            if (host.GetKnownLabel && host.GetKnownLabel(i, buf, (int)sizeof(buf))) knownLabels.emplace_back(buf);
        }
    }

    bool anyFilterActive = false;
    for (const char* t : {"t:model", "t:texture", "t:sound", "t:scene", "t:prefab", "t:folder"})
        anyFilterActive |= SearchHasToken(search, t);
    for (const auto& lbl : knownLabels)
        anyFilterActive |= SearchHasToken(search, "l:" + lbl);

    // Favourites-only toggle (#236 G).
    ImGui::SameLine();
    {
        bool favOnly = host.GetAssetFavoritesOnly && host.GetAssetFavoritesOnly();
        if (favOnly) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab));
        if (ImGui::Button(ICON_FA_STAR "##favonly") && host.SetAssetFavoritesOnly)
            host.SetAssetFavoritesOnly(!favOnly);
        if (favOnly) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            Tooltip(host, favOnly ? "Showing favourites only (click to show all)"
                                  : "Show favourites only");
    }

    // Search scope toggle (#236 G): folder (+subfolders) vs whole project.
    ImGui::SameLine();
    {
        bool global = host.GetAssetSearchGlobal && host.GetAssetSearchGlobal();
        if (global) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab));
        if (ImGui::Button(global ? ICON_FA_GLOBE : ICON_FA_FOLDER_TREE) && host.SetAssetSearchGlobal)
            host.SetAssetSearchGlobal(!global);
        if (global) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            Tooltip(host, global ? "Search scope: whole project (click for this folder)"
                                 : "Search scope: this folder + subfolders (click for whole project)");
    }

    ImGui::SameLine();
    if (anyFilterActive) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    const bool openFilters = ImGui::Button(ICON_FA_FILTER);
    if (anyFilterActive) ImGui::PopStyleColor();
    if (openFilters) ImGui::OpenPopup("##AssetFilters");
    if (ImGui::IsItemHovered()) Tooltip(host, "Filter by asset type or label");
    if (ImGui::BeginPopup("##AssetFilters")) {
        ImGui::SeparatorText("Type");
        static const std::pair<const char*, const char*> kTypes[] = {
            {"Model", "model"}, {"Texture", "texture"}, {"Sound", "sound"},
            {"Scene", "scene"}, {"Prefab", "prefab"}, {"Folder", "folder"},
        };
        for (const auto& [label, token] : kTypes) {
            const std::string full = std::string("t:") + token;
            if (ImGui::MenuItem(label, nullptr, SearchHasToken(search, full)))
                ToggleSearchToken(search, full);
        }

        ImGui::SeparatorText("Label");
        if (knownLabels.empty()) {
            ImGui::TextDisabled("No labels yet - add one from an\nasset's right-click menu.");
        } else {
            std::string labelMenuFilter = HostString(host.GetAssetLabelMenuFilter);
            char lbuf[64];
            std::snprintf(lbuf, sizeof(lbuf), "%s", labelMenuFilter.c_str());
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputTextWithHint("##LabelMenuFilter", ICON_FA_MAGNIFYING_GLASS "  Search labels...",
                    lbuf, sizeof(lbuf)) && host.SetAssetLabelMenuFilter) {
                host.SetAssetLabelMenuFilter(lbuf);
                labelMenuFilter = lbuf;
            }
            for (const auto& lbl : knownLabels) {
                if (!MatchesFilter(labelMenuFilter, lbl)) continue;
                const std::string full = "l:" + lbl;
                if (ImGui::MenuItem(lbl.c_str(), nullptr, SearchHasToken(search, full)))
                    ToggleSearchToken(search, full);
            }
        }

        if (anyFilterActive) {
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_XMARK "  Clear all filters")) {
                std::string kept, w;
                for (size_t i = 0; i <= search.size(); ++i) {
                    if (i == search.size() || search[i] == ' ' || search[i] == '\t') {
                        if (!w.empty() && w.rfind("t:", 0) != 0 && w.rfind("l:", 0) != 0)
                            kept += (kept.empty() ? "" : " ") + w;
                        w.clear();
                    } else w += search[i];
                }
                search = kept;
            }
        }
        ImGui::EndPopup();
    }

    // Sort control + Refresh (#236 G).
    ImGui::SameLine();
    const int sortPacked = host.GetAssetSort ? host.GetAssetSort() : 0;
    int sortMode = (sortPacked >> 1) & 3;
    bool sortDesc = (sortPacked & 1) != 0;
    if (ImGui::Button(ICON_FA_ARROW_DOWN_SHORT_WIDE)) ImGui::OpenPopup("##AssetSort");
    if (ImGui::IsItemHovered()) Tooltip(host, "Sort the grid");
    if (ImGui::BeginPopup("##AssetSort")) {
        static const char* kModes[] = { "Name", "Type", "Date modified", "Size" };
        for (int i = 0; i < 4; ++i)
            if (ImGui::MenuItem(kModes[i], nullptr, sortMode == i)) sortMode = i;
        ImGui::Separator();
        if (ImGui::MenuItem("Ascending", nullptr, !sortDesc)) sortDesc = false;
        if (ImGui::MenuItem("Descending", nullptr, sortDesc)) sortDesc = true;
        ImGui::EndPopup();
    }
    const int newPacked = sortMode * 2 + (sortDesc ? 1 : 0);
    if (newPacked != sortPacked && host.SetAssetSort) host.SetAssetSort(newPacked);

    ImGui::SameLine();
    if (refreshFlash > 0.0f)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.85f, 0.52f, 1.0f)); // green while confirming
    if (ImGui::Button(ICON_FA_ROTATE) && host.RefreshAssetBrowser) host.RefreshAssetBrowser();
    if (refreshFlash > 0.0f) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) Tooltip(host, "Refresh - re-scan folders and thumbnails (Ctrl+R)");

    ImGui::EndChild(); // ##AssetToolbar

    if (search != searchBefore && host.SetAssetSearch) host.SetAssetSearch(search.c_str());

    ImGui::Separator();

    // --- tree | splitter | grid --------------------------------------------------------
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
    const float contentHeight = ImGui::GetContentRegionAvail().y - footerHeight;
    const float treeWidth = host.GetAssetTreeWidth ? host.GetAssetTreeWidth() : 180.0f;

    ImGui::BeginChild("##AssetTree", ImVec2(treeWidth, contentHeight), ImGuiChildFlags_None);
    DrawFolderNode(host, folders, "", true, curFolder, reveal);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    ImGui::Button("##AssetTreeSplitter", ImVec2(6.0f, contentHeight));
    ImGui::PopStyleColor(3);
    {
        const bool active = ImGui::IsItemActive();
        const bool hot = active || ImGui::IsItemHovered();
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const float cx = ImFloor((mn.x + mx.x) * 0.5f) + 0.5f;
        const ImU32 col = ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive
                                           : hot ? ImGuiCol_SeparatorHovered : ImGuiCol_Border);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(cx, mn.y + 2.0f), ImVec2(cx, mx.y - 2.0f),
                                            col, hot ? 2.0f : 1.0f);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive() && host.SetAssetTreeWidth)
        host.SetAssetTreeWidth(treeWidth + ImGui::GetIO().MouseDelta.x, /*commit=*/false);
    if (ImGui::IsItemDeactivated() && host.SetAssetTreeWidth)
        host.SetAssetTreeWidth(host.GetAssetTreeWidth ? host.GetAssetTreeWidth() : treeWidth, /*commit=*/true);
    ImGui::SameLine();

    DrawAssetGrid(host, contentHeight);

    ImGui::End();
}

} // namespace EditorModuleAssetBrowser
