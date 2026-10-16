#pragma once
// The Glass design system's tokens: palette, spacing law, radii, type roles and the motion matrix.
// docs/EDITOR_DESIGN_SYSTEM.md is the human-readable spec; this header is the one place the
// numbers live. Everything here is constexpr data with no behaviour, so the theme
// (GlassTheme.cpp), the motion runtime (GlassMotion.cpp), the panels (EditorLayer_Glass.cpp) and
// the unit tests (UnitTests_Glass.cpp) all read one source and can't drift.
//
// Opt-in: nothing reads these unless Preferences > General > Editor theme is "Black Glass". The default
// Tartarus theme (EditorTheme.h) is untouched.
//
// Host-only. The reloadable editor module (TartarusEditor.dll) doesn't include this; it keeps
// drawing with EditorTheme.h and simply inherits the Glass ImGuiStyle through the shared context.

#include <imgui.h>

namespace Glass {

constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, a); }

// =================================================================================================
// 1. The 'Black glass' palette: two materials, no in-between
// =================================================================================================
// The rule: things that float are glass, things you work in are solid. Docked panels, the menu bar,
// the status bar and the dock gaps are solid black, lifted only by a few steps of near-black. What
// floats (popups, menus, tooltips, combo lists, modals, undocked windows, the viewport toolbar and
// tool rail, the viewport HUDs and notifications) is glass: a heavy blur of whatever is behind it,
// a 25% black tint and a 1 px edge of 15% white. GlassCompositor.cpp renders that blur for ImGui
// windows; DrawFrostedPlate does it for the HUD cards drawn inside the Scene view.
//
// The UI itself is monochrome: white text on black, no accent hue in the chrome. Colour comes from
// content (the scene in the viewport) and from colours that carry meaning (axes, asset kinds,
// status). The solid steps are the default Tartarus theme's (EditorTheme.h), so the two themes
// share one black.

// --- Solid ----------------------------------------------------------------------------------------
constexpr ImVec4 Base     = Rgb(0x00, 0x00, 0x00); // menu bar, status bar, dock gaps, the canvas
constexpr ImVec4 Panel    = Rgb(0x05, 0x05, 0x05); // a docked panel (WindowBg)
constexpr ImVec4 Card     = Rgb(0x09, 0x09, 0x09); // a card inside a panel (component bodies)
constexpr ImVec4 Raised   = Rgb(0x0E, 0x0E, 0x0E); // headers, the selected tab, table headers
constexpr ImVec4 Hover    = Rgb(0x1C, 0x1C, 0x1C);
constexpr ImVec4 Pressed  = Rgb(0x2A, 0x2A, 0x2A);
constexpr ImVec4 Hairline = Rgb(0x24, 0x24, 0x24); // 1 px borders and rules on solid surfaces

// --- Glass ----------------------------------------------------------------------------------------
constexpr ImVec4 GlassTint   = Rgb(0x00, 0x00, 0x00, 0.25f); // laid over the blurred backdrop
constexpr ImVec4 GlassEdge   = Rgb(0xFF, 0xFF, 0xFF, 0.15f); // the 1 px border of a glass surface
constexpr float  GlassRadius = 14.0f;                        // corners of floating surfaces (1x)
// The blur: the backdrop pyramid (GlassBackdrop) runs this many half-resolution levels deep. Each
// level doubles the footprint, so 6 levels is a ~128 px soft field at full resolution: no shape
// survives, only colour, as on the Windows Start menu and acrylic windows.
constexpr int    GlassBlurLevels = 6;
constexpr float  GlassSaturation = 0.85f; // a touch of grey: colour fields, not a coloured window
// Highlights behind glass are compressed to this encoded luminance before the tint, so white text
// on glass keeps >= 4.5:1 over anything: 0.52 * 0.75 (the tint) = a #636363 worst case.
constexpr float  GlassCeiling    = 0.52f;
// Where no blur is available (Frosted blur off in Preferences, a compute-shader failure, a window
// dragged out to its own OS window) glass falls back to this opaque near-black, so text on it
// stays legible over anything.
constexpr ImVec4 GlassFallback = Rgb(0x14, 0x14, 0x14);
// A popup's own background colour (ImGuiCol_PopupBg) and a floating window's title bar. The
// compositor removes these from glass windows and paints the glass instead, so they only show
// where the compositor doesn't run (see GlassFallback).
constexpr ImVec4 PopupSolid = GlassFallback;

