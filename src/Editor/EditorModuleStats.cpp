// The editor's Statistics panel, living inside TartarusEditor.dll so its code hot-reloads while
// the editor stays open with its scene loaded. FPS/ms, draw/tri/vert counts, per-category entity
// counts, the Profiler CPU/GPU sample lists, the GL-state-cache bind table.
//
// Phase 3 item 8 (audit #5) — this used to be a transparent, click-through overlay pinned to the
// Scene viewport's top-left corner (#149), forced there every frame (NoDocking/NoMove/NoInputs)
// so it could never be dragged, resized, or interacted with. It's a real dockable panel now, same
// as Hierarchy/Inspector/Console — draggable into any dock node, its position persisted in
// imgui.ini like theirs, with a title bar and a working close box. The glanceable, always-visible
// role the old corner HUD served is now the viewport status bar's FPS/ms cluster (Phase 3 item
// 5, EditorLayer_Toolbar.cpp's DrawViewportStatusBar) — already a collapsed FPS/frame-ms reading
// that opens this exact panel on click, so this pass didn't need to invent a second one.
//
// What changed in the move: every number it shows is host-owned and now arrives through the
// EditorModuleHostAPI callback table (RenderStats, Profiler samples, GL frame stats, entity
// counts, the smoothed frame time). It used to steer its text between white-on-dark and black-on-
// light with an async GPU luminance readback the host drove; Defect #54 (Phase 1) replaced that
// with a fixed opaque plate behind fixed light text for the corner-HUD era. As an ordinary docked
// panel now, it just follows the theme's normal WindowBg/Text like every other panel — no more
// bespoke plate.

#include "EditorModuleAPI.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace EditorModuleStats {

namespace {

constexpr int kMaxProfilerSamples = 32;

} // namespace

void Draw(const EditorModuleHostAPI& host) {
    const bool shown = host.GetShowStats && host.GetShowStats();
    if (!shown) return;

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

    // Phase 1 item 5 — pushed before ImGui::Begin so every row below (and ImGui's own layout
    // pass) measures the same font it renders with: every label was hand-padded with spaces
    // ("Draw calls   %d") to fake column alignment against the proportional UI font, which only
    // lines up under a true monospace face.
    ImGui::PushFont(host.GetMonoFont ? host.GetMonoFont() : nullptr, 0.0f);

    bool open = shown;
    if (ImGui::Begin(ICON_FA_CHART_SIMPLE "  Statistics", &open)) {
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
    }
    ImGui::End();
    ImGui::PopFont();

    if (!open && host.SetShowStats) host.SetShowStats(false); // the title-bar X
}

} // namespace EditorModuleStats
