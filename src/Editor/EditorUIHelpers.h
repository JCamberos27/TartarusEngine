#pragma once
#include <imgui.h>
#include <cmath>

// Thin wrappers around ImGui's own tooltip calls that additionally respect
// EditorSettings::Get().ShowTooltips — the single choke point every panel (Inspector,
// Hierarchy, Console, Asset Browser, Toolbar Settings) routes its contextual help through, so
// the one global preference actually governs all of them instead of each panel needing its own
// "if enabled" check. Call these exactly where you'd otherwise have called
// ImGui::SetTooltip/the old free-function HelpMarker — the call sites don't change shape.
namespace EditorUI {
    // Same contract as ImGui::SetTooltip: call after an ImGui::IsItemHovered() check (or a
    // compound condition — e.g. IsItemHovered() && !IsItemActive() — when a widget needs to
    // suppress the tooltip during its own active-edit state). When tooltips are globally
    // disabled this returns before touching the format string or calling into ImGui at all, so
    // a disabled preference costs nothing beyond one bool check per call site.
    void SetTooltip(const char* fmt, ...) IM_FMTARGS(1);

    // Unity-style "(i)" marker: draws the glyph and shows `desc` on hover. Draws nothing at all
    // (not even the glyph) when tooltips are globally disabled, rather than leaving an inert
    // icon on screen that no longer does anything if hovered.
    void HelpMarker(const char* desc);

    // #127 — ColorEdit3 for a colour stored in LINEAR space (every render colour: base/emissive,
    // light, sky, reflected Color fields). The swatch, hex and 0-255 inputs show sRGB, like
    // Unity's picker and every art tool, so "#808080" means display mid-grey; the value written
    // back is linear. Values above 1 (HDR emission / light colours) aren't clamped. Only writes
    // `linear` on an actual edit, so an untouched field never drifts through the round trip.
    inline float LinearToSrgbChannel(float c) {
        if (c <= 0.0f) return 0.0f;
        return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    }
    inline float SrgbToLinearChannel(float c) {
        if (c <= 0.0f) return 0.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    inline bool ColorEditLinear(const char* label, float linear[3],
                                ImGuiColorEditFlags flags = ImGuiColorEditFlags_DisplayHex) {
        float srgb[3];
        bool hdr = false;
        for (int i = 0; i < 3; ++i) {
            srgb[i] = LinearToSrgbChannel(linear[i]);
            hdr |= linear[i] > 1.0f;
        }
        if (hdr) flags |= ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float;
        if (!ImGui::ColorEdit3(label, srgb, flags)) return false;
        for (int i = 0; i < 3; ++i) linear[i] = SrgbToLinearChannel(srgb[i]);
        return true;
    }

    // A vertical rule for separating clusters of controls on a single horizontal row (toolbar
    // strips, the Console header, the Asset Browser toolbar). Replaces the hand-rolled
    // `ImGui::TextDisabled("|")` idiom: a glyph carries its own baseline, height and font colour,
    // so it reads as content and never matches the row it divides. This draws a 1px line in
    // ImGuiCol_Separator spanning the current frame height, with ItemSpacing.x * gapScale of
    // breathing room on each side. Call it between two items on the same line, exactly where a
    // `SameLine()` would go; it advances the cursor itself, so the next widget just calls
    // SameLine() as usual.
    void VSeparator(float gapScale = 1.0f);

    // Styled slider: a thin rounded track with a circular grab handle, plus an editable numeric
    // box on the right — click it and type a value in directly. Drop-in replacement for
    // ImGui::SliderFloat / ImGui::SliderInt: identical signature, identical return (true on the
    // frame the value changes). Honors SetNextItemWidth / PushItemWidth for the whole widget
    // (track + box together). Pass "" as the format to drop the number box (track + handle only).
    //
    // outActivated / outDeactivatedAfterEdit, when non-null, receive this frame's combined
    // "interaction started" / "interaction finished after an edit" signal for the WHOLE widget —
    // true if EITHER the track OR the trailing number box triggered it. This exists because the
    // widget draws two separate ImGui items (the track via SliderBehavior, then an InputText box);
    // ImGui's own IsItemActivated()/IsItemDeactivatedAfterEdit(), called after this returns, only
    // ever see the LAST item submitted — the number box — so a caller relying on those directly
    // silently misses every drag on the track itself. Any call site that stages/commits an undo
    // step or persists a value on activate/deactivate MUST use these out-params instead of
    // IsItemActivated()/IsItemDeactivatedAfterEdit() after the call.
    bool SliderFloat(const char* label, float* v, float v_min, float v_max,
                     const char* format = "%.3f", ImGuiSliderFlags flags = 0,
                     bool* outActivated = nullptr, bool* outDeactivatedAfterEdit = nullptr);
    bool SliderInt(const char* label, int* v, int v_min, int v_max,
                   const char* format = "%d", ImGuiSliderFlags flags = 0,
                   bool* outActivated = nullptr, bool* outDeactivatedAfterEdit = nullptr);
}
