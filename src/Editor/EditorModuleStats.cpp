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
//
// Phase 6 item 5 — the audit's "unaligned text dump" rebuild: a 120-frame sparkline (host ring
// buffer, GetFrameTimeHistory, API v30) under the FPS line; every numeric readout moved into a
// right-aligned table column with thousands separators instead of hand-padded space-strings; the
// CPU/GPU profiler sample lists are collapsible sections, each row with a proportional bar behind
// its ms figure (relative to that list's own slowest sample) instead of being bare text.

#include "EditorModuleAPI.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace EditorModuleStats {

namespace {

constexpr int kMaxProfilerSamples = 32;
constexpr int kFrameTimeHistoryCap = 120;

// "12345" -> "12,345". Stats values here are always non-negative frame counters, so no sign
// handling is needed.
const char* FormatThousands(int value, char* buf, size_t bufSize) {
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%d", value);
    const int len = (int)std::strlen(digits);
    int o = 0;
    for (int i = 0; i < len && o < (int)bufSize - 1; ++i) {
        buf[o++] = digits[i];
        const int remaining = len - i - 1;
        if (remaining > 0 && remaining % 3 == 0 && o < (int)bufSize - 1) buf[o++] = ',';
    }
    buf[o] = '\0';
    return buf;
}

// One row of the stats table: label left, right-aligned comma-formatted value. `dim` greys both
// columns for a secondary reading (e.g. "Culled", "Inactive") without a whole separate style pass.
void StatRow(const char* label, int value, bool dim = false) {
    char buf[16];
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    const std::string formatted = FormatThousands(value, buf, sizeof(buf));
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - ImGui::CalcTextSize(formatted.c_str()).x);
    ImGui::TextUnformatted(formatted.c_str());
    if (dim) ImGui::PopStyleColor();
}

// A profiler sample row: name, a bar sized to its share of `maxMs` in the same list (so the
// slowest scope in CPU or GPU reads full-width, everything else scales against it), then the ms
// figure right-aligned. Deliberately its own thin bar via the draw list rather than
// ImGui::ProgressBar — a full-height progress bar per row reads heavier than this list needs.
void ProfilerRow(const EditorModuleProfilerSample& s, float maxMs) {
    ImGui::PushID(s.Name);
    ImGui::TextUnformatted(s.Name);
    // #184 — sized off the font (which is baked at the editor's UI scale) rather than fixed
    // pixels, so the name column and bar keep their proportions at 150%/200% scaling.
    const float em = ImGui::GetFontSize();
    ImGui::SameLine(em * 9.0f);

    const float barW = em * 8.5f, barH = ImGui::GetTextLineHeight() * 0.6f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float frac = maxMs > 0.0001f ? std::clamp(s.Milliseconds / maxMs, 0.0f, 1.0f) : 0.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float yOff = (ImGui::GetTextLineHeight() - barH) * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, p.y + yOff), ImVec2(p.x + barW, p.y + yOff + barH),
                       ImGui::GetColorU32(ImGuiCol_FrameBg));
    if (frac > 0.0f) {
        dl->AddRectFilled(ImVec2(p.x, p.y + yOff), ImVec2(p.x + barW * frac, p.y + yOff + barH),
                           ImGui::GetColorU32(ImGuiCol_PlotHistogram));
    }
    ImGui::Dummy(ImVec2(barW, ImGui::GetTextLineHeight()));
    ImGui::SameLine();
    ImGui::Text("%6.3f ms", s.Milliseconds);
    ImGui::PopID();
}

