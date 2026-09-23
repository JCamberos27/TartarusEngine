// Editor chrome: the top toolbar and its menus, the window controls, the play/stop button,
// the viewport status bar and its adaptive-contrast HUD text, and the Console, Stats and
// History panels. Split out of EditorLayer.cpp for build time (#179).

#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorIcons.h"
#include "FileDialog.h"
#include "AssetLibrary.h"
#include "World.h"
#include "Camera.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"
#include "AudioEngine.h"
#include "Screenshot.h"
#include "SceneSerializer.h"
#include "AABB.h"
#include "Log.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h" // SuccessColor() — the Play button's green
#include "PhysicsWorld.h"             // #185 — PDD_* debug-draw channel flags
#include "EditorModuleAPI.h"          // EditorConsoleState — the Console panel lives in the module now
#include "HotReloadEditorModule.h"    // EditorModuleHost::ConsoleState()
#include "AssetImporterInspector.h"
#include "Profiler.h"
#include "ProjectPaths.h"
#include "LayerRegistry.h"
#include "GLStateCache.h"
#include "Framebuffer.h"
#include "gl.h"

#include <imgui.h>
#include <imgui_internal.h> // ImMax/ImFloor, ImGuiWindow, and the item-flag helpers the panels use
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <IconsFontAwesome6.h>

#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp> // extractEulerAngleYXZ - must match ComposeTransform's order (#108)

#include <filesystem>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <sstream>
#include <fstream>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <functional>
#include <cfloat>

using namespace EditorInternal;


// Called from DrawViewportActionBar, which pushes a fixed light ImGuiCol_Text for its dark HUD
// plate before calling this — so this deliberately doesn't touch ImGuiCol_Text itself.
void EditorLayer::DrawPlayTransportButtons(bool playing, bool maximized, bool paused) {
    // Flat "vector" buttons: no body at rest, just the glyph + label; a faint wash of the ambient
    // foreground on hover/press so they still read as pressable without fighting whatever plate
    // (or lack of one) the caller drew behind them.
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.18f));
    // The Unity-dark reskin's FrameBorderSize=1 (EditorLayer.cpp ApplyThemeStyle) draws a hairline
    // around every frame widget including plain Button() — suppress it for these flat transport
    // buttons the same way EditorUIPrimitives::ActionButton does.
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    if (!playing) {
        // The one control every other toolbar button treatment defers to — Unity's own Play
        // button is the single green accent in an otherwise monochrome toolbar, and that's the
        // whole point of the color: it has to read as "the important one" at a glance, not blend
        // into the flat/grey buttons around it.
        const ImVec4 green = EditorUIPrimitives::SuccessColor();
        ImGui::PushStyleColor(ImGuiCol_Text,          green);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(green.x, green.y, green.z, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(green.x, green.y, green.z, 0.32f));
        if (ImGui::Button(ICON_FA_PLAY "  Play")) m_PlayStopRequested = true;
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Play the scene in the Game panel (F1)");
    } else {
        if (ImGui::Button(ICON_FA_STOP "  Stop")) m_PlayStopRequested = true;
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Stop and revert the scene (F1)");

        // Pause toggle + single-frame Step (#236). Pause reads as pressed-in while active; Step
        // is only meaningful (and only enabled) once paused.
        ImGui::SameLine();
        if (paused) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.18f));
        if (ImGui::Button(paused ? ICON_FA_PLAY "  Resume" : ICON_FA_PAUSE "  Pause")) m_PauseToggleRequested = true;
        if (paused) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip(paused ? "Resume simulation (F2)" : "Freeze simulation, keep rendering (F2)");

        ImGui::SameLine();
        ImGui::BeginDisabled(!paused);
        if (ImGui::Button(ICON_FA_FORWARD_STEP "  Step")) m_StepRequested = true;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Advance exactly one frame (F3)");

        ImGui::SameLine();
        const char* fsLabel = maximized ? ICON_FA_COMPRESS "  Restore" : ICON_FA_EXPAND "  Fullscreen";
        if (ImGui::Button(fsLabel)) m_MaximizeToggleRequested = true;
        if (ImGui::IsItemHovered()) {
            EditorUI::SetTooltip(maximized
                ? "Back to windowed play (editor panels return) (F4)"
                : "Maximize the Game view over the editor panels (F4)");
        }
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

// Phase 3 item 2's toolbar Zone B — Play/Stop/Pause/Step drawn inline into the module-owned
// toolbar strip — is dead now that the whole icon row moved out of the toolbar (see
// EditorModuleToolbar.cpp's file comment); DrawViewportActionBar below is the only place these
// controls render, always, not just during maximized play. DrawPlayControlsBody / the
// m_Cached* play-state mirror it read from are kept as unused rather than torn out, since
// EditorModuleHostAPI is an additive, versioned contract (each entry is annotated with the API
// version that added it) that other code hasn't been audited to confirm nothing else expects.
void EditorLayer::DrawPlayControlsBody() {
    DrawPlayTransportButtons(m_CachedPlaying, m_CachedPlayMaximized, m_CachedPaused);
}

