#pragma once
// The Glass theme ("Black glass"): a complete ImGuiStyle for the opt-in editor look, plus the
// drawing helpers its panels share (the canvas, continuous-curvature "squircle" surfaces, glass HUD
// plates). Floating windows become glass in GlassCompositor.h. Tokens come from GlassTokens.h; the spec is docs/EDITOR_DESIGN_SYSTEM.md.

#include "GlassTokens.h"

#include <imgui.h>

struct ImDrawList;

// Overrides every ImGuiStyle field the editor relies on: metrics (padding, spacing, rounding,
// borders) at 1x UI scale, and the whole colour array. Call on a style reset to defaults; the
// caller applies ScaleAllSizes(uiScale) afterwards exactly once (EditorLayer::ApplyThemeStyle does
// both). Safe to call repeatedly.
void ConfigureMacEngineTheme(ImGuiStyle& style);

// Black Glass as the editor uses it: turns a style that already carries the Tartarus metrics and
// palette into glass. Surfaces you work on become translucent black over the canvas
// (DrawWallpaper), fields and buttons become white washes, borders become faint white hairlines,
// and floating windows get rounded corners (GlassCompositor lays the blur under those). Sizes,
// spacing and fonts stay exactly Tartarus's. Call before ScaleAllSizes.
void ApplyGlassMaterials(ImGuiStyle& style);

namespace Glass {

// --- Metrics ----------------------------------------------------------------------------------------
// A spacing-law length (GlassTokens.h) in screen pixels at the current UI scale.
float Px(float designPx);
// The active UI scale, recovered from the UI font's baked size (same as EditorTheme::Scale()).
float Scale();

// --- Drawing ----------------------------------------------------------------------------------------
// Paints the canvas behind everything (ImGui's background draw list for the main viewport): solid
// black. Once per frame while the Glass theme is active and the window isn't see-through.
void DrawWallpaper();

// A filled rounded rectangle whose corners are superellipse arcs (continuous curvature), the shape
// macOS uses for windows and cards. `radius` is the nominal (circular-equivalent) radius.
void PathSquircle(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius);
void DrawSquircle(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float radius);
void DrawSquircleBorder(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float radius, float thickness);
// A soft drop shadow under a card: a few expanding, fading squircles. Cheap (4 fills).
void DrawSoftShadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius, float alpha, float spreadPx);

// A glass plate drawn inside the Scene view (the viewport HUDs): the blurred scene (GlassBackdrop)
// sampled under exactly this rect, the 25% black tint, the 15% white edge and a soft shadow.
// `blurTex` 0 = no blur available (the option is off, or the GPU path failed): falls back to the
// opaque GlassFallback.
// `viewportMin`/`viewportMax` is the screen rect the blurred texture covers. `blurVtx` (optional)
// receives the [first, end) vertices that sample the blur ({0, 0} when none), for a caller that
// blurs only part of the scene to set their UVs again once it has (EditorLayer's viewport HUDs).
void DrawFrostedPlate(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius, ImTextureID blurTex,
                      ImVec2 viewportMin, ImVec2 viewportMax, float alpha = 1.0f, int* blurVtx = nullptr);

inline ImU32 U32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImVec4 WithAlpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }

} // namespace Glass
