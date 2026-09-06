// The Asset Browser's chrome — the dock window, the "+ Create / Import" toolbar row, the
// breadcrumb, the search box + Filters popup, the left folder-tree pane and the tree/grid
// splitter — living inside TartarusEditor.dll so its layout hot-reloads while the editor stays
// open. Ported in behaviour from EditorLayer::DrawAssetBrowser / DrawFolderTreeNode
// (EditorLayer_AssetBrowser.cpp).
//
// Thin slice (issue #229): the asset GRID itself — tiles, GL thumbnails, context menus,
// rename-in-place, drag sources, delete/duplicate, the scenes/screenshots virtual folders — is
// still host code, drawn into this window through host.DrawAssetGridBody(). AssetLibrary never
// crosses the DLL boundary: the folder list arrives as a per-frame string snapshot
// (GetFolderCount/GetFolder), every mutation is an undoable command callback, and folder-tree
// expansion state stays host-side (so the Left/Right-arrow shortcuts keep working and it survives
// a reload) — reached here through Is/SetAssetFolderExpanded. Drag payloads are bare path strings,
// which are already boundary-safe.

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

    // Breadcrumb — context only, non-interactive.
    {
        std::string crumb = "Assets";
        for (char c : curFolder) crumb += (c == '/') ? " / " : std::string(1, c);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", crumb.c_str());
    }

    // Search box + the Filters button, pinned to the right edge (or a new line if the breadcrumb
    // has crowded it out).
    const float filterButtonsWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const float targetX = ImGui::GetWindowContentRegionMax().x - (searchWidth + filterButtonsWidth);
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

    if (host.DrawAssetGridBody) host.DrawAssetGridBody(contentHeight);

    ImGui::End();
}

} // namespace EditorModuleAssetBrowser