// The floating action bar centered over whichever of the Scene/Game viewports is on screen —
// Undo/Redo/Save, the Play/Stop/Pause/Step/Fullscreen transport, and the panel-toggle /
// screenshot / notification cluster that all used to live in the toolbar's one-click icon row.
// Moving them here (instead of a fixed toolbar strip) means they're always over the content
// they act on, in both Scene and Game view, and freed the toolbar down to just its menu bar
// (kToolbarHeight shrank to match — more of the window is now viewport). Replaces the old
// DrawPlayStopButton, which only handled this positioning for the one case (maximized play)
// where the toolbar was hidden entirely; every other state routed Play/Stop through the
// toolbar's Zone B instead. Now there's only one path, used unconditionally.
void EditorLayer::DrawViewportActionBar(World& world, AssetLibrary& assets,
                                         bool playing, bool maximized, bool paused) {
    int ww, wh;
    glfwGetWindowSize(m_Window, &ww, &wh);
    float w = (float)ww;

    // NoDocking: without it this is technically a dockable floating window, and Reset Layout's
    // DockBuilderRemoveNode + full dockspace rebuild (in Draw()) can knock an undocked-but-
    // dockable window out of the visible window list entirely.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;

    // Rides over whichever of the two shared-dock-node viewports is actually on screen: the Game
    // view when its tab is up, otherwise the Scene view — so it never floats in a plate over the
    // middle of the window, and it's on the Scene view too when you tab there mid-play.
    const bool overGame  = m_GameViewImgSize.x > 1.0f && m_GameViewImgSize.y > 1.0f;
    const bool overScene = !overGame && m_ViewportSize.x > 1.0f && m_ViewportSize.y > 1.0f;

    if (overScene || overGame) {
        const ImVec2 imgPos  = overGame ? m_GameViewImgPos  : ImVec2(m_ViewportPos.x, m_ViewportPos.y);
        const ImVec2 imgSize = overGame ? m_GameViewImgSize : ImVec2(m_ViewportSize.x, m_ViewportSize.y);
        const float cx = imgPos.x + imgSize.x * 0.5f;
        const float cy = imgPos.y + 8.0f * m_UIScale;
        ImGui::SetNextWindowPos(ImVec2(cx, cy), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    } else {
        // No viewport rect to anchor to (before the first Scene/Game frame) — float near the
        // top of the window.
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, 10.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    }

    // #5 item 2 — this used to be a 60%-alpha wash behind fixed near-white text, which (on a pale
    // scene) was the audit's "single most important control ... the least visible thing on
    // screen" at ~2:1 contrast. Same fixed opaque plate + fixed light text every other viewport
    // HUD uses since Defect #54 (kHudPlateColor/kHudTextColor), not a bespoke translucency.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorUIPrimitives::kHudPlateColor);
    ImGui::PushStyleColor(ImGuiCol_Text,     EditorUIPrimitives::kHudTextColor);

    ImGui::Begin("##ViewportActionBar", nullptr, flags);
    // Forces this to the front of the display order every frame so a dock rebuild elsewhere
    // (Reset Layout) can't bury it behind whatever the freshly recreated dock host window
    // ends up as. Skipped while a popup is open (e.g. Capture options) so the bar doesn't punch
    // through a menu that legitimately overlays the viewport top-centre.
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    // A short inset tick between clusters — same treatment the old toolbar row's dividers used,
    // lighter than the icons themselves rather than another hard-edged shape the same weight.
    auto divider = [this]() {
        ImGui::SameLine(0.0f, 10.0f * m_UIScale);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        const float inset = h * 0.22f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + inset), ImVec2(p.x, p.y + h - inset),
                                            ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(1.0f, h));
        ImGui::SameLine(0.0f, 10.0f * m_UIScale);
    };

    // --- Undo / Redo / Save ------------------------------------------------------------------
    if (ActionButton(EDITOR_ICON_UNDO, "Undo (Ctrl+Z)")) ToolbarUndo(world, assets);
    ImGui::SameLine();
    if (ActionButton(EDITOR_ICON_REDO, "Redo (Ctrl+Y)")) ToolbarRedo(world, assets);
    ImGui::SameLine();
    if (ActionButton(EDITOR_ICON_SAVE, "Save (Ctrl+S)")) SaveScene(world, assets);

    // --- Play / Stop / Pause / Step / Fullscreen ---------------------------------------------
    divider();
    DrawPlayTransportButtons(playing, maximized, paused);

    // --- Panel toggles ------------------------------------------------------------------------
    divider();
    {
        bool showStats = EditorSettings::Get().SceneShowStats;
        if (ActionButton(EDITOR_ICON_STATISTICS, "Toggle Statistics", showStats)) {
            EditorSettings::Get().SceneShowStats = !showStats;
            EditorSettings::Save();
        }
    }
    ImGui::SameLine();
    {
        // Defect #11 — flip Visible off only when Console is already the front/selected tab;
        // if it's hidden behind a sibling dock tab (Asset Browser), focus it instead of closing
        // an already-closed panel.
        EditorConsoleState& cs = EditorModuleHost::ConsoleState();
        if (ActionButton(EDITOR_ICON_CONSOLE, "Toggle Console", cs.Visible)) {
            if (!cs.Visible) {
                cs.Visible = true;
            } else {
                ImGuiWindow* consoleWin = ImGui::FindWindowByName(ICON_FA_TERMINAL "  Console");
                if (consoleWin && consoleWin->Hidden) ImGui::FocusWindow(consoleWin);
                else cs.Visible = false;
            }
        }
    }
    ImGui::SameLine();
    if (ActionButton(EDITOR_ICON_HISTORY, "Toggle History", m_ShowHistory)) m_ShowHistory = !m_ShowHistory;

    // --- Capture + notifications ---------------------------------------------------------------
    divider();
    {
        char tip[128];
        const EditorSettings& s = EditorSettings::Get();
        static const char* kModes[] = { "Full editor window", "Scene viewport",
                                        "Scene viewport (clean)", "Game view" };
        const int cm = std::clamp(s.CaptureMode, 0, 3);
        std::snprintf(tip, sizeof(tip), "Capture screenshot \xe2\x80\x94 %s%s (Print Screen)",
                      kModes[cm], (cm == 1 || cm == 2) && s.CaptureScale > 1 ? " x2+" : "");
        if (ActionButton(EDITOR_ICON_CAPTURE, tip)) RequestCapture();
        ImGui::SameLine(0.0f, 1.0f * m_UIScale);
        if (ActionButton(EDITOR_ICON_CARET_DOWN, "Capture options")) ImGui::OpenPopup("##ActionBarCapturePopup");
        if (ImGui::BeginPopup("##ActionBarCapturePopup")) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            DrawCaptureOptionsPopupBody();
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();
    {
        const int unread = NotificationUnreadCount();
        char tip[32];
        std::snprintf(tip, sizeof(tip), "Notifications%s", unread > 0 ? " (unread)" : "");
        if (ActionButton(ICON_FA_BELL, tip)) {
            ImGui::OpenPopup("##ActionBarNotificationsPopup");
            MarkNotificationsRead();
        }
        if (unread > 0) {
            const ImVec2 btnMin = ImGui::GetItemRectMin();
            const ImVec2 btnMax = ImGui::GetItemRectMax();
            char countStr[8];
            std::snprintf(countStr, sizeof(countStr), "%d", unread > 99 ? 99 : unread);
            const ImVec2 textSize = ImGui::CalcTextSize(countStr);
            const float badgeR = std::max(7.0f * m_UIScale, textSize.x * 0.5f + 2.0f * m_UIScale);
            const ImVec2 center(btnMax.x - badgeR * 0.7f, btnMin.y + badgeR * 0.7f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(center, badgeR, IM_COL32(219, 60, 60, 255));
            dl->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
                        IM_COL32(255, 255, 255, 255), countStr);
        }
        if (ImGui::BeginPopup("##ActionBarNotificationsPopup")) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            ImGui::TextDisabled("Notifications");
            ImGui::Separator();
            DrawNotificationsPopupBody();
            ImGui::EndPopup();
        }
    }

    ImGui::PopStyleColor(2);
    ImGui::End();
}

