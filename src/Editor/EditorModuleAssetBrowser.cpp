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
#include "EditorPanels.h"
#include "EditorTheme.h"
#include "EditorUIPrimitives.h"

#include <imgui.h>
#include <imgui_internal.h> // ImFloor
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>
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

// Forwards to the shared implementation (EditorUIPrimitives.h, Defect #53).
bool ActionButton(const EditorModuleHostAPI& host, const char* icon, const char* tooltip, bool active = false) {
    const float s = ImGui::GetFrameHeight();
    return EditorUIPrimitives::ActionButton(icon, tooltip, host.SetTooltip, active, ImVec2(s, s));
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

// EditorInternal::PushTabChromeText — used to keep the dock tab bar's text white against
// Windows XP's saturated-green tab chrome. Phase 1 item 9 removed that theme; Light's tabs are
// neutral and already pair with its own normal text colour, so this is a permanent no-op now
// (see EditorLayerInternal.h's PanelChromeIsLight for the full explanation).
bool PanelChromeIsLight() {
    return false;
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

// What a drawn folder row tells its parent, so the parent can draw the tree lines joining them.
struct FolderRowGeom { float MidY = 0.0f; float LeftX = 0.0f; };

// Ported from EditorLayer::DrawFolderTreeNode. `folders` is this frame's snapshot; `curFolder`
// and `reveal` come from the host. Selection / expansion / drag-drop all route through host
// callbacks — nothing is mutated locally.
//
// Editor Enhancers / vFolders (API v40): the row is a label-less TreeNode (it keeps the arrow,
// hit box, selection and drag/drop); the icon, name and content minimap are painted over it at
// GetTreeNodeToLabelSpacing() - the same approach as the Hierarchy's rows - so the icon can take
// the folder's colour without tinting the name. A colour wash / zebra stripe goes on a draw-list
// channel underneath the node's own selection highlight.
FolderRowGeom DrawFolderNode(const EditorModuleHostAPI& host, const std::vector<std::string>& folders,
                             const std::string& folderPath, bool isRoot,
                             const std::string& curFolder, const std::string& reveal,
                             unsigned flags, int& rowIndex) {
    std::vector<std::string> children;
    for (const auto& f : folders)
        if (ParentFolderOf(f) == folderPath) children.push_back(f);
    std::sort(children.begin(), children.end(),
              [](const std::string& a, const std::string& b) { // A-Z ignoring case, like the grid
                  const std::string la = LeafNameOf(a), lb = LeafNameOf(b);
                  return std::lexicographical_compare(la.begin(), la.end(), lb.begin(), lb.end(), [](char x, char y) {
                      return std::tolower((unsigned char)x) < std::tolower((unsigned char)y); });
              });

    const bool hasChildren = !children.empty();
    const bool wasExpanded = isRoot ||
        (host.IsAssetFolderExpanded && host.IsAssetFolderExpanded(folderPath.c_str()));
    if (!isRoot) ImGui::SetNextItemOpen(wasExpanded);

    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (curFolder == folderPath) nodeFlags |= ImGuiTreeNodeFlags_Selected;
    if (!hasChildren) nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (isRoot) nodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;

    EditorFolderVisual vis;
    if (!isRoot && host.GetFolderVisual) host.GetFolderVisual(folderPath.c_str(), &vis);
    const bool styled = (flags & kFolderTreeStyles) != 0;
    const char* glyph = isRoot ? ICON_FA_FOLDER_TREE
                      : (styled && vis.Icon[0]) ? vis.Icon
                      : (flags & kFolderTreeMinimal) ? nullptr
                      : (wasExpanded && hasChildren) ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER;
    const std::string name = isRoot ? std::string("Assets") : LeafNameOf(folderPath);
    const int myRow = rowIndex++;

    ImGui::PushID(folderPath.c_str());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    // NoNav (Defect #43): the host's own Enter/Backspace/Left/Right folder-traversal block
    // (EditorLayer.cpp) already drives m_CurrentAssetFolder/m_ExpandedAssetFolders off these same
    // keys — without this flag, ImGui's keyboard nav would also toggle whichever row last got
    // mouse focus, independently of m_CurrentAssetFolder, once NavEnableKeyboard is on.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const bool open = ImGui::TreeNodeEx("##node", nodeFlags);
    ImGui::PopItemFlag();
    const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
    const bool rowHovered = ImGui::IsItemHovered();

    // Underneath the node (channel 0): zebra stripe, then the folder colour as a wash.
    dl->ChannelsSetCurrent(0);
    const float wx0 = ImGui::GetWindowPos().x, wx1 = wx0 + ImGui::GetWindowWidth();
    if ((flags & kFolderTreeZebra) && (myRow % 2) == 1)
        dl->AddRectFilled(ImVec2(wx0, rmin.y), ImVec2(wx1, rmax.y), EditorTheme::U32(EditorTheme::Stripe));
    if (styled && (flags & kFolderTreeWash) && vis.Color)
        dl->AddRectFilledMultiColor(ImVec2(rmin.x, rmin.y), ImVec2(wx1, rmax.y),
                                    (vis.Color & 0x00FFFFFFu) | 0x50000000u, vis.Color & 0x00FFFFFFu,
                                    vis.Color & 0x00FFFFFFu, (vis.Color & 0x00FFFFFFu) | 0x50000000u);
    dl->ChannelsMerge();

    // Icon + name over the node, then the content minimap right-aligned in the small font.
    {
        const float x0 = rmin.x + ImGui::GetTreeNodeToLabelSpacing();
        const float cy = (rmin.y + rmax.y) * 0.5f;
        float right = rmax.x - EditorTheme::Px(4.0f);
        if ((flags & kFolderTreeMinimap) && vis.MiniCount > 0 && !isRoot) {
            EditorTheme::PushSmall();
            const float cell = ImGui::GetFontSize() * 1.15f;
            float mx = right - vis.MiniCount * cell;
            right = mx - EditorTheme::Px(4.0f);
            for (int k = 0; k < vis.MiniCount; ++k, mx += cell) {
                const ImVec2 gs = ImGui::CalcTextSize(vis.Mini[k]);
                dl->AddText(ImVec2(mx + (cell - gs.x) * 0.5f, cy - gs.y * 0.5f), EditorTheme::U32(EditorTheme::Dim), vis.Mini[k]);
            }
            EditorTheme::PopFont();
        }
        float x = x0;
        if (glyph) {
            const ImVec2 gs = ImGui::CalcTextSize(glyph);
            const ImU32 gc = (styled && vis.Color) ? (vis.Color | 0xFF000000u) : EditorTheme::U32(EditorTheme::Secondary);
            dl->AddText(ImVec2(x, cy - gs.y * 0.5f), gc, glyph);
            x += std::max(gs.x, ImGui::GetFontSize()) + EditorTheme::Px(6.0f);
        }
        const ImVec2 ns = ImGui::CalcTextSize(name.c_str());
        dl->PushClipRect(ImVec2(x, rmin.y), ImVec2(std::max(x, right), rmax.y), true);
        dl->AddText(ImVec2(x, cy - ns.y * 0.5f), EditorTheme::U32(EditorTheme::Text), name.c_str());
        dl->PopClipRect();
    }

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
    if (rowHovered && host.SetHoveredAssetFolder) host.SetHoveredAssetFolder(folderPath.c_str()); // vFolders hover keys

    if (!isRoot && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("ASSET_FOLDER_PATH", folderPath.c_str(), folderPath.size() + 1);
        ImGui::TextUnformatted(LeafNameOf(folderPath).c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        for (const char* type : {"ASSET_MODEL_PATH", "ASSET_TEXTURE_PATH",
                                 "ASSET_SOUND_PATH", "ASSET_PREFAB_PATH",
                                 "ASSET_MATERIAL_PATH", // #184 - materials were missing
                                 "ASSET_FILE_PATH"}) {  // controllers, weapon definitions, scripts
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
    if (rowHovered && !ImGui::IsItemToggledOpen()) {
        if (isRoot) {
            Tooltip(host, "The root of every asset the editor knows about.\nAlt+click a folder's arrow to expand/collapse it and everything under it.");
        } else {
            std::string tip = "Click to browse. Drag assets or other folders onto it to file them here.";
            if (vis.Total > 0) tip += "\n" + std::to_string(vis.Total) + (vis.Total == 1 ? " asset" : " assets") + " directly inside.";
            tip += "\nE expand  /  Shift+E isolate  /  right-click for colour, icon and bookmark.";
            Tooltip(host, tip.c_str());
        }
    }
    // vFolders: colour, icon, bookmark, rule (host-drawn menu body).
    if (host.DrawFolderContextMenuBody && ImGui::BeginPopupContextItem("##folderCtx")) {
        host.DrawFolderContextMenuBody(folderPath.c_str());
        ImGui::EndPopup();
    }
    ImGui::PopID();

    FolderRowGeom me;
    me.MidY = (rmin.y + rmax.y) * 0.5f;
    me.LeftX = rmin.x;
    if (open) {
        std::vector<FolderRowGeom> kids;
        kids.reserve(children.size());
        for (const auto& child : children)
            kids.push_back(DrawFolderNode(host, folders, child, false, curFolder, reveal, flags, rowIndex));
        // Tree lines: one vertical from under this row's arrow down to its last child, and a stub
        // into each child. The root's children start at the left edge, so it draws none.
        if ((flags & kFolderTreeLines) && !kids.empty() && !isRoot) {
            const float x = std::floor(rmin.x + ImGui::GetFontSize() * 0.5f + ImGui::GetStyle().FramePadding.x) + 0.5f;
            const ImU32 lc = EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Dim, 0.55f));
            for (const auto& k : kids)
                dl->AddLine(ImVec2(x, std::floor(k.MidY) + 0.5f), ImVec2(k.LeftX + EditorTheme::Px(2.0f), std::floor(k.MidY) + 0.5f), lc);
            dl->AddLine(ImVec2(x, rmax.y), ImVec2(x, std::floor(kids.back().MidY) + 0.5f), lc);
        }
        // NoTreePushOnOpen is set whenever !hasChildren (root or not), so TreePop is gated on
        // hasChildren alone — see the host history at DrawFolderTreeNode.
        if (hasChildren) ImGui::TreePop();
    }
    return me;
}

// Details view: a real ImGui table (resizable, sortable, header frozen). The host's DrawAssetCell
// fills column 0 (icon + the row's Selectable, spanning all columns) and the Type / Size /
// Modified cells itself. Header clicks drive the same sort state as the toolbar's Sort menu
// (packed mode*2+desc; modes 0 Name, 1 Type, 2 Date modified, 3 Size) - each column's user id is
// its sort mode.
void DrawDetailsTable(const EditorModuleHostAPI& host, float uiScale, int cellCount,
                      float cellWidth, float cellHeight) {
    namespace T = EditorTheme;
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,     T::Raised);
    ImGui::PushStyleColor(ImGuiCol_TableRowBg,        T::WithAlpha(T::Panel, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt,     T::Stripe);
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight,  T::Hairline);
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, T::Hairline);

    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("##AssetDetails", 4, flags, ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("NAME",     ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 0.0f, 0);
        ImGui::TableSetupColumn("TYPE",     ImGuiTableColumnFlags_WidthFixed, kAssetDetailsTypeColW * uiScale, 1);
        ImGui::TableSetupColumn("SIZE",     ImGuiTableColumnFlags_WidthFixed, kAssetDetailsSizeColW * uiScale, 3);
        ImGui::TableSetupColumn("MODIFIED", ImGuiTableColumnFlags_WidthFixed, kAssetDetailsModifiedColW * uiScale, 2);

        // Keep the table's sort indicator and the host's sort state in step both ways.
        const int hostPacked = host.GetAssetSort ? host.GetAssetSort() : 0;
        const int hostMode = (hostPacked >> 1) & 3;
        const bool hostDesc = (hostPacked & 1) != 0;
        ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
        const bool haveSpec = specs && specs->SpecsCount > 0;
        const int specMode = haveSpec ? (int)specs->Specs[0].ColumnUserID : -1;
        const bool specDesc = haveSpec && specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
        if (specs && specs->SpecsDirty) {
            if (haveSpec && (specMode != hostMode || specDesc != hostDesc) && host.SetAssetSort)
                host.SetAssetSort(specMode * 2 + (specDesc ? 1 : 0));
            specs->SpecsDirty = false;
        } else if (!haveSpec || specMode != hostMode || specDesc != hostDesc) {
            const int col = hostMode == 0 ? 0 : hostMode == 1 ? 1 : hostMode == 3 ? 2 : 3;
            ImGui::TableSetColumnSortDirection(col, hostDesc ? ImGuiSortDirection_Descending
                                                              : ImGuiSortDirection_Ascending, false);
        }

        // Column titles in the heading voice (mono capitals), muted until hovered.
        ImGui::PushStyleColor(ImGuiCol_Text, T::Secondary);
        T::PushHeading();
        ImGui::TableHeadersRow();
        T::PopFont();
        ImGui::PopStyleColor();

        ImGuiListClipper clipper;
        clipper.Begin(cellCount);
        while (clipper.Step()) {
            for (int idx = clipper.DisplayStart; idx < clipper.DisplayEnd; ++idx) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (host.DrawAssetCell) host.DrawAssetCell(idx, cellWidth, cellHeight, false);
            }
        }
        clipper.End();
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(5);
}

// The asset grid (API v6): the module owns the ##AssetList child, the ImGuiListClipper row loop
// with per-row SameLine wrapping, and the footer; every cell + the background + the delete-confirm
// popup are host code (DrawAssetCell / HandleAssetGridBackground / AssetGridFrameEnd). Cell size
// and cellsPerRow are pure math from GetAssetGridMetrics — ported from the host's old
// DrawAssetGridBody.
void DrawAssetGrid(const EditorModuleHostAPI& host, float contentHeight) {
    float iconSize = 64.0f, uiScale = 1.0f, listMinIcon = 24.0f;
    if (host.GetAssetGridMetrics) host.GetAssetGridMetrics(&iconSize, &uiScale, &listMinIcon);

    // Phase 5 item 4 — Details is a third mode layered on top of the existing icon-size-derived
    // Grid/List split: it always forces single-column rows (like List), plus a fixed header row
    // above the scrolling list (Explorer/Finder's own Details convention) that this function
    // draws before BeginChild so it doesn't scroll with the rows.
    const int viewMode = host.GetAssetViewMode ? host.GetAssetViewMode() : -1;
    const bool detailsMode = viewMode == 2;
    const bool gridMode = !detailsMode && iconSize > listMinIcon * uiScale;

    // Details view's column header lives inside the table (DrawDetailsTable), so the list child
    // simply fills the space.
    const float headerH = 0.0f;

    // ImGui::BeginChild treats a <=0 size specially ("fill available" for 0, "fill available minus
    // |size|" for negative) rather than as a literal small height - on a short docked panel where
    // headerH can meet or exceed contentHeight, contentHeight - headerH could be exactly that, and
    // the child would balloon past the panel instead of shrinking, pushing its rows (and the
    // footer below it) off-screen. Floor it to at least one row so the size is always explicit.
    const float listH = std::max(contentHeight - headerH, ImGui::GetFrameHeightWithSpacing());
    ImGui::BeginChild("##AssetList", ImVec2(0, listH), ImGuiChildFlags_None);

    if (host.AssetGridFrameBegin) host.AssetGridFrameBegin();

    const float cellPadding = 8.0f * uiScale;
    const float cellWidth = iconSize + cellPadding * 2.0f;
    // Phase 5 item 4 — grid labels wrap to 2 lines now (was 1); reserve a second line height.
    // List/Details rows are unaffected (clipRowHeight below uses GetFrameHeightWithSpacing() for
    // those, not cellHeight).
    const float cellHeight = iconSize + cellPadding + ImGui::GetTextLineHeightWithSpacing() * 2.0f;

    const int cellCount = host.AssetGridCellCount ? host.AssetGridCellCount() : 0;

    int cellsPerRow = 1;
    if (gridMode) {
        const float availW = ImGui::GetContentRegionAvail().x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        cellsPerRow = (int)((availW + spacing) / (cellWidth + spacing));
        if (cellsPerRow < 1) cellsPerRow = 1;
    }
    const int totalRows = cellCount == 0 ? 0 : (cellCount + cellsPerRow - 1) / cellsPerRow;
    // Match the actual cursor advance in DrawAssetCell: list rows are text-sized
    // Selectables, not framed controls. A larger stride makes the clipper submit
    // too few rows and leaves a blank band at the bottom of the list. Grid tiles
    // advance by their explicit height plus the spacing between rows.
    const float clipRowHeight = gridMode ? cellHeight + ImGui::GetStyle().ItemSpacing.y
                                        : ImGui::GetTextLineHeightWithSpacing();

    if (detailsMode) {
        DrawDetailsTable(host, uiScale, cellCount, cellWidth, cellHeight);
    } else {
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
    }

    if (host.HandleAssetGridBackground) host.HandleAssetGridBackground();

    ImGui::EndChild(); // ##AssetList

    // Footer: selection summary on the left, the icon-size slider on the right (drag to the
    // minimum for the compact list view).
    ImGui::BeginChild("##AssetGridFooter", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    char summary[256] = {};
    if (host.GetAssetSelectionSummary) host.GetAssetSelectionSummary(summary, (int)sizeof(summary));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(EditorTheme::Dim, "%s", summary);

    // Phase 5 item 3/4: an explicit Grid/List/Details toggle beside the slider — the slider alone
    // (drag-to-minimum) was the only way to reach list view, which isn't discoverable, and Details
    // has no icon-size equivalent at all. Icon + tooltip reflect the mode you'd SWITCH TO
    // (Grid -> List -> Details -> Grid), matching the rest of the toolbar's toggle buttons.
    const float toggleWidth = ImGui::GetFrameHeight();
    const float sliderWidth = 132.0f * uiScale; // #184
    const float toggleX = ImGui::GetWindowContentRegionMax().x - sliderWidth - toggleWidth - ImGui::GetStyle().ItemSpacing.x;
    if (toggleX > ImGui::GetCursorPosX()) ImGui::SameLine(toggleX);
    else ImGui::NewLine();
    const char* nextIcon = viewMode == 0 ? ICON_FA_LIST : viewMode == 1 ? ICON_FA_TABLE_LIST : ICON_FA_TABLE_CELLS_LARGE;
    const char* nextTip  = viewMode == 0 ? "List view"  : viewMode == 1 ? "Details view"       : "Grid view";
    if (ActionButton(host, nextIcon, nextTip) && host.ToggleAssetViewMode)
        host.ToggleAssetViewMode();

    // A normal-looking slider: a visible rounded track (the footer sits straight on the panel
    // background, where the theme's default FrameBg is near-invisible) and the px value shown,
    // matching the sliders in Preferences.
    const float sliderX = ImGui::GetWindowContentRegionMax().x - sliderWidth;
    ImGui::SameLine(sliderX);
    ImGui::SetNextItemWidth(sliderWidth);
    float sliderVal = iconSize;
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        EditorTheme::Field);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, EditorTheme::Rgb(0x1A, 0x1A, 0x20));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  EditorTheme::Rgb(0x20, 0x20, 0x27));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);
    // Details rows don't use the icon-size slider at all (a fixed small icon, like List) — disable
    // rather than hide it, so the toggle button next to it doesn't jump position when cycling.
    ImGui::BeginDisabled(detailsMode);
    const bool changed = ImGui::SliderFloat("##IconSize", &sliderVal,
                                            listMinIcon * uiScale, 128.0f * uiScale, "%.0f px");
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (changed && host.SetAssetIconSize) host.SetAssetIconSize(sliderVal, /*commit=*/false);
    if (ImGui::IsItemHovered() && host.SetTooltip)
        host.SetTooltip(detailsMode ? "Icon size (Grid/List only)" :
                        "Icon size - drag all the way to the left for a compact list view.");
    if (ImGui::IsItemDeactivatedAfterEdit() && host.SetAssetIconSize)
        host.SetAssetIconSize(sliderVal, /*commit=*/true);
    ImGui::EndChild();

    if (host.AssetGridFrameEnd) host.AssetGridFrameEnd();
}

} // namespace

