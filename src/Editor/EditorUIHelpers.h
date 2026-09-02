#pragma once
#include <imgui.h>

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

    // A vertical rule for separating clusters of controls on a single horizontal row (toolbar
    // strips, the Console header, the Asset Browser toolbar). Replaces the hand-rolled
    // `ImGui::TextDisabled("|")` idiom: a glyph carries its own baseline, height and font colour,
    // so it reads as content and never matches the row it divides. This draws a 1px line in
    // ImGuiCol_Separator spanning the current frame height, with ItemSpacing.x * gapScale of
    // breathing room on each side. Call it between two items on the same line, exactly where a
    // `SameLine()` would go; it advances the cursor itself, so the next widget just calls
    // SameLine() as usual.
    void VSeparator(float gapScale = 1.0f);
}