// The Console panel moved into TartarusEditor.dll (src/Editor/EditorModuleConsole.cpp) so its
// code hot-reloads while the editor stays open. The host still owns the log itself and the
// panel's persistent UI state (EditorModuleHost::ConsoleState()), which the module reads through
// the EditorModuleHostAPI callback table; main.cpp draws it via editorModule.Draw().

// The Statistics HUD itself lives in the reloadable editor module (src/Editor/EditorModuleStats.cpp).

void EditorLayer::DrawViewportStatusBar(World& world, Camera& editorCamera) {
    if (!m_SceneViewportVisible || m_ViewportSize.x < 1.0f || m_ViewportSize.y < 1.0f) return;

    const char* toolName =
        m_HandTool                       ? "Hand"      :
        m_GizmoOp == GizmoOp::Translate  ? "Translate" :
        m_GizmoOp == GizmoOp::Rotate     ? "Rotate"    :
        m_GizmoOp == GizmoOp::Scale      ? "Scale"     :
        m_GizmoOp == GizmoOp::Universal  ? "Transform" : "Rect";

    int selCount = (int)GetSelectedItems().size();
    float fps = m_SmoothedFrameMs > 0.0001f ? 1000.0f / m_SmoothedFrameMs : 0.0f;

    auto compact = [](int n) -> std::string {
        if (n >= 1000000) { char b[32]; snprintf(b, sizeof(b), "%.1fM", n / 1e6); return b; }
        if (n >= 1000)    { char b[32]; snprintf(b, sizeof(b), "%.1fk", n / 1e3); return b; }
        return std::to_string(n);
    };

    const float barH = ImGui::GetTextLineHeight() + 8.0f * m_UIScale;

    // #54 — used to be a bare line of text over the 3D view, kept legible by sampling the scene
    // luminance behind it and steering white-on-dark / dark-on-light; now a fixed opaque strip
    // behind fixed near-white text, legible over anything.
    ImGui::SetNextWindowPos(ImVec2(m_ViewportPos.x, m_ViewportPos.y + m_ViewportSize.y - barH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(m_ViewportSize.x, barH), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * m_UIScale, 3.0f * m_UIScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,     EditorUIPrimitives::kHudPlateColor);
    ImGui::PushStyleColor(ImGuiCol_Text,         EditorUIPrimitives::kHudTextColor);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, EditorUIPrimitives::kHudTextDisabledColor);
    // Phase 3 item 5 — NoInputs is gone: the bar used to be pure decoration ("today it is
    // ImGuiWindowFlags_NoInputs — nothing clickable", audit #5). Individual segments below opt
    // into a click action via IsItemClicked() on whatever they just drew (Text() items still
    // register a hoverable/clickable rect — this doesn't require switching them to buttons).
    if (ImGui::Begin("##ViewportStatusBar", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove)) {

        auto sep = [&]() { ImGui::SameLine(0, 6); ImGui::TextDisabled("\xc2\xb7"); ImGui::SameLine(0, 6); };
        // Every interactive segment below shares this: hand cursor + tooltip on hover, act on
        // click. Kept as a helper rather than repeating the three lines per segment.
        auto clickable = [](const char* tooltip) -> bool {
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                EditorUI::SetTooltip("%s", tooltip);
            }
            return ImGui::IsItemClicked();
        };

        ImGui::AlignTextToFramePadding();
        // Build config, so it's obvious at a glance whether this is the slow (Debug) binary or an
        // optimized one. TARTARUS_BUILD_CONFIG is "$<CONFIG>" from CMake; NDEBUG is the canonical
        // "optimized" signal (Release / RelWithDebInfo define it), so only a true Debug build gets
        // the amber warning colour — the rest render quiet.
#ifndef TARTARUS_BUILD_CONFIG
#define TARTARUS_BUILD_CONFIG "Build?"
#endif
#if defined(NDEBUG)
        ImGui::TextDisabled("%s", TARTARUS_BUILD_CONFIG);
#else
        ImGui::TextColored(ImVec4(1.00f, 0.62f, 0.20f, 1.00f), "%s", TARTARUS_BUILD_CONFIG);
#endif
        sep();
        // The FPS/ms/draws/tris cluster is one click target — jumps to the deeper Statistics
        // panel readout instead of duplicating it here.
        ImGui::Text("%.0f FPS", fps);
        if (clickable("Open the Statistics panel")) {
            EditorSettings::Get().SceneShowStats = !EditorSettings::Get().SceneShowStats;
            EditorSettings::Save();
        }
        sep(); ImGui::TextDisabled("%.1f ms", m_SmoothedFrameMs);
        if (clickable("Open the Statistics panel")) {
            EditorSettings::Get().SceneShowStats = !EditorSettings::Get().SceneShowStats;
            EditorSettings::Save();
        }
        sep(); ImGui::Text("%s draws", compact(m_RenderStats.DrawCalls).c_str());
        if (clickable("Open the Statistics panel")) {
            EditorSettings::Get().SceneShowStats = !EditorSettings::Get().SceneShowStats;
            EditorSettings::Save();
        }
        sep(); ImGui::TextDisabled("%s tris", compact(m_RenderStats.Triangles).c_str());
        if (clickable("Open the Statistics panel")) {
            EditorSettings::Get().SceneShowStats = !EditorSettings::Get().SceneShowStats;
            EditorSettings::Save();
        }

        // Scene name (audit #5 item 5's explicit ask) — click reveals the file, same "Show in
        // folder" action Capture's toast/menu already use.
        sep();
        {
            std::string sceneName = std::filesystem::path(m_CurrentScenePath).filename().string();
            if (sceneName.empty()) sceneName = "Untitled";
            if (m_Dirty) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s*", sceneName.c_str());
            else         ImGui::TextUnformatted(sceneName.c_str());
            if (clickable(m_CurrentScenePath.empty() ? "Untitled scene — Save to give it a file"
                                                      : "Show the scene file in its folder")) {
                if (!m_CurrentScenePath.empty()) Screenshot::ShowInFolder(m_CurrentScenePath);
            }
        }

        // Camera readout — where the Scene view is and what it's looking at, so a screenshot is
        // enough to reproduce a view exactly.
        sep();
        ImGui::TextDisabled("cam %.2f, %.2f, %.2f  yaw %.1f  pitch %.1f", editorCamera.Position.x,
                            editorCamera.Position.y, editorCamera.Position.z, editorCamera.Yaw, editorCamera.Pitch);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Scene camera position and yaw / pitch (degrees)");

        sep();
        if (selCount == 0) {
            ImGui::TextDisabled("no selection");
        } else {
            ImGui::Text("%d selected", selCount);
            if (clickable("Frame the selection (F)")) FocusOnSelection(world, editorCamera);
        }

        if (m_LockViewToSelection) {
            sep();
            ImGui::Text("%s follow", ICON_FA_LOCK); // Shift+F (#236 E)
            if (clickable("Stop following the selection (Shift+F)")) SetLockViewToSelection(false);
        }

        // Active tool pinned to the right — click toggles the Hand tool, the one binary state
        // (as opposed to the 5-way gizmo-op choice, already one click away in the tool palette).
        char toolBuf[32]; snprintf(toolBuf, sizeof(toolBuf), "%s  %s",
            m_HandTool ? ICON_FA_HAND : ICON_FA_UP_DOWN_LEFT_RIGHT, toolName);
        float tw = ImGui::CalcTextSize(toolBuf).x;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - tw);
        ImGui::TextUnformatted(toolBuf);
        if (clickable(m_HandTool ? "Switch back to the last transform tool (Q)" : "Hand tool — drag to pan the view (Q)"))
            SetHandToolActive(!m_HandTool);
    }
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

