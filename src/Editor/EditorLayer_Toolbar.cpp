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
#include "EditorModuleAPI.h"          // EditorConsoleState — the Console panel lives in the module now
#include "HotReloadEditorModule.h"    // EditorModuleHost::ConsoleState()
#include "AssetImporterInspector.h"
#include "Profiler.h"
#include "ProjectPaths.h"
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
#include <functional>
#include <cfloat>

using namespace EditorInternal;


// Max side of the sampled patch, both here and in the fixed-size PBOs in AsyncLuminanceReadback.
static constexpr int kMaxLumPatch = 64;

float EditorLayer::SampleTextureLuminance(AsyncLuminanceReadback& rb, unsigned int colorTex, int texW, int texH,
                                          ImVec2 imgPos, ImVec2 imgSize, ImVec2 centerScreen, float boxPx) {
    // Async readback via a ping-ponged pair of PBOs (#178): rather than glReadPixels straight into
    // client memory (which stalls the GPU pipeline until the transfer finishes), this call kicks
    // off a non-blocking read into one PBO — glReadPixels returns immediately when a buffer is
    // bound to GL_PIXEL_PACK_BUFFER — and, in the same call, maps+consumes whatever the OTHER PBO
    // was loaded with by the PREVIOUS kickoff (one throttled ~100ms tick earlier). The result is
    // therefore a frame or two stale, which is invisible: it only steers a slowly-eased HUD tint.
    float result = -1.0f;

    // 1) Consume the other slot's pending result from the previous call, if any.
    const int readSlot = 1 - rb.Next;
    if (rb.Pending[readSlot] > 0) {
        if (void* ptr = glMapNamedBuffer(rb.Pbo[readSlot], GL_READ_ONLY)) {
            const unsigned char* px = (const unsigned char*)ptr;
            const int n = rb.Pending[readSlot];
            double sum = 0.0;
            for (int i = 0; i < n; ++i)
                sum += 0.2126 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.0722 * px[i * 4 + 2];
            result = (float)(sum / (n * 255.0)); // 0 = black behind the box, 1 = white
            glUnmapNamedBuffer(rb.Pbo[readSlot]);
        }
        rb.Pending[readSlot] = 0;
    }

    if (colorTex == 0 || texW < 1 || texH < 1 || imgSize.x < 1.0f || imgSize.y < 1.0f) return result;

    // screen box -> fraction of the displayed image -> texels (GL bottom-left origin). Handles
    // the Game view too, where the on-screen image is letterboxed and a different size than the
    // framebuffer it samples.
    const float sx = texW / imgSize.x, sy = texH / imgSize.y;
    int rw = (int)(boxPx * sx), rh = (int)(boxPx * sy);
    int rx = (int)((centerScreen.x - imgPos.x - boxPx * 0.5f) * sx);
    int ry = (int)((imgSize.y - ((centerScreen.y - imgPos.y) + boxPx * 0.5f)) * sy);
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > texW) rw = texW - rx;
    if (ry + rh > texH) rh = texH - ry;
    if (rw > kMaxLumPatch) { rx += (rw - kMaxLumPatch) / 2; rw = kMaxLumPatch; }
    if (rh > kMaxLumPatch) { ry += (rh - kMaxLumPatch) / 2; rh = kMaxLumPatch; }
    if (rx < 0 || ry < 0 || rw < 1 || rh < 1) return result;

    // 2) Kick off this call's read into the OTHER slot, for a future call to consume.
    const int writeSlot = readSlot;
    if (rb.Pbo[writeSlot] == 0) {
        glCreateBuffers(1, &rb.Pbo[writeSlot]);
        glNamedBufferStorage(rb.Pbo[writeSlot], kMaxLumPatch * kMaxLumPatch * 4, nullptr, GL_MAP_READ_BIT);
    }

    if (m_MarkSampleFbo == 0) glGenFramebuffers(1, &m_MarkSampleFbo);
    GLint prevReadFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_MarkSampleFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, rb.Pbo[writeSlot]);
    glReadPixels(rx, ry, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); // offset 0 into the bound PBO — async
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int)prevReadFbo);

    rb.Pending[writeSlot] = rw * rh;
    rb.Next = readSlot; // next call reads what we just wrote and writes into what we just read

    return result;
}

