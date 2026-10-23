// The editor's two toolbars, laid out the Unreal 5 way, so nothing floats over the scene:
//
//   Main toolbar row (under the menus, the whole window wide):
//   [Save] [Hand Move Rotate Scale Rect Transform] [World Pivot] [Snap 1 m v] [Ground] [Tools v]
//                                [ > Play  || Pause  >| Step ]            [Stats Console History] [Gear]
//
//   Viewport header (a slim row at the top of the Scene panel, above the image):
//   [Perspective v] [Shaded v] [Show v]                                      [Scene | Game] [8 m/s]
//
// Buttons sit in rounded groups (one plate per group, Unreal's segmented look) at one height, the
// active tool filled. The main row holds what you do; the viewport header only how you look.

#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorIcons.h"
#include "World.h"
#include "Camera.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "EditorTheme.h"
#include "EditorPanels.h"
#include "EditorModuleAPI.h"          // EditorConsoleState
#include "HotReloadEditorModule.h"    // EditorModuleHost::ConsoleState()
#include "EditorSound.h"
#include "GameViewPanel.h"

#include <imgui.h>
#include <imgui_internal.h> // ImMax, used by EditorUIPrimitives
#include <IconsFontAwesome6.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace EditorInternal;

namespace {
// One rounded plate behind a run of buttons (drawn underneath them with a draw-list splitter, so
// the plate is sized to what the run turned out to be).
struct ToolGroup {
    ImDrawList* Dl;
    ImDrawListSplitter Splitter;
    explicit ToolGroup(ImDrawList* dl) : Dl(dl) {
        Splitter.Split(Dl, 2);
        Splitter.SetCurrentChannel(Dl, 1);
        ImGui::BeginGroup();
    }
    ~ToolGroup() {
        ImGui::EndGroup();
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const float pad = EditorTheme::Px(2.0f);
        Splitter.SetCurrentChannel(Dl, 0);
        Dl->AddRectFilled(ImVec2(mn.x - pad, mn.y - pad), ImVec2(mx.x + pad, mx.y + pad),
                          EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Text, 0.06f)), EditorTheme::Px(6.0f));
        Splitter.Merge(Dl);
    }
};

// The gap between groups.
void GroupGap() { ImGui::SameLine(0.0f, EditorTheme::Px(10.0f)); }

// "1 m", "0.25 m", "15 deg": a snap increment without trailing zeros.
std::string SnapLabel(float v, const char* unit) {
    char b[32];
    std::snprintf(b, sizeof(b), "%g %s", v, unit);
    return b;
}
} // namespace

// --- Main toolbar row -------------------------------------------------------------------------------