// Unity-style Undo History: every recorded change, oldest to newest, with the current position
// highlighted. Clicking any entry jumps straight there via JumpToUndoEntry/JumpToRedoEntry -
// each step is still a single full-snapshot load (see PushUndo's own comment), not incremental
// command replay, so jumping several steps at once stays cheap regardless of distance.
//
// The HUD window itself — the bottom-right pin, the height ceiling, the contrast-adaptive tint —
// moved into TartarusEditor.dll (EditorModuleHistory.cpp, issue #229 API v15). These three are
// the host half the module reaches back through EditorModuleHostAPI.

// "Draw the History HUD this frame?" — plus the viewport rect, UI scale and row count the module
// sizes its window against. Mirrors the old DrawHistoryPanel early-outs: History toggled on, a
// live non-degenerate Scene viewport, and overlays not suppressed for a clean capture.
bool EditorLayer::HistoryHudFrame(float* outVpX, float* outVpY, float* outVpW, float* outVpH,
                                  float* outUIScale, int* outRowCount) {
    if (outVpX)     *outVpX = m_ViewportPos.x;
    if (outVpY)     *outVpY = m_ViewportPos.y;
    if (outVpW)     *outVpW = m_ViewportSize.x;
    if (outVpH)     *outVpH = m_ViewportSize.y;
    if (outUIScale) *outUIScale = m_UIScale;
    if (outRowCount)
        *outRowCount = (int)m_UndoStack.size() + 1 /*Current*/ + (int)m_RedoStack.size();
    return m_ShowHistory && !m_HideOverlaysThisFrame && m_SceneViewportVisible &&
           m_ViewportSize.x >= 1.0f && m_ViewportSize.y >= 1.0f;
}

// The click-to-jump rows, drawn host-side into the module's window between its heading Separator
// and its End. The undo/redo stacks, World& and AssetLibrary& never cross the DLL boundary.
void EditorLayer::DrawHistoryListBody(World& world, AssetLibrary& assets) {
    if (m_UndoStack.empty() && m_RedoStack.empty()) {
        ImGui::TextDisabled("No changes yet.");
        return;
    }

    for (size_t k = 0; k < m_UndoStack.size(); ++k) {
        ImGui::PushID((int)k);
        // Q6/Phase 6 item 6 — a selection-change row reads as secondary (dimmed, cursor glyph)
        // next to a real edit, without hiding it: the panel must list every step, selection
        // included, so Ctrl+Z's press count always matches what's shown here.
        const bool selOnly = m_UndoStack[k].SelectionOnly;
        if (selOnly) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const std::string label = (selOnly ? std::string(ICON_FA_ARROW_POINTER "  ") : std::string()) + m_UndoStack[k].Label;
        bool clicked = ImGui::Selectable(label.c_str());
        if (selOnly) ImGui::PopStyleColor();
        if (clicked) JumpToUndoEntry(world, assets, k);
        ImGui::PopID();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
    ImGui::Selectable(ICON_FA_LOCATION_DOT "  Current", true);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Where you are right now");

    // Newest-future-first won't read naturally, so this walks the redo stack back-to-front
    // (Redo() always consumes from .back()) to show it oldest-to-newest like everything above.
    for (size_t idx = m_RedoStack.size(); idx-- > 0;) {
        ImGui::PushID((int)(100000 + idx)); // distinct ID range from the undo rows above
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        bool clicked = ImGui::Selectable(m_RedoStack[idx].Label.c_str());
        ImGui::PopStyleColor();
        if (clicked) JumpToRedoEntry(world, assets, idx);
        ImGui::PopID();
    }
}
// Fixed output sizes offered by the Capture popup's "Resolution" dropdown. Index 0 keeps the
// live viewport size (and lets Supersample apply); the rest force an exact render target.
// Shared by the toolbar popup and RequestCapture().
static const struct { const char* label; int w, h; } kCaptureRes[] = {
    { "Match viewport", 0,    0    },
    { "1280 x 720",     1280, 720  },
    { "1920 x 1080",    1920, 1080 },
    { "2560 x 1440",    2560, 1440 },
    { "3840 x 2160",    3840, 2160 },
};

float EditorLayer::ToolbarHeightPx() const { return kToolbarHeight * m_UIScale; }

// The toolbar strip + its menu bar + the window min/max/close controls moved into
// TartarusEditor.dll (EditorModuleToolbar.cpp, issue #229). What stays here is the deep host
// logic behind the menus — scene load/save, camera framing, the capture options popup — exposed
// to the module through EditorModuleHostAPI (API v4) as the four Draw*Body methods below.
// DrawAddEntityItems (EditorLayer_Hierarchy.cpp) is the fifth; the Shift+A quick-add popup and
// the Hierarchy's context menu call it too. The play/stop button, viewport status bar and
// History HUD further down this file are still host-drawn (a later #229 pass).

void EditorLayer::DrawFileMenuBody(World& world, AssetLibrary& assets) {
            if (ImGui::MenuItem(ICON_FA_FILE "  New Scene", "Ctrl+N")) {
                RequestNewScene(world, assets);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open...", "Ctrl+O")) {
                RequestOpenScene(world, assets, FileDialog::OpenFile(
                    "Scene Files\0*.json\0All Files\0*.*\0", m_Window));
            }
            // Q12 (Phase 4 / #6) — disabled while Playing rather than left live: a Play-mode edit
            // reverts on Stop anyway (OnEnterPlayMode snapshots the scene), so saving mid-Play
            // would either silently discard that guarantee or persist a state the user never
            // meant to keep. Editing itself stays fully live — this only gates the two ways to
            // write it to disk. AllowWhenDisabled so the "why" tooltip still shows on the greyed
            // item, not just when it's enabled.
            if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save", "Ctrl+S", false, !m_InPlayMode)) {
                DoSave(world, assets);
            }
            if (m_InPlayMode && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                EditorUI::SetTooltip("Disabled while Playing \xE2\x80\x94 changes here revert on Stop anyway.");
            if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save As...", "Ctrl+Shift+S", false, !m_InPlayMode)) {
                DoSaveAs(world, assets);
            }
            if (m_InPlayMode && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                EditorUI::SetTooltip("Disabled while Playing \xE2\x80\x94 changes here revert on Stop anyway.");
            if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Revert Scene", nullptr, false,
                                !m_CurrentScenePath.empty())) {
                RequestRevertScene(world, assets); // #236 R2 — reload from disk, discard edits
            }
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Reload the scene file from disk, discarding unsaved (and in-play) changes.");
            ImGui::Separator();
            {
                std::string cur = std::filesystem::path(m_CurrentScenePath).filename().string();
                if (cur.empty()) cur = "Untitled";
                ImGui::TextDisabled("Current: %s%s", cur.c_str(), m_Dirty ? " (unsaved)" : "");
            }
            if (EditorSettings::Get().AutoSaveEnabled) {
                float remaining = std::max(0.0f, EditorSettings::Get().AutoSaveIntervalMinutes * 60.0f - m_AutoSaveTimer);
                ImGui::TextDisabled("Next auto-save in %.0fs%s", remaining, m_Dirty ? "" : " (nothing to save)");
            }

            ImGui::Separator();
            if (ImGui::BeginMenu(ICON_FA_FILE_IMPORT "  Import")) {
                if (ImGui::MenuItem(ICON_FA_CUBE "  Model...")) {
                    const std::vector<std::string> paths = FileDialog::OpenFiles( // #139 multi-select
                        "3D Models\0*.fbx;*.obj;*.gltf;*.glb\0All Files\0*.*\0", m_Window);
                    // Imports into the library only — doesn't place an instance in the scene.
                    // Drag it from the Asset Browser into the Viewport to place one. #125 — same
                    // pipeline as drag-drop: copied into the project (with its companion files)
                    // and filed into the open Asset Browser folder.
                    for (const std::string& path : paths) ImportFileIntoProject(world, assets, path);
                }
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("FBX / OBJ / glTF - added to the asset library");
                if (ImGui::MenuItem(ICON_FA_IMAGE "  Texture...")) {
                    const std::vector<std::string> paths = FileDialog::OpenFiles( // #139 multi-select
                        "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0", m_Window);
                    for (const std::string& path : paths) ImportFileIntoProject(world, assets, path); // #125
                }
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("PNG / JPG / TGA / BMP");
                if (ImGui::MenuItem(ICON_FA_MUSIC "  Sound...")) {
                    const std::vector<std::string> paths = FileDialog::OpenFiles( // #139 multi-select
                        "Audio\0*.wav;*.mp3;*.ogg;*.flac\0All Files\0*.*\0", m_Window);
                    for (const std::string& path : paths) ImportFileIntoProject(world, assets, path); // #125
                }
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("WAV / MP3 / OGG / FLAC");
                ImGui::EndMenu();
            }
}

