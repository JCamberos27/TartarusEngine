#pragma once
// The Glass theme: a complete ImGuiStyle for the opt-in macOS-style editor look, plus the drawing
// helpers its panels share (the wallpaper, continuous-curvature "squircle" surfaces, frosted HUD
// plates). Tokens come from GlassTokens.h; the spec is docs/EDITOR_DESIGN_SYSTEM.md.

#include "GlassTokens.h"

#include <imgui.h>

struct ImDrawList;

// Overrides every ImGuiStyle field the editor relies on: metrics (padding, spacing, rounding,
// borders) at 1x UI scale, and the whole colour array. Call on a style reset to defaults; the
// caller applies ScaleAllSizes(uiScale) afterwards exactly once (EditorLayer::ApplyThemeStyle does
// both). Safe to call repeatedly.
void ConfigureMacEngineTheme(ImGuiStyle& style);

namespace Glass {

// --- Colour space ---------------------------------------------------------------------------------
// The ImGui OpenGL backend blends in the framebuffer's encoded (sRGB) space: the engine never
// enables GL_FRAMEBUFFER_SRGB for the UI pass. Blending 80% grey glass over a near-black wallpaper
// in encoded space lands darker and muddier than the same blend in linear light would.
//
// LinearCorrectOver returns the colour C' (same alpha) for which ImGui's encoded-space blend
//     a * C' + (1 - a) * B
// equals the physically correct linear-light composite
//     Encode(a * Linear(C) + (1 - a) * Linear(B))
// over the known backdrop B. For the wallpaper it's exact; over anything else (the 3D view behind a
// floating window) it's the same bias in the right direction. Channels are clamped to [0, 1].
ImVec4 LinearCorrectOver(ImVec4 tintSrgb, float opacity, ImVec4 backdropSrgb);
float SrgbToLinear(float c);
float LinearToSrgb(float c);
// The colour a GlassSurface resolves to (linear-corrected over the wallpaper).
ImVec4 Resolve(const GlassSurface& s);
// What a surface looks like composited onto the wallpaper, as an opaque colour (contrast checks).
ImVec4 CompositeOnWallpaper(const GlassSurface& s);

// --- Metrics ----------------------------------------------------------------------------------------
// A spacing-law length (GlassTokens.h) in screen pixels at the current UI scale.
float Px(float designPx);
// The active UI scale, recovered from the UI font's baked size (same as EditorTheme::Scale()).
float Scale();

// --- Drawing ----------------------------------------------------------------------------------------
// Layer 1: paints the wallpaper gradient behind everything (ImGui's background draw list for the
// main viewport). Once per frame while the Glass theme is active.
void DrawWallpaper();

// A filled rounded rectangle whose corners are superellipse arcs (continuous curvature), the shape
// macOS uses for windows and cards. `radius` is the nominal (circular-equivalent) radius.
void PathSquircle(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius);
void DrawSquircle(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float radius);
void DrawSquircleBorder(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float radius, float thickness);
// A soft drop shadow under a card: a few expanding, fading squircles. Cheap (4 fills).
void DrawSoftShadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius, float alpha, float spreadPx);

// A frosted-glass plate: the blurred scene (GlassBackdrop) sampled under exactly this rect, a tint,
// a hairline rim lit from the top and a soft shadow. `blurTex` 0 = no blur available (e.g. the
// option is off, or the GPU path failed): falls back to a plain translucent tint.
// `viewportMin`/`viewportMax` is the screen rect the blurred texture covers.
void DrawFrostedPlate(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius, ImTextureID blurTex,
                      ImVec2 viewportMin, ImVec2 viewportMax, float alpha = 1.0f);

inline ImU32 U32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImVec4 WithAlpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }

} // namespace Glass