void DrawProfilerSection(const char* title, const EditorModuleProfilerSample* samples, int n) {
    if (n <= 0) return;
    if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) return;
    float maxMs = 0.0001f;
    for (int i = 0; i < n; ++i) maxMs = std::max(maxMs, samples[i].Milliseconds);
    for (int i = 0; i < n; ++i) ProfilerRow(samples[i], maxMs);
}

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

    const float smoothedMs = host.GetSmoothedFrameMs ? host.GetSmoothedFrameMs() : 0.0f;

    float frameHistory[kFrameTimeHistoryCap];
    const int frameHistoryN = host.GetFrameTimeHistory ? host.GetFrameTimeHistory(frameHistory, kFrameTimeHistoryCap) : 0;

    // Phase 1 item 5 — pushed before ImGui::Begin so every row below (and ImGui's own layout
    // pass) measures the same font it renders with: every label was hand-padded with spaces
    // ("Draw calls   %d") to fake column alignment against the proportional UI font, which only
    // lines up under a true monospace face.
    ImGui::PushFont(host.GetMonoFont ? host.GetMonoFont() : nullptr, 0.0f);

    bool open = shown;
    if (ImGui::Begin(ICON_FA_CHART_SIMPLE "  Statistics", &open)) {
        ImGui::Text("%.1f FPS  (%.2f ms)", smoothedMs > 0.0001f ? 1000.0f / smoothedMs : 0.0f, smoothedMs);

        if (frameHistoryN > 1) {
            // Scale to the worst frame in the window (floor 8ms so an all-smooth stretch doesn't
            // make the line look artificially spiky) rather than a fixed range — a project running
            // at 8ms and one hitching to 80ms both want the sparkline to actually use its height.
            float worst = 8.0f;
            for (int i = 0; i < frameHistoryN; ++i) worst = std::max(worst, frameHistory[i]);
            char overlay[32];
            std::snprintf(overlay, sizeof(overlay), "%.0f ms peak", worst);
            ImGui::PlotLines("##frametime", frameHistory, frameHistoryN, 0, overlay, 0.0f, worst,
                              ImVec2(0.0f, 40.0f));
        }
        ImGui::Separator();

        if (ImGui::BeginTable("##rendertbl", 2, ImGuiTableFlags_SizingStretchProp)) {
            StatRow("Draw calls", rs.DrawCalls);
            StatRow("Triangles", rs.Triangles);
            StatRow("Vertices", rs.Vertices);
            if (rs.Culled > 0) StatRow("Culled (outside view)", rs.Culled, true);
            ImGui::EndTable();
        }
        ImGui::Separator();
        if (ImGui::BeginTable("##entitytbl", 2, ImGuiTableFlags_SizingStretchProp)) {
            StatRow("Entities", entityCount);
            StatRow("Renderers", renderableCount);
            StatRow("Colliders", colliderCount);
            StatRow("Lights", lightCount);
            if (inactiveCount > 0) StatRow("Inactive", inactiveCount, true);
            ImGui::EndTable();
        }

        // Numbers from the frame that just finished (this frame's own "Scene Draw"/"ImGui
        // Render" scopes haven't run yet at this point) - same one-frame-behind convention the
        // smoothed FPS figure above already uses, so it's not called out as its own oddity.
        ImGui::Spacing();
        DrawProfilerSection("Profiler (CPU)", cpuSamples, profN);
        // GPU timings land several frames later than their CPU counterparts (pipelining), so
        // these are "most recently completed", not "this frame" - the CPU section above already
        // reads one frame behind; this one just trails a little further (#197).
        DrawProfilerSection("Profiler (GPU)", gpuSamples, profGpuN);

        ImGui::Separator();
        EditorModuleGLFrameStats gl;
        if (host.GetGLFrameStats) host.GetGLFrameStats(&gl);
        if (ImGui::BeginTable("##gltbl", 2, ImGuiTableFlags_SizingStretchProp)) {
            char label[48];
            std::snprintf(label, sizeof(label), "Shader binds (%d skipped)", gl.ProgramBindsSkipped);
            StatRow(label, gl.ProgramBinds);
            std::snprintf(label, sizeof(label), "Texture binds (%d skipped)", gl.TextureBindsSkipped);
            StatRow(label, gl.TextureBinds);
            std::snprintf(label, sizeof(label), "VAO binds (%d skipped)", gl.VaoBindsSkipped);
            StatRow(label, gl.VaoBinds);
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopFont();

    if (!open && host.SetShowStats) host.SetShowStats(false); // the title-bar X
}

} // namespace EditorModuleStats