void EditorLayer::DrawViewMenuBody(World& world, Camera& editorCamera) {
            if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS_PLUS "  Frame Selected", "F", false, HasAnySelection())) {
                FocusOnSelection(world, editorCamera);
            }
            if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Frame All")) {
                FrameSceneBounds(world, editorCamera);
            }

            ImGui::SeparatorText("Selection");
            if (ImGui::MenuItem(ICON_FA_ARROW_LEFT "  Selection Back", "Ctrl+[", false, CanSelectionHistoryBack()))
                SelectionHistoryBack(world);
            if (ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Selection Forward", "Ctrl+]", false, CanSelectionHistoryForward()))
                SelectionHistoryForward(world);

            // #4 item 7 — Draw mode moved out: it has its own viewport control (the left tool
            // palette's draw-mode icon, DrawToolPalette in EditorLayer_ToolPalette.cpp), so this
            // menu copy was a live duplicate. Grid lives only there too (#148) — the menu keeps
            // just the toggles that have no viewport-control home.
            ImGui::SeparatorText("Options");
            ImGui::MenuItem(EDITOR_ICON_TOGGLE_GIZMOS "  Transform Gizmo", nullptr, &m_ShowGizmos);
            if (ImGui::MenuItem(ICON_FA_RULER "  Measure Tool", "M", m_MeasureTool)) {
                m_MeasureTool = !m_MeasureTool;
                m_MeasurePoints.clear();
            }
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Click to chain measurement points; right-click clears.");
            ImGui::MenuItem(ICON_FA_CROSSHAIRS "  Frame on Select", nullptr, &m_FrameOnSelect);
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Move the camera to frame each object as you select it.");

            {
                bool muted = EditorSettings::Get().AudioMuted;
                if (ImGui::MenuItem(muted ? ICON_FA_VOLUME_XMARK "  Mute Audio"
                                          : ICON_FA_VOLUME_HIGH "  Mute Audio", nullptr, muted)) {
                    EditorSettings::Get().AudioMuted = !muted;
                    AudioEngine::SetMuted(!muted);
                    EditorSettings::Save();
                }
                if (ImGui::IsItemHovered())
                    EditorUI::SetTooltip("Master-mute the audio engine (editor previews and Play-mode sound).");
            }
            if (ImGui::MenuItem(EDITOR_ICON_ORTHOGRAPHIC "  Orthographic", "5", editorCamera.Orthographic)) {
                ToggleOrthographic(world, editorCamera);
            }

            // #4 item 7 — Camera sliders (FOV/Fly speed/Near/Far) moved to Preferences > Viewport
            // > Camera, the only home they didn't already have; the persisted fields are unchanged
            // (EditorSettings::SceneCameraFov etc.), so existing editor_prefs.json values carry over.

            ImGui::SeparatorText("Snap to view");
            if (ImGui::MenuItem("  Iso", "0")) {
                SnapToView(world, editorCamera, -45.0f, -35.264f, true);
            }
            if (ImGui::MenuItem("  Front", "1")) SnapToView(world, editorCamera, -90.0f, 0.0f, true);
            if (ImGui::MenuItem("  Back", "Ctrl+1")) SnapToView(world, editorCamera, 90.0f, 0.0f, true);
            if (ImGui::MenuItem("  Right", "3")) SnapToView(world, editorCamera, 180.0f, 0.0f, true);
            if (ImGui::MenuItem("  Left", "Ctrl+3")) SnapToView(world, editorCamera, 0.0f, 0.0f, true);
            if (ImGui::MenuItem("  Top", "7")) SnapToView(world, editorCamera, -90.0f, -89.9f, true);
            if (ImGui::MenuItem("  Bottom", "Ctrl+7")) SnapToView(world, editorCamera, -90.0f, 89.9f, true);
}

