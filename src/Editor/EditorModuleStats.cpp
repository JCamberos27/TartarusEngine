// The editor's Statistics HUD, living inside TartarusEditor.dll so its code hot-reloads while the
// editor stays open with its scene loaded. Ported in behaviour from EditorLayer::DrawStatsPanel
// (EditorLayer_Toolbar.cpp): the transparent, click-through overlay pinned to the Scene
// viewport's top-left corner (#149) — FPS/ms, draw/tri/vert counts, per-category entity counts,
// the Profiler CPU/GPU sample lists, the GL-state-cache bind table — auto-sized to its content
// and height-capped so it never spills past the viewport status bar.
//
// What changed in the move: every number it shows is host-owned and now arrives through the
// EditorModuleHostAPI callback table (viewport rect, RenderStats, Profiler samples, GL frame
// stats, entity counts, the smoothed frame time). The contrast-adaptive text tint is owned here
// now (issue #229 decision): the host still drives the async PBO luminance readback — it owns the
// Scene framebuffer and the GL context — but the ~10 Hz throttle, the eased/target luminance pair
// and the ImGuiCol_Text / ImGuiCol_TextDisabled push all live module-side. That easing state is
// two floats in function-local statics; after a reload they re-converge within ~0.15 s, same as
// every other adaptive HUD in the editor does on its first few frames.

#include "EditorModuleAPI.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace EditorModuleStats {

namespace {

constexpr int kMaxProfilerSamples = 32;

// Contrast-adaptive HUD text — the module-side half of what EditorLayer::PushAdaptiveHudText did
// host-side. Pushes exactly two style colours (Text + TextDisabled); the caller pops them after
// its content. Same maths as the host's DrawEngineMark / DrawViewportStatusBar / History HUD.
void PushAdaptiveHudText(const EditorModuleHostAPI& host, float centerX, float centerY, float boxPx, float dt) {
    static float s_easedLum = 1.0f;
    static float s_targetLum = 1.0f;
    static float s_sampleAccum = 0.0f;

    s_sampleAccum += dt;
    if (s_sampleAccum >= 0.1f) {
        s_sampleAccum = 0.0f;
        const float lum = host.SampleViewportLuminance ? host.SampleViewportLuminance(centerX, centerY, boxPx) : -1.0f;
        if (lum >= 0.0f) {
            float t = (lum - 0.30f) / (0.62f - 0.30f);
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            s_targetLum = 1.0f - t * t * (3.0f - 2.0f * t); // 1 = white on dark, 0 = black on light
        }
    }
    const float k = 1.0f - std::exp(-dt / 0.15f);
    s_easedLum += (s_targetLum - s_easedLum) * k;
    int v = (int)(s_easedLum * 255.0f + 0.5f);
    v = v < 0 ? 0 : (v > 255 ? 255 : v);
    ImGui::PushStyleColor(ImGuiCol_Text,         IM_COL32(v, v, v, 240));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(v, v, v, 150));
}

} // namespace

