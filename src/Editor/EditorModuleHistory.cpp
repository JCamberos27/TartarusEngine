// The editor's Undo History HUD, living inside TartarusEditor.dll so its chrome hot-reloads while
// the editor stays open with its scene loaded. Ported in behaviour from
// EditorLayer::DrawHistoryPanel (EditorLayer_Toolbar.cpp): the compact, transparent HUD pinned to
// the Scene viewport's bottom-right corner (mirroring the Stats HUD at top-left) — the "History"
// heading, then every recorded change oldest-to-newest with the current position highlighted,
// auto-sized to its content and height-capped so a long history never climbs into the top-right
// nav cluster or the toolbar.
//
// Thin slice (issue #229): the row list itself — the undo/redo stacks, the click-to-jump
// (JumpToUndo/RedoEntry), the per-row tooltips — stays host code, drawn into this window through
// host.DrawHistoryListBody(). Unlike Stats, the rows are interactive (click to jump), so this
// window is NOT NoInputs. It used to steer its text between white-on-dark and black-on-light
// with an async GPU luminance readback the host drove; Defect #54 (Phase 1) replaced that with a
// fixed opaque plate behind fixed light text. See EditorUIPrimitives.h's "Viewport HUD
// legibility" section.

#include "EditorModuleAPI.h"
#include "EditorUIPrimitives.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>

namespace EditorModuleHistory {

void Draw(const EditorModuleHostAPI& host) {
    float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f, uiScale = 1.0f;
    int rowCount = 0;
    if (!host.GetHistoryHudFrame ||
        !host.GetHistoryHudFrame(&vx, &vy, &vw, &vh, &uiScale, &rowCount)) {
        return;
    }

    // Pinned to the viewport's bottom-right corner, sized to its text, fully transparent, not
    // dockable, not persisted so the pin always wins. Height ceiling: grow upward from the pin
    // only until a clear line below the top-right nav cluster (rotate ring + dolly/pan box +
    // "Persp" label ≈ 220px @ 1x); beyond that the list scrolls internally. Matches the maths in
    // the host's old DrawHistoryPanel exactly.
    const float hpad = 12.0f * uiScale;
    const float statusBarH = ImGui::GetTextLineHeight() + 8.0f * uiScale;
    const float gizmoZoneH = 220.0f * uiScale;
    const float maxH = std::max(120.0f * uiScale, vh - hpad - statusBarH - gizmoZoneH);

    const ImGuiStyle& st = ImGui::GetStyle();
    const float chromeH = ImGui::GetTextLineHeightWithSpacing()   // "History" line
                        + st.ItemSpacing.y + 2.0f                 // separator
                        + st.WindowPadding.y * 2.0f;
    const float desiredH = chromeH + std::max(rowCount, 1) * ImGui::GetTextLineHeightWithSpacing();
    const float winH = std::min(desiredH, maxH);

    ImGui::SetNextWindowPos(ImVec2(vx + vw - hpad, vy + vh - hpad - statusBarH),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(0.0f, winH)); // x=0 → auto-fit width, height clamped
    // #54 — a real (opaque-ish) plate behind the HUD, not a fully transparent window tinted by
    // sampling the scene behind it. Legible over anything, no GPU readback.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorUIPrimitives::kHudPlateColor);
    ImGui::PushStyleColor(ImGuiCol_Text,         EditorUIPrimitives::kHudTextColor);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, EditorUIPrimitives::kHudTextDisabledColor);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove;

    bool visible = true;
    if (!ImGui::Begin(ICON_FA_CLOCK_ROTATE_LEFT "  History", &visible, flags)) {
        ImGui::End();
        ImGui::PopStyleColor(3);
        return;
    }
    if (!visible && host.SetShowHistory) host.SetShowHistory(false); // the title-bar X

    ImGui::TextUnformatted(ICON_FA_CLOCK_ROTATE_LEFT "  History");
    // Explanation on the heading tooltip (#156) — no persistent "(?)" glyph.
    if (ImGui::IsItemHovered() && host.SetTooltip) host.SetTooltip(
        "Every recorded change, oldest to newest. Click any entry to jump\n"
        "straight there - undoing or redoing everything in between automatically.");
    ImGui::Separator();

    if (host.DrawHistoryListBody) host.DrawHistoryListBody();

    ImGui::End();
    ImGui::PopStyleColor(3); // WindowBg + Text + TextDisabled
}

} // namespace EditorModuleHistory