void EditorLayer::DrawWindowMenuBody() {
            // #4 item 5 — every dockable surface listed here, not just the ones without their own
            // toolbar toggle. Scene and Game get entries too (audit's explicit ask), even though
            // Scene can never be closed and Game's toggle round-trips through main.cpp (it owns
            // GameViewPanel, not this class) via SetGameViewOpenState/ConsumeGameViewOpenRequest.
            bool sceneAlwaysOpen = true;
            ImGui::MenuItem(ICON_FA_CAMERA "  Scene", nullptr, &sceneAlwaysOpen, false);
            // IsItemHovered() ignores disabled items by default (AllowWhenDisabled) — without this
            // flag the tooltip below never fires, since this MenuItem is always disabled (#69 QA).
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                EditorUI::SetTooltip("Always open — the core viewport can't be closed.");
            {
                bool gameOpen = m_GameViewOpenCached;
                if (ImGui::MenuItem(ICON_FA_DESKTOP "  Game", nullptr, &gameOpen))
                    m_GameViewOpenRequest = gameOpen ? 1 : 0;
            }
            ImGui::MenuItem(ICON_FA_SITEMAP "  Scene Hierarchy", nullptr, &m_ShowHierarchy);
            ImGui::MenuItem(ICON_FA_SLIDERS "  Inspector", nullptr, &m_ShowInspector);
            ImGui::MenuItem(ICON_FA_FOLDER_TREE "  Asset Browser", nullptr, &m_ShowAssetBrowser);
            ImGui::MenuItem(ICON_FA_DIAGRAM_PROJECT "  Animator", nullptr, &m_ShowAnimator);
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("The Animator Controller graph editor: states, transitions, layers and parameters.");
            ImGui::MenuItem(ICON_FA_LIGHTBULB "  Lighting", nullptr, &m_ShowLighting);
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Environment (sky / ambient), post-processing (exposure / tone map) and shadow settings in one place.");
            {
                bool consoleOpen = EditorModuleHost::ConsoleState().Visible;
                if (ImGui::MenuItem(ICON_FA_TERMINAL "  Console", nullptr, &consoleOpen))
                    EditorModuleHost::ConsoleState().Visible = consoleOpen;
            }
            if (ImGui::MenuItem(ICON_FA_CHART_SIMPLE "  Statistics", nullptr, &EditorSettings::Get().SceneShowStats))
                EditorSettings::Save();
            ImGui::MenuItem(ICON_FA_CLOCK_ROTATE_LEFT "  History", nullptr, &m_ShowHistory);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_CUBES "  Physics Debug", nullptr, &EditorSettings::Get().ShowPhysicsPanel))
                EditorSettings::Save();
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Live sim stats, visual debug-draw channels, slow-mo / break-on, body + joint + event inspectors (#185).");
            if (ImGui::MenuItem(ICON_FA_GAUGE_HIGH "  Physics HUD overlay", nullptr, &EditorSettings::Get().PhysicsHudOverlay))
                EditorSettings::Save();
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Corner text overlay on the Scene viewport while playing.");
            ImGui::Separator();
            // Console, Statistics, History and every panel above also keep their existing toolbar
            // toggle (#148) — the menu entry is a second, discoverable path to the same state, not
            // a replacement. The engine mark lives in Preferences ▸ Viewport. Project Settings
            // moved to its own menu-bar item beside Preferences (it's a settings window, not a
            // dock panel).
            if (ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Reset Layout")) {
                m_ResetLayoutRequested = true;
            }

            // Layout presets (#236 R2, extended Phase 6 item 10) — named ImGui-ini snapshots in
            // the user's layouts folder (#184), plus four shipped arrangements that need no file.
            if (ImGui::BeginMenu(ICON_FA_TABLE_COLUMNS "  Layout Presets")) {
                if (ImGui::BeginMenu(ICON_FA_TABLE_CELLS "  Default Layouts")) {
                    if (ImGui::MenuItem("Default")) RequestDefaultLayout(LayoutKind::Default);
                    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("The standard arrangement: Hierarchy / Scene / Inspector, Asset Browser + Console below.");
                    if (ImGui::MenuItem("Wide")) RequestDefaultLayout(LayoutKind::Wide);
                    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Narrower side panels, more viewport width — for ultrawide monitors.");
                    if (ImGui::MenuItem("Tall")) RequestDefaultLayout(LayoutKind::Tall);
                    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Taller Asset Browser / Console strip — for portrait or stacked monitors.");
                    if (ImGui::MenuItem("Focus")) RequestDefaultLayout(LayoutKind::Focus);
                    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Hides Hierarchy and Inspector to maximize the Scene viewport.");
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                const std::vector<std::string> presets = LayoutPresetNames();
                if (presets.empty()) ImGui::TextDisabled("(none saved yet)");
                for (const std::string& name : presets) {
                    ImGui::PushID(name.c_str());
                    if (m_RenamingLayoutPreset == name) {
                        ImGui::SetNextItemWidth(150.0f * m_UIScale);
                        bool enter = ImGui::InputText("##Rename", m_RenameLayoutBuf, sizeof(m_RenameLayoutBuf),
                                                       ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                        if (ImGui::IsWindowAppearing() || ImGui::IsItemActivated()) ImGui::SetKeyboardFocusHere(-1);
                        if (enter) {
                            RenameLayoutPreset(name, m_RenameLayoutBuf);
                            m_RenamingLayoutPreset.clear();
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(ICON_FA_CHECK)) { RenameLayoutPreset(name, m_RenameLayoutBuf); m_RenamingLayoutPreset.clear(); }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(ICON_FA_XMARK)) m_RenamingLayoutPreset.clear();
                    } else {
                        const bool isDefault = EditorSettings::Get().DefaultLayoutPreset == name;
                        std::string label = isDefault ? (ICON_FA_STAR "  " + name) : name;
                        // MenuItem spans the full row by default, so without AllowOverlap it eats
                        // the click before it ever reaches the rename/duplicate/star/delete
                        // buttons drawn on top of it below (same fix as EditorLayer_Inspector.cpp).
                        ImGui::SetNextItemAllowOverlap();
                        if (ImGui::MenuItem(label.c_str())) RequestLoadLayoutPreset(name);
                        if (ImGui::IsItemHovered() && isDefault) EditorUI::SetTooltip("Startup default — Reset Layout rebuilds to this preset.");
                        ImGui::SameLine();
                        if (ImGui::SmallButton(ICON_FA_PEN)) {
                            m_RenamingLayoutPreset = name;
                            std::snprintf(m_RenameLayoutBuf, sizeof(m_RenameLayoutBuf), "%s", name.c_str());
                        }
                        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Rename this preset");
                        ImGui::SameLine();
                        if (ImGui::SmallButton(ICON_FA_COPY)) DuplicateLayoutPreset(name);
                        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Duplicate this preset");
                        ImGui::SameLine();
                        if (!isDefault) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                        const bool starClicked = ImGui::SmallButton(ICON_FA_STAR);
                        if (!isDefault) ImGui::PopStyleColor();
                        if (starClicked) {
                            EditorSettings::Get().DefaultLayoutPreset = isDefault ? std::string() : name;
                            EditorSettings::Save();
                        }
                        if (ImGui::IsItemHovered()) EditorUI::SetTooltip(isDefault ? "Unset as startup default" : "Set as startup default (Reset Layout rebuilds to this)");
                        ImGui::SameLine();
                        // #184: a preset is gone for good once deleted (no undo), so the first
                        // click only arms the row; a second click on "Delete?" confirms. Opening
                        // the menu again or arming another row disarms it.
                        if (ImGui::IsWindowAppearing()) m_ConfirmDeleteLayoutPreset.clear();
                        if (m_ConfirmDeleteLayoutPreset == name) {
                            ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::DangerColor());
                            const bool confirmed = ImGui::SmallButton(ICON_FA_TRASH "  Delete?");
                            ImGui::PopStyleColor();
                            if (confirmed) {
                                DeleteLayoutPreset(name);
                                m_ConfirmDeleteLayoutPreset.clear();
                            }
                            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Click again to delete this preset permanently");
                        } else {
                            if (ImGui::SmallButton(ICON_FA_TRASH)) m_ConfirmDeleteLayoutPreset = name;
                            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Delete this preset");
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save Current Layout\xE2\x80\xA6")) {
                    m_SaveLayoutName[0] = '\0';
                    m_ShowSaveLayout = true;
                }
                ImGui::EndMenu();
            }
}

// The capture options popup body (Mode / Resolution / Supersample / Format / Flash / Sound +
// "Capture now" + "Open screenshots folder"). The module owns the toolbar camera button and the
// caret that opens this; the module wraps the call in its own BeginPopup("##CapturePopup") and
// xpMenuTextPush/Pop.
void EditorLayer::DrawCaptureOptionsPopupBody() {
    auto& cs = EditorSettings::Get();
    static const char* kModes[] = { "Full editor window", "Scene viewport", "Scene viewport (clean)", "Game view" };
    {
        {
            ImGui::PushItemWidth(150.0f * m_UIScale);
            ImGui::TextDisabled("Capture");
            if (ImGui::Combo("Mode", &cs.CaptureMode, kModes, IM_ARRAYSIZE(kModes))) EditorSettings::Save();
            const bool viewportMode = (cs.CaptureMode == 1 || cs.CaptureMode == 2);
            ImGui::BeginDisabled(!viewportMode);
            const char* kResLabels[IM_ARRAYSIZE(kCaptureRes)];
            for (int i = 0; i < IM_ARRAYSIZE(kCaptureRes); ++i) kResLabels[i] = kCaptureRes[i].label;
            if (ImGui::Combo("Resolution", &cs.CaptureResPreset, kResLabels, IM_ARRAYSIZE(kResLabels)))
                EditorSettings::Save();
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!viewportMode || cs.CaptureResPreset != 0);
            static const char* kScales[] = { "1x", "2x", "4x" };
            int si = cs.CaptureScale >= 4 ? 2 : (cs.CaptureScale >= 2 ? 1 : 0);
            if (ImGui::Combo("Supersample", &si, kScales, IM_ARRAYSIZE(kScales))) {
                cs.CaptureScale = si == 2 ? 4 : (si == 1 ? 2 : 1); EditorSettings::Save();
            }
            ImGui::EndDisabled();
            static const char* kFmt[] = { "PNG", "JPG" };
            if (ImGui::Combo("Format", &cs.CaptureFormat, kFmt, IM_ARRAYSIZE(kFmt))) EditorSettings::Save();
            if (EditorUIPrimitives::Checkbox("Flash", &cs.CaptureFlash)) EditorSettings::Save();
            ImGui::SameLine();
            if (EditorUIPrimitives::Checkbox("Sound", &cs.CaptureSound)) EditorSettings::Save();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_CAMERA_RETRO "  Capture now")) { RequestCapture(); ImGui::CloseCurrentPopup(); }
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open screenshots folder"))
                Screenshot::ShowInFolder(m_LastCapturePath.empty() ? Screenshot::Dir() : m_LastCapturePath);
            ImGui::PopItemWidth();
        }
    }
}

