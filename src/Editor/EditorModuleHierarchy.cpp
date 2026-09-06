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

    bool visible = true;
    PushTabChromeText();
    const bool open = ImGui::Begin("Scene Hierarchy", &visible, ImGuiWindowFlags_None);
    PopTabChromeText();
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
                        "Type \"t:\" followed by a tag (e.g. t:Enemy) to filter by Tag instead.");
    }

    ImGui::SameLine();
    if (FlatGlyphButton(host, ICON_FA_ANGLES_DOWN, "Expand all") && host.HierarchyExpandAll)
        host.HierarchyExpandAll(true);
    ImGui::SameLine();
    if (FlatGlyphButton(host, ICON_FA_ANGLES_UP, "Collapse all") && host.HierarchyExpandAll)
        host.HierarchyExpandAll(false);

    if (host.DrawHierarchyTreeBody) host.DrawHierarchyTreeBody();

    ImGui::End();
}

} // namespace EditorModuleHierarchy
