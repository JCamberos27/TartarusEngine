// The Scene viewport's header strip: a solid row along the top of the Scene panel, above the
// rendered image (Unreal's viewport toolbar), holding the tools and view options that used to
// float over the scene in a vertical rail (Phase 3 item 3's T-panel). Nothing sits on the image
// now - the rail and the floating action bar both left it - so the scene reads edge to edge.
//
//   [Hand Move Rotate Scale Rect Transform | Local Pivot | Snap 1 m v  Ground | Tools v]
//                                     ...   [Shaded v  Persp | Gizmos v  Grid | 8 m/s]
//
// The left group is the work (the transform tools always stay visible); the right group is how
// the view looks. When the panel is too narrow for both, the right group's labels drop to icons,
// then the group folds into a "..." menu.

#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorIcons.h"
#include "World.h"
#include "Camera.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "EditorTheme.h"
#include "GameViewPanel.h"

#include <imgui.h>
#include <imgui_internal.h> // ImMax, used by EditorUIPrimitives
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace EditorInternal;

namespace {
// A thin vertical rule between groups (lighter than the icons, like the old rail's separators).
void HeaderDivider(float frameH) {
    ImGui::SameLine(0.0f, EditorTheme::Px(8.0f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float inset = frameH * 0.24f;
    const float x = std::floor(p.x) + 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p.y + inset), ImVec2(x, p.y + frameH - inset),
                                        EditorTheme::U32(EditorTheme::Strong));
    ImGui::Dummy(ImVec2(1.0f, frameH));
    ImGui::SameLine(0.0f, EditorTheme::Px(8.0f));
}

// "1 m", "0.25 m", "15 deg": a snap increment without trailing zeros.
std::string SnapLabel(float v, const char* unit) {
    char b[32];
    std::snprintf(b, sizeof(b), "%g %s", v, unit);
    return b;
}
} // namespace

float EditorLayer::ViewportHeaderHeight() const {
    return std::floor(30.0f * m_UIScale);
}