void EditorLayer::DrawGridSnapPopupBody() {
    auto& gs = EditorSettings::Get();
    ImGui::PushItemWidth(120.0f * m_UIScale);

    ImGui::TextDisabled("GRID");
    // #4 item 4 — "Show grid" everywhere this toggle appears (Preferences > Viewport, the Gizmos
    // popover below, and here) instead of three different labels for the same m_ShowGrid bool.
    bool showGrid = m_ShowGrid;
    if (EditorUIPrimitives::Checkbox("Show grid", &showGrid)) m_ShowGrid = showGrid;
    // EditorUI::SliderFloat's out-param is needed — a bare IsItemDeactivatedAfterEdit() after the
    // call only ever sees the trailing number box, so releasing a track drag wouldn't save.
    {
        bool committed = false;
        EditorUI::SliderFloat("Cell size", &gs.GridMinorSpacing, 0.05f, 50.0f, "%.2f m",
                              ImGuiSliderFlags_Logarithmic, nullptr, &committed);
        if (committed) EditorSettings::Save();
    }
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("World units between minor grid lines.\nAlso the step used when snapping a dropped object to the ground grid.");
    {
        bool committed = false;
        EditorUI::SliderInt("Major every", &gs.GridMajorEvery, 2, 100, "%d cells",
                            0, nullptr, &committed);
        if (committed) EditorSettings::Save();
    }

    ImGui::Separator();
    ImGui::TextDisabled("SNAP  (hold Ctrl while dragging to invert)");
    bool snap = m_GridSnapEnabled;
    if (EditorUIPrimitives::Checkbox("Snap enabled", &snap)) m_GridSnapEnabled = snap;
    ImGui::BeginDisabled(!m_GridSnapEnabled);
    // #4 item 4 — same vocabulary as Preferences > Grid & Snapping's sliders (same three fields).
    EditorUI::SliderFloat("Move snap",   &m_SnapTranslation, 0.001f, 100.0f, "%.3f m", ImGuiSliderFlags_Logarithmic);
    EditorUI::SliderFloat("Rotate snap", &m_SnapRotationDeg, 0.1f,   180.0f, "%.1f deg");
    EditorUI::SliderFloat("Scale snap",  &m_SnapScale,       0.001f, 10.0f,  "%.3f",   ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();

    if (ImGui::SmallButton("Match Move snap to grid")) m_SnapTranslation = gs.GridMinorSpacing;
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Set the gizmo Move increment equal to the grid cell size.");

    ImGui::Separator();
    ImGui::TextDisabled("SURFACE  (hold Shift while dragging to invert)");
    EditorUIPrimitives::Checkbox("Snap to surface under cursor", &m_SurfaceSnap);
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("While dragging the Move gizmo, drop the object where the cursor\nray meets another object's surface instead of following the axis.");
    ImGui::BeginDisabled(!m_SurfaceSnap);
    EditorUIPrimitives::Checkbox("Align to surface normal", &m_SurfaceSnapAlign);
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Also orient the object's up axis to the face it lands on.");
    ImGui::EndDisabled();

    ImGui::PopItemWidth();
}

void EditorLayer::DrawGizmosPopupBody() {
    EditorUIPrimitives::Checkbox("Gizmos", &m_GizmosMasterVisible);
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Master switch for every viewport gizmo and icon below.\nThe nav cube and selection outline are unaffected.");
    ImGui::Separator();

    ImGui::BeginDisabled(!m_GizmosMasterVisible);
    // #4 item 4 — "Show transform gizmo" everywhere this checkbox appears, matching Preferences >
    // Viewport (the View menu's "Transform Gizmo" MenuItem keeps menu-style title case — a MenuItem
    // reads as a noun, a Checkbox as a sentence, so that's a genuine format difference, not the
    // same three-different-labels problem this pass is fixing).
    EditorUIPrimitives::Checkbox("Show transform gizmo", &m_ShowGizmos);
    EditorUIPrimitives::Checkbox("Entity icons", &m_ShowEntityIcons);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("The billboard light / camera / empty markers.");
    bool lightGiz = EditorSettings::Get().ShowLightGizmos;
    if (EditorUIPrimitives::Checkbox("Light gizmos", &lightGiz)) {
        EditorSettings::Get().ShowLightGizmos = lightGiz;
        EditorSettings::Save();
    }
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Range spheres and spot cones drawn from each light.");
    bool colliderGiz = EditorSettings::Get().ShowColliders;
    if (EditorUIPrimitives::Checkbox("Colliders", &colliderGiz)) {
        EditorSettings::Get().ShowColliders = colliderGiz;
        EditorSettings::Save();
    }
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Green wireframe of every Collider's shape (#185), edit and Play mode.");
    ImGui::EndDisabled();

    bool physDbg = EditorSettings::Get().PhysicsDebugInput;
    if (EditorUIPrimitives::Checkbox("Physics debug input", &physDbg)) {
        EditorSettings::Get().PhysicsDebugInput = physDbg;
        EditorSettings::Save();
    }
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("While playing (Game view focused): right-click = gravity-gun grab,\nleft-click = launch / raycast-shove, G = shockwave at the Player (#185).");

    {
        unsigned& ddf = EditorSettings::Get().PhysicsDebugDrawFlags;
        bool anyDraw = ddf != 0u;
        if (EditorUIPrimitives::Checkbox("Physics debug draw", &anyDraw)) {
            // Toggle a sensible default bundle; the Physics panel has the per-channel toggles.
            ddf = anyDraw ? (PhysicsWorld::PDD_Contacts | PhysicsWorld::PDD_Raycasts |
                             PhysicsWorld::PDD_Velocity) : 0u;
            EditorSettings::Save();
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Contact sparks, raycasts and velocity arrows in the Scene viewport.\nWindow \xE2\x96\xB8 Physics has the individual channels + a slow-mo slider.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Panel\xE2\x80\xA6")) {
            EditorSettings::Get().ShowPhysicsPanel = true;
            EditorSettings::Save();
        }
    }

    ImGui::Separator();
    EditorUIPrimitives::Checkbox("Show grid", &m_ShowGrid); // independent of the master switch, like Unity's grid

    // --- Layers (#236 A1) -------------------------------------------------------------
    // Per-layer Scene-viewport visibility (eye) + pick-lock (padlock), plus rename for the
    // seven authorable slots. Visibility/lock masks are per-user (EditorSettings); the slot
    // names are project content (LayerRegistry / project/layers.json).
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Layers")) {
        EditorSettings& s = EditorSettings::Get();
        for (int i = 0; i < LayerRegistry::kCount; ++i) {
            if (!LayerRegistry::IsListed(i)) continue; // #150: 32 slots; show Default + named ones
            ImGui::PushID(i);
            const unsigned bit = 1u << i;

            bool vis = (s.LayerVisibleMask & bit) != 0;
            if (EditorUIPrimitives::Checkbox("##vis", &vis)) {
                if (vis) s.LayerVisibleMask |= bit; else s.LayerVisibleMask &= ~bit;
                EditorSettings::Save();
            }
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Show this layer in the Scene viewport.\nHidden layers stay in the scene, the Hierarchy and every save.");

            ImGui::SameLine();
            bool locked = (s.LayerPickLockMask & bit) != 0;
            if (EditorUIPrimitives::Checkbox("##lock", &locked)) {
                if (locked) s.LayerPickLockMask |= bit; else s.LayerPickLockMask &= ~bit;
                EditorSettings::Save();
            }
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Lock: objects on this layer can't be clicked in the viewport.\nHierarchy selection still works.");

            ImGui::SameLine();
            // Read-only label here — renaming lives in Project Settings ▸ Tags & Layers (#236 A4),
            // since the slot names are project content, not per-user viewport state.
            ImGui::TextUnformatted(LayerRegistry::DisplayName(i).c_str());
            ImGui::PopID();
        }
        ImGui::TextDisabled("Rename in Project Settings \xE2\x96\xB8 Tags & Layers");
    }
}

void EditorLayer::RequestCapture() {
    const auto& s = EditorSettings::Get();
    const int rp = std::clamp(s.CaptureResPreset, 0, (int)IM_ARRAYSIZE(kCaptureRes) - 1);
    m_CaptureReq = { /*pending*/ true, /*primed*/ false, s.CaptureMode, std::max(1, s.CaptureScale),
                     s.CaptureFormat, kCaptureRes[rp].w, kCaptureRes[rp].h };
}
