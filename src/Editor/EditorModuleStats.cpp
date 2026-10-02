// The editor's Statistics panel, living inside TartarusEditor.dll so its code hot-reloads while
// the editor stays open with its scene loaded. FPS/ms, draw/tri/vert counts, per-category entity
// counts, the Profiler CPU/GPU sample lists, the GL-state-cache bind table.
//
// Phase 3 item 8 (audit #5) — this used to be a transparent, click-through overlay pinned to the
// Scene viewport's top-left corner (#149), forced there every frame (NoDocking/NoMove/NoInputs)
// so it could never be dragged, resized, or interacted with. It's a real dockable panel now, same
// as Hierarchy/Inspector/Console — draggable into any dock node, its position persisted in
// imgui.ini like theirs, with a title bar and a working close box. The glanceable, always-visible
// role the old corner HUD served is now the status bar's fps / ms segment (Phase 3 item
// 5, EditorLayer_Toolbar.cpp's DrawStatusBar) — already a collapsed FPS/frame-ms reading
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
#include "EditorPanels.h"
#include "EditorTheme.h"
#include "EditorUIPrimitives.h"

#include <imgui.h>
#include <imgui_internal.h> // RenderTextEllipsis
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

// One row of a stats table: label left in the secondary colour, the value right-aligned in the
// monospace face. `dim` greys both for a secondary reading (e.g. "Culled", "Inactive").
void StatRow(const char* label, int value, bool dim = false) {
    char buf[16];
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(dim ? EditorTheme::Dim : EditorTheme::Secondary, "%s", label);
    ImGui::TableNextColumn();
    FormatThousands(value, buf, sizeof(buf));
    EditorTheme::PushMono();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - ImGui::CalcTextSize(buf).x);
    ImGui::TextColored(dim ? EditorTheme::Dim : EditorTheme::Text, "%s", buf);
    EditorTheme::PopFont();
}

// A profiler sample row: the scope's name (cut with an ellipsis to its column), a thin bar sized
// to its share of the slowest sample in the same list, and the milliseconds right-aligned.
void ProfilerRow(const EditorModuleProfilerSample& s, float maxMs) {
    ImGui::PushID(s.Name);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float nameW = ImGui::GetContentRegionAvail().x;
    const ImVec2 ts = ImGui::CalcTextSize(s.Name);
    ImGui::Dummy(ImVec2(nameW, ts.y));
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Secondary);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + nameW, p0.y + ts.y), p0.x + nameW, s.Name, nullptr, &ts);
    ImGui::PopStyleColor();
    if (ts.x > nameW && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.Name);

    ImGui::TableNextColumn();
    const float barW = ImGui::GetContentRegionAvail().x, barH = EditorTheme::Px(4.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float frac = maxMs > 0.0001f ? std::clamp(s.Milliseconds / maxMs, 0.0f, 1.0f) : 0.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float yOff = std::floor((ImGui::GetTextLineHeight() - barH) * 0.5f);
    dl->AddRectFilled(ImVec2(p.x, p.y + yOff), ImVec2(p.x + barW, p.y + yOff + barH), EditorTheme::U32(EditorTheme::Field), barH * 0.5f);
    if (frac > 0.0f)
        dl->AddRectFilled(ImVec2(p.x, p.y + yOff), ImVec2(p.x + std::max(barH, barW * frac), p.y + yOff + barH),
                          EditorTheme::U32(EditorTheme::Accent), barH * 0.5f);
    ImGui::Dummy(ImVec2(barW, ImGui::GetTextLineHeight()));

    ImGui::TableNextColumn();
    char ms[32];
    std::snprintf(ms, sizeof(ms), "%.2f", s.Milliseconds);
    EditorTheme::PushMono();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(ms).x);
    ImGui::TextColored(EditorTheme::Text, "%s", ms);
    EditorTheme::PopFont();
    ImGui::PopID();
}

void DrawProfilerSection(const char* title, const EditorModuleProfilerSample* samples, int n) {
    if (n <= 0) return;
    EditorUIPrimitives::SectionHeader(title);
    float maxMs = 0.0001f;
    for (int i = 0; i < n; ++i) maxMs = std::max(maxMs, samples[i].Milliseconds);
    if (ImGui::BeginTable(title, 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.2f);
        for (int i = 0; i < n; ++i) ProfilerRow(samples[i], maxMs);
        ImGui::EndTable();
    }
}