float EditorLayer::SampleSceneLuminance(AsyncLuminanceReadback& rb, ImVec2 centerScreen, float boxPx) {
    // The editor Scene framebuffer is sized 1:1 with the on-screen viewport, so texW/texH ARE
    // the viewport size.
    return SampleTextureLuminance(rb, m_SceneColorTexture, (int)m_ViewportSize.x, (int)m_ViewportSize.y,
                                  ImVec2(m_ViewportPos.x, m_ViewportPos.y),
                                  ImVec2(m_ViewportSize.x, m_ViewportSize.y), centerScreen, boxPx);
}
void EditorLayer::DrawPlayStopButton(bool playing, bool maximized) {
    int ww, wh;
    glfwGetWindowSize(m_Window, &ww, &wh);
    float w = (float)ww;

    // NoDocking: without it this is technically a dockable floating window, and Reset Layout's
    // DockBuilderRemoveNode + full dockspace rebuild (in Draw()) can knock an undocked-but-
    // dockable window out of the visible window list entirely.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;

    // Where the control lives: over the Scene viewport while editing, over the Game viewport while
    // playing. Both are the same flat "vector + label" treatment with the engine-mark contrast
    // readback (throttled ~10 Hz, eased per frame) so the glyph rides white-on-dark / dark-on-
    // light against whatever's rendered behind it.
    const bool overScene = !playing && m_ViewportSize.x > 1.0f && m_ViewportSize.y > 1.0f;
    const bool overGame  =  playing && m_GameViewImgSize.x > 1.0f && m_GameViewImgSize.y > 1.0f;

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

        ImGui::SameLine();
        const char* fsLabel = maximized ? ICON_FA_COMPRESS "  Restore" : ICON_FA_EXPAND "  Fullscreen";
        if (ImGui::Button(fsLabel)) m_MaximizeToggleRequested = true;
        if (ImGui::IsItemHovered()) {
            EditorUI::SetTooltip(maximized
                ? "Back to windowed play (editor panels return)"
                : "Maximize the Game view over the editor panels");
        }
    }

    ImGui::PopStyleColor(4);
    ImGui::End();
}

// The Console panel moved into TartarusEditor.dll (src/Editor/EditorModuleConsole.cpp) so its
// code hot-reloads while the editor stays open. The host still owns the log itself and the
// panel's persistent UI state (EditorModuleHost::ConsoleState()), which the module reads through
// the EditorModuleHostAPI callback table; main.cpp draws it via editorModule.Draw().