// --- Glass over the Tartarus look -----------------------------------------------------------------
// Black Glass keeps the Tartarus theme's sizes, spacing, fonts and panels and changes only the
// materials (ApplyGlassMaterials). Panels, tab strips, the menu bar and the status bar are black
// glass over the black canvas, or over the desktop when the window is see-through.
constexpr float GlassPanelAlpha   = 0.15f; // a docked panel: black at this alpha over the canvas
constexpr float GlassChromeAlpha  = 0.80f; // menu bar, tab strips, status bar: a little darker
// Everything that floats (popups, menus, tooltips, floating windows and the Scene view's overlays):
// glass in the chrome's tint over the blur, with a faint edge and no drop shadow.
constexpr ImVec4 OverlayTint = Rgb(0x00, 0x00, 0x00, GlassChromeAlpha);
// Corners of floating windows and popups at 1x. Docked panels stay square, as in Tartarus.
constexpr float GlassWindowRounding = 10.0f;
constexpr float GlassPopupRounding  = 8.0f;

// Fields, hover and press washes are plain white at low alpha: they lighten whatever they sit on
// (solid black or glass) by the same perceived step.
constexpr ImVec4 Field        = Rgb(0xFF, 0xFF, 0xFF, 0.055f);
constexpr ImVec4 FieldHover   = Rgb(0xFF, 0xFF, 0xFF, 0.085f);
constexpr ImVec4 FieldActive  = Rgb(0xFF, 0xFF, 0xFF, 0.115f);
constexpr ImVec4 WashHover    = Rgb(0xFF, 0xFF, 0xFF, 0.060f);
constexpr ImVec4 WashPressed  = Rgb(0xFF, 0xFF, 0xFF, 0.100f);
constexpr ImVec4 Separator    = Rgb(0xFF, 0xFF, 0xFF, 0.100f);
constexpr ImVec4 Shadow       = Rgb(0x00, 0x00, 0x00, 0.450f);

// --- Text and the (monochrome) accent -------------------------------------------------------------
constexpr ImVec4 TextPrimary   = Rgb(0xE6, 0xE8, 0xE6); // white, a hair off pure
constexpr ImVec4 TextSecondary = Rgb(0x9C, 0x9E, 0x9C); // labels, captions, metadata
constexpr ImVec4 TextTertiary  = Rgb(0x6A, 0x6C, 0x6A); // hints, disabled, placeholders
// Selection, the active tab's underline, focus and "on" are white, never a hue. A selected pill is
// inverse video: a white fill with black text.
constexpr ImVec4 Accent       = Rgb(0xF4, 0xF6, 0xF4);
constexpr ImVec4 AccentBright = Rgb(0xFF, 0xFF, 0xFF);
constexpr ImVec4 AccentWash   = Rgb(0xFF, 0xFF, 0xFF, 0.12f); // selected row body
constexpr ImVec4 AccentGlow   = Rgb(0xFF, 0xFF, 0xFF, 0.40f); // focus ring, drop target
constexpr ImVec4 OnAccent     = Rgb(0x00, 0x00, 0x00);

// Fixed-meaning status colours (EditorTheme's muted set).
constexpr ImVec4 Danger  = Rgb(0xE8, 0x5C, 0x54);
constexpr ImVec4 Warning = Rgb(0xE6, 0xA4, 0x50);
constexpr ImVec4 Success = Rgb(0x7C, 0xD8, 0x8C);

// Axis colours for vector rows and gizmo labels: the editor's own (EditorTheme.h), since colour
// that carries meaning is the one place hue belongs.
constexpr ImVec4 AxisX = Rgb(0xE5, 0x48, 0x4D);
constexpr ImVec4 AxisY = Rgb(0x4C, 0xC3, 0x8A);
constexpr ImVec4 AxisZ = Rgb(0x4C, 0x8D, 0xF6);

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

// Corner radii: 14 px on floating surfaces, smaller and nested inside them. Docked panels are
// attached to an edge and stay square (ImGui never rounds a docked window). ImGui's own widgets
// round with circular arcs at these radii; surfaces the Glass layer draws itself (HUDs, cards, tiles, selection
// panels) use DrawSquircle's continuous-curvature corner, which reads as the same radius but
// without the visible tangent break.
constexpr float RadiusWindow  = GlassRadius; // floating windows, popups, menus, tooltips
constexpr float RadiusCard    = 10.0f;
constexpr float RadiusControl = 8.0f;  // frames, tabs, buttons, input boxes
constexpr float RadiusRow     = 6.0f;  // a selection panel inside a list (row inset 4 px from a 10 px card)
constexpr float RadiusGrab    = 8.0f;

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
// The selection panel sliding row to row: a spring with a small (~9%) overshoot, so it visibly
// lands. zeta = 16 / (2 sqrt(170)) = 0.61, zeta * omega_n = 8 (settled well before u = 1).
constexpr MotionSpec SelectionGlide = {EaseId::Spring, 1.0f, 170.0f, 16.0f, 0.0f, 380.0f};
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