void Draw(const EditorModuleHostAPI& host) {
    if (host.GetShowAssetBrowser && !host.GetShowAssetBrowser()) return;

    // #37 — hoisted to the top of Draw() so every unscaled-literal fix below in this function can
    // share it; GetToolbarMetrics is the cheapest host call that reports UI scale from here.
    float uiScale = 1.0f;
    if (host.GetToolbarMetrics) host.GetToolbarMetrics(nullptr, nullptr, &uiScale);

    bool visible = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * uiScale, 4.0f * uiScale)); // #37
    PushTabChromeText();
    const bool open = ImGui::Begin(EditorPanels::Assets, &visible, ImGuiWindowFlags_None);
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
    EditorUIPrimitives::BeginPanelToolbar("AssetToolbar");

    // #14 — was a bare 200.0f, so at anything other than 1x UI scale the search box stayed a
    // fixed pixel width while every neighbouring control (buttons, breadcrumb text) scaled with it.
    const float searchWidth = 200.0f * uiScale;

    if (ActionButton(host, ICON_FA_PLUS, "Create / Import")) ImGui::OpenPopup("##AssetCreateMenu");
    if (ImGui::BeginPopup("##AssetCreateMenu")) {
        if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Folder")) {
            const std::string candidate = UniqueNewFolder(folders, curFolder);
            if (host.CreateFolderUndoable) host.CreateFolderUndoable(candidate.c_str());
            if (host.BeginRenameFolder) host.BeginRenameFolder(candidate.c_str());
        }
        ImGui::Separator();
        // Phase 5 item 5 — one "Import Asset..." entry replacing the three separate
        // Model/Texture/Sound items; ImportAssetViaDialog(3, ...) opens a combined-filter file
        // picker and ImportDroppedFile (host-side) infers the real type from the extension either
        // way, the same as it always has for a drag-drop or a folder-drop import.
        if (ImGui::MenuItem(ICON_FA_FILE_IMPORT "  Import Asset...") && host.ImportAssetViaDialog)
            host.ImportAssetViaDialog(3, curFolder.c_str());
        if (ImGui::IsItemHovered())
            Tooltip(host, "Model (.fbx/.obj/.gltf/.glb), image (.png/.jpg/.tga/.bmp) or\n"
                          "audio (.wav/.mp3/.ogg/.flac) - the type is detected automatically.\n"
                          "You can also just drag a file in from Explorer.");
        ImGui::EndPopup();
    }

    VSeparator();

    // Phase 5 item 3 — Back / Forward / Up. Back/Forward walk the host's NavigateAssetFolder
    // trail; Up is just "go to my own parent," computed client-side and sent through the same
    // SetCurrentAssetFolder the tree/breadcrumb already use (which now records history itself).
    {
        const bool canBack = host.CanAssetFolderHistoryBack && host.CanAssetFolderHistoryBack();
        const bool canForward = host.CanAssetFolderHistoryForward && host.CanAssetFolderHistoryForward();
        const bool canUp = !curFolder.empty();

        ImGui::BeginDisabled(!canBack);
        if (ActionButton(host, ICON_FA_ARROW_LEFT, "Back") && host.AssetFolderHistoryBack)
            host.AssetFolderHistoryBack();
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, EditorTheme::Px(4.0f));
        ImGui::BeginDisabled(!canForward);
        if (ActionButton(host, ICON_FA_ARROW_RIGHT, "Forward") && host.AssetFolderHistoryForward)
            host.AssetFolderHistoryForward();
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, EditorTheme::Px(4.0f));
        ImGui::BeginDisabled(!canUp);
        if (ActionButton(host, ICON_FA_ARROW_UP, "Up one folder") && host.SetCurrentAssetFolder)
            host.SetCurrentAssetFolder(ParentFolderOf(curFolder).c_str());
        ImGui::EndDisabled();
    }

    VSeparator();

    const float refreshFlash = host.GetAssetRefreshFlash ? host.GetAssetRefreshFlash() : 0.0f;

    // Breadcrumb — each segment is its own borderless button, jumping straight to that folder
    // (Phase 5 item 3; used to be one static TextDisabled line, "context only, non-interactive").
    // The post-refresh confirmation (#236 G) still rides on the toolbar here rather than a row
    // below it; it fades over its last second.
    {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorUIPrimitives::FlatHover());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorUIPrimitives::FlatPressed());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * uiScale, ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f); // flat breadcrumb, no hairline box

        // The root segment is only a link when we're not already there; otherwise it's just the
        // "you are here" label like every other trailing segment below.
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Secondary);
        if (curFolder.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Text);
            ImGui::TextUnformatted("Assets");
            ImGui::PopStyleColor();
        } else if (ImGui::Button("Assets") && host.SetCurrentAssetFolder) {
            host.SetCurrentAssetFolder("");
        }
        if (!curFolder.empty() && ImGui::IsItemHovered()) Tooltip(host, "Go to the Assets root.");

        std::string prefix;
        size_t segStart = 0;
        while (segStart < curFolder.size()) {
            size_t slash = curFolder.find('/', segStart);
            std::string segment = curFolder.substr(segStart, slash == std::string::npos ? std::string::npos : slash - segStart);
            prefix = prefix.empty() ? segment : (prefix + "/" + segment);
            const bool isLast = slash == std::string::npos;

            ImGui::SameLine(0.0f, EditorTheme::Px(2.0f));
            EditorTheme::PushSmall();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(EditorTheme::Dim, ICON_FA_CHEVRON_RIGHT);
            EditorTheme::PopFont();
            ImGui::SameLine(0.0f, EditorTheme::Px(2.0f));
            ImGui::PushID((int)segStart);
            if (isLast) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(EditorTheme::Text, "%s", segment.c_str());
            } else {
                if (ImGui::Button(segment.c_str()) && host.SetCurrentAssetFolder)
                    host.SetCurrentAssetFolder(prefix.c_str());
                if (ImGui::IsItemHovered()) Tooltip(host, ("Go to " + prefix + ".").c_str());
            }
            ImGui::PopID();

            if (isLast) break;
            segStart = slash + 1;
        }
        ImGui::PopStyleColor(); // the segments' secondary text
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);

        if (refreshFlash > 0.0f) {
            const float a = refreshFlash > 1.0f ? 1.0f : refreshFlash;
            ImGui::SameLine(0.0f, 12.0f * uiScale);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::WithAlpha(EditorTheme::Success, a));
            ImGui::TextUnformatted(ICON_FA_CIRCLE_CHECK "  Assets refreshed");
            ImGui::PopStyleColor();
        }
    }

    // Search box + the trailing icon buttons (Filters, Sort, Refresh — #236 G), pinned to the
    // right edge (or a new line if the breadcrumb has crowded them out).
    const float clusterSpacing=EditorTheme::Px(4.0f);
    const float trailingButtonsWidth=ImGui::GetFrameHeight()*5.0f+clusterSpacing*4.0f+EditorTheme::Px(6.0f);
    const float targetX=ImGui::GetWindowContentRegionMax().x-EditorTheme::Px(6.0f)-(searchWidth+trailingButtonsWidth);
    const float breadcrumbEndX=ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x+EditorTheme::Px(8.0f);
    if (targetX >= breadcrumbEndX) ImGui::SameLine(targetX);
    else ImGui::NewLine();

    char searchBuf[128];
    std::snprintf(searchBuf, sizeof(searchBuf), "%s", search.c_str());
    if (host.ConsumeAssetSearchFocus && host.ConsumeAssetSearchFocus()) ImGui::SetKeyboardFocusHere();
    if (EditorUIPrimitives::SearchField("##AssetFilter", searchBuf, sizeof(searchBuf), "Search assets", searchWidth))
        search = searchBuf;
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
        Tooltip(host,
            "Search every asset by name, across all folders.\n"
            "Type multiple words to match all of them (AND).\n"
            "t:Model / t:Texture / t:Material / t:Shader / t:Sound / t:Scene /\n"
            "  t:Prefab / t:Screenshot / t:Folder\n"
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
    for (const char* t : {"t:model", "t:texture", "t:material", "t:shader", "t:sound", "t:scene",
                          "t:prefab", "t:screenshot", "t:folder"})
        anyFilterActive |= SearchHasToken(search, t);
    for (const auto& lbl : knownLabels)
        anyFilterActive |= SearchHasToken(search, "l:" + lbl);

    // Favourites-only toggle (#236 G).
    ImGui::SameLine(0.0f, EditorTheme::Px(6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(clusterSpacing, ImGui::GetStyle().ItemSpacing.y));
    {
        bool favOnly = host.GetAssetFavoritesOnly && host.GetAssetFavoritesOnly();
        if (ActionButton(host, ICON_FA_STAR, favOnly ? "Showing favorites only (click to show all)" // #19 — en-US
                                                     : "Show favorites only", favOnly) && host.SetAssetFavoritesOnly)
            host.SetAssetFavoritesOnly(!favOnly);
    }

    // Search scope toggle (#236 G): folder (+subfolders) vs whole project.
    ImGui::SameLine();
    {
        bool global = host.GetAssetSearchGlobal && host.GetAssetSearchGlobal();
        if (ActionButton(host, global ? ICON_FA_GLOBE : ICON_FA_FOLDER_TREE,
                         global ? "Search scope: whole project (click for this folder)"
                                : "Search scope: this folder + subfolders (click for whole project)", global) &&
            host.SetAssetSearchGlobal)
            host.SetAssetSearchGlobal(!global);
    }

    ImGui::SameLine();
    if (ActionButton(host, ICON_FA_FILTER, "Filter by asset type or label", anyFilterActive))
        ImGui::OpenPopup("##AssetFilters");
    if (ImGui::BeginPopup("##AssetFilters")) {
        EditorUIPrimitives::SectionHeader("TYPE");
        static const std::pair<const char*, const char*> kTypes[] = {
            {"Model", "model"}, {"Texture", "texture"}, {"Material", "material"}, // #184
            {"Shader", "shader"}, {"Sound", "sound"}, {"Scene", "scene"},
            {"Prefab", "prefab"}, {"Screenshot", "screenshot"}, {"Folder", "folder"},
        };
        for (const auto& [label, token] : kTypes) {
            const std::string full = std::string("t:") + token;
            if (ImGui::MenuItem(label, nullptr, SearchHasToken(search, full)))
                ToggleSearchToken(search, full);
        }

        EditorUIPrimitives::SectionHeader("LABEL");
        if (knownLabels.empty()) {
            ImGui::TextDisabled("No labels yet - add one from an\nasset's right-click menu.");
        } else {
            std::string labelMenuFilter = HostString(host.GetAssetLabelMenuFilter);
            char lbuf[64];
            std::snprintf(lbuf, sizeof(lbuf), "%s", labelMenuFilter.c_str());
            if (EditorUIPrimitives::SearchField("##LabelMenuFilter", lbuf, sizeof(lbuf), "Search labels", 180.0f * uiScale) &&
                host.SetAssetLabelMenuFilter) {
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
    if (ActionButton(host, ICON_FA_ARROW_DOWN_SHORT_WIDE, "Sort the grid", sortPacked != 0)) ImGui::OpenPopup("##AssetSort");
    if (ImGui::BeginPopup("##AssetSort")) {
        EditorUIPrimitives::SectionHeader("SORT BY");
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
    if (ActionButton(host, ICON_FA_ROTATE, "Refresh - re-scan folders and thumbnails (Ctrl+R)") && host.RefreshAssetBrowser)
        host.RefreshAssetBrowser();
    ImGui::PopStyleVar(); // the icon cluster's tight spacing
    EditorUIPrimitives::EndPanelToolbar();

    // Editor Enhancers / vFolders: folder bookmark chips (API v40; draws nothing when unused).
    if (host.DrawFolderNavBar) host.DrawFolderNavBar();

    if (search != searchBefore && host.SetAssetSearch) host.SetAssetSearch(search.c_str());

    // --- tree | splitter | grid --------------------------------------------------------
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
    const float contentHeight = ImGui::GetContentRegionAvail().y - footerHeight;
    const float treeWidth = host.GetAssetTreeWidth ? host.GetAssetTreeWidth() : 180.0f;

    ImGui::BeginChild("##AssetTree", ImVec2(treeWidth, contentHeight), ImGuiChildFlags_None);
    {
        const unsigned treeFlags = host.GetFolderTreeFlags ? host.GetFolderTreeFlags() : 0u; // vFolders (API v40)
        int rowIndex = 0;
        DrawFolderNode(host, folders, "", true, curFolder, reveal, treeFlags, rowIndex);
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::InvisibleButton("##AssetTreeSplitter", ImVec2(6.0f * uiScale, contentHeight)); // #37 — the drag handle
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