void EditorLayer::DrawViewportHeader(World& world, Camera& editorCamera) {
    auto& settings = EditorSettings::Get();
    const float stripH = ViewportHeaderHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float stripW = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Solid, like every surface you work in (docs/EDITOR_DESIGN_SYSTEM.md): the panel's own base
    // with a hairline under it, so the strip belongs to the panel and not to the scene.
    dl->AddRectFilled(origin, ImVec2(origin.x + stripW, origin.y + stripH), EditorTheme::U32(EditorTheme::Base));
    dl->AddLine(ImVec2(origin.x, origin.y + stripH - 0.5f), ImVec2(origin.x + stripW, origin.y + stripH - 0.5f),
                EditorTheme::U32(EditorTheme::Hairline));

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(EditorTheme::Px(6.0f), EditorTheme::Px(3.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(EditorTheme::Px(2.0f), 0.0f));
    const float frameH = ImGui::GetFrameHeight();
    const float btn = frameH;
    const float rowY = origin.y + std::floor((stripH - frameH) * 0.5f);
    const float padX = EditorTheme::Px(8.0f);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + padX, rowY));

    int& quiet = EditorSoundEvents::SuppressDepth(); // tools, space and snap play EditorSound::ToolFrame

    // Scene | Game first: the rest of the strip belongs to whichever is showing.
    DrawViewportModeSwitch(btn);
    if (m_ViewportMode == ViewportMode::Game && !GameViewPoppedOut() && m_GameView) {
        // Game: its aspect / resolution on the left, pop-out on the right. The scene tools wait.
        HeaderDivider(frameH);
        m_GameView->DrawAspectControl();
        ImGui::SetCursorScreenPos(ImVec2(std::floor(origin.x + stripW - padX - btn), rowY));
        if (ActionButton(ICON_FA_UP_RIGHT_FROM_SQUARE "##popout", "Pop the Game view out into its own window (Window > Game)",
                         false, ImVec2(btn, btn))) {
            m_GameView->SetWindowOpen(true);
            SetViewportMode(ViewportMode::Scene);
        }
        ImGui::PopStyleVar(2);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + stripH));
        return;
    }
    HeaderDivider(frameH);

    // --- Left: the exclusive tools, with one underline that glides to the active one ------------
    float activeX = -1.0f, activeW = 0.0f;
    auto noteActive = [&](bool on) {
        if (!on) return;
        activeX = ImGui::GetItemRectMin().x - ImGui::GetWindowPos().x;
        activeW = ImGui::GetItemRectSize().x;
    };
    const bool handTool = m_HandTool;
    const int gizmoOp = GizmoOpIndex(); // 0 Translate 1 Rotate 2 Scale 3 Rect 4 Universal
    ++quiet;
    if (ActionButton(EDITOR_ICON_HAND_TOOL, "Hand - drag to pan the view (Q)", handTool, ImVec2(btn, btn)))
        SetHandToolActive(!handTool);
    noteActive(handTool);
    struct Tool { const char* Icon; const char* Tip; };
    static const Tool kTools[] = {
        {EDITOR_ICON_TRANSLATE, "Move (W)"},
        {EDITOR_ICON_ROTATE, "Rotate (E)"},
        {EDITOR_ICON_SCALE, "Scale (R)"},
        {EDITOR_ICON_RECT_TOOL, "Rect - move + non-uniform scale via corner/edge handles (T)"},
        {EDITOR_ICON_UNIVERSAL, "Transform - move + rotate + scale in one gizmo (Y)"},
    };
    for (int i = 0; i < IM_ARRAYSIZE(kTools); ++i) {
        ImGui::SameLine();
        const bool on = !handTool && gizmoOp == i;
        if (ActionButton(kTools[i].Icon, kTools[i].Tip, on, ImVec2(btn, btn))) SetGizmoOpIndex(i);
        noteActive(on);
    }
    --quiet;
    if (activeX >= 0.0f) {
        // Glide only when the tool changes; a resize just moves with it.
        const int tool = handTool ? -1 : gizmoOp;
        const bool switched = m_ToolGlideLast != -2 && m_ToolGlideLast != tool;
        m_ToolGlideLast = tool;
        const float x = EditorTheme::MotionFollow(ImGui::GetID("##toolGlide"), activeX, EditorMotionToken::Glide, !switched) +
                        ImGui::GetWindowPos().x;
        const float inset = EditorTheme::Px(6.0f), h = EditorTheme::Px(2.0f);
        const float y = origin.y + stripH - h - EditorTheme::Px(1.0f);
        dl->AddRectFilled(ImVec2(x + inset, y), ImVec2(x + activeW - inset, y + h), EditorTheme::U32(EditorTheme::Accent), h * 0.5f);
    }

    HeaderDivider(frameH);
    const bool localSpace = GizmoLocalSpace();
    ++quiet;
    if (ActionButton(localSpace ? EDITOR_ICON_LOCAL_SPACE : EDITOR_ICON_WORLD_SPACE,
            localSpace ? "Local space (click for World) - Alt+X" : "World space (click for Local) - Alt+X", false, ImVec2(btn, btn)))
        SetGizmoLocalSpace(!localSpace);
    --quiet;
    ImGui::SameLine();
    const bool pivotCenter = GizmoPivotCenter();
    if (ActionButton(pivotCenter ? EDITOR_ICON_PIVOT_CENTER : EDITOR_ICON_PIVOT_ORIGIN,
            pivotCenter ? "Center - gizmo on the bounding-box center (click for Pivot) - Alt+Shift+X"
                        : "Pivot - gizmo on the object's own origin (click for Center) - Alt+Shift+X",
            false, ImVec2(btn, btn)))
        SetGizmoPivotCenter(!pivotCenter);

    // Snap: one split control. The face toggles snapping and names the step; the caret opens the
    // grid and snap increments.
    HeaderDivider(frameH);
    {
        const bool gridSnap = m_GridSnapEnabled;
        const std::string label = std::string(EDITOR_ICON_SNAP_TO_GRID "  ") +
                                  (m_GizmoOp == GizmoOp::Rotate ? SnapLabel(m_SnapRotationDeg, "deg") : SnapLabel(m_SnapTranslation, "m")) +
                                  "##snap";
        ++quiet;
        if (ActionButton(label.c_str(), "Snap to grid (Alt+Shift+G; hold Ctrl to invert while dragging)", gridSnap, ImVec2(0.0f, btn)))
            m_GridSnapEnabled = !gridSnap;
        --quiet;
        ImGui::SameLine(0.0f, 0.0f);
        if (ActionButton(ICON_FA_CARET_DOWN "##snapcaret", "Grid and snap increments", false, ImVec2(std::floor(btn * 0.6f), btn)))
            ImGui::OpenPopup("##HeaderGridSnapPopup");
        if (ImGui::BeginPopup("##HeaderGridSnapPopup")) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            DrawGridSnapPopupBody();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        const bool canSnap = CanSnapSelectionToGround(world);
        ImGui::BeginDisabled(!canSnap);
        ++quiet; // SnapSelectionToGround plays SnapGround
        if (ActionButton(EDITOR_ICON_SNAP_TO_GROUND, "Snap the selection to the ground (Alt+H)", false, ImVec2(btn, btn)))
            SnapSelectionToGround(world);
        --quiet;
        ImGui::EndDisabled();
    }

    // Tools: the less frequent ones, behind one dropdown that shows whichever is active.
    HeaderDivider(frameH);
    {
        const bool measureTool = MeasureToolActive();
        const DecalToolMode decalTool = DecalTool();
        const char* icon = measureTool ? EDITOR_ICON_MEASURE
                         : decalTool == DecalToolMode::Place ? EDITOR_ICON_PLACE_DECAL
                         : decalTool == DecalToolMode::Brush ? EDITOR_ICON_DECAL_BRUSH : ICON_FA_TOOLBOX;
        const bool anyOn = measureTool || decalTool != DecalToolMode::None;
        const std::string label = std::string(icon) + "  Tools  " ICON_FA_CARET_DOWN "##tools";
        if (ActionButton(label.c_str(), "Measure, decal tools, duplicate array", anyOn, ImVec2(0.0f, btn)))
            ImGui::OpenPopup("##HeaderToolsPopup");
        if (ImGui::BeginPopup("##HeaderToolsPopup")) {
            ++quiet;
            if (ImGui::MenuItem(EDITOR_ICON_MEASURE "  Measure", "M", measureTool)) SetMeasureToolActive(!measureTool);
            if (ImGui::MenuItem(EDITOR_ICON_PLACE_DECAL "  Place Decal", "P", decalTool == DecalToolMode::Place))
                SetDecalTool(DecalToolMode::Place);
            if (ImGui::MenuItem(EDITOR_ICON_DECAL_BRUSH "  Decal Brush", "B", decalTool == DecalToolMode::Brush))
                SetDecalTool(DecalToolMode::Brush);
            --quiet;
            ImGui::Separator();
            if (ImGui::MenuItem(EDITOR_ICON_DUPLICATE_ARRAY "  Duplicate Array...", "Ctrl+Shift+D")) RequestArrayDuplicateModal();
            ImGui::EndPopup();
        }
    }
    const float leftEnd = ImGui::GetItemRectMax().x;

    // --- Right: how the view looks -------------------------------------------------------------
    static const char* kDrawModes[] = {"Shaded", "Wireframe", "Unlit", "Normals", "Shadow Cascades", "Mip / Texel Density"};
    static const char* kDrawModeIcons[] = {EDITOR_ICON_SHADED_MODE, EDITOR_ICON_WIREFRAME_MODE, EDITOR_ICON_UNLIT_MODE,
                                           EDITOR_ICON_NORMALS_MODE, EDITOR_ICON_CASCADES_MODE, EDITOR_ICON_MIP_MODE};
    int shading = ShadingModeIndex();
    if (shading < 0 || shading >= IM_ARRAYSIZE(kDrawModes)) shading = 0;
    const bool ortho = editorCamera.Orthographic;
    char speed[32];
    std::snprintf(speed, sizeof(speed), "%g m/s", std::round(settings.SceneCameraFlySpeed * 10.0f) / 10.0f);

    // The right group's labels, full and compact. Measured first so the group can right-align and
    // choose how much of itself fits beside the left group.
    const std::string drawFull = std::string(kDrawModeIcons[shading]) + "  " + kDrawModes[shading] + "  " ICON_FA_CARET_DOWN "##draw";
    const std::string drawIcon = std::string(kDrawModeIcons[shading]) + " " ICON_FA_CARET_DOWN "##draw";
    const std::string projFull = std::string(ortho ? EDITOR_ICON_ORTHOGRAPHIC : EDITOR_ICON_PERSPECTIVE) + (ortho ? "  Ortho##proj" : "  Persp##proj");
    const std::string projIcon = std::string(ortho ? EDITOR_ICON_ORTHOGRAPHIC : EDITOR_ICON_PERSPECTIVE) + "##proj";
    const std::string speedFull = std::string(ICON_FA_GAUGE_HIGH "  ") + speed + "##speed";
    const std::string speedShort = std::string(speed) + "##speed";
    const float framePadX = ImGui::GetStyle().FramePadding.x;
    auto textW = [&](const std::string& s) { return std::max(btn, ImGui::CalcTextSize(s.c_str(), nullptr, true).x + framePadX * 2.0f); };
    const float gapW = EditorTheme::Px(2.0f), divW = EditorTheme::Px(17.0f), caretW = std::floor(btn * 0.6f);
    const float fullW = textW(drawFull) + gapW + textW(projFull) + divW + btn + caretW + gapW + btn + divW + textW(speedFull);
    const float compactW = textW(drawIcon) + gapW + btn + divW + btn + caretW + gapW + btn + divW + textW(speedShort);
    const float room = origin.x + stripW - padX - (leftEnd + EditorTheme::Px(24.0f));
    const int fit = room >= fullW ? 2 : room >= compactW ? 1 : 0; // 2 labels, 1 icons, 0 the overflow menu

    auto drawModeMenu = [&]() {
        for (int i = 0; i < IM_ARRAYSIZE(kDrawModes); ++i) {
            const std::string item = std::string(kDrawModeIcons[i]) + "  " + kDrawModes[i];
            if (ImGui::MenuItem(item.c_str(), nullptr, shading == i)) SetShadingModeIndex(i);
        }
    };
    auto speedBody = [&]() {
        ImGui::TextDisabled("Camera fly speed (or RMB + wheel)");
        ImGui::SetNextItemWidth(EditorTheme::Px(180.0f));
        float v = settings.SceneCameraFlySpeed;
        if (ImGui::SliderFloat("##flySpeed", &v, 0.5f, 60.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic)) {
            settings.SceneCameraFlySpeed = v;
            EditorSettings::Save();
        }
    };

    if (fit == 0) {
        ImGui::SetCursorScreenPos(ImVec2(std::floor(origin.x + stripW - padX - btn), rowY));
        if (ActionButton(ICON_FA_ELLIPSIS "##overflow", "View options", false, ImVec2(btn, btn))) ImGui::OpenPopup("##HeaderOverflow");
        if (ImGui::BeginPopup("##HeaderOverflow")) {
            if (ImGui::BeginMenu("Draw Mode")) { drawModeMenu(); ImGui::EndMenu(); }
            if (ImGui::MenuItem(ortho ? "Perspective" : "Orthographic", "5")) ToolbarToggleOrthographic(world, editorCamera);
            ImGui::Separator();
            if (ImGui::MenuItem("Gizmos", "Shift+G", m_GizmosMasterVisible)) m_GizmosMasterVisible = !m_GizmosMasterVisible;
            if (ImGui::BeginMenu("Gizmo Visibility")) { DrawGizmosPopupBody(); ImGui::EndMenu(); }
            if (ImGui::MenuItem("Grid", "Alt+G", m_ShowGrid)) m_ShowGrid = !m_ShowGrid;
            ImGui::Separator();
            speedBody();
            ImGui::EndPopup();
        }
    } else {
        const bool labels = fit == 2;
        ImGui::SetCursorScreenPos(ImVec2(std::floor(origin.x + stripW - padX - (labels ? fullW : compactW)), rowY));
        if (ActionButton((labels ? drawFull : drawIcon).c_str(), "Draw mode (Alt+D cycles)", shading != 0, ImVec2(0.0f, btn)))
            ImGui::OpenPopup("##HeaderDrawModePopup");
        if (ImGui::BeginPopup("##HeaderDrawModePopup")) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            drawModeMenu();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ActionButton((labels ? projFull : projIcon).c_str(),
                ortho ? "Orthographic (click for Perspective) - 5" : "Perspective (click for Orthographic) - 5",
                ortho, ImVec2(labels ? 0.0f : btn, btn)))
            ToolbarToggleOrthographic(world, editorCamera);

        HeaderDivider(frameH);
        const bool gizmosOn = m_GizmosMasterVisible;
        if (ActionButton(EDITOR_ICON_TOGGLE_GIZMOS, "Gizmos (Shift+G)", gizmosOn, ImVec2(btn, btn)))
            m_GizmosMasterVisible = !gizmosOn;
        ImGui::SameLine(0.0f, 0.0f);
        if (ActionButton(ICON_FA_CARET_DOWN "##gizmocaret", "What the gizmos show", false, ImVec2(caretW, btn)))
            ImGui::OpenPopup("##HeaderGizmosPopup");
        if (ImGui::BeginPopup("##HeaderGizmosPopup")) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            DrawGizmosPopupBody();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ActionButton(EDITOR_ICON_GRID, "Grid (Alt+G)", m_ShowGrid, ImVec2(btn, btn))) m_ShowGrid = !m_ShowGrid;

        HeaderDivider(frameH);
        if (ActionButton((labels ? speedFull : speedShort).c_str(), "Camera fly speed - click to set, scroll over it to change",
                false, ImVec2(0.0f, btn)))
            ImGui::OpenPopup("##HeaderSpeedPopup");
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
            const float step = ImGui::GetIO().MouseWheel > 0.0f ? 1.2f : 1.0f / 1.2f;
            settings.SceneCameraFlySpeed = std::clamp(settings.SceneCameraFlySpeed * step, 0.5f, 60.0f);
            EditorSettings::Save();
        }
        if (ImGui::BeginPopup("##HeaderSpeedPopup")) {
            speedBody();
            ImGui::EndPopup();
        }
    }

    ImGui::PopStyleVar(2);
    // Leave the layout cursor at the strip's bottom-left, where the image goes.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + stripH));
}