void EditorLayer::PushAdaptiveHudText(AsyncLuminanceReadback& rb, ImVec2 centerScreen, float boxPx, float dt,
                                     float& easedLum, float& targetLum, float& sampleAccum) {
    // Throttled (~10 Hz) async readback — see SampleTextureLuminance for the PBO ping-pong that
    // keeps this off the GPU pipeline's critical path.
    sampleAccum += dt;
    if (sampleAccum >= 0.1f) {
        sampleAccum = 0.0f;
        float lum = SampleSceneLuminance(rb, centerScreen, boxPx);
        if (lum >= 0.0f) {
            float t = (lum - 0.30f) / (0.62f - 0.30f);
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            targetLum = 1.0f - t * t * (3.0f - 2.0f * t); // 1 = white on dark, 0 = black on light
        }
    }
    float k = 1.0f - expf(-dt / 0.15f);
    easedLum += (targetLum - easedLum) * k;
    int v = (int)(easedLum * 255.0f + 0.5f);
    v = v < 0 ? 0 : (v > 255 ? 255 : v);
    ImGui::PushStyleColor(ImGuiCol_Text,         IM_COL32(v, v, v, 240));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(v, v, v, 150));
}

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
        m_GizmoOp == GizmoOp::Translate ? "Translate" :
        m_GizmoOp == GizmoOp::Rotate    ? "Rotate"    :
        m_GizmoOp == GizmoOp::Scale     ? "Scale"     : "Rect";

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

        // Active tool pinned to the right.
        char toolBuf[32]; snprintf(toolBuf, sizeof(toolBuf), "%s  %s", ICON_FA_UP_DOWN_LEFT_RIGHT, toolName);
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
void EditorLayer::DrawHistoryPanel(World& world, AssetLibrary& assets) {
    if (!m_ShowHistory) return;

    // Compact transparent HUD pinned to the viewport's bottom-right corner, matching the Stats
    // HUD at top-left (#149 follow-up): sized to its text, fully transparent, not dockable, not
    // persisted so the pin always wins. Unlike Stats it stays interactive — the rows are
    // click-to-jump.
    const float hpad = 12.0f * m_UIScale;
    const float statusBarH = ImGui::GetTextLineHeight() + 8.0f * m_UIScale;
    // Height ceiling: grow upward from the pin only until a clear line below the top-right nav
    // cluster, so a long history never climbs into that corner (or past it into the toolbar).
    // Derived from DrawViewGizmo's layout: margin 14 + rotate-ring Ø128*0.5 + spacing 8 + the
    // dolly/pan button box + the "Persp" label, all *m_UIScale, plus headroom (~220px @ 1x).
    // Beyond the ceiling the list scrolls internally instead.
    const float gizmoZoneH = 220.0f * m_UIScale;
    const float maxH = std::max(120.0f * m_UIScale,
                                m_ViewportSize.y - hpad - statusBarH - gizmoZoneH);

    // Rows + chrome, measured directly so the window is exactly as tall as its content up to maxH.
    const int rowCount = (int)m_UndoStack.size() + 1 /*Current*/ + (int)m_RedoStack.size();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float chromeH = ImGui::GetTextLineHeightWithSpacing()       // "History" line
                        + st.ItemSpacing.y + 2.0f                     // separator
                        + st.WindowPadding.y * 2.0f;
    const float desiredH = chromeH + std::max(rowCount, 1) * ImGui::GetTextLineHeightWithSpacing();
    const float winH = std::min(desiredH, maxH);

    ImGui::SetNextWindowPos(
        ImVec2(m_ViewportPos.x + m_ViewportSize.x - hpad,
               m_ViewportPos.y + m_ViewportSize.y - hpad - statusBarH),
        ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(0.0f, winH)); // x=0 → auto-fit width, height clamped to the ceiling
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;
    if (!ImGui::Begin(ICON_FA_CLOCK_ROTATE_LEFT "  History", &m_ShowHistory, flags)) { ImGui::End(); return; }

    // Contrast-adaptive tint (like the corner mark): sample the scene behind the HUD so the text
    // stays legible white-on-dark / dark-on-light with no plate behind it.
    const ImVec2 wpos = ImGui::GetWindowPos(), wsz = ImGui::GetWindowSize();
    PushAdaptiveHudText(m_HistoryHudReadback, ImVec2(wpos.x + wsz.x * 0.5f, wpos.y + wsz.y * 0.5f), 48.0f * m_UIScale,
                        ImGui::GetIO().DeltaTime, m_HistoryHudContrastLum, m_HistoryHudContrastTarget,
                        m_HistoryHudSampleAccum);

    ImGui::TextUnformatted(ICON_FA_CLOCK_ROTATE_LEFT "  History");
    // Explanation on the heading tooltip (#156) — no persistent "(?)" glyph.
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip(
        "Every recorded change, oldest to newest. Click any entry to jump\nstraight there - undoing or redoing everything in between automatically.");
    ImGui::Separator();

    if (m_UndoStack.empty() && m_RedoStack.empty()) {
        ImGui::TextDisabled("No changes yet.");
        ImGui::PopStyleColor(2); // adaptive Text + TextDisabled
        ImGui::End();
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

    ImGui::PopStyleColor(2); // adaptive Text + TextDisabled
    ImGui::End();
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

void EditorLayer::DrawTopToolbar(World& world, AssetLibrary& assets, Camera& editorCamera) {
    // Always pinned regardless of Lock Layout — pos/size are forced every frame by the caller.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
    // Tight vertical window padding so the icon row hugs the menu bar and the bottom edge — the
    // strip is only as tall as its two rows now (kToolbarHeight).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 3.0f));

    // Windows XP theme (2): the toolbar strip is the Luna taskbar blue with white menu/icon text
    // — the one spot the theme shows blue chrome (panels stay beige). ImGui paints one global
    // text colour, so the white is pushed for the whole strip and then locally reverted to the
    // body near-black inside each menu-dropdown / options popup (those render on the beige
    // PopupBg). See xpMenuTextPush/Pop below.
    const bool xpBar = (EditorSettings::Get().EditorTheme == 2);
    const ImVec4 xpBodyText(0.09f, 0.09f, 0.09f, 1.0f);
    const ImVec4 xpBodyTextDim(0.50f, 0.50f, 0.50f, 1.0f);
    auto xpMenuTextPush = [&]() {
        if (xpBar) { ImGui::PushStyleColor(ImGuiCol_Text, xpBodyText);
                     ImGui::PushStyleColor(ImGuiCol_TextDisabled, xpBodyTextDim); }
    };
    auto xpMenuTextPop = [&]() { if (xpBar) ImGui::PopStyleColor(2); };
    const int xpBarCols = xpBar ? 4 : 0;
    if (xpBar) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg,     ImVec4(0.161f, 0.396f, 0.878f, 1.0f)); // #295EE0 Luna blue
        ImGui::PushStyleColor(ImGuiCol_MenuBarBg,    ImVec4(0.133f, 0.337f, 0.804f, 1.0f)); // #2256CD
        ImGui::PushStyleColor(ImGuiCol_Text,         ImVec4(0.97f, 0.98f, 1.0f, 1.0f));     // white chrome text
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.74f, 0.81f, 0.95f, 1.0f));
    }

    if (!ImGui::Begin("##Toolbar", nullptr, flags)) {
        ImGui::End();
        if (xpBarCols) ImGui::PopStyleColor(xpBarCols);
        ImGui::PopStyleVar();
        return;
    }

    // Real dropdown menus for the stuff you reach for occasionally (import, add primitive,
    // scene save/load) — keeps the always-visible row below reserved for one-click toggles.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(ICON_FA_FOLDER_OPEN " File")) {
            xpMenuTextPush(); // beige dropdown -> revert the strip's white text to body near-black
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
            xpMenuTextPop();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_CUBES " Add")) {
            xpMenuTextPush();
            DrawAddEntityItems(world, assets, editorCamera);
            xpMenuTextPop();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_CAMERA " View")) {
            xpMenuTextPush();
            if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS_PLUS "  Frame Selected", "F", false, HasAnySelection())) {
                FocusOnSelection(world, editorCamera);
            }
            if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Frame All")) {
                FrameSceneBounds(world, editorCamera);
            }

            ImGui::SeparatorText("Shading");
            if (ImGui::MenuItem("  Shaded", nullptr, m_ShadingMode == ShadingMode::Shaded))
                m_ShadingMode = ShadingMode::Shaded;
            if (ImGui::MenuItem("  Wireframe", nullptr, m_ShadingMode == ShadingMode::Wireframe))
                m_ShadingMode = ShadingMode::Wireframe;
            if (ImGui::MenuItem("  Unlit", nullptr, m_ShadingMode == ShadingMode::Unlit))
                m_ShadingMode = ShadingMode::Unlit;

            // Grid lives only on the toolbar now (#148) — the menu keeps just the toggles that
            // have no toolbar home.
            ImGui::SeparatorText("Options");
            ImGui::MenuItem(ICON_FA_UP_DOWN_LEFT_RIGHT "  Transform Gizmo", nullptr, &m_ShowGizmos);
            ImGui::MenuItem(ICON_FA_CROSSHAIRS "  Frame on Select", nullptr, &m_FrameOnSelect);
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Move the camera to frame each object as you select it.");
            if (ImGui::MenuItem(ICON_FA_BORDER_ALL "  Orthographic", "5", editorCamera.Orthographic)) {
                ToggleOrthographic(world, editorCamera);
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
            xpMenuTextPop();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_TABLE_COLUMNS " Window")) {
            xpMenuTextPush();
            ImGui::MenuItem(ICON_FA_SITEMAP "  Scene Hierarchy", nullptr, &m_ShowHierarchy);
            ImGui::MenuItem(ICON_FA_SLIDERS "  Inspector", nullptr, &m_ShowInspector);
            ImGui::MenuItem(ICON_FA_FOLDER_TREE "  Asset Browser", nullptr, &m_ShowAssetBrowser);
            ImGui::Separator();
            // Console / Statistics / History / Light Gizmos have dedicated toolbar toggles (#148);
            // the engine mark lives in Preferences ▸ Viewport.
            if (ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Reset Layout")) {
                m_ResetLayoutRequested = true;
            }
            xpMenuTextPop();
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem(ICON_FA_GEAR " Preferences")) m_ShowPreferences = true;
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Editor settings, environment, shortcuts (Ctrl+,)");

        // Custom window controls, right-aligned — the OS title bar is gone (Win32 custom frame,
        // Window.cpp), so minimize / maximize-restore / close live here instead.
        DrawWindowControls();

        ImGui::EndMenuBar();
    }

    // A spaced group separator — a real 1px rule with breathing room on both sides so the toolbar
    // reads as distinct clusters (history · tools · grid/snap · view · panels · lock) instead of
    // one dense left-jammed run of identical squares (audit #66 / #147).
    auto divider = []() { EditorUI::VSeparator(1.5f); };

    // Toolbar tools/toggles use the shared flat treatment (#160): ActionButton(icon, tip, active).
    auto iconButton = [](const char* icon, const char* tooltip, bool active = false) {
        return ActionButton(icon, tooltip, active);
    };

    if (iconButton(ICON_FA_ROTATE_LEFT, "Undo (Ctrl+Z)")) Undo(world, assets);
    ImGui::SameLine();
    if (iconButton(ICON_FA_ROTATE_RIGHT, "Redo (Ctrl+Y)")) Redo(world, assets);

    divider();
    if (iconButton(ICON_FA_UP_DOWN_LEFT_RIGHT, "Translate (W)", m_GizmoOp == GizmoOp::Translate)) m_GizmoOp = GizmoOp::Translate;
    ImGui::SameLine();
    if (iconButton(ICON_FA_ARROWS_SPIN, "Rotate (E)", m_GizmoOp == GizmoOp::Rotate)) m_GizmoOp = GizmoOp::Rotate;
    ImGui::SameLine();
    if (iconButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Scale (R)", m_GizmoOp == GizmoOp::Scale)) m_GizmoOp = GizmoOp::Scale;
    ImGui::SameLine();
    if (iconButton(ICON_FA_VECTOR_SQUARE, "Rect — move + non-uniform scale via corner/edge handles (T)",
            m_GizmoOp == GizmoOp::Rect)) m_GizmoOp = GizmoOp::Rect;

    divider(); // transform tools | gizmo-space modifiers
    if (iconButton(m_GizmoLocalSpace ? ICON_FA_ARROWS_TO_DOT : ICON_FA_GLOBE,
            m_GizmoLocalSpace ? "Local space (click for World)" : "World space (click for Local)")) {
        m_GizmoLocalSpace = !m_GizmoLocalSpace;
    }
    ImGui::SameLine();
    if (iconButton(m_GizmoPivotCenter ? ICON_FA_CIRCLE_DOT : ICON_FA_CROSSHAIRS,
            m_GizmoPivotCenter
                ? "Center - gizmo sits on the bounding-box center (click for Pivot)"
                : "Pivot - gizmo sits on the object's own origin (click for Center)")) {
        m_GizmoPivotCenter = !m_GizmoPivotCenter;
    }

    divider();
    if (iconButton(ICON_FA_TABLE_CELLS, "Toggle Grid", m_ShowGrid)) m_ShowGrid = !m_ShowGrid;
    ImGui::SameLine();
    if (iconButton(ICON_FA_MAGNET, "Toggle Snap to Grid (hold Ctrl to invert while dragging)", m_GridSnapEnabled)) {
        m_GridSnapEnabled = !m_GridSnapEnabled;
    }
    ImGui::SameLine();
    {
        bool canSnap = CanSnapSelectionToGround(world);
        ImGui::BeginDisabled(!canSnap);
        if (iconButton(ICON_FA_DOWN_LONG, "Snap selection to ground")) SnapSelectionToGround(world);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    {
        auto& prefs = EditorSettings::Get();
        if (iconButton(ICON_FA_CIRCLE_NODES, "Toggle Light Gizmos (range/cone/aim in the viewport)",
                prefs.ShowLightGizmos)) {
            prefs.ShowLightGizmos = !prefs.ShowLightGizmos;
            EditorSettings::Save();
        }
    }

    ImGui::SameLine();
    // Scene-view shading, cycling Shaded -> Wireframe -> Unlit like a draw-mode dropdown — part of
    // the same "what the viewport shows" cluster as grid / snap / light gizmos.
    {
        const char* shadingIcon = ICON_FA_CIRCLE_HALF_STROKE;
        const char* shadingTip = "Shaded (click for Wireframe)";
        if (m_ShadingMode == ShadingMode::Wireframe) {
            shadingIcon = ICON_FA_BORDER_NONE;
            shadingTip = "Wireframe (click for Unlit)";
        } else if (m_ShadingMode == ShadingMode::Unlit) {
            shadingIcon = ICON_FA_SUN;
            shadingTip = "Unlit (click for Shaded)";
        }
        if (iconButton(shadingIcon, shadingTip, m_ShadingMode != ShadingMode::Shaded)) {
            m_ShadingMode = m_ShadingMode == ShadingMode::Shaded ? ShadingMode::Wireframe
                : (m_ShadingMode == ShadingMode::Wireframe ? ShadingMode::Unlit : ShadingMode::Shaded);
        }
    }

    ImGui::SameLine();
    // Orthographic / perspective toggle (shortcut 5) — was menu-only; it has a distinct on/off
    // state so it belongs on the strip beside the shading mode (#148).
    if (iconButton(ICON_FA_BORDER_ALL,
            editorCamera.Orthographic ? "Orthographic (click for Perspective) — 5"
                                      : "Perspective (click for Orthographic) — 5",
            editorCamera.Orthographic)) {
        ToggleOrthographic(world, editorCamera);
    }

    divider();
    if (iconButton(ICON_FA_CHART_SIMPLE, "Toggle Statistics", EditorSettings::Get().SceneShowStats)) {
        EditorSettings::Get().SceneShowStats = !EditorSettings::Get().SceneShowStats;
        EditorSettings::Save();
    }
    ImGui::SameLine();
    {
        // Console visibility lives host-side (not in the module) so this toggle keeps working
        // across a TartarusEditor.dll reload, and so a hidden Console stays hidden through one.
        bool& consoleVisible = EditorModuleHost::ConsoleState().Visible;
        if (iconButton(ICON_FA_TERMINAL, "Toggle Console", consoleVisible)) consoleVisible = !consoleVisible;
    }
    ImGui::SameLine();
    if (iconButton(ICON_FA_CLOCK_ROTATE_LEFT, "Toggle History", m_ShowHistory)) m_ShowHistory = !m_ShowHistory;

    divider();
    // Capture: click = shoot with the current settings; the caret opens the options popup.
    {
        auto& cs = EditorSettings::Get();
        static const char* kModes[] = { "Full editor window", "Scene viewport", "Scene viewport (clean)", "Game view" };
        const int cm = std::clamp(cs.CaptureMode, 0, 3);
        char tip[128];
        snprintf(tip, sizeof(tip), "Capture screenshot — %s%s (Print Screen)",
                 kModes[cm], (cm == 1 || cm == 2) && cs.CaptureScale > 1 ? " x2+" : "");
        if (iconButton(ICON_FA_CAMERA_RETRO, tip)) RequestCapture();
        ImGui::SameLine(0.0f, 1.0f);
        ImGui::PushID("##capOpts");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        if (ImGui::Button(ICON_FA_CARET_DOWN)) ImGui::OpenPopup("##CapturePopup");
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Capture options");
        if (ImGui::BeginPopup("##CapturePopup")) {
            xpMenuTextPush();
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
            xpMenuTextPop();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }


    // The toolbar's empty space is the window drag handle (the OS caption is gone). True only
    // when the cursor is over this strip and not over any widget / open menu / active drag —
    // Window.cpp's WM_NCHITTEST reads this to return HTCAPTION.
    m_TitleBarDragHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        !ImGui::IsAnyItemHovered() &&
        !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

    ImGui::End();
    if (xpBarCols) ImGui::PopStyleColor(xpBarCols); // XP toolbar blue + white chrome text
    ImGui::PopStyleVar(); // WindowPadding
}

// Minimize / maximize-restore / close, right-aligned in the toolbar's menu-bar row. Operates
// directly on the GLFW window; the visual frame removal happens in Window.cpp.
void EditorLayer::DrawWindowControls() {
    const float h = ImGui::GetFrameHeight();
    const float bw = std::floor(h * 1.6f);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float startX = ImGui::GetWindowWidth() - bw * 3.0f - st.WindowPadding.x;
    ImGui::SameLine(startX > 0.0f ? startX : 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, st.ItemSpacing.y));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.09f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.16f));

    ImGui::PushID("win_min");
    if (ImGui::Button(ICON_FA_MINUS, ImVec2(bw, 0.0f))) glfwIconifyWindow(m_Window);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Minimize");
    ImGui::PopID();
    ImGui::SameLine();

    const bool maxed = glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED) != 0;
    ImGui::PushID("win_max");
    if (ImGui::Button(maxed ? ICON_FA_COMPRESS : ICON_FA_EXPAND, ImVec2(bw, 0.0f))) {
        if (maxed) glfwRestoreWindow(m_Window); else glfwMaximizeWindow(m_Window);
    }
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip(maxed ? "Restore" : "Maximize");
    ImGui::PopID();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.15f, 0.18f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.12f, 0.15f, 1.0f));
    ImGui::PushID("win_close");
    if (ImGui::Button(ICON_FA_XMARK, ImVec2(bw, 0.0f))) glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Close");
    ImGui::PopID();
    ImGui::PopStyleColor(2);

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}
void EditorLayer::RequestCapture() {
    const auto& s = EditorSettings::Get();
    const int rp = std::clamp(s.CaptureResPreset, 0, (int)IM_ARRAYSIZE(kCaptureRes) - 1);
    m_CaptureReq = { /*pending*/ true, /*primed*/ false, s.CaptureMode, std::max(1, s.CaptureScale),
                     s.CaptureFormat, kCaptureRes[rp].w, kCaptureRes[rp].h };
}