// --- Category 4: surfaces arriving (GlassCompositor.cpp) ----------------------------------------
// Every floating glass surface eases in when it appears, and a docked panel fades up when its tab
// is switched to. Applied to the window's finished vertices at render time, so it covers every
// window in the editor without touching any panel's code. Input is live from the first frame.
// Menus, combo lists and context menus: fade, settle down from 6 px above, grow from 96%.
constexpr MotionSpec FloatIn        = {EaseId::OutCubic, 0, 0, 0, 0, 200.0f};
constexpr float      FloatInFromScale = 0.96f;
constexpr float      FloatInDropPx    = 6.0f;
// Tooltips: a quick fade only (they follow the pointer; anything more is noise).
constexpr MotionSpec TooltipIn      = {EaseId::OutCubic, 0, 0, 0, 0, 120.0f};
// Modals and undocked windows: a spring pop from 94% about their centre. zeta = 20 / (2 sqrt(220))
// = 0.67 (a ~6% overshoot), zeta * omega_n = 10.
constexpr MotionSpec WindowPop      = {EaseId::Spring, 1.0f, 220.0f, 20.0f, 0.0f, 460.0f};
constexpr float      WindowPopFromScale = 0.94f;
constexpr MotionSpec WindowFade     = {EaseId::OutCubic, 0, 0, 0, 0, 180.0f};
// A docked panel whose tab is switched to (or that opens): its content fades up from 6 px below.
constexpr MotionSpec PanelIn        = {EaseId::OutCubic, 0, 0, 0, 0, 220.0f};
constexpr float      PanelInRisePx  = 6.0f;
// The rim glint: as a glass surface arrives, its 1 px edge catches the light (up to this much
// extra white) and cools back to GlassEdge.
constexpr MotionSpec RimGlint       = {EaseId::OutCubic, 0, 0, 0, 0, 700.0f};
constexpr float      RimGlintBoost  = 0.35f;

// --- Category 5: controls (buttons, checkboxes, fields; EditorUIPrimitives via the bridge) -------
// A press squashes quickly and releases on a spring with a small overshoot. zeta = 24 /
// (2 sqrt(300)) = 0.69 (~5% overshoot), zeta * omega_n = 12.
constexpr MotionSpec PressDown      = {EaseId::OutCubic, 0, 0, 0, 0, 90.0f};
constexpr MotionSpec PressRelease   = {EaseId::Spring, 1.0f, 300.0f, 24.0f, 0.0f, 260.0f};
constexpr float      PressScale     = 0.97f;
// A checkbox tick draws on along its path; unchecking erases it faster.
constexpr MotionSpec CheckDraw      = {GLASS_EASE_SMOOTH, 180.0f};
constexpr MotionSpec CheckErase     = {EaseId::OutCubic, 0, 0, 0, 0, 120.0f};
// An input's 1 px focus ring.
constexpr MotionSpec FocusIn        = {EaseId::OutCubic, 0, 0, 0, 0, 120.0f};
constexpr MotionSpec FocusOut       = {EaseId::OutCubic, 0, 0, 0, 0, 200.0f};
constexpr float      FocusRingAlpha = 0.55f;
// Rejected input: one small horizontal wiggle (f ~ 6.4 Hz, zeta = 0.3), much gentler than the
// compile-failure shake.
constexpr ShakeSpec  InvalidWiggle  = {1.0f, 1600.0f, 24.0f, 3.0f, 360.0f};
// A one-off wash (a new or reparented row, a new console error): up instantly, fades out.
constexpr MotionSpec RowFlash       = {EaseId::OutCubic, 0, 0, 0, 0, 600.0f};
constexpr float      RowFlashAlpha  = 0.10f;
// An eased scroll jump (reveal a row, jump to the console's bottom).
constexpr MotionSpec ScrollEase     = {GLASS_EASE_SMOOTH, 280.0f};

