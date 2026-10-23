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
constexpr ImVec4 AxisX = Rgb(0xF5, 0x50, 0x4F);
constexpr ImVec4 AxisY = Rgb(0x5F, 0xE3, 0x5A);
constexpr ImVec4 AxisZ = Rgb(0x4D, 0x8B, 0xFF);

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

// =================================================================================================
// 4. Motion v2: bolder, still a tool (docs/EDITOR_DESIGN_SYSTEM.md §3, categories 10 to 15)
// =================================================================================================
// Everything here stays monochrome: gradients run white to transparent, and the state "tints"
// (play, prefab, viewport focus) are brightness and opacity, never hue.

// --- Category 10: theme and state moods (GlassStyleMotion.cpp) -----------------------------------
// Switching the editor theme eases the whole ImGuiStyle (iam_style_blend_to) instead of snapping.
constexpr MotionSpec ThemeTween       = {GLASS_EASE_SMOOTH, 420.0f};
// The moods ease in and out on the same curve; play is a little slower so it reads as an event.
constexpr MotionSpec MoodIn           = {GLASS_EASE_SMOOTH, 360.0f};
constexpr MotionSpec MoodOut          = {GLASS_EASE_SMOOTH, 300.0f};
constexpr MotionSpec PlayMoodIn       = {GLASS_EASE_SMOOTH, 520.0f};
// Play: panels recede (their black deepens, text drops toward secondary).
constexpr float      PlayRecedeBgAlpha   = 0.38f; // Glass panel black 0.15 -> this
constexpr float      PlayRecedeDarken    = 0.55f; // Tartarus panel rgb *= this
constexpr float      PlayRecedeText      = 0.30f; // text mixes this far toward disabled
// Viewport focus: the same, gentler, so the scene pops.
constexpr float      FocusDimBgAlpha     = 0.24f;
constexpr float      FocusDimDarken      = 0.80f;
constexpr float      FocusDimText        = 0.12f;
// Prefab mode: the chrome lifts (menu bar, tabs, headers +white, borders brighter).
constexpr float      PrefabLift          = 0.07f;
constexpr float      PrefabBorderLift    = 0.10f;
// The Game view's punch-in when play starts: 1.00 -> 1.01 -> 1.00 on a spring, with a rim glint.
constexpr MotionSpec PlayPunch        = {EaseId::Spring, 1.0f, 240.0f, 14.0f, 0.0f, 520.0f};
constexpr float      PlayPunchScale   = 0.010f;
// Toolbar buttons that don't apply in play mode fold away (width and alpha) and return on Stop.
constexpr MotionSpec ToolbarFold      = {GLASS_EASE_SMOOTH, 260.0f};

// --- Category 11: living gradients (stateless; GlassMotion.cpp) ----------------------------------
// Busy sheen: a soft white band sweeping the menu and status bars while something is working.
constexpr float      SheenSweepSeconds  = 2.4f;
constexpr float      SheenBandFraction  = 0.22f;  // of the bar's width
constexpr float      SheenAlpha         = 0.10f;
// A gradient segment travelling around a rectangle (play-mode frame, selection rim).
constexpr float      RimTravelSeconds   = 6.0f;   // play frame: one lap
constexpr float      RimTravelArc       = 0.28f;  // of the perimeter
constexpr float      RimTravelAlpha     = 0.55f;
constexpr float      SelectionRimSeconds = 9.0f;
constexpr float      SelectionRimAlpha   = 0.22f;
// Hover gradient that follows the pointer across a primary button.
constexpr float      CursorGlowRadiusPx = 56.0f;
constexpr float      CursorGlowAlpha    = 0.16f;

// --- Category 12: rolling numbers (Odometer) ------------------------------------------------------
// Only the digits that changed move: the old one slides out and fades as the new one slides in, one
// step, never counting through the values between. No overshoot, no flash.
constexpr MotionSpec DigitSlide       = {EaseId::OutCubic, 0, 0, 0, 0, 160.0f};
constexpr float      DigitSlideTravel = 0.6f; // of the line height

