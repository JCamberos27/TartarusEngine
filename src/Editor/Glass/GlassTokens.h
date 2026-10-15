#pragma once
// The Glass design system's tokens: palette, spacing law, radii, type roles and the motion matrix.
// docs/EDITOR_DESIGN_SYSTEM.md is the human-readable spec; this header is the one place the
// numbers live. Everything here is constexpr data with no behaviour, so the theme
// (GlassTheme.cpp), the motion runtime (GlassMotion.cpp), the panels (EditorLayer_Glass.cpp) and
// the unit tests (UnitTests_Glass.cpp) all read one source and can't drift.
//
// Opt-in: nothing reads these unless Preferences > General > Editor theme is "Glass". The default
// Tartarus theme (EditorTheme.h) is untouched.
//
// Host-only. The reloadable editor module (TartarusEditor.dll) doesn't include this; it keeps
// drawing with EditorTheme.h and simply inherits the Glass ImGuiStyle through the shared context.

#include <imgui.h>

namespace Glass {

constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, a); }

// =================================================================================================
// 1. The 'Apple Dark' palette: three layers
// =================================================================================================
// Layer 1: the base wallpaper canvas. The deepest neutral, behind every floating element. Painted
// once per frame into ImGui's background draw list (a faint vertical gradient from Top to Bottom),
// so docking gaps and the space around floating windows read as one continuous desk.
constexpr ImVec4 WallpaperTop    = Rgb(0x14, 0x14, 0x17); // obsidian, lifted a hair at the top edge
constexpr ImVec4 WallpaperBottom = Rgb(0x0B, 0x0B, 0x0D); // obsidian
constexpr ImVec4 Wallpaper       = Rgb(0x0E, 0x0E, 0x10); // the solid used for contrast math and linear prep

// Layer 2: translucent glassmorphic surfaces. Authored as "this colour at this opacity", the way a
// designer specifies them; GlassTheme.cpp converts each into the colour ImGui must blend so that
// ImGui's gamma-space blend over the wallpaper lands on the linear-light result (see
// LinearCorrectOver). Space greys after Apple's dark systemGray6/5/4.
struct GlassSurface { ImVec4 tint; float opacity; };
constexpr GlassSurface SurfacePanel   = {Rgb(0x1C, 0x1C, 0x1E), 0.80f}; // docked panels (WindowBg)
constexpr GlassSurface SurfaceRaised  = {Rgb(0x2C, 0x2C, 0x2E), 0.74f}; // headers, cards, tabs
constexpr GlassSurface SurfaceFloat   = {Rgb(0x26, 0x26, 0x29), 0.86f}; // popups, menus, tooltips
constexpr GlassSurface SurfaceHud     = {Rgb(0x1E, 0x1E, 0x21), 0.58f}; // viewport HUDs, over the blur
constexpr GlassSurface SurfaceTitle   = {Rgb(0x16, 0x16, 0x18), 0.90f}; // title / menu / status bars

// Fields, hover and press washes are drawn over a surface rather than the wallpaper, so they are
// plain white at low alpha: they lighten whatever surface they sit on by the same perceived step.
constexpr ImVec4 Field        = Rgb(0xFF, 0xFF, 0xFF, 0.055f);
constexpr ImVec4 FieldHover   = Rgb(0xFF, 0xFF, 0xFF, 0.085f);
constexpr ImVec4 FieldActive  = Rgb(0xFF, 0xFF, 0xFF, 0.115f);
constexpr ImVec4 WashHover    = Rgb(0xFF, 0xFF, 0xFF, 0.060f);
constexpr ImVec4 WashPressed  = Rgb(0xFF, 0xFF, 0xFF, 0.100f);
constexpr ImVec4 Hairline     = Rgb(0xFF, 0xFF, 0xFF, 0.080f); // borders: a light edge, never a dark one
constexpr ImVec4 Separator    = Rgb(0xFF, 0xFF, 0xFF, 0.100f);
constexpr ImVec4 GlassEdge    = Rgb(0xFF, 0xFF, 0xFF, 0.140f); // the top-lit rim on a frosted HUD
constexpr ImVec4 Shadow       = Rgb(0x00, 0x00, 0x00, 0.450f);

// Layer 3: typography and the accent.
// San Francisco-style off-whites. Primary is Apple's #F5F5F7 label colour; secondary a low-contrast
// dusk grey for metadata; tertiary for disabled and placeholder text.
constexpr ImVec4 TextPrimary   = Rgb(0xF5, 0xF5, 0xF7);
constexpr ImVec4 TextSecondary = Rgb(0xA1, 0xA1, 0xA6);
constexpr ImVec4 TextTertiary  = Rgb(0x6E, 0x6E, 0x73);
// The one vibrant, monochromatic accent: active selection, the underline of the active tab, focus,
// and gizmo states that are "critical" (the handle being dragged). Every accent variant is this one
// hue at a different strength, never a second hue.
constexpr ImVec4 Accent       = Rgb(0x0A, 0x84, 0xFF);        // systemBlue (dark)
constexpr ImVec4 AccentBright = Rgb(0x40, 0x9C, 0xFF);        // hover / pressed on an accent fill
constexpr ImVec4 AccentWash   = Rgb(0x0A, 0x84, 0xFF, 0.24f); // selected row body
constexpr ImVec4 AccentGlow   = Rgb(0x0A, 0x84, 0xFF, 0.45f); // focus ring, drop target
constexpr ImVec4 OnAccent     = Rgb(0xFF, 0xFF, 0xFF);