// --- Category 6: the dock space (GlassDockMotion.cpp) -------------------------------------------
// The selected tab's overline glides with SelectionGlide. A docked panel that takes focus: its edge
// glints white and cools.
constexpr MotionSpec DockRimGlint   = {EaseId::OutCubic, 0, 0, 0, 0, 600.0f};
constexpr float      DockRimAlpha   = 0.22f;

// --- Category 7: play mode and the viewport -----------------------------------------------------
// Play <-> Stop: the transport glyph morphs triangle <-> square (iam_path_morph) and its colour
// crosses green -> red in OKLAB.
constexpr MotionSpec TransportMorph  = {GLASS_EASE_SMOOTH, 220.0f};
// The play-mode frame: a 1 px white inner border on the Scene and Game views. Fades in on Play,
// breathes very slightly while playing, dims while paused.
constexpr MotionSpec PlayFrameIn     = {EaseId::OutCubic, 0, 0, 0, 0, 240.0f};
constexpr MotionSpec PlayFrameOut    = {EaseId::OutCubic, 0, 0, 0, 0, 320.0f};
constexpr float      PlayFrameAlpha  = 0.22f;
constexpr float      PlayFrameBreath = 0.04f;  // +-
constexpr float      PlayFrameHz     = 0.25f;  // one breath every 4 s
constexpr float      PlayFramePaused = 0.12f;
// Frame selection (F): the camera glides on the smooth curve, a little longer for a longer trip.
constexpr MotionSpec CameraFrame          = {GLASS_EASE_SMOOTH, 340.0f};
constexpr float      CameraFramePerOctaveMs = 40.0f; // + per doubling of distance (world units)
constexpr float      CameraFrameMaxMs       = 560.0f;

// --- Category 8: drag and drop, lists (GlassDragMotion.cpp, the Hierarchy) ---------------------
// The drag ghost trails the cursor with a slight lag and fades up as the drag starts.
constexpr MotionSpec DragGhostFollow  = {EaseId::OutCubic, 0, 0, 0, 0, 90.0f};
constexpr MotionSpec DragGhostIn      = {EaseId::OutCubic, 0, 0, 0, 0, 140.0f};
constexpr float      DragGhostOffsetPx = 14.0f; // right of and below the cursor
constexpr float      DragGhostShadowAlpha = 0.30f;
// Released on a target: it hops in along a short arc, shrinking and fading. Cancelled: it drifts home.
constexpr MotionSpec DropFly          = {EaseId::OutCubic, 0, 0, 0, 0, 260.0f};
constexpr MotionSpec DropReturn       = {GLASS_EASE_SMOOTH, 240.0f};
constexpr float      DropFlyToScale   = 0.55f;
constexpr float      DropArcLiftPx    = 18.0f;
// The Hierarchy's reorder / reparent marker glides between slots instead of jumping.
constexpr MotionSpec DropMarkerGlide  = {EaseId::OutCubic, 0, 0, 0, 0, 110.0f};

// --- Category 9: feedback moments (GlassFeedbackMotion.cpp) --------------------------------------
// Save: the check draws on, holds, fades (a keyframe clip).
constexpr float      SaveCheckDrawMs   = 240.0f;
constexpr float      SaveCheckFadeAtMs = 700.0f;
constexpr float      SaveCheckTotalMs  = 1000.0f;
// Progress: a determinate fill glides to each new value.
constexpr MotionSpec ProgressGlide     = {EaseId::OutCubic, 0, 0, 0, 0, 260.0f};
// Busy: the indeterminate segment's sweep, and the noise drifting its width and brightness.
constexpr float      BusySweepSeconds  = 1.4f;
constexpr float      BusySegment       = 0.30f;  // of the track
constexpr float      BusySegmentDrift  = 0.08f;  // +-
constexpr float      BusyBrightDrift   = 0.18f;  // alpha 0.64 .. 1.0
constexpr float      BusyNoiseHz       = 0.8f;
// Headlines (empty states, About): characters fade and rise in, one after another.
constexpr float      HeadlineCharMs     = 14.0f;  // stagger between characters
constexpr float      HeadlineCharFadeMs = 220.0f;
constexpr float      HeadlineRisePx     = 4.0f;
// A module rebuild that succeeded: a quiet toast with a check drawn on.
constexpr MotionSpec ToastCheckDraw    = {GLASS_EASE_SMOOTH, 320.0f};

#undef GLASS_EASE_SMOOTH

} // namespace Glass