// The frame-time history as a line over a field-coloured plot, with the 60 and 30 fps budgets as
// faint guides, scaled to the worst frame shown (at least 20 ms so a smooth run doesn't look spiky).
void FrameTimeGraph(const float* ms, int n) {
    const float h = EditorTheme::Px(44.0f);
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), EditorTheme::U32(EditorTheme::Field), EditorTheme::Px(3.0f));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), EditorTheme::U32(EditorTheme::Hairline), EditorTheme::Px(3.0f));
    if (n < 2) return;
    float worst = 20.0f;
    for (int i = 0; i < n; ++i) worst = std::max(worst, ms[i]);
    const float top = worst * 1.1f;
    auto yOf = [&](float v) { return p.y + h - 2.0f - (h - 4.0f) * std::clamp(v / top, 0.0f, 1.0f); };
    for (float guide : {16.667f, 33.333f}) {
        if (guide >= top) continue;
        const float y = std::floor(yOf(guide)) + 0.5f;
        dl->AddLine(ImVec2(p.x + 1.0f, y), ImVec2(p.x + w - 1.0f, y), EditorTheme::U32(EditorTheme::Hairline));
    }
    ImVec2 pts[kFrameTimeHistoryCap];
    for (int i = 0; i < n; ++i) pts[i] = ImVec2(p.x + 2.0f + (w - 4.0f) * (float)i / (float)(n - 1), yOf(ms[i]));
    dl->AddPolyline(pts, n, EditorTheme::U32(EditorTheme::Accent), ImDrawFlags_None, std::max(1.0f, EditorTheme::Px(1.25f)));
    char peak[32];
    std::snprintf(peak, sizeof(peak), "peak %.0f ms", worst);
    EditorTheme::PushMonoSmall();
    const ImVec2 ps = ImGui::CalcTextSize(peak);
    dl->AddText(ImVec2(p.x + w - ps.x - EditorTheme::Px(6.0f), p.y + EditorTheme::Px(3.0f)), EditorTheme::U32(EditorTheme::Dim), peak);
    EditorTheme::PopFont();
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

    bool open = shown;
    if (ImGui::Begin(EditorPanels::Statistics, &open)) {
        // The headline: frames per second large, the frame time beside it.
        {
            char fps[32], ms[32];
            std::snprintf(fps, sizeof(fps), "%.0f", smoothedMs > 0.0001f ? 1000.0f / smoothedMs : 0.0f);
            std::snprintf(ms, sizeof(ms), "%.2f ms", smoothedMs);
            ImGui::PushFont(EditorTheme::MonoFont(), EditorTheme::Px(26.0f));
            ImGui::TextColored(EditorTheme::Text, "%s", fps);
            ImGui::PopFont();
            ImGui::SameLine(0.0f, EditorTheme::Px(6.0f));
            EditorTheme::PushHeading();
            const float base = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(base + EditorTheme::Px(12.0f));
            EditorTheme::TrackedText("FPS", EditorTheme::Dim, EditorTheme::HeadingTracking());
            EditorTheme::PopFont();
            ImGui::SameLine();
            EditorTheme::PushMono();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(ms).x);
            ImGui::SetCursorPosY(base + EditorTheme::Px(9.0f));
            ImGui::TextColored(EditorTheme::Secondary, "%s", ms);
            EditorTheme::PopFont();
        }
        FrameTimeGraph(frameHistory, frameHistoryN);

        EditorUIPrimitives::SectionHeader("RENDERING");
        if (ImGui::BeginTable("##rendertbl", 2, ImGuiTableFlags_SizingStretchProp)) {
            StatRow("Draw calls", rs.DrawCalls);
            StatRow("Triangles", rs.Triangles);
            StatRow("Vertices", rs.Vertices);
            if (rs.Culled > 0) StatRow("Culled (outside view)", rs.Culled, true);
            ImGui::EndTable();
        }
        EditorUIPrimitives::SectionHeader("SCENE");
        if (ImGui::BeginTable("##entitytbl", 2, ImGuiTableFlags_SizingStretchProp)) {
            StatRow("Entities", entityCount);
            StatRow("Renderers", renderableCount);
            StatRow("Colliders", colliderCount);
            StatRow("Lights", lightCount);
            if (inactiveCount > 0) StatRow("Inactive", inactiveCount, true);
            ImGui::EndTable();
        }

        // Numbers from the frame that just finished (this frame's own "Scene Draw"/"ImGui
        // Render" scopes haven't run yet at this point). GPU timings land several frames later
        // than their CPU counterparts (pipelining), so they are "most recently completed" (#197).
        DrawProfilerSection("CPU  (MS)", cpuSamples, profN);
        DrawProfilerSection("GPU  (MS)", gpuSamples, profGpuN);

        EditorUIPrimitives::SectionHeader("STATE CACHE");
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

    if (!open && host.SetShowStats) host.SetShowStats(false); // the title-bar X
}

} // namespace EditorModuleStats