// Fixed-meaning status colours (never stand in for the accent).
constexpr ImVec4 Danger  = Rgb(0xFF, 0x45, 0x3A);
constexpr ImVec4 Warning = Rgb(0xFF, 0xD6, 0x0A);
constexpr ImVec4 Success = Rgb(0x30, 0xD1, 0x58);

// Flat axis colours for vector rows and gizmo labels. The macOS window-control hues, a step
// brighter so they read as a 3px bar on the glass. Z is a cooler, lighter blue than the accent so
// "Z" and "selected" never look alike.
constexpr ImVec4 AxisX = Rgb(0xFF, 0x5F, 0x57);
constexpr ImVec4 AxisY = Rgb(0x28, 0xC8, 0x40);
constexpr ImVec4 AxisZ = Rgb(0x5A, 0xA9, 0xFF);

// =================================================================================================
// 2. The continuous rounding and spacing law
// =================================================================================================
// One unit, U = 8 px at 1x UI scale. Every padding, margin and gap is k * U for a whole k; the one
// exception is the half step (U/2 = 4 px), allowed only *inside* a control (the vertical inset of a
// frame, the gap between an icon and its label). Nothing is ever 3, 5, 6 or 10 px.
//
//   Space(k) = k * U * uiScale            (k = 0, 0.5, 1, 1.5, 2, 3, 4, 6)
//
// Radii follow the same rule from the other side: a nested shape's radius is its parent's radius
// minus the inset between them (Concentric(r, inset)), floored at the control radius, so corners
// of nested cards stay parallel instead of pinching.
constexpr float U = 8.0f;

constexpr float SpaceHalf = U * 0.5f; //  4 px: intra-control only
constexpr float Space1    = U * 1.0f; //  8 px: between related items
constexpr float Space1h   = U * 1.5f; // 12 px: control's horizontal inset
constexpr float Space2    = U * 2.0f; // 16 px: window padding, between groups
constexpr float Space3    = U * 3.0f; // 24 px: between sections
constexpr float Space4    = U * 4.0f; // 32 px: outer margin of floating HUDs from the viewport edge

// Corner radii: "squircle-like" 10-12 px everywhere. ImGui's own widgets round with circular
// arcs at these radii; surfaces the Glass layer draws itself (HUDs, cards, tiles, selection
// panels) use DrawSquircle's continuous-curvature corner, which reads as the same radius but
// without the visible tangent break.
constexpr float RadiusWindow  = 12.0f;
constexpr float RadiusCard    = 12.0f;
constexpr float RadiusControl = 10.0f; // frames, tabs, buttons, input boxes
constexpr float RadiusRow     = 8.0f;  // a selection panel inside a list (row inset 4 px from a 12 px card)
constexpr float RadiusGrab    = 10.0f;

constexpr float Concentric(float outerRadius, float inset) {
    return outerRadius - inset > RadiusRow ? outerRadius - inset : RadiusRow;
}

// Exponent of the superellipse corner DrawSquircle traces. 2 is a circle; Apple's continuous
// corner sits near 5. Its radius is scaled up by SquircleRadiusScale so it covers about the same
// area as a circular corner of the nominal radius.
constexpr float SquircleExponent    = 5.0f;
constexpr float SquircleRadiusScale = 1.28f;

// Type roles, as sizes at 1x (they follow the UI scale through the font's baked size).
constexpr float TypeBody     = 15.0f;
constexpr float TypeCaption  = 13.0f;
constexpr float TypeHeadline = 13.0f; // section headers: medium weight via the UI face, not caps
constexpr float TypeTitle    = 20.0f;

// =================================================================================================
// 3. The fluid motion matrix (docs/EDITOR_DESIGN_SYSTEM.md, "Motion" has the table)
// =================================================================================================
// Ease ids and parameters are ImAnim's (im_anim.h: iam_ease_type, iam_ease_desc). For
// iam_ease_spring ImAnim evaluates the spring over *normalised* time u = t / duration
// (spring_unit in im_anim.cpp), so Mass / Stiffness / Damping below are in per-duration units:
// the spring's physical angular frequency is sqrt(k / m) / duration. Each spring here is tuned so
// zeta * omega_n >= 5, i.e. its envelope has decayed below 1% by u = 1 and the tween's final snap
// to the target is invisible.
struct MotionSpec {
    int   ease;               // iam_ease_type
    float p0, p1, p2, p3;     // ease parameters (bezier x1 y1 x2 y2, or spring m k c v0)
    float durationMs;
};

