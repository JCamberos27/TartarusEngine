// Phase 3 item 3 — the vertical tool palette docked to the Scene viewport's left edge and the
// view-state chips pinned to its top-right, both moved out of the old 25-icon top toolbar strip
// (EditorModuleToolbar.cpp) per the audit's "nothing important is a ghost overlay" exit
// criterion. Split into its own file rather than folded into EditorLayer_Gizmos.cpp or
// EditorLayer_Toolbar.cpp since it's a new, self-contained concern (#179's precedent for keeping
// EditorLayer.cpp's translation units topic-sized).

#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorIcons.h"
#include "World.h"
#include "Camera.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"

#include <imgui.h>
#include <imgui_internal.h> // ImMax, used by EditorUIPrimitives
#include <IconsFontAwesome6.h>

#include <cstdio>

using namespace EditorInternal;

// The vertical rail (Blender's T-panel model, audit #5 Q7) — a child region drawn INSIDE Scene's
// own Begin/End (see EditorLayer.cpp, right after the viewport Image()) rather than a separate
// floating window, so it clips and reorders with the Scene panel automatically instead of
// recreating the always-on-top-window problem the redesign is trying to solve. Its own opaque
// plate (same kHudPlateColor every other viewport HUD uses since Defect #54) keeps it legible
// over the rendered scene without needing to shrink the actual render target.
void EditorLayer::DrawToolPalette(World& world, Camera& editorCamera) {
    (void)world; (void)editorCamera; // reserved: a future tool (e.g. a palette-hosted Vertex tool) may need these

    auto& settings = EditorSettings::Get();
    const float margin = 8.0f * m_UIScale;
    const float btn = 26.0f * m_UIScale;
    const float railW = btn + 12.0f * m_UIScale;

    ImGui::SetCursorScreenPos(ImVec2(m_ViewportPos.x + margin, m_ViewportPos.y + margin));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorUIPrimitives::kHudPlateColor);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * m_UIScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * m_UIScale, 6.0f * m_UIScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 4.0f * m_UIScale));

    ImGui::BeginChild("##ToolPalette", ImVec2(railW, 0.0f),
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const bool collapsed = settings.ToolPaletteCollapsed;
    if (ActionButton(collapsed ? ICON_FA_CHEVRON_RIGHT : ICON_FA_CHEVRON_LEFT,
            collapsed ? "Expand the tool palette" : "Collapse the tool palette", false, ImVec2(btn, btn))) {
        settings.ToolPaletteCollapsed = !collapsed;
        EditorSettings::Save();
    }

    if (!collapsed) {
        ImGui::Separator();

        const bool handTool = m_HandTool;
        if (ActionButton(EDITOR_ICON_HAND_TOOL, "Hand — drag to pan the view (Q)", handTool, ImVec2(btn, btn)))
            SetHandToolActive(!handTool);

        const int gizmoOp = GizmoOpIndex(); // 0 Translate 1 Rotate 2 Scale 3 Rect 4 Universal
        if (ActionButton(EDITOR_ICON_TRANSLATE, "Translate (W)", !handTool && gizmoOp == 0, ImVec2(btn, btn)))
            SetGizmoOpIndex(0);
        if (ActionButton(EDITOR_ICON_ROTATE, "Rotate (E)", !handTool && gizmoOp == 1, ImVec2(btn, btn)))
            SetGizmoOpIndex(1);
        if (ActionButton(EDITOR_ICON_SCALE, "Scale (R)", !handTool && gizmoOp == 2, ImVec2(btn, btn)))
            SetGizmoOpIndex(2);
        if (ActionButton(EDITOR_ICON_RECT_TOOL,
                "Rect — move + non-uniform scale via corner/edge handles (T)",
                !handTool && gizmoOp == 3, ImVec2(btn, btn)))
            SetGizmoOpIndex(3);
        if (ActionButton(EDITOR_ICON_UNIVERSAL,
                "Transform — move + rotate + scale in one gizmo (Y)",
                !handTool && gizmoOp == 4, ImVec2(btn, btn)))
            SetGizmoOpIndex(4);

        ImGui::Separator();

        const bool measureTool = MeasureToolActive();
        if (ActionButton(EDITOR_ICON_MEASURE,
                "Measure — click to chain measurement points; right-click clears (M)",
                measureTool, ImVec2(btn, btn)))
            SetMeasureToolActive(!measureTool);
        if (ActionButton(EDITOR_ICON_DUPLICATE_ARRAY, "Duplicate Array — line/grid of copies of the selection (Ctrl+Shift+D)",
                false, ImVec2(btn, btn)))
            RequestArrayDuplicateModal();

        ImGui::Separator();

        const bool localSpace = GizmoLocalSpace();
        if (ActionButton(localSpace ? EDITOR_ICON_LOCAL_SPACE : EDITOR_ICON_WORLD_SPACE,
                localSpace ? "Local space (click for World)" : "World space (click for Local)",
                false, ImVec2(btn, btn)))
            SetGizmoLocalSpace(!localSpace);

        const bool pivotCenter = GizmoPivotCenter();
        if (ActionButton(pivotCenter ? EDITOR_ICON_PIVOT_CENTER : EDITOR_ICON_PIVOT_ORIGIN,
                pivotCenter
                    ? "Center - gizmo sits on the bounding-box center (click for Pivot)"
                    : "Pivot - gizmo sits on the object's own origin (click for Center)",
                false, ImVec2(btn, btn)))
            SetGizmoPivotCenter(!pivotCenter);

        // Grid / snap / gizmo-visibility joined this rail from the old top toolbar's icon row,
        // which is gone now (moved into viewport clusters per request) — these are viewport
        // display options in the same spirit as Hand/Translate/Rotate above, so they belong in
        // the same strip rather than a separate cluster. No room for a second slim caret button
        // per icon in a one-column rail, so the settings popovers (grid cell size / snap
        // increments, per-type gizmo visibility) open on right-click instead of a dedicated caret
        // — left-click still just toggles.
        ImGui::Separator();

        const bool showGrid = m_ShowGrid;
        if (ActionButton(EDITOR_ICON_GRID, "Toggle Grid (right-click: grid & snap settings)",
                showGrid, ImVec2(btn, btn)))
            m_ShowGrid = !showGrid;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##PaletteGridSnapPopup");
        if (ImGui::BeginPopup("##PaletteGridSnapPopup")) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            DrawGridSnapPopupBody();
            ImGui::EndPopup();
        }

        const bool gridSnap = m_GridSnapEnabled;
        if (ActionButton(EDITOR_ICON_SNAP_TO_GRID,
                "Toggle Snap to Grid (hold Ctrl to invert while dragging)", gridSnap, ImVec2(btn, btn)))
            m_GridSnapEnabled = !gridSnap;

        {
            const bool canSnap = CanSnapSelectionToGround(world);
            ImGui::BeginDisabled(!canSnap);
            if (ActionButton(EDITOR_ICON_SNAP_TO_GROUND, "Snap selection to ground", false, ImVec2(btn, btn)))
                SnapSelectionToGround(world);
            ImGui::EndDisabled();
        }

        ImGui::Separator();

        const bool gizmosOn = m_GizmosMasterVisible;
        if (ActionButton(EDITOR_ICON_TOGGLE_GIZMOS,
                "Toggle Gizmos (right-click: per-type visibility)", gizmosOn, ImVec2(btn, btn)))
            m_GizmosMasterVisible = !gizmosOn;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##PaletteGizmosPopup");
        if (ImGui::BeginPopup("##PaletteGizmosPopup")) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            DrawGizmosPopupBody();
            ImGui::EndPopup();
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// The view-state chips (audit #5 item 3) — draw mode + orthographic/perspective, pinned to the
// viewport's top-right. Offset left of DrawViewGizmo's nav-gizmo cluster by a fixed clearance
// mirroring that function's own margin/gizmoRadius math (EditorLayer_Gizmos.cpp).
void EditorLayer::DrawViewStateChips(World& world, Camera& editorCamera) {
    if (!m_SceneViewportVisible || m_ViewportSize.x < 1.0f || m_ViewportSize.y < 1.0f) return;
    if (m_HideOverlaysThisFrame) return;

    const float margin = 14.0f * m_UIScale;
    // Phase 3 item 6 enlarged the nav gizmo (style.scale 0.5 -> 0.7, gizmoRadius 64px -> ~90px at
    // UIScale 1) — this clearance grew with it: margin(14) + 2*gizmoRadius(~179) + a buffer.
    const float navGizmoClearance = 210.0f * m_UIScale; // ~= margin + 2*gizmoRadius + buffer

    ImGui::SetNextWindowPos(
        ImVec2(m_ViewportPos.x + m_ViewportSize.x - navGizmoClearance, m_ViewportPos.y + margin),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.0f); // chips paint their own plates; the row window itself stays invisible

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::Begin("##ViewStateChips", nullptr, flags);

    static const char* kDrawModes[] = { "Shaded", "Wireframe", "Unlit",
                                        "Normals", "Shadow Cascades", "Mip / Texel Density" };
    static const char* kIcons[] = { EDITOR_ICON_SHADED_MODE, EDITOR_ICON_WIREFRAME_MODE, EDITOR_ICON_UNLIT_MODE,
                                    EDITOR_ICON_NORMALS_MODE, EDITOR_ICON_CASCADES_MODE, EDITOR_ICON_MIP_MODE };
    int shading = ShadingModeIndex();
    if (shading < 0 || shading >= IM_ARRAYSIZE(kDrawModes)) shading = 0;
    char chipLabel[64];
    std::snprintf(chipLabel, sizeof(chipLabel), "%s  %s", kIcons[shading], kDrawModes[shading]);
    if (ActionButton(chipLabel, "Draw mode (click to change)", shading != 0)) ImGui::OpenPopup("##DrawModeChipPopup");
    if (ImGui::BeginPopup("##DrawModeChipPopup")) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        for (int i = 0; i < IM_ARRAYSIZE(kDrawModes); ++i)
            if (ImGui::MenuItem(kDrawModes[i], nullptr, shading == i)) SetShadingModeIndex(i);
        ImGui::EndPopup();
    }

    ImGui::SameLine();

    const bool ortho = editorCamera.Orthographic;
    char orthoLabel[32];
    std::snprintf(orthoLabel, sizeof(orthoLabel), "%s  %s",
                 ortho ? EDITOR_ICON_ORTHOGRAPHIC : EDITOR_ICON_PERSPECTIVE,
                 ortho ? "Ortho" : "Persp");
    if (ActionButton(orthoLabel,
            ortho ? "Orthographic (click for Perspective) — 5" : "Perspective (click for Orthographic) — 5",
            ortho)) {
        ToolbarToggleOrthographic(world, editorCamera);
    }

    ImGui::End();
}