void EditorLayer::DrawMainToolbar(World& world, AssetLibrary& assets, bool playing, bool paused, bool transport) {
    int ww = 0, wh = 0;
    glfwGetWindowSize(m_Window, &ww, &wh);
    const float rowH = MainToolbarHeightPx();
    if (ww <= 0 || rowH <= 0.0f) return;

    ImGui::SetNextWindowPos(ImVec2(0.0f, ToolbarHeightPx()), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)ww, rowH), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::Base);
    const bool open = ImGui::Begin("##MainToolbar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!open) { ImGui::End(); return; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wpos = ImGui::GetWindowPos();
    const float w = (float)ww;
    dl->AddLine(ImVec2(wpos.x, wpos.y + rowH - 0.5f), ImVec2(wpos.x + w, wpos.y + rowH - 0.5f),
                EditorTheme::U32(EditorTheme::Hairline));

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(EditorTheme::Px(8.0f), EditorTheme::Px(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(EditorTheme::Px(2.0f), 0.0f));
    const float btn = ImGui::GetFrameHeight();
    const float rowY = wpos.y + std::floor((rowH - btn) * 0.5f);
    const float padX = EditorTheme::Px(10.0f);
    // Narrow windows: the space / pivot buttons drop their words.
    const bool compact = w < EditorTheme::Px(1350.0f);
    int& quiet = EditorSoundEvents::SuppressDepth(); // tools, space and snap play EditorSound::ToolFrame

    // --- Left: Save, the tools, space / pivot, snap, Tools ----------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + padX, rowY));
    {
        ToolGroup g(dl);
        EditorSoundEvents::ScopedClickSilence silent; // Save sounds when it lands
        if (ActionButton(EDITOR_ICON_SAVE "##save", "Save the scene (Ctrl+S)", false, ImVec2(btn, btn))) SaveScene(world, assets);
    }
    GroupGap();
    {
        ToolGroup g(dl);
        const bool handTool = m_HandTool;
        const int gizmoOp = GizmoOpIndex(); // 0 Translate 1 Rotate 2 Scale 3 Rect 4 Universal
        ++quiet;
        if (ActionButton(EDITOR_ICON_HAND_TOOL, "Hand - drag to pan the view (Q)", handTool, ImVec2(btn, btn)))
            SetHandToolActive(!handTool);
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
            if (ActionButton(kTools[i].Icon, kTools[i].Tip, !handTool && gizmoOp == i, ImVec2(btn, btn))) SetGizmoOpIndex(i);
        }
        --quiet;
    }
    GroupGap();
    {
        ToolGroup g(dl);
        const bool localSpace = GizmoLocalSpace();
        const std::string space = std::string(localSpace ? EDITOR_ICON_LOCAL_SPACE : EDITOR_ICON_WORLD_SPACE) +
                                  (compact ? "" : (localSpace ? "  Local" : "  World")) + "##space";
        ++quiet;
        if (ActionButton(space.c_str(), localSpace ? "Local space (click for World) - Alt+X" : "World space (click for Local) - Alt+X",
                         false, ImVec2(compact ? btn : 0.0f, btn)))
            SetGizmoLocalSpace(!localSpace);
        --quiet;
        ImGui::SameLine();
        const bool pivotCenter = GizmoPivotCenter();
        const std::string pivot = std::string(pivotCenter ? EDITOR_ICON_PIVOT_CENTER : EDITOR_ICON_PIVOT_ORIGIN) +
                                  (compact ? "" : (pivotCenter ? "  Center" : "  Pivot")) + "##pivot";
        if (ActionButton(pivot.c_str(),
                pivotCenter ? "Center - gizmo on the bounding-box center (click for Pivot) - Alt+Shift+X"
                            : "Pivot - gizmo on the object's own origin (click for Center) - Alt+Shift+X",
                false, ImVec2(compact ? btn : 0.0f, btn)))
            SetGizmoPivotCenter(!pivotCenter);
    }
    GroupGap();
    {
        // Snap: one split control. The face toggles snapping and names the step; the caret opens
        // the grid and snap increments. Ground snap sits beside it.
        ToolGroup g(dl);
        const bool gridSnap = m_GridSnapEnabled;
        const std::string label = std::string(EDITOR_ICON_SNAP_TO_GRID "  ") +
                                  (m_GizmoOp == GizmoOp::Rotate ? SnapLabel(m_SnapRotationDeg, "deg") : SnapLabel(m_SnapTranslation, "m")) +
                                  "##snap";
        ++quiet;
        if (ActionButton(label.c_str(), "Snap to grid (Alt+Shift+G; hold Ctrl to invert while dragging)", gridSnap, ImVec2(0.0f, btn)))
            m_GridSnapEnabled = !gridSnap;
        --quiet;
        ImGui::SameLine(0.0f, 0.0f);
        if (ActionButton(ICON_FA_CARET_DOWN "##snapcaret", "Grid and snap increments", false, ImVec2(std::floor(btn * 0.7f), btn)))
            ImGui::OpenPopup("##ToolbarGridSnapPopup");
        if (ImGui::BeginPopup("##ToolbarGridSnapPopup")) {
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
    GroupGap();
    {
        // The less frequent tools, behind one dropdown that shows whichever is active.
        ToolGroup g(dl);
        const bool measureTool = MeasureToolActive();
        const DecalToolMode decalTool = DecalTool();
        const char* icon = measureTool ? EDITOR_ICON_MEASURE
                         : decalTool == DecalToolMode::Place ? EDITOR_ICON_PLACE_DECAL
                         : decalTool == DecalToolMode::Brush ? EDITOR_ICON_DECAL_BRUSH : ICON_FA_TOOLBOX;
        const bool anyOn = measureTool || decalTool != DecalToolMode::None;
        const std::string label = std::string(icon) + "  Tools  " ICON_FA_CARET_DOWN "##tools";
        if (ActionButton(label.c_str(), "Measure, decal tools, duplicate array", anyOn, ImVec2(0.0f, btn)))
            ImGui::OpenPopup("##ToolbarToolsPopup");
        if (ImGui::BeginPopup("##ToolbarToolsPopup")) {
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
    const float leftEnd = ImGui::GetItemRectMax().x + EditorTheme::Px(2.0f);

    // --- Right: the panel toggles and Preferences ------------------------------------------------
    const float rightW = btn * 4.0f + EditorTheme::Px(2.0f) * 2.0f + EditorTheme::Px(10.0f) + EditorTheme::Px(4.0f);
    const float rightX = std::floor(wpos.x + w - padX - rightW);
    ImGui::SetCursorScreenPos(ImVec2(rightX, rowY));
    {
        ToolGroup g(dl);
        {
            EditorSoundEvents::ScopedClickSilence silent; // StatsOn / StatsOff instead
            const bool showStats = EditorSettings::Get().GameViewShowStats;
            if (ActionButton(EDITOR_ICON_STATISTICS, "Show stats over the Game view", showStats, ImVec2(btn, btn))) {
                EditorSettings::Get().GameViewShowStats = !showStats;
                EditorSettings::Save();
                EditorSound::Play(showStats ? EditorSound::Event::StatsOff : EditorSound::Event::StatsOn, EditorSound::LastItemScreenX());
            }
        }
        ImGui::SameLine();
        {
            // Defect #11: hidden behind a sibling tab focuses it rather than closing it.
            EditorConsoleState& cs = EditorModuleHost::ConsoleState();
            if (ActionButton(EDITOR_ICON_CONSOLE, "Toggle Console", cs.Visible, ImVec2(btn, btn))) {
                if (!cs.Visible) cs.Visible = true;
                else {
                    ImGuiWindow* consoleWin = ImGui::FindWindowByName(EditorPanels::Console);
                    if (consoleWin && consoleWin->Hidden) ImGui::FocusWindow(consoleWin);
                    else cs.Visible = false;
                }
            }
        }
        ImGui::SameLine();
        if (ActionButton(EDITOR_ICON_HISTORY, "Toggle History", m_ShowHistory, ImVec2(btn, btn))) m_ShowHistory = !m_ShowHistory;
    }
    GroupGap();
    {
        ToolGroup g(dl);
        if (ActionButton(ICON_FA_GEAR "##prefs", "Preferences", m_ShowPreferences, ImVec2(btn, btn))) OpenPreferences();
    }

    // --- Centre: Play / Pause / Step, on the window's centre line when there's room ---------------
    if (transport) {
        const float tw = m_TitleTransportW > 0.0f ? m_TitleTransportW : EditorTheme::Px(110.0f);
        float x = std::floor(wpos.x + w * 0.5f - tw * 0.5f);
        x = std::min(x, rightX - EditorTheme::Px(16.0f) - tw);
        x = std::max(x, leftEnd + EditorTheme::Px(16.0f));
        ImGui::SetCursorScreenPos(ImVec2(x, rowY));
        {
            ToolGroup g(dl);
            DrawPlayTransportButtons(playing, /*maximized=*/false, paused);
        }
        m_TitleTransportW = ImGui::GetItemRectSize().x;
        // The play clock beside it: how long this run has been going.
        if (playing && m_PlayStartedAt >= 0.0) {
            const int secs = (int)(ImGui::GetTime() - m_PlayStartedAt);
            char clock[32];
            std::snprintf(clock, sizeof(clock), paused ? "%d:%02d  paused" : "%d:%02d", secs / 60, secs % 60);
            const float cx = ImGui::GetItemRectMax().x + EditorTheme::Px(12.0f);
            if (cx + ImGui::CalcTextSize(clock).x < rightX - EditorTheme::Px(8.0f)) {
                EditorTheme::PushMono();
                dl->AddText(ImVec2(cx, wpos.y + std::floor((rowH - ImGui::GetFontSize()) * 0.5f)),
                            EditorTheme::U32(EditorTheme::Secondary), clock);
                EditorTheme::PopFont();
            }
        }
    }

    ImGui::PopStyleVar(2);
    ImGui::End();
}

// --- Viewport header ----------------------------------------------------------------------------------

float EditorLayer::ViewportHeaderHeight() const {
    return std::floor(28.0f * m_UIScale);
}

void EditorLayer::DrawViewportHeader(World& world, Camera& editorCamera) {
    auto& settings = EditorSettings::Get();
    const float stripH = ViewportHeaderHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float stripW = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Solid, like every surface you work in (docs/EDITOR_DESIGN_SYSTEM.md), with a hairline under it.
    dl->AddRectFilled(origin, ImVec2(origin.x + stripW, origin.y + stripH), EditorTheme::U32(EditorTheme::Base));
    dl->AddLine(ImVec2(origin.x, origin.y + stripH - 0.5f), ImVec2(origin.x + stripW, origin.y + stripH - 0.5f),
                EditorTheme::U32(EditorTheme::Hairline));

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(EditorTheme::Px(8.0f), EditorTheme::Px(3.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(EditorTheme::Px(2.0f), 0.0f));
    const float btn = ImGui::GetFrameHeight();
    const float rowY = origin.y + std::floor((stripH - btn) * 0.5f);
    const float padX = EditorTheme::Px(6.0f);
    const bool gameShowing = m_ViewportMode == ViewportMode::Game && !GameViewPoppedOut() && m_GameView;
    auto textW = [&](const char* s) {
        return std::max(btn, ImGui::CalcTextSize(s, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f);
    };

    // --- Right: Scene | Game, then camera speed (Scene) or pop-out (Game) -------------------------
    char speed[48];
    std::snprintf(speed, sizeof(speed), ICON_FA_GAUGE_HIGH "  %g m/s##speed", std::round(settings.SceneCameraFlySpeed * 10.0f) / 10.0f);
    const float switchW = textW(ICON_FA_CUBE "  Scene") + textW(ICON_FA_GAMEPAD "  Game");
    const float tailW = gameShowing ? btn : textW(speed);
    float rightX = std::floor(origin.x + stripW - padX - tailW - EditorTheme::Px(10.0f) - switchW);
    ImGui::SetCursorScreenPos(ImVec2(rightX, rowY));
    {
        ToolGroup g(dl);
        DrawViewportModeSwitch(btn);
    }
    GroupGap();
    if (gameShowing) {
        if (ActionButton(ICON_FA_UP_RIGHT_FROM_SQUARE "##popout", "Pop the Game view out into its own window (Window > Game)",
                         false, ImVec2(btn, btn))) {
            m_GameView->SetWindowOpen(true);
            SetViewportMode(ViewportMode::Scene);
        }
    } else {
        if (ActionButton(speed, "Camera fly speed - click to set, scroll over it to change", false, ImVec2(0.0f, btn)))
            ImGui::OpenPopup("##HeaderSpeedPopup");
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
            const float step = ImGui::GetIO().MouseWheel > 0.0f ? 1.2f : 1.0f / 1.2f;
            settings.SceneCameraFlySpeed = std::clamp(settings.SceneCameraFlySpeed * step, 0.5f, 60.0f);
            EditorSettings::Save();
        }
        if (ImGui::BeginPopup("##HeaderSpeedPopup")) {
            ImGui::TextDisabled("Camera fly speed (or RMB + wheel)");
            ImGui::SetNextItemWidth(EditorTheme::Px(180.0f));
            float v = settings.SceneCameraFlySpeed;
            if (ImGui::SliderFloat("##flySpeed", &v, 0.5f, 60.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic)) {
                settings.SceneCameraFlySpeed = v;
                EditorSettings::Save();
            }
            ImGui::EndPopup();
        }
    }

    // --- Left: how the view looks (Game: its aspect / resolution) ----------------------------------
    ImGui::SetCursorScreenPos(ImVec2(origin.x + padX, rowY));
    if (gameShowing) {
        m_GameView->DrawAspectControl();
    } else {
        // Perspective: projection and the view presets.
        const bool ortho = editorCamera.Orthographic;
        const std::string proj = std::string(ortho ? EDITOR_ICON_ORTHOGRAPHIC : EDITOR_ICON_PERSPECTIVE) +
                                 (ortho ? "  Orthographic  " : "  Perspective  ") + ICON_FA_CARET_DOWN "##proj";
        if (ActionButton(proj.c_str(), "Projection and view presets", ortho, ImVec2(0.0f, btn)))
            ImGui::OpenPopup("##HeaderProjPopup");
        if (ImGui::BeginPopup("##HeaderProjPopup")) {
            if (ImGui::MenuItem("Perspective", "5", !ortho) && ortho) ToolbarToggleOrthographic(world, editorCamera);
            if (ImGui::MenuItem("Orthographic", "5", ortho) && !ortho) ToolbarToggleOrthographic(world, editorCamera);
            ImGui::Separator();
            if (ImGui::MenuItem("Front", "1"))       SnapToView(world, editorCamera, -90.0f, 0.0f, true);
            if (ImGui::MenuItem("Back", "Ctrl+1"))   SnapToView(world, editorCamera, 90.0f, 0.0f, true);
            if (ImGui::MenuItem("Right", "3"))       SnapToView(world, editorCamera, 180.0f, 0.0f, true);
            if (ImGui::MenuItem("Left", "Ctrl+3"))   SnapToView(world, editorCamera, 0.0f, 0.0f, true);
            if (ImGui::MenuItem("Top", "7"))         SnapToView(world, editorCamera, -90.0f, -89.9f, true);
            if (ImGui::MenuItem("Bottom", "Ctrl+7")) SnapToView(world, editorCamera, -90.0f, 89.9f, true);
            if (ImGui::MenuItem("Three-quarter", "0")) SnapToView(world, editorCamera, -45.0f, -35.264f, true);
            ImGui::EndPopup();
        }
        ImGui::SameLine();

        // Draw mode.
        static const char* kDrawModes[] = {"Shaded", "Wireframe", "Unlit", "Normals", "Shadow Cascades", "Mip / Texel Density"};
        static const char* kDrawModeIcons[] = {EDITOR_ICON_SHADED_MODE, EDITOR_ICON_WIREFRAME_MODE, EDITOR_ICON_UNLIT_MODE,
                                               EDITOR_ICON_NORMALS_MODE, EDITOR_ICON_CASCADES_MODE, EDITOR_ICON_MIP_MODE};
        int shading = ShadingModeIndex();
        if (shading < 0 || shading >= IM_ARRAYSIZE(kDrawModes)) shading = 0;
        const std::string draw = std::string(kDrawModeIcons[shading]) + "  " + kDrawModes[shading] + "  " ICON_FA_CARET_DOWN "##draw";
        if (ActionButton(draw.c_str(), "Draw mode (Alt+D cycles)", shading != 0, ImVec2(0.0f, btn)))
            ImGui::OpenPopup("##HeaderDrawModePopup");
        if (ImGui::BeginPopup("##HeaderDrawModePopup")) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            for (int i = 0; i < IM_ARRAYSIZE(kDrawModes); ++i) {
                const std::string item = std::string(kDrawModeIcons[i]) + "  " + kDrawModes[i];
                if (ImGui::MenuItem(item.c_str(), nullptr, shading == i)) SetShadingModeIndex(i);
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();

        // Show: grid and everything the gizmos draw.
        const std::string show = std::string(EDITOR_ICON_TOGGLE_GIZMOS "  Show  ") + ICON_FA_CARET_DOWN "##show";
        if (ActionButton(show.c_str(), "Grid, gizmos and what they show (Shift+G: all gizmos)", !m_GizmosMasterVisible, ImVec2(0.0f, btn)))
            ImGui::OpenPopup("##HeaderShowPopup");
        if (ImGui::BeginPopup("##HeaderShowPopup")) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            if (ImGui::MenuItem("Grid", "Alt+G", m_ShowGrid)) m_ShowGrid = !m_ShowGrid;
            ImGui::Separator();
            DrawGizmosPopupBody();
            ImGui::EndPopup();
        }
    }

    ImGui::PopStyleVar(2);
    // Leave the layout cursor at the strip's bottom-left, where the image goes.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + stripH));
}