// Easing ids mirrored from im_anim.h so this header stays include-light. Static-asserted
// against the real enum in GlassMotion.cpp.
namespace EaseId {
constexpr int OutCubic    = 5;
constexpr int InCubic     = 4;
constexpr int OutQuart    = 8;
constexpr int CubicBezier = 32;
constexpr int Spring      = 33;
} // namespace EaseId

// The macOS "smooth" curve: a fast start and a long, soft landing.
#define GLASS_EASE_SMOOTH EaseId::CubicBezier, 0.32f, 0.72f, 0.00f, 1.00f

// --- Category 1: high-frequency navigation (tree rows, asset tiles) -----------------------------
constexpr MotionSpec HoverIn       = {EaseId::OutCubic, 0, 0, 0, 0, 140.0f};
constexpr MotionSpec HoverOut      = {EaseId::OutCubic, 0, 0, 0, 0, 280.0f}; // a slower release reads as a trail
constexpr MotionSpec SelectionGlide = {GLASS_EASE_SMOOTH, 220.0f};          // the selection panel sliding row to row
// Visual deltas (all at 1x UI scale)
constexpr float HoverWashAlpha   = 0.060f; // row body 0 -> 6% white
constexpr float HoverTextMix     = 1.0f;   // secondary -> primary text, full blend in OKLAB
constexpr float TileHoverScale   = 1.03f;  // asset tile 1.00 -> 1.03
constexpr float TileHoverLiftPx  = 2.0f;   // asset tile rises 2 px
constexpr float TileShadowAlpha  = 0.35f;  // and its shadow comes up from 0
// Velocity-aware gate: while the pointer sweeps faster than this, rows it crosses don't start a
// hover-in (no tween channel is created at all), so a fast pass over a 2,000-row tree costs
// nothing and leaves no comet trail. Rows already lit keep easing out normally.
constexpr float HoverGatePxPerSec = 1800.0f;
constexpr float PointerVelocitySmoothing = 0.35f; // EMA weight of the newest frame's speed

// --- Category 2: structural panel reveals (foldouts, accordion inspectors) ---------------------
constexpr MotionSpec RevealOpen  = {GLASS_EASE_SMOOTH, 320.0f};
constexpr MotionSpec RevealClose = {GLASS_EASE_SMOOTH, 240.0f};
constexpr MotionSpec ChevronTurn = {GLASS_EASE_SMOOTH, 240.0f};
// Layout-aware stagger: each child row of an opening section fades and settles in after the
// previous one, capped so a long section never waits on its tail.
constexpr float RevealStaggerMs   = 18.0f;
constexpr int   RevealStaggerCap  = 8;     // rows past the 8th share the 8th's delay
constexpr float RevealRowShiftPx  = 6.0f;  // a row settles from +6 px to 0
constexpr float RevealRowFadeMs   = 200.0f;
// Asset grid wave: tiles fade in along a diagonal wave (row + column), each one this far behind.
constexpr float GridWaveStaggerMs = 22.0f;
constexpr float GridWaveFadeMs    = 260.0f;
constexpr int   GridWaveCap       = 24;    // past 24 steps every tile shares the last delay
constexpr float GridWaveRisePx    = 8.0f;

// --- Category 3: transient notification pushes --------------------------------------------------
// Elastic pop: zeta = c / (2 sqrt(k m)) = 0.37 -> ~28% overshoot, envelope e^-5 by u = 1.
constexpr MotionSpec ToastPop  = {EaseId::Spring, 1.0f, 180.0f, 10.0f, 0.0f, 620.0f};
constexpr MotionSpec ToastExit = {EaseId::InCubic, 0, 0, 0, 0, 180.0f};
constexpr float ToastPopFromScale = 0.92f;
constexpr float ToastPopRisePx    = 12.0f;
constexpr float ToastLifetimeMs   = 4200.0f;
// Snap pop (a node landing on the grid): a tighter spring, zeta = 0.43.
constexpr MotionSpec SnapPop = {EaseId::Spring, 1.0f, 260.0f, 14.0f, 0.0f, 420.0f};
constexpr float SnapRingFromScale = 0.60f;
constexpr float SnapRingToScale   = 1.00f;
// Contextual shake (a compile failure): the impulse response of an under-damped spring in *real*
// seconds (Glass::SpringImpulse), not an ImAnim tween, so it needs no channel at all.
struct ShakeSpec { float mass, stiffness, damping, amplitudePx, durationMs; };
constexpr ShakeSpec ErrorShake = {1.0f, 1600.0f, 16.0f, 10.0f, 520.0f}; // f ~ 6.4 Hz, zeta = 0.2

#undef GLASS_EASE_SMOOTH

} // namespace Glass
