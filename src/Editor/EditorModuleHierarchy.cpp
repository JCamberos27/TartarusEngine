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

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <cstdio>

namespace EditorModuleHierarchy {

namespace {

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

// The Hierarchy's two flat glyph buttons (Expand-all / Collapse-all), copied from the host lambda.
bool FlatGlyphButton(const EditorModuleHostAPI& host, const char* icon, const char* tip) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // flat at rest
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.245f, 0.250f, 0.275f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.300f, 0.310f, 0.345f, 1.0f));
    ImGui::PushID(tip);
    bool clicked = ImGui::Button(icon);
    ImGui::PopID();
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered() && host.SetTooltip) host.SetTooltip(tip);
    return clicked;
}

} // namespace

void Draw(const EditorModuleHostAPI& host) {
    if (host.GetShowHierarchy && !host.GetShowHierarchy()) return;

    // Q12 (Phase 4 / #6) — same faint warm wash as the Inspector while Playing; see its Draw() for
    // the full rationale (editing stays live, this is a "reverts on Stop" reminder, not a lock).
    const bool inPlayMode = host.GetInPlayMode && host.GetInPlayMode();
    if (inPlayMode) {
        const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        const ImVec4 amber(1.0f, 0.549f, 0.157f, 1.0f);
        const float mix = 0.08f;
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
            ImVec4(bg.x + (amber.x - bg.x) * mix, bg.y + (amber.y - bg.y) * mix,
                   bg.z + (amber.z - bg.z) * mix, bg.w));
    }

    bool visible = true;
    PushTabChromeText();
    const bool open = ImGui::Begin("Scene Hierarchy", &visible, ImGuiWindowFlags_None);
    PopTabChromeText();
    if (inPlayMode) ImGui::PopStyleColor();
    if (host.SetShowHierarchy) host.SetShowHierarchy(visible); // capture the title-bar X
    if (!open) { ImGui::End(); return; }

    // Search box. A bare string matches names; "t:Tag" matches TagComponent instead. Width leaves
    // room for the two glyph buttons + the spacing on either side.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float glyphBtnW = ImGui::CalcTextSize(ICON_FA_ANGLES_UP).x + st.FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-(glyphBtnW * 2.0f + st.ItemSpacing.x * 2.0f));
    char filterBuf[128] = {};
    if (host.GetHierarchyFilter) host.GetHierarchyFilter(filterBuf, (int)sizeof(filterBuf));
    if (ImGui::InputTextWithHint("##HierarchyFilter",
            ICON_FA_MAGNIFYING_GLASS "  Search (t:Tag to filter by tag)",
            filterBuf, sizeof(filterBuf)) && host.SetHierarchyFilter) {
        host.SetHierarchyFilter(filterBuf);
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive() && host.SetTooltip) {
        host.SetTooltip("Type a name to filter the list below.\n"
                        "Type \"t:\" followed by a tag (e.g. t:Enemy) to filter by Tag instead.\n\n"
                        "Tip: click a row in the tree, then type a name (without clicking here) "
                        "to jump to the next entity starting with those letters.");
    }

    ImGui::SameLine();
    if (FlatGlyphButton(host, ICON_FA_ANGLES_DOWN, "Expand all") && host.HierarchyExpandAll)
        host.HierarchyExpandAll(true);
    ImGui::SameLine();
    if (FlatGlyphButton(host, ICON_FA_ANGLES_UP, "Collapse all") && host.HierarchyExpandAll)
        host.HierarchyExpandAll(false);

    // Phase 5 item 6 (remainder) — type-filter chips + sort control, a second toolbar row. Chips
    // are additive toggles (multiple kinds can be shown at once); an active chip is tinted like a
    // pressed button so the filter state reads at a glance without a tooltip.
    int typeMask = host.GetHierarchyTypeFilter ? host.GetHierarchyTypeFilter() : 0;
    auto chip = [&](const char* icon, const char* tip, int bit) {
        const bool active = (typeMask & bit) != 0;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(icon)) {
            typeMask ^= bit;
            if (host.SetHierarchyTypeFilter) host.SetHierarchyTypeFilter(typeMask);
        }
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && host.SetTooltip) host.SetTooltip(tip);
        ImGui::SameLine();
    };
    chip(ICON_FA_CUBE, "Meshes only", kHierarchyFilterMesh);
    chip(ICON_FA_LIGHTBULB, "Lights only", kHierarchyFilterLight);
    chip(ICON_FA_VIDEO, "Cameras only", kHierarchyFilterCamera);
    chip(ICON_FA_VECTOR_SQUARE, "Empties / other only", kHierarchyFilterOther);

    const int sortPacked = host.GetHierarchySort ? host.GetHierarchySort() : 0;
    int sortMode = (sortPacked >> 1) & 3;
    bool sortDesc = (sortPacked & 1) != 0;
    const float sortBtnW = ImGui::CalcTextSize(ICON_FA_ARROW_DOWN_SHORT_WIDE).x + st.FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - sortBtnW);
    if (ImGui::Button(ICON_FA_ARROW_DOWN_SHORT_WIDE)) ImGui::OpenPopup("##HierarchySort");
    if (ImGui::IsItemHovered() && host.SetTooltip) host.SetTooltip("Sort the list");
    if (ImGui::BeginPopup("##HierarchySort")) {
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

    if (host.DrawHierarchyTreeBody) host.DrawHierarchyTreeBody();

    ImGui::End();
}

} // namespace EditorModuleHierarchy