// --- Category 13: inertia (macOS trackpad feel) ---------------------------------------------------
// Velocity decays as e^(-friction t): a coast from v0 travels v0 / friction. 4.2/s is a long, soft
// glide; a hard flick travels several screens.
constexpr float      InertiaFriction     = 4.2f;
constexpr float      InertiaStopPxPerSec = 12.0f;  // below this the coast ends (and its entry)
constexpr float      WheelBoostWindowMs  = 140.0f; // notches closer than this accelerate
constexpr float      WheelBoostMax       = 2.6f;
constexpr float      FlingMinPxPerSec    = 220.0f; // a release slower than this doesn't coast
constexpr float      VelocitySmoothing   = 0.45f;  // EMA weight of the newest drag sample

// --- Category 14: structure (transforms, ripples, flips) ------------------------------------------
constexpr MotionSpec CardFlip         = {GLASS_EASE_SMOOTH, 380.0f};
constexpr MotionSpec FolderZoom       = {GLASS_EASE_SMOOTH, 340.0f};
constexpr float      FolderZoomFrom   = 0.86f;  // diving in: the new grid grows from this
constexpr MotionSpec Unfold           = {EaseId::Spring, 1.0f, 200.0f, 20.0f, 0.0f, 440.0f};
constexpr float      RippleStepMs     = 26.0f;  // grid ripple: per step of distance from the origin
constexpr int        RippleCap        = 18;
constexpr MotionSpec ReparentFly      = {GLASS_EASE_SMOOTH, 360.0f};
constexpr MotionSpec NodePop          = {EaseId::Spring, 1.0f, 220.0f, 15.0f, 0.0f, 480.0f};
constexpr MotionSpec WireMorph        = {EaseId::OutCubic, 0, 0, 0, 0, 180.0f};
constexpr float      WireFlowSpeed    = 1.4f;   // pulses per second along an active transition
constexpr MotionSpec LogSlide         = {EaseId::OutCubic, 0, 0, 0, 0, 220.0f};
constexpr float      LogSlidePx       = 10.0f;
constexpr MotionSpec CaretGlide       = {EaseId::OutCubic, 0, 0, 0, 0, 70.0f};
constexpr MotionSpec LineSweep        = {GLASS_EASE_SMOOTH, 700.0f};

// --- Category 15: viewport and the startup sequence ----------------------------------------------
constexpr MotionSpec GizmoHover       = {EaseId::Spring, 1.0f, 260.0f, 18.0f, 0.0f, 360.0f};
constexpr float      GizmoHoverGrow   = 0.18f;
constexpr float      GizmoActiveHz    = 2.2f;
constexpr MotionSpec OutlineSweep     = {GLASS_EASE_SMOOTH, 300.0f};
constexpr MotionSpec ViewCubeTurn     = {EaseId::Spring, 1.0f, 180.0f, 18.0f, 0.0f, 520.0f};
constexpr MotionSpec RadialOpen       = {EaseId::Spring, 1.0f, 240.0f, 17.0f, 0.0f, 360.0f};
constexpr MotionSpec RadialItem       = {EaseId::OutCubic, 0, 0, 0, 0, 120.0f};
constexpr float      RadialRadiusPx   = 92.0f;
constexpr float      RadialStaggerMs  = 18.0f;  // each item blooms this long after the previous one
constexpr float      RadialHoldMs     = 180.0f; // Space held this long opens the menu (a tap does nothing)
// Snap ticks: every snap step a gizmo drag crosses pops a small ring and the snapped number at the
// cursor (the SnapPop look, smaller); the tick sound is rate-capped so a fast drag doesn't buzz.
constexpr float      SnapTickRingPx   = 10.0f;
constexpr float      SnapTickMinMs    = 45.0f;
// Ortho <-> perspective morph: the field of view narrows to this while the camera pulls back (a
// dolly zoom that keeps the pivot's size), then the projection switches flat.
constexpr float      ViewMorphMinFovDeg = 8.0f;
// The startup sequence (signature): panels assemble in a wave from the centre, rims glint in
// order, the headline staggers in. ~1.2 s in all; input is live and any click or key settles it.
constexpr float      StartupSeconds      = 1.2f;
constexpr float      StartupPanelMs      = 520.0f; // each panel's own arrival
constexpr float      StartupStepMs       = 70.0f;  // per step of distance from the centre
constexpr float      StartupFromScale    = 0.94f;
constexpr float      StartupDropPx       = 14.0f;
// --- Decals & Graffiti (EditorLayer_DecalLibrary.cpp), the Viewport category ----------------------
// The placement ghost breathes (opacity and a hair of scale), a placed decal stamps in (its box
// settles from a little large, a ring flashes out from the hit and fades), library tiles cascade in
// and lift on hover, and a favourite's heart pops. All monochrome white.
constexpr MotionSpec DecalStampPop   = {EaseId::Spring, 1.0f, 260.0f, 14.0f, 0.0f, 460.0f}; // zeta*omega = 7
constexpr float      DecalStampFromScale = 1.18f;  // the stamped decal's box settles from this
constexpr MotionSpec DecalRingFlash  = {EaseId::OutCubic, 0, 0, 0, 0, 380.0f};
constexpr float      DecalRingPx     = 34.0f;      // the flash ring's final radius at 1x
constexpr MotionSpec DecalGhostFade  = {EaseId::OutCubic, 0, 0, 0, 0, 160.0f};
constexpr float      DecalGhostAlpha = 0.62f;      // the ghost decal's opacity
constexpr float      DecalGhostBreath = 0.14f;     // +- of that, slowly
constexpr float      DecalGhostBreathHz = 0.7f;
constexpr MotionSpec DecalTileIn     = {EaseId::OutCubic, 0, 0, 0, 0, 240.0f};
constexpr float      DecalTileStaggerMs = 16.0f;   // per tile, spreading from the first
constexpr int        DecalTileStaggerCap = 28;     // tiles past the 28th share the last delay
constexpr float      DecalTileRisePx = 8.0f;
constexpr MotionSpec DecalHeartPop   = {EaseId::Spring, 1.0f, 300.0f, 12.0f, 0.0f, 520.0f}; // zeta*omega = 6
constexpr float      DecalHeartFromScale = 0.55f;
constexpr MotionSpec DecalPreviewIn  = {EaseId::OutCubic, 0, 0, 0, 0, 180.0f};              // the hover card on its wall swatch
constexpr float      DecalPreviewDelayMs = 280.0f;  // a tile must be hovered this long before its card opens

