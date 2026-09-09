// The editor's Undo History HUD, living inside TartarusEditor.dll so its chrome hot-reloads while
// the editor stays open with its scene loaded. Ported in behaviour from
// EditorLayer::DrawHistoryPanel (EditorLayer_Toolbar.cpp): the compact, transparent HUD pinned to
// the Scene viewport's bottom-right corner (mirroring the Stats HUD at top-left) — the "History"
// heading, then every recorded change oldest-to-newest with the current position highlighted,
// auto-sized to its content and height-capped so a long history never climbs into the top-right
// nav cluster or the toolbar.
//
// Thin slice (issue #229): the row list itself — the undo/redo stacks, the click-to-jump
// (JumpToUndo/RedoEntry), the per-row tooltips — stays host code, drawn into this window through
// host.DrawHistoryListBody(). Unlike Stats, the rows are interactive (click to jump), so this
// window is NOT NoInputs. The contrast-adaptive text tint is owned here now (same decision as the
// Stats HUD): the host still drives the async PBO luminance readback — it owns the Scene
// framebuffer and the GL context — but the ~10 Hz throttle and the eased/target luminance pair
// live module-side as function-local statics that re-converge within ~0.15 s after a reload.

#include "EditorModuleAPI.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>

namespace EditorModuleHistory {

namespace {

// Contrast-adaptive HUD text — identical maths to EditorModuleStats' copy and the host's old
// EditorLayer::PushAdaptiveHudText. Pushes exactly two style colours (Text + TextDisabled); the
// caller pops them after its content.
void PushAdaptiveHudText(const EditorModuleHostAPI& host, float centerX, float centerY, float boxPx, float dt) {
    static float s_easedLum = 1.0f;
    static float s_targetLum = 1.0f;
    static float s_sampleAccum = 0.0f;

    // #275 toggle: static near-white text, no sampling.
    if (host.GetAdaptiveHudContrast && !host.GetAdaptiveHudContrast()) {
        s_easedLum = s_targetLum = 1.0f;
        ImGui::PushStyleColor(ImGuiCol_Text,         IM_COL32(255, 255, 255, 240));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(255, 255, 255, 150));
        return;
    }

    s_sampleAccum += dt;
    if (s_sampleAccum >= 0.1f) {
        s_sampleAccum = 0.0f;
        const float lum = host.SampleHistoryHudLuminance
                              ? host.SampleHistoryHudLuminance(centerX, centerY, boxPx) : -1.0f;
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
    int rowCount = 0;
    if (!host.GetHistoryHudFrame ||
        !host.GetHistoryHudFrame(&vx, &vy, &vw, &vh, &uiScale, &rowCount)) {
        return;
    }

    // Pinned to the viewport's bottom-right corner, sized to its text, fully transparent, not
    // dockable, not persisted so the pin always wins. Height ceiling: grow upward from the pin
    // only until a clear line below the top-right nav cluster (rotate ring + dolly/pan box +
    // "Persp" label ≈ 220px @ 1x); beyond that the list scrolls internally. Matches the maths in
    // the host's old DrawHistoryPanel exactly.
    const float hpad = 12.0f * uiScale;
    const float statusBarH = ImGui::GetTextLineHeight() + 8.0f * uiScale;
    const float gizmoZoneH = 220.0f * uiScale;
    const float maxH = std::max(120.0f * uiScale, vh - hpad - statusBarH - gizmoZoneH);

    const ImGuiStyle& st = ImGui::GetStyle();
    const float chromeH = ImGui::GetTextLineHeightWithSpacing()   // "History" line
                        + st.ItemSpacing.y + 2.0f                 // separator
                        + st.WindowPadding.y * 2.0f;
    const float desiredH = chromeH + std::max(rowCount, 1) * ImGui::GetTextLineHeightWithSpacing();
    const float winH = std::min(desiredH, maxH);

    ImGui::SetNextWindowPos(ImVec2(vx + vw - hpad, vy + vh - hpad - statusBarH),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(0.0f, winH)); // x=0 → auto-fit width, height clamped
    ImGui::SetNextWindowBgAlpha(0.0f);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;

    bool visible = true;
    if (!ImGui::Begin(ICON_FA_CLOCK_ROTATE_LEFT "  History", &visible, flags)) {
        ImGui::End();
        return;
    }
    if (!visible && host.SetShowHistory) host.SetShowHistory(false); // the title-bar X

    // Sample the scene behind the HUD so the text stays legible white-on-dark / dark-on-light
    // with no plate behind it.
    const ImVec2 wpos = ImGui::GetWindowPos(), wsz = ImGui::GetWindowSize();
    PushAdaptiveHudText(host, wpos.x + wsz.x * 0.5f, wpos.y + wsz.y * 0.5f, 48.0f * uiScale,
                        ImGui::GetIO().DeltaTime);

    ImGui::TextUnformatted(ICON_FA_CLOCK_ROTATE_LEFT "  History");
    // Explanation on the heading tooltip (#156) — no persistent "(?)" glyph.
    if (ImGui::IsItemHovered() && host.SetTooltip) host.SetTooltip(
        "Every recorded change, oldest to newest. Click any entry to jump\n"
        "straight there - undoing or redoing everything in between automatically.");
    ImGui::Separator();

    if (host.DrawHistoryListBody) host.DrawHistoryListBody();

    ImGui::PopStyleColor(2); // adaptive Text + TextDisabled
    ImGui::End();
}

} // namespace EditorModuleHistory