void Draw(const EditorModuleHostAPI& host) {
    float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f, uiScale = 1.0f;
    bool enabled = false;
    if (host.GetViewportRect) host.GetViewportRect(&vx, &vy, &vw, &vh, &uiScale, &enabled);

    // Toggled off (View > Statistics), or no live Scene viewport to anchor to — draw nothing and
    // release the engine-mark hide flag so the corner monogram comes back.
    if (!enabled) {
        if (host.SetHideEngineMark) host.SetHideEngineMark(false);
        return;
    }

    EditorModuleRenderStats rs;
    if (host.GetRenderStats) host.GetRenderStats(&rs);

    int entityCount = 0, renderableCount = 0, colliderCount = 0, lightCount = 0, inactiveCount = 0;
    if (host.GetSceneEntityCounts)
        host.GetSceneEntityCounts(&entityCount, &renderableCount, &colliderCount, &lightCount, &inactiveCount);

    EditorModuleProfilerSample cpuSamples[kMaxProfilerSamples];
    EditorModuleProfilerSample gpuSamples[kMaxProfilerSamples];
    const int profN    = host.GetProfilerSamples ? host.GetProfilerSamples(cpuSamples, kMaxProfilerSamples, false) : 0;
    const int profGpuN = host.GetProfilerSamples ? host.GetProfilerSamples(gpuSamples, kMaxProfilerSamples, true)  : 0;
    const bool hasGpuSection = profGpuN > 0;

    const float smoothedMs = host.GetSmoothedFrameMs ? host.GetSmoothedFrameMs() : 0.0f;

    // A compact HUD pinned to the viewport's top-left corner (#149): fully transparent so it
    // doesn't box off the scene, click-through (NoInputs) so it never eats camera-look. Height
    // is measured from its content and capped so it never spills past the status bar into the
    // panels below (same treatment as the History HUD); the profiler tail clips rather than
    // overflowing. Not dockable, not persisted — the pin wins.
    const float pad = 12.0f * uiScale;
    const float statusBarH = ImGui::GetTextLineHeight() + 8.0f * uiScale;
    const ImGuiStyle& stStats = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const int rows = 1 /*fps*/ + 3 /*draw/tri/vert*/ + (rs.Culled > 0 ? 1 : 0)
                   + 4 /*ent/rend/coll/light*/ + (inactiveCount > 0 ? 1 : 0)
                   + (rs.LightBufferOverflowed ? 1 : 0) // #204
                   + (rs.ClusterSaturated ? 1 : 0)      // #204
                   + profN + (hasGpuSection ? profGpuN : 0) + 3 /*shader/texture/VAO binds*/;
    const float chromeH = lineH * (2.0f + (hasGpuSection ? 1.0f : 0.0f))  // "Statistics" + "Profiler (CPU)" [+ "Profiler (GPU)"]
                        + (4.0f + (hasGpuSection ? 1.0f : 0.0f)) * (stStats.ItemSpacing.y + 2.0f) // Separator() rules
                        + stStats.WindowPadding.y * 2.0f + 4.0f;
    const float desiredH = chromeH + rows * lineH;
    const float maxH = std::max(120.0f * uiScale, vh - statusBarH - 2.0f * pad);
    const float winH = std::min(desiredH, maxH);

    // If the box would reach down into the corner monogram, ask the host to hide the monogram
    // until it doesn't (host checks this at its DrawEngineMark call site).
    const float markTop = vh - statusBarH - 14.0f * uiScale - 54.0f * uiScale;
    if (host.SetHideEngineMark) host.SetHideEngineMark((pad + winH + 8.0f * uiScale) > markTop);

    ImGui::SetNextWindowPos(ImVec2(vx + pad, vy + pad), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(0.0f, winH)); // x=0 -> auto-fit width; height clamped
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##Stats", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground)) {
        // Contrast-adaptive tint (like the corner mark): sample the scene behind the HUD so the
        // text stays legible white-on-dark / dark-on-light with no plate behind it.
        const ImVec2 wpos = ImGui::GetWindowPos(), wsz = ImGui::GetWindowSize();
        PushAdaptiveHudText(host, wpos.x + wsz.x * 0.5f, wpos.y + wsz.y * 0.5f, 48.0f * uiScale,
                            ImGui::GetIO().DeltaTime);

        ImGui::TextUnformatted(ICON_FA_CHART_SIMPLE "  Statistics");
        ImGui::Separator();
        ImGui::Text("%.1f FPS  (%.2f ms)", smoothedMs > 0.0001f ? 1000.0f / smoothedMs : 0.0f, smoothedMs);
        ImGui::Separator();
        ImGui::Text("Draw calls   %d", rs.DrawCalls);
        ImGui::Text("Triangles    %d", rs.Triangles);
        ImGui::Text("Vertices     %d", rs.Vertices);
        if (rs.Culled > 0) ImGui::TextDisabled("Culled       %d (outside view)", rs.Culled);
        ImGui::Separator();
        ImGui::Text("Entities     %d", entityCount);
        ImGui::Text("Renderers    %d", renderableCount);
        ImGui::Text("Colliders    %d", colliderCount);
        ImGui::Text("Lights       %d", lightCount);
        if (inactiveCount > 0) ImGui::TextDisabled("Inactive     %d", inactiveCount);

        // Numbers from the frame that just finished (this frame's own "Scene Draw"/"ImGui
        // Render" scopes haven't run yet at this point) - same one-frame-behind convention the
        // smoothed FPS figure above already uses, so it's not called out as its own oddity.
        ImGui::SeparatorText("Profiler (CPU)");
        for (int i = 0; i < profN; ++i) {
            ImGui::Text("%-16s %.3f ms", cpuSamples[i].Name, cpuSamples[i].Milliseconds);
        }
        // GPU timings land several frames later than their CPU counterparts (pipelining), so
        // these are "most recently completed", not "this frame" - the CPU section above already
        // reads one frame behind; this one just trails a little further (#197).
        if (hasGpuSection) {
            ImGui::SeparatorText("Profiler (GPU)");
            for (int i = 0; i < profGpuN; ++i) {
                ImGui::Text("%-16s %.3f ms", gpuSamples[i].Name, gpuSamples[i].Milliseconds);
            }
        }
        ImGui::Separator();
        EditorModuleGLFrameStats gl;
        if (host.GetGLFrameStats) host.GetGLFrameStats(&gl);
        ImGui::Text("Shader binds   %d (%d skipped)", gl.ProgramBinds, gl.ProgramBindsSkipped);
        ImGui::Text("Texture binds  %d (%d skipped)", gl.TextureBinds, gl.TextureBindsSkipped);
        ImGui::Text("VAO binds      %d (%d skipped)", gl.VaoBinds, gl.VaoBindsSkipped);
        ImGui::PopStyleColor(2); // adaptive Text + TextDisabled
    }
    ImGui::End();
}

} // namespace EditorModuleStats
