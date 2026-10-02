// The Scene Hierarchy panel's frame — Begin("Scene Hierarchy"), the search box, and the
// Expand-all / Collapse-all buttons — living inside TartarusEditor.dll so the panel chrome
// hot-reloads while the editor stays open. Ported from the top of EditorLayer::DrawHierarchy
// (EditorLayer_Hierarchy.cpp).
//
// Thin slice (issue #229): the whole entity tree below the search row — the recursive rows,
// drag-reparent, the row + empty-space context menus, click/shift/ctrl selection, the delta
// undo, Ctrl+A-select-all — stays host code, drawn into this window through
// host.DrawHierarchyTreeBody(). EnTT and Components.h never cross the DLL boundary.

#include "EditorModuleAPI.h"
#include "EditorPanels.h"
#include "EditorTheme.h"
#include "EditorUIPrimitives.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <cmath>
#include <cstdio>

namespace EditorModuleHierarchy {

void Draw(const EditorModuleHostAPI& host) {
    if (host.GetShowHierarchy && !host.GetShowHierarchy()) return;

    bool visible = true;
    const bool open = ImGui::Begin(EditorPanels::Hierarchy, &visible, ImGuiWindowFlags_None);
    if (host.SetShowHierarchy) host.SetShowHierarchy(visible); // capture the title-bar X
    if (!open) { ImGui::End(); return; }

    // One toolbar row: the search box, then the type filter, sort and expand / collapse icons.
    // A bare string matches names; "t:Tag" matches TagComponent instead.
    EditorUIPrimitives::BeginPanelToolbar("HierarchyToolbar");
    const ImGuiStyle& st = ImGui::GetStyle();
    const float iconW = ImGui::GetFrameHeight();
    const int kIcons = 4;
    char filterBuf[128] = {};
    if (host.GetHierarchyFilter) host.GetHierarchyFilter(filterBuf, (int)sizeof(filterBuf));
    if (EditorUIPrimitives::SearchField("##HierarchyFilter", filterBuf, sizeof(filterBuf), "Search",
                                        -(iconW * kIcons + st.ItemSpacing.x * 0.5f * kIcons)) &&
        host.SetHierarchyFilter) {
        host.SetHierarchyFilter(filterBuf);
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive() && host.SetTooltip) {
        host.SetTooltip("Type a name to filter the list below.\n"
                        "Type \"t:\" followed by a tag (e.g. t:Enemy) to filter by Tag instead.\n\n"
                        "Tip: click a row in the tree, then type a name (without clicking here) "
                        "to jump to the next entity starting with those letters.");
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x * 0.5f, st.ItemSpacing.y));

    // Type filter (Phase 5 item 6): additive kinds in a popup; the icon takes the accent while any is on.
    int typeMask = host.GetHierarchyTypeFilter ? host.GetHierarchyTypeFilter() : 0;
    ImGui::SameLine();
    if (EditorUIPrimitives::ActionButton(ICON_FA_FILTER, typeMask ? "Filtered by type (click to change)" : "Show only some kinds of object",
                                         host.SetTooltip, typeMask != 0, ImVec2(iconW, iconW)))
        ImGui::OpenPopup("##HierarchyTypeFilter");
    if (ImGui::BeginPopup("##HierarchyTypeFilter")) {
        EditorUIPrimitives::SectionHeader("SHOW ONLY");
        auto kind = [&](const char* label, int bit) {
            bool on = (typeMask & bit) != 0;
            if (ImGui::MenuItem(label, nullptr, &on)) {
                typeMask ^= bit;
                if (host.SetHierarchyTypeFilter) host.SetHierarchyTypeFilter(typeMask);
            }
        };
        kind(ICON_FA_CUBE "  Meshes", kHierarchyFilterMesh);
        kind(ICON_FA_LIGHTBULB "  Lights", kHierarchyFilterLight);
        kind(ICON_FA_VIDEO "  Cameras", kHierarchyFilterCamera);
        kind(ICON_FA_VECTOR_SQUARE "  Empties and other", kHierarchyFilterOther);
        ImGui::Separator();
        if (ImGui::MenuItem("Show everything", nullptr, false, typeMask != 0) && host.SetHierarchyTypeFilter)
            host.SetHierarchyTypeFilter(0);
        ImGui::EndPopup();
    }

    const int sortPacked = host.GetHierarchySort ? host.GetHierarchySort() : 0;
    int sortMode = (sortPacked >> 1) & 3;
    bool sortDesc = (sortPacked & 1) != 0;
    ImGui::SameLine();
    if (EditorUIPrimitives::ActionButton(ICON_FA_ARROW_DOWN_SHORT_WIDE, "Sort the list", host.SetTooltip,
                                         sortPacked != 0, ImVec2(iconW, iconW)))
        ImGui::OpenPopup("##HierarchySort");
    if (ImGui::BeginPopup("##HierarchySort")) {
        EditorUIPrimitives::SectionHeader("SORT BY");
        static const char* kModes[] = { "Creation order", "Name", "Type" };
        for (int i = 0; i < 3; ++i)
            if (ImGui::MenuItem(kModes[i], nullptr, sortMode == i)) sortMode = i;
        ImGui::Separator();
        if (ImGui::MenuItem("Ascending", nullptr, !sortDesc)) sortDesc = false;
        if (ImGui::MenuItem("Descending", nullptr, sortDesc)) sortDesc = true;
        ImGui::EndPopup();
    }
    const int newSortPacked = sortMode * 2 + (sortDesc ? 1 : 0);
    if (newSortPacked != sortPacked && host.SetHierarchySort) host.SetHierarchySort(newSortPacked);

    ImGui::SameLine();
    if (EditorUIPrimitives::ActionButton(ICON_FA_ANGLES_DOWN, "Expand all", host.SetTooltip, false, ImVec2(iconW, iconW)) &&
        host.HierarchyExpandAll)
        host.HierarchyExpandAll(true);
    ImGui::SameLine();
    if (EditorUIPrimitives::ActionButton(ICON_FA_ANGLES_UP, "Collapse all", host.SetTooltip, false, ImVec2(iconW, iconW)) &&
        host.HierarchyExpandAll)
        host.HierarchyExpandAll(false);
    ImGui::PopStyleVar();
    EditorUIPrimitives::EndPanelToolbar();

    if (host.DrawHierarchyTreeBody) host.DrawHierarchyTreeBody();

    ImGui::End();
}

} // namespace EditorModuleHierarchy
