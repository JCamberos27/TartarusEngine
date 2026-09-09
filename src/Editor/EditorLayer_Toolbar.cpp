// Editor chrome: the top toolbar and its menus, the window controls, the play/stop button,
// the viewport status bar and its adaptive-contrast HUD text, and the Console, Stats and
// History panels. Split out of EditorLayer.cpp for build time (#179).

#include "EditorLayer.h"
#include "EditorLayerInternal.h"
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


float EditorLayer::SampleSceneLuminance(AsyncLuminanceReadback& rb, ImVec2 centerScreen, float boxPx) {
    // The editor Scene framebuffer is sized 1:1 with the on-screen viewport, so texW/texH ARE
    // the viewport size.
    return SampleTextureLuminance(rb, m_SceneColorTexture, (int)m_ViewportSize.x, (int)m_ViewportSize.y,
                                  ImVec2(m_ViewportPos.x, m_ViewportPos.y),
                                  ImVec2(m_ViewportSize.x, m_ViewportSize.y), centerScreen, boxPx);
}
void EditorLayer::DrawPlayStopButton(bool playing, bool maximized, bool paused) {
    int ww, wh;
    glfwGetWindowSize(m_Window, &ww, &wh);
    float w = (float)ww;

    // NoDocking: without it this is technically a dockable floating window, and Reset Layout's
    // DockBuilderRemoveNode + full dockspace rebuild (in Draw()) can knock an undocked-but-
    // dockable window out of the visible window list entirely.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;

    // The transport (Play, or Stop/Pause/Step/Fullscreen) rides over whichever of the two shared-
    // dock-node viewports is actually on screen: the Game view when its tab is up, otherwise the
    // Scene view — so it never floats in a plate over the middle of the window, and it's on the
    // Scene view too when you tab there mid-play. Same flat "vector + label" treatment with the
    // engine-mark contrast readback (throttled ~10 Hz, eased) so the glyph stays legible.
    const bool overGame  = m_GameViewImgSize.x > 1.0f && m_GameViewImgSize.y > 1.0f;
    const bool overScene = !overGame && m_ViewportSize.x > 1.0f && m_ViewportSize.y > 1.0f;

    int fgV = 235; // near-white until the first sample lands

    if (overScene || overGame) {
        const ImVec2 imgPos  = overGame ? m_GameViewImgPos  : ImVec2(m_ViewportPos.x, m_ViewportPos.y);
        const ImVec2 imgSize = overGame ? m_GameViewImgSize : ImVec2(m_ViewportSize.x, m_ViewportSize.y);
        const float cx = imgPos.x + imgSize.x * 0.5f;
        const float cy = imgPos.y + 8.0f * m_UIScale;

        m_PlayBtnSampleAccum += ImGui::GetIO().DeltaTime;
        if (m_PlayBtnSampleAccum >= 0.1f) {
            m_PlayBtnSampleAccum = 0.0f;
            const ImVec2 probe(cx, cy + 14.0f * m_UIScale);
            const float box = 48.0f * m_UIScale;
            float lum = overGame
                ? SampleTextureLuminance(m_PlayBtnReadback, m_GameViewTex, m_GameViewTexW, m_GameViewTexH, imgPos, imgSize, probe, box)
                : SampleSceneLuminance(m_PlayBtnReadback, probe, box);
            if (lum >= 0.0f) {
                float t = (lum - 0.30f) / (0.62f - 0.30f);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                m_PlayBtnContrastTarget = 1.0f - t * t * (3.0f - 2.0f * t); // 1 = white on dark, 0 = black on light
            }
        }
        float k = 1.0f - expf(-ImGui::GetIO().DeltaTime / 0.15f);
        m_PlayBtnContrastLum += (m_PlayBtnContrastTarget - m_PlayBtnContrastLum) * k;
        fgV = (int)(m_PlayBtnContrastLum * 255.0f + 0.5f);
        fgV = fgV < 0 ? 0 : (fgV > 255 ? 255 : fgV);

        ImGui::SetNextWindowPos(ImVec2(cx, cy), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        flags |= ImGuiWindowFlags_NoBackground;
    } else {
        // No viewport rect to anchor to (maximized play before the first Game-panel frame, or
        // editor UI hidden) — float near the top of the window with a faint plate to stay legible.
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, 10.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.6f);
    }

    ImGui::Begin("##PlayStopButton", nullptr, flags);
    // Forces this to the front of the display order every frame so a dock rebuild elsewhere
    // (Reset Layout) can't bury it behind whatever the freshly recreated dock host window
    // ends up as. Skipped while a popup is open (e.g. the toolbar's Capture options) so the
    // button doesn't punch through a menu that legitimately overlays the viewport top-centre.
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    // Flat "vector" button: no body at rest, just the white (or dark) PLAY glyph + label; a faint
    // plate of the inverse grey on hover/press so it still reads as pressable.
    const float f  = fgV / 255.0f;
    const float iv = 1.0f - f;
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(f, f, f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(iv, iv, iv, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(iv, iv, iv, 0.28f));

    if (!playing) {
        if (ImGui::Button(ICON_FA_PLAY "  Play")) m_PlayStopRequested = true;
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Play the scene in the Game panel (F1)");
    } else {
        if (ImGui::Button(ICON_FA_STOP "  Stop")) m_PlayStopRequested = true;
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Stop and revert the scene (F1)");

        // Pause toggle + single-frame Step (#236). Pause reads as pressed-in while active; Step
        // is only meaningful (and only enabled) once paused.
        ImGui::SameLine();
        if (paused) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(iv, iv, iv, 0.28f));
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

    ImGui::PopStyleColor(4);
    ImGui::End();
}

// The Console panel moved into TartarusEditor.dll (src/Editor/EditorModuleConsole.cpp) so its
// code hot-reloads while the editor stays open. The host still owns the log itself and the
// panel's persistent UI state (EditorModuleHost::ConsoleState()), which the module reads through
// the EditorModuleHostAPI callback table; main.cpp draws it via editorModule.Draw().

// EditorLayer::PushAdaptiveHudText used to live here — the host-side eased contrast tint shared by
// the Stats and History HUDs. Both HUDs are in TartarusEditor.dll now (EditorModuleStats.cpp /
// EditorModuleHistory.cpp), each with its own module-side copy of the same ~10-line easing maths;
// the host keeps only SampleSceneLuminance + the per-HUD readback objects. Other adaptive text
// (the engine mark, the viewport status bar) inlines the same maths directly.

// The Statistics HUD itself moved into the reloadable editor module
// (src/Editor/EditorModuleStats.cpp). This is the host half of its one GL dependency: the
// module asks for a luminance sample under a screen box through EditorModuleHostAPI, and the
// host drives the async PBO readback (it owns the Scene framebuffer and the GL context) against
// the same ping-ponged pair the panel always used.
float EditorLayer::SampleStatsHudLuminance(float screenCenterX, float screenCenterY, float boxPx) {
    return SampleSceneLuminance(m_StatsHudReadback, ImVec2(screenCenterX, screenCenterY), boxPx);
}

void EditorLayer::DrawViewportStatusBar() {
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

    // No strip behind the readout any more — it's a bare line of text over the 3D view. To stay
    // legible it borrows the engine mark's contrast-adaptive trick: sample the scene luminance
    // behind it and steer the text white-on-dark / dark-on-light (throttled ~10 Hz, eased).
    {
        m_StatusBarSampleAccum += ImGui::GetIO().DeltaTime;
        if (m_StatusBarSampleAccum >= 0.1f) {
            m_StatusBarSampleAccum = 0.0f;
            float lum = SampleSceneLuminance(m_StatusBarReadback,
                ImVec2(m_ViewportPos.x + m_ViewportSize.x * 0.5f,
                       m_ViewportPos.y + m_ViewportSize.y - barH * 0.5f),
                barH);
            if (lum >= 0.0f) {
                float t = (lum - 0.30f) / (0.62f - 0.30f);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                m_StatusBarContrastTarget = 1.0f - t * t * (3.0f - 2.0f * t); // 1 = white on dark, 0 = black on light
            }
        }
        float k = 1.0f - expf(-ImGui::GetIO().DeltaTime / 0.15f);
        m_StatusBarContrastLum += (m_StatusBarContrastTarget - m_StatusBarContrastLum) * k;
    }
    int sbV = (int)(m_StatusBarContrastLum * 255.0f + 0.5f);
    sbV = sbV < 0 ? 0 : (sbV > 255 ? 255 : sbV);

    ImGui::SetNextWindowPos(ImVec2(m_ViewportPos.x, m_ViewportPos.y + m_ViewportSize.y - barH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(m_ViewportSize.x, barH), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * m_UIScale, 3.0f * m_UIScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text,         IM_COL32(sbV, sbV, sbV, 240));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(sbV, sbV, sbV, 150));
    if (ImGui::Begin("##ViewportStatusBar", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs)) {

        auto sep = [&]() { ImGui::SameLine(0, 6); ImGui::TextDisabled("\xc2\xb7"); ImGui::SameLine(0, 6); };

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
        ImGui::Text("%.0f FPS", fps);
        sep(); ImGui::TextDisabled("%.1f ms", m_SmoothedFrameMs);
        sep(); ImGui::Text("%s draws", compact(m_RenderStats.DrawCalls).c_str());
        sep(); ImGui::TextDisabled("%s tris", compact(m_RenderStats.Triangles).c_str());
        sep();
        if (selCount == 0) ImGui::TextDisabled("no selection");
        else               ImGui::Text("%d selected", selCount);

        if (m_LockViewToSelection) { sep(); ImGui::Text("%s follow", ICON_FA_LOCK); } // Shift+F (#236 E)

        // Active tool pinned to the right.
        char toolBuf[32]; snprintf(toolBuf, sizeof(toolBuf), "%s  %s",
            m_HandTool ? ICON_FA_HAND : ICON_FA_UP_DOWN_LEFT_RIGHT, toolName);
        float tw = ImGui::CalcTextSize(toolBuf).x;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - tw);
        ImGui::TextUnformatted(toolBuf);
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
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

// The host half of the History HUD's one GL dependency: the module asks for a luminance sample
// under a screen box, the host drives the async PBO readback against its own ping-ponged pair
// (it owns the Scene framebuffer + GL context). Same shape as SampleStatsHudLuminance.
float EditorLayer::SampleHistoryHudLuminance(float screenCenterX, float screenCenterY, float boxPx) {
    return SampleSceneLuminance(m_HistoryHudReadback, ImVec2(screenCenterX, screenCenterY), boxPx);
}

// The click-to-jump rows, drawn host-side into the module's window between its heading Separator
// and its End. The undo/redo stacks, World& and AssetLibrary& never cross the DLL boundary.
// Pop-balanced on its own pushes; the module owns the two adaptive-tint colours around this call.
void EditorLayer::DrawHistoryListBody(World& world, AssetLibrary& assets) {
    if (m_UndoStack.empty() && m_RedoStack.empty()) {
        ImGui::TextDisabled("No changes yet.");
        return;
    }

    for (size_t k = 0; k < m_UndoStack.size(); ++k) {
        ImGui::PushID((int)k);
        if (ImGui::Selectable(m_UndoStack[k].Label.c_str())) {
            JumpToUndoEntry(world, assets, k);
        }
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
            if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save", "Ctrl+S")) {
                DoSave(world, assets);
            }
            if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save As...", "Ctrl+Shift+S")) {
                DoSaveAs(world, assets);
            }
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
                    std::string path = FileDialog::OpenFile(
                        "3D Models\0*.fbx;*.obj;*.gltf;*.glb\0All Files\0*.*\0", m_Window);
                    // Imports into the library only — doesn't place an instance in the scene.
                    // Drag it from the Asset Browser into the Viewport to place one.
                    if (!path.empty()) assets.LoadModel(path);
                }
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("FBX / OBJ / glTF - added to the asset library");
                if (ImGui::MenuItem(ICON_FA_IMAGE "  Texture...")) {
                    std::string path = FileDialog::OpenFile(
                        "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0", m_Window);
                    if (!path.empty()) assets.LoadTexture(path);
                }
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("PNG / JPG / TGA / BMP");
                if (ImGui::MenuItem(ICON_FA_MUSIC "  Sound...")) {
                    std::string path = FileDialog::OpenFile(
                        "Audio\0*.wav;*.mp3;*.ogg;*.flac\0All Files\0*.*\0", m_Window);
                    if (!path.empty() && AudioEngine::Load(path)) assets.RegisterSound(path);
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

            ImGui::SeparatorText("Draw mode");
            {
                static const char* kDrawModes[] = { "Shaded", "Wireframe", "Unlit",
                                                    "Normals", "Shadow Cascades", "Mip / Texel Density" };
                for (int i = 0; i < IM_ARRAYSIZE(kDrawModes); ++i) {
                    if (ImGui::MenuItem(kDrawModes[i], nullptr, (int)m_ShadingMode == i))
                        m_ShadingMode = (ShadingMode)i;
                }
                if (ImGui::IsItemHovered())
                    EditorUI::SetTooltip("Mip: albedo texture LOD as a blue\xE2\x86\x92red ramp (needs a textured object).");
            }

            // Grid lives only on the toolbar now (#148) — the menu keeps just the toggles that
            // have no toolbar home.
            ImGui::SeparatorText("Options");
            ImGui::MenuItem(ICON_FA_UP_DOWN_LEFT_RIGHT "  Transform Gizmo", nullptr, &m_ShowGizmos);
            if (ImGui::MenuItem(ICON_FA_RULER "  Measure Tool", nullptr, m_MeasureTool)) {
                m_MeasureTool = !m_MeasureTool;
                m_MeasureCount = 0;
            }
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Click two points in the viewport to measure the distance. Right-click clears.");
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
            if (ImGui::MenuItem(ICON_FA_BORDER_ALL "  Orthographic", "5", editorCamera.Orthographic)) {
                ToggleOrthographic(world, editorCamera);
            }

            ImGui::SeparatorText("Camera");
            {
                auto& cs = EditorSettings::Get();
                ImGui::PushItemWidth(190.0f * m_UIScale);
                if (EditorUI::SliderFloat("FOV", &cs.SceneCameraFov, 30.0f, 110.0f, "%.0f\xC2\xB0")) {
                    editorCamera.Fov = cs.SceneCameraFov;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
                if (EditorUI::SliderFloat("Fly speed", &cs.SceneCameraFlySpeed, 0.5f, 60.0f, "%.1f")) {}
                if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
                if (ImGui::IsItemHovered())
                    EditorUI::SetTooltip("Editor fly-camera speed (Shift = ×3). Also: scroll while holding right-drag.");

                if (EditorUI::SliderFloat("Near", &cs.SceneCameraNear, 0.001f, 10.0f, "%.3f",
                                          ImGuiSliderFlags_Logarithmic)) {
                    cs.SceneCameraNear = std::min(cs.SceneCameraNear, cs.SceneCameraFar - 0.01f);
                    editorCamera.NearPlane = cs.SceneCameraNear;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
                if (EditorUI::SliderFloat("Far", &cs.SceneCameraFar, 1.0f, 10000.0f, "%.0f",
                                          ImGuiSliderFlags_Logarithmic)) {
                    cs.SceneCameraFar = std::max(cs.SceneCameraFar, cs.SceneCameraNear + 0.01f);
                    editorCamera.FarPlane = cs.SceneCameraFar;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
                ImGui::PopItemWidth();
            }

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
            ImGui::MenuItem(ICON_FA_SITEMAP "  Scene Hierarchy", nullptr, &m_ShowHierarchy);
            ImGui::MenuItem(ICON_FA_SLIDERS "  Inspector", nullptr, &m_ShowInspector);
            ImGui::MenuItem(ICON_FA_FOLDER_TREE "  Asset Browser", nullptr, &m_ShowAssetBrowser);
            ImGui::MenuItem(ICON_FA_LIGHTBULB "  Lighting", nullptr, &m_ShowLighting);
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Environment (sky / ambient), post-processing (exposure / tone map) and shadow settings in one place.");
            ImGui::Separator();
            ImGui::MenuItem(ICON_FA_GEARS "  Project Settings", nullptr, &m_ShowProjectSettings);
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Project-scoped settings (physics, tags, layer names) - saved with the project, not your editor prefs.");
            if (ImGui::MenuItem(ICON_FA_CUBES "  Physics Debug", nullptr, &EditorSettings::Get().ShowPhysicsPanel))
                EditorSettings::Save();
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Live sim stats, visual debug-draw channels, slow-mo / break-on, body + joint + event inspectors (#185).");
            if (ImGui::MenuItem(ICON_FA_GAUGE_HIGH "  Physics HUD overlay", nullptr, &EditorSettings::Get().PhysicsHudOverlay))
                EditorSettings::Save();
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Corner text overlay on the Scene viewport while playing.");
            ImGui::Separator();
            // Console / Statistics / History / Light Gizmos have dedicated toolbar toggles (#148);
            // the engine mark lives in Preferences ▸ Viewport.
            if (ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Reset Layout")) {
                m_ResetLayoutRequested = true;
            }

            // Layout presets (#236 R2) — named ImGui-ini snapshots in project/layouts/.
            if (ImGui::BeginMenu(ICON_FA_TABLE_COLUMNS "  Layout Presets")) {
                const std::vector<std::string> presets = LayoutPresetNames();
                if (presets.empty()) ImGui::TextDisabled("(none saved yet)");
                for (const std::string& name : presets) {
                    if (ImGui::MenuItem(name.c_str())) RequestLoadLayoutPreset(name);
                    ImGui::SameLine();
                    ImGui::PushID(name.c_str());
                    if (ImGui::SmallButton(ICON_FA_XMARK)) DeleteLayoutPreset(name);
                    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Delete this preset");
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
            if (ImGui::Checkbox("Flash", &cs.CaptureFlash)) EditorSettings::Save();
            ImGui::SameLine();
            if (ImGui::Checkbox("Sound", &cs.CaptureSound)) EditorSettings::Save();
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
    bool showGrid = m_ShowGrid;
    if (ImGui::Checkbox("Visible", &showGrid)) m_ShowGrid = showGrid;
    EditorUI::SliderFloat("Cell size", &gs.GridMinorSpacing, 0.05f, 50.0f, "%.2f m",
                          ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("World units between minor grid lines.\nAlso the step used when snapping a dropped object to the ground grid.");
    if (EditorUI::SliderInt("Major every", &gs.GridMajorEvery, 2, 100, "%d cells"))
        EditorSettings::Save();

    ImGui::Separator();
    ImGui::TextDisabled("SNAP  (hold Ctrl while dragging to invert)");
    bool snap = m_GridSnapEnabled;
    if (ImGui::Checkbox("Snap enabled", &snap)) m_GridSnapEnabled = snap;
    ImGui::BeginDisabled(!m_GridSnapEnabled);
    EditorUI::SliderFloat("Move",   &m_SnapTranslation, 0.001f, 100.0f, "%.3f m", ImGuiSliderFlags_Logarithmic);
    EditorUI::SliderFloat("Rotate", &m_SnapRotationDeg, 0.1f,   180.0f, "%.1f deg");
    EditorUI::SliderFloat("Scale",  &m_SnapScale,       0.001f, 10.0f,  "%.3f",   ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();

    if (ImGui::SmallButton("Match Move snap to grid")) m_SnapTranslation = gs.GridMinorSpacing;
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Set the gizmo Move increment equal to the grid cell size.");

    ImGui::Separator();
    ImGui::TextDisabled("SURFACE  (hold Shift while dragging to invert)");
    ImGui::Checkbox("Snap to surface under cursor", &m_SurfaceSnap);
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("While dragging the Move gizmo, drop the object where the cursor\nray meets another object's surface instead of following the axis.");
    ImGui::BeginDisabled(!m_SurfaceSnap);
    ImGui::Checkbox("Align to surface normal", &m_SurfaceSnapAlign);
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Also orient the object's up axis to the face it lands on.");
    ImGui::EndDisabled();

    ImGui::PopItemWidth();
}

void EditorLayer::DrawGizmosPopupBody() {
    ImGui::Checkbox("Gizmos", &m_GizmosMasterVisible);
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Master switch for every viewport gizmo and icon below.\nThe nav cube and selection outline are unaffected.");
    ImGui::Separator();

    ImGui::BeginDisabled(!m_GizmosMasterVisible);
    ImGui::Checkbox("Transform gizmo", &m_ShowGizmos);
    ImGui::Checkbox("Entity icons", &m_ShowEntityIcons);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("The billboard light / camera / empty markers.");
    bool lightGiz = EditorSettings::Get().ShowLightGizmos;
    if (ImGui::Checkbox("Light gizmos", &lightGiz)) {
        EditorSettings::Get().ShowLightGizmos = lightGiz;
        EditorSettings::Save();
    }
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Range spheres and spot cones drawn from each light.");
    bool colliderGiz = EditorSettings::Get().ShowColliders;
    if (ImGui::Checkbox("Colliders", &colliderGiz)) {
        EditorSettings::Get().ShowColliders = colliderGiz;
        EditorSettings::Save();
    }
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Green wireframe of every Collider's shape (#185), edit and Play mode.");
    ImGui::EndDisabled();

    bool physDbg = EditorSettings::Get().PhysicsDebugInput;
    if (ImGui::Checkbox("Physics debug input", &physDbg)) {
        EditorSettings::Get().PhysicsDebugInput = physDbg;
        EditorSettings::Save();
    }
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("While playing (Game view focused): right-click = gravity-gun grab,\nleft-click = launch / raycast-shove, G = shockwave at the Player (#185).");

    {
        unsigned& ddf = EditorSettings::Get().PhysicsDebugDrawFlags;
        bool anyDraw = ddf != 0u;
        if (ImGui::Checkbox("Physics debug draw", &anyDraw)) {
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
    ImGui::Checkbox("Grid", &m_ShowGrid); // independent of the master switch, like Unity's grid

    // --- Layers (#236 A1) -------------------------------------------------------------
    // Per-layer Scene-viewport visibility (eye) + pick-lock (padlock), plus rename for the
    // seven authorable slots. Visibility/lock masks are per-user (EditorSettings); the slot
    // names are project content (LayerRegistry / project/layers.json).
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Layers")) {
        EditorSettings& s = EditorSettings::Get();
        for (int i = 0; i < LayerRegistry::kCount; ++i) {
            ImGui::PushID(i);
            const unsigned bit = 1u << i;

            bool vis = (s.LayerVisibleMask & bit) != 0;
            if (ImGui::Checkbox("##vis", &vis)) {
                if (vis) s.LayerVisibleMask |= bit; else s.LayerVisibleMask &= ~bit;
                EditorSettings::Save();
            }
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Show this layer in the Scene viewport.\nHidden layers stay in the scene, the Hierarchy and every save.");

            ImGui::SameLine();
            bool locked = (s.LayerPickLockMask & bit) != 0;
            if (ImGui::Checkbox("##lock", &locked)) {
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