// Shell and theme (GlassCompositor.cpp, GlassDockMotion.cpp, EditorLayer_Glass.cpp).
// Each panel's own arrival curve; it fades in over the first StartupFadeShare of it. A panel's
// delay is its distance from the screen centre (0 at the centre, 1 at a corner) times the steps
// that fit in StartupSeconds after the last panel's own arrival.
constexpr MotionSpec StartupPanelIn      = {GLASS_EASE_SMOOTH, StartupPanelMs};
constexpr float      StartupFadeShare    = 0.6f;
// As a panel lands its rim catches the light (extra white over a 1 px edge), one after another.
constexpr float      StartupRimBoost     = 0.38f;
constexpr float      StartupRimAtShare   = 0.45f;  // of its arrival, when the glint starts
// The headline (the editor's name, the scene below it) staggers in over the viewport, holds,
// and fades before the sequence ends.
constexpr float      StartupHeadlineAt   = 0.10f;  // s
constexpr float      StartupSublineAt    = 0.30f;  // s
constexpr float      StartupHeadlineOutAt = 0.95f; // s
constexpr float      StartupHeadlineOutS = 0.40f;
constexpr float      StartupHeadlineSize = 30.0f;  // px at 1x
// A first frame slower than this (shaders compiling, a scene loading) restarts the clock, so the
// sequence plays once frames flow instead of finishing unseen.
constexpr float      StartupHitchSeconds = 0.10f;
// The play-mode rim's travelling segment while paused, relative to playing.
constexpr float      RimTravelPausedScale = 0.45f;
// A toast that leaves: the ones above restack on a spring (zeta = 19 / (2 sqrt(210)) = 0.66).
constexpr MotionSpec ToastRestack        = {EaseId::Spring, 1.0f, 210.0f, 19.0f, 0.0f, 460.0f};
// The rebuild toast's busy ring: one turn per this many seconds, an arc of this share of it.
constexpr float      ToastBusyTurnSeconds = 0.9f;
constexpr float      ToastBusyArc         = 0.28f;

#undef GLASS_EASE_SMOOTH

} // namespace Glass
