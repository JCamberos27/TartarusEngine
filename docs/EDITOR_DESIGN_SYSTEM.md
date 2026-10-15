# Glass: the editor design system

Glass is an opt-in, macOS-inspired look for the Tartarus editor. It covers a palette, a spacing
law, a motion language, a complete `ImGuiStyle`, three Glass Workspace panels, frosted viewport HUDs
and the GPU path behind them. The default Tartarus theme is unchanged; nothing in this document
runs until you switch to Glass.

**Turn it on:** *Preferences > General > Appearance > Editor theme > Glass*. Two more options
appear there:

* *Viewport HUDs* shows the floating selection card, stats pill and notifications over the Scene view.
* *Frosted blur* turns the blur behind those HUDs on or off. With it off they use a flat tint.

*Window > Glass Workspace* toggles the Objects, Properties and Library panels and the HUDs.
The setting is stored as `glassTheme` in the editor preferences.

| Where | What |
| --- | --- |
| `src/Editor/Glass/GlassTokens.h` | Every number in this document: palette, spacing, radii, type, motion. |
| `src/Editor/Glass/GlassTheme.{h,cpp}` | `ConfigureMacEngineTheme`, linear-space colour prep, wallpaper, squircles, frosted plates. |
| `src/Editor/Glass/GlassMotion.{h,cpp}` | `ImAnimContext`: the zero-at-rest animation map in front of ImAnim. |
| `src/Editor/Glass/GlassLayout.{h,cpp}` | Clip guard, animated-height reveals, staggered rows, the input policy, `MotionButton`. |
| `src/Editor/EditorLayer_Glass.cpp` | The panels, HUDs, notifications and theme lifecycle. |
| `src/Renderer/GlassBackdrop.{h,cpp}` and `shaders/Glass*.comp.glsl` | The frosted-glass compute pipeline. |
| `src/Tests/UnitTests_Glass.cpp`, `tests/smoke-scenes/smoke_glass.json` | Tests. |

ImAnim ([soufianekhiat/ImAnim](https://github.com/soufianekhiat/ImAnim), MIT) is fetched by CMake
at a pinned commit and compiled with the other vendored ImGui sources.

---

## 1. Palette: three layers

All values are sRGB. "Prepared" is the colour actually written to the `ImGuiStyle`; see
[Linear-space blending](#linear-space-blending).

### Layer 1: the wallpaper canvas

The deepest neutral, behind every floating element. `Glass::DrawWallpaper()` paints it once per
frame into ImGui's background draw list as a faint vertical gradient, so docking gaps and the
space around floating windows read as one continuous desk.

| Token | HEX | RGBA (0-255) | Use |
| --- | --- | --- | --- |
| `WallpaperTop` | `#141417` | 20, 20, 23, 255 | Top of the gradient |
| `WallpaperBottom` | `#0B0B0D` | 11, 11, 13, 255 | Bottom of the gradient |
| `Wallpaper` | `#0E0E10` | 14, 14, 16, 255 | Solid reference for contrast and linear prep |

### Layer 2: translucent glass surfaces

Each surface is specified the way a designer specifies one: a tint plus an opacity.

| Token | Tint | Opacity | Prepared RGBA | Used for |
| --- | --- | --- | --- | --- |
| `SurfacePanel` | `#1C1C1E` | 0.80 | `#1D1D1F` @ 0.80 | Docked panels (`WindowBg`, `ChildBg`) |
| `SurfaceRaised` | `#2C2C2E` | 0.74 | `#2F2F31` @ 0.74 | Headers, cards, tabs |
| `SurfaceFloat` | `#262629` | 0.86 | `#27272A` @ 0.86 | Popups, menus, tooltips |
| `SurfaceHud` | `#1E1E21` | 0.58 | `#202023` @ 0.58 | Viewport HUDs, over the blur |
| `SurfaceTitle` | `#161618` | 0.90 | `#161618` @ 0.90 | Title, menu and status bars |

Washes drawn *on* a surface are plain white at low alpha, so they lighten any surface by the same
perceived step:

| Token | RGBA | Use |
| --- | --- | --- |
| `Field` / `FieldHover` / `FieldActive` | `#FFFFFF` @ 0.055 / 0.085 / 0.115 | Input boxes, sliders, combos |
| `WashHover` / `WashPressed` | `#FFFFFF` @ 0.060 / 0.100 | Row and button hover and press |
| `Hairline` | `#FFFFFF` @ 0.080 | Borders: a light edge, never a dark outline |
| `Separator` | `#FFFFFF` @ 0.100 | Separators, resize grips |
| `GlassEdge` | `#FFFFFF` @ 0.140 | Top-lit rim of a frosted HUD |
| `Shadow` | `#000000` @ 0.450 | Soft card shadows |

### Layer 3: typography and the accent

| Token | HEX | Use |
| --- | --- | --- |
| `TextPrimary` | `#F5F5F7` | Off-white labels and values |
| `TextSecondary` | `#A1A1A6` | Dusk grey: metadata, captions, unhovered rows |
| `TextTertiary` | `#6E6E73` | Disabled and placeholder text |
| `Accent` | `#0A84FF` | **The** accent: selection, active-tab underline, focus, critical gizmo states |
| `AccentBright` | `#409CFF` | Hover or press on an accent fill; the active gizmo handle |
| `AccentWash` | `#0A84FF` @ 0.24 | Selected row body |
| `AccentGlow` | `#0A84FF` @ 0.45 | Focus ring, drop target |
| `OnAccent` | `#FFFFFF` | Text on an accent fill |

The accent is one hue at different strengths and is never used decoratively. Status colours have
fixed meanings and never stand in for it: `Danger #FF453A`, `Warning #FFD60A`, `Success #30D158`.
Axis colours for vector rows are `X #FF5F57`, `Y #28C840` and `Z #5AA9FF`. Z is deliberately lighter
and cooler than the accent so that "Z" and "selected" never look alike.

While Glass is active, ImGuizmo's selection colour is set to the Glass `AccentBright`. It goes
back to the Tartarus accent when you switch away.

### Linear-space blending

The ImGui OpenGL backend blends in the framebuffer's encoded (sRGB) space; the UI pass never
enables `GL_FRAMEBUFFER_SRGB`. An 80% grey glass over a near-black wallpaper blended in encoded
space lands darker and muddier than the physically correct linear-light composite.

`Glass::LinearCorrectOver(tint, a, backdrop)` solves for the colour `C'` that makes ImGui's blend
`a·C' + (1−a)·B` equal `Encode(a·Linear(C) + (1−a)·Linear(B))` over the wallpaper. Every surface
colour in the style array goes through it (`Glass::Resolve`), so the alphas in `ImGuiCol_*` are
the designer's opacities and the RGB is pre-compensated. Over the wallpaper the correction is
exact. Over anything else, such as a 3D view behind a floating window, it errs in the right
direction.

`ApplyThemeStyle` checks, whenever the theme is applied, that the text tokens clear WCAG contrast
against the *composited* panel and raised surfaces (`CompositeOnWallpaper`). A failure is logged
as a warning with the prefix `[Glass theme]`. The unit tests enforce the same floors.

---

## 2. Spacing law and rounding

**One unit, U = 8 px** at 1x UI scale:

```
Space(k) = k · U · uiScale        k ∈ {0, 0.5, 1, 1.5, 2, 3, 4}
```

| Token | px @1x | Use |
| --- | --- | --- |
| `SpaceHalf` | 4 | **Inside a control only**: vertical inset of a cell, icon-to-label gap |
| `Space1` | 8 | Between related items (`ItemSpacing`) |
| `Space1h` | 12 | A control's horizontal inset (`FramePadding.x`) |
| `Space2` | 16 | Window padding, between groups, one tree indent |
| `Space3` | 24 | Between sections |
| `Space4` | 32 | Outer margin of floating HUDs from the viewport edge |

Nothing is ever 3, 5, 6 or 10 px. Every length goes through `Glass::Px()` or `ScaleAllSizes`, so the
law holds at every UI scale.

**Radii.** Squircle-like corners of 10 to 12 px:

| Token | px | Applies to |
| --- | --- | --- |
| `RadiusWindow` | 12 | Windows, popups |
| `RadiusCard` | 12 | Child windows, Inspector cards, HUDs, asset tiles |
| `RadiusControl` | 10 | Frames, buttons, tabs, scrollbars |
| `RadiusGrab` | 10 | Slider grabs |
| `RadiusRow` | 8 | A selection panel inside a list (4 px inset from a 12 px card) |

Nested shapes are concentric: `Concentric(outer, inset) = max(outer − inset, RadiusRow)`, so a
card's corners stay parallel to its window's corners.

ImGui's own widgets round with circular arcs. Surfaces the Glass layer draws itself (cards, tiles,
selection panels, HUDs) use `DrawSquircle`. Its corners are superellipse arcs with exponent 5,
which is close to Apple's continuous corner. The radius is scaled by 1.28 so the corner covers
about the same area as a circular one, so the two kinds of shape sit together without a visible
tangent break.

**Type roles** (baked font sizes at 1x): Body 15, Caption 13, Headline 13 (section headers, set in
the UI face, not caps), Title 20.

---

## 3. Motion

All motion is ImAnim (`iam_ease_*`). The springs are ImAnim's `iam_ease_spring`, which evaluates
over *normalised* time `u = t / duration`. Mass, stiffness and damping below are therefore in
per-duration units: the physical angular frequency is `√(k/m) / duration`. Every spring is tuned
so that `ζ·ωₙ ≥ 5`, which means its envelope is under 1% by `u = 1` and the tween's final snap to
the target is invisible.

The "smooth" curve is the macOS-style cubic bezier `(0.32, 0.72, 0.00, 1.00)`: a fast start and a
long, soft landing.

| Category | Interaction | Easing | Spring m / k / c | Duration (ms) | Visual delta bounds |
| --- | --- | --- | --- | --- | --- |
| **1. Navigation hovers** | Row / tile hover in | `OutCubic` | n/a | 140 | Row wash alpha 0 → 0.06; text Secondary → Primary (OKLAB mix) |
| | Hover out | `OutCubic` | n/a | 280 | Back to rest; the slower release reads as a trail |
| | Asset tile hover | `OutCubic` | n/a | 140 in / 280 out | Scale 1.00 → 1.03; lift 0 → 2 px; shadow alpha 0 → 0.35 |
| | Selection panel glide | smooth bezier | n/a | 220 | Panel slides row to row; 3 px accent bar rides with it |
| | **Velocity gate** | none | n/a | n/a | While the pointer moves faster than **1800 px/s** (EMA weight 0.35), unlit rows don't start a hover-in. No channel is created, so there's no comet trail |
| **2. Structural reveals** | Accordion open | smooth bezier | n/a | 320 | Height 0 → natural height (re-measured every frame) |
| | Accordion close | smooth bezier | n/a | 240 | Natural height → 0 |
| | Chevron | smooth bezier | n/a | 240 | Rotation 0° → 90° |
| | **Layout-aware stagger** (rows) | `OutCubic` | n/a | 200 per row, +18 per index | Alpha 0 → 1; settle +6 px → 0; delay capped at the 8th row |
| | Asset grid wave | `OutCubic` | n/a | 260 per tile, +22 per diagonal step | Alpha 0 → 1; rise +8 px → 0; delay capped at 24 steps |
| | Inspector card cascade | `OutCubic` | n/a | 200 per card, +18 per index | Same as rows: fade and settle |
| **3. Notifications** | Toast elastic pop | spring | **1 / 180 / 10** (ζ ≈ 0.37) | 620 | Scale 0.92 → 1.00 with ~28% overshoot; rise 12 px |
| | Toast exit | `InCubic` | n/a | 180 | Alpha 1 → 0, scale −4%; auto-dismiss after 4200 ms (paused while hovered) |
| | Grid snap pop | spring | **1 / 260 / 14** (ζ ≈ 0.43) | 420 | Accent ring at the snap point, scale 0.60 → 1.00, fading out |
| | Compile-fail shake | spring impulse (real time) | **1 / 1600 / 16** (ζ = 0.2, f ≈ 6.4 Hz) | 520 | Horizontal ±10 px, decaying to 0 |

The shake is not a tween. `Glass::SpringImpulse(t, m, k, c, A)` is a pure function of the time since
the error, so it holds no animation state at all.

*Reduce motion* (the editor's existing preference) resolves every tween instantly and skips the
pops and the shake.

**Triggers.**

* *Compile failure toasts.* The HUD watches the log. A new error that starts with `C# error:`, or
  contains `failed to compile` or `compile error`, becomes a red toast that pops and shakes. It
  only reacts to errors logged after the HUD first appears, never to the backlog.
* *Grid snap.* Ending a gizmo drag with snapping on plays the snap pop at the cursor.
* *Toast limits.* At most 4 toasts show at once. Click a toast to dismiss it.

---

## 4. `ConfigureMacEngineTheme`

```cpp
void ConfigureMacEngineTheme(ImGuiStyle& style); // GlassTheme.h
```

Call it on a default-constructed `ImGuiStyle`, then apply `ScaleAllSizes(uiScale)` once.
`EditorLayer::ApplyThemeStyle` does exactly that. It sets:

* **Metrics at 1x.**
  * Padding and spacing: `WindowPadding` 16,16; `FramePadding` 12,8; `ItemSpacing` 8,8;
    `ItemInnerSpacing` 8,4; `CellPadding` 8,4; `IndentSpacing` 16.
  * Sizes: `ScrollbarSize` 12; `GrabMinSize` 12.
  * Rounding: windows, popups and children 12; frames, tabs, scrollbars and grabs 10.
  * Borders: a 1 px hairline on windows; no frame borders and no tab-bar rule.
  * `DisabledAlpha` 0.42.
* **The whole `ImGuiCol_` array.** The unit tests check that every slot was written and none is
  left at the sentinel. Surfaces are linear-prepared as described above. Text, accent and washes
  come straight from the tokens. Tabs read as raised glass with the accent underline on the active
  tab. Docking preview, nav highlight, drag-drop target and text selection all use the one accent.

The reloadable editor module (`TartarusEditor.dll`) doesn't include any Glass headers. It inherits
the Glass style through the shared ImGui context. Its hand-coded `EditorTheme` fills stay in the
Tartarus palette (see [Limitations](#limitations)).

---

## 5. The Glass Workspace panels

All three panels are built on `ImGui::BeginTable` with an `ImGuiListClipper`. None uses legacy
columns. They share the editor's selection, undo, drag-and-drop payloads and context menus, so they
can be used next to, or instead of, the classic panels.

### Objects (Scene Hierarchy)

* **Rows.** Full-width rows (`Selectable` spanning all columns). The primary selection is a squircle
  accent-wash panel with a 3 px accent bar, which *glides* from row to row (`Follow`, 220 ms) rather
  than jumping. Hover is a white wash plus an OKLAB text blend from secondary to primary, behind
  the velocity gate.
* **Expanding.** Chevrons rotate with `AnimateToggle`. Selecting an entity elsewhere auto-expands
  its ancestors. A right-hand column shows each entity's child count.
* **Search.** Filtering switches to a flat, scene-ordered list.
* **Interaction.**
  * Click, Ctrl-click and Shift-click range select. Double-click frames the selection.
  * Drag a row to the Scene view or into fields. The row is the editor's `HIERARCHY_ENTITY` payload.
  * Right-click opens the same menu as the classic Hierarchy.

### Properties (Accordion Inspector)

* **Cards.** Each component is a squircle card whose header has a rotating chevron. The body opens
  with an animated-height reveal (320 ms open, 240 ms close). Its rows cascade in with the
  layout-aware stagger. A new selection plays a card entrance cascade.
* **Identity card.** The name edits live with one undo step per rename. It also shows the
  component count and a caption when several objects are selected.
* **Transform card.** A flat 4-column table: label, then X, Y and Z drag fields, each with a 3 px
  red, green or blue axis bar. Non-finite input is rejected. Edits are undoable as Move, Rotate and
  Scale.
* **Reflected components.** These use the same field drawers as the classic Inspector, so every
  reflected field, attribute and custom C# inspector works. Right-click a card header to *Reset*
  or *Remove* it; both are undoable. The *Add Component* menu sits at the bottom.

### Library (Asset Browser grid)

* **Grid.** Responsive: the column count follows the panel width, on a fixed-size table. The
  breadcrumb's last crumb carries the accent underline.
* **Wave reveal.** Changing folder plays a diagonal wave. Tiles fade and rise in by row plus column,
  22 ms apart and capped at 24 steps.
* **Tiles.** Hovering lifts, scales and shadows a tile. Selected tiles get a Finder-style accent
  label pill. Thumbnails come from the same caches as the classic Asset Browser: textures, models,
  materials, and the unloaded-asset loader.
* **Interaction.** Selection, Shift and Ctrl select, sound preview, double-click actions and drag
  payloads all match the Asset Browser. Folders accept drops.

### Viewport HUDs

These are floating frosted-glass plates over the Scene view. They are hidden for viewports under
200×120 and in clean captures.

* **Selection card** (bottom right). Shows the glyph, name, component count (or how many are
  selected) and the position with axis-coloured X/Y/Z. It fades in and out with
  the selection; click it to frame the selection.
* **Stats pill.** Object count, fps and frame time, above the selection card.
* **Toasts** (bottom centre). Elastic pop, exit, restack glide and error shake.
* **Snap ring.** Played when a snapped gizmo drag ends.

---

## 6. Architecture

### 6.1 Frosted glass on OpenGL 4.6

The HUD glass shows a real blur of the 3D scene behind it, produced entirely in compute
(`GlassBackdrop`):

```
Scene colour (RGBA8, display-encoded, viewport size)
  │  1. Downsample  src → D0 → … → D(n−1)   13-tap (Jimenez) filter per level,
  │                                          sRGB decoded to linear on the first read  [RGBA16F mips, half res]
  │  2. Upsample    D(n−1) → U(n−2) → … → U0 3×3 tent per level, pure blur (no add-back) [RGBA16F]
  │  3. Resolve     U0 → Out                 linear → sRGB encode, saturation ×1.12   [RGBA8, half res]
  ▼
Glass::DrawFrostedPlate: samples Out with UVs mapped from the plate's screen rect into the
viewport, so each plate shows exactly the blurred scene behind it. Then the SurfaceHud tint,
the GlassEdge rim lit from the top, and a soft shadow.
```

* **Source.** No extra copy is needed: the editor already resolves the tonemapped scene into its
  own texture (`SetSceneTexture`).
* **Levels.** Up to 5, clamped so the smallest level stays at least 4 px on its short side. That
  stops the blur "breathing" as the viewport resizes.
* **Cost.** Each level is a quarter of the one above, so the whole chain costs about 1.4× its first
  downsample. It shows as **"Glass Backdrop"** in the Statistics GPU scopes. It hasn't been
  profiled on the 1080 Ti yet.
* **Barriers.** `glMemoryBarrier(TEXTURE_FETCH | SHADER_IMAGE_ACCESS)` runs between passes. No pass
  reads the mip level it writes.
* **State.** Afterwards the image and texture unit 0 are unbound and the GL state cache is
  invalidated.
* **Lifetime.** It runs only on frames where a frosted HUD is visible and *Frosted blur* is on. It
  holds no GPU memory until first used and frees everything when the theme is switched off.
* **Fallback.** If a shader fails to compile, one error is logged and the HUDs fall back to a flat
  tint.

### 6.2 `ImAnimContext`: zero memory at rest

ImAnim keeps a tween channel alive for as long as something calls it, and a settled channel that
is touched every frame is never collected. Calling `iam_tween_*` for every row on every frame would
pin one channel per widget for the life of the panel. `Glass::ImAnimContext` (`Glass::Motion()`)
sits in front of ImAnim, keyed by `ImGuiID`:

* **At rest, nothing exists.** `Animate(id, target, rest, spec)` returns `target` directly and makes
  no ImAnim call. No map entry exists.
* **Departing from rest** creates an entry and an ImAnim channel.
* **Landing on rest** erases the entry the same frame and stops calling ImAnim. `iam_gc()` (every
  30 frames) reclaims the orphaned channel.
* **Abandoned entries.** Entries untouched for 2 frames are swept, for example when a window closes
  mid-animation.
* **Empty map.** Its buffer is freed. After 90 idle frames, ImAnim's pools are released too
  (`iam_pool_clear`).
* **Timers** (`Pulse`, used for staggers and toasts) follow the same rule. A missing timer reads as
  "finished".

The result is that an idle editor holds **0 bytes** of animation state. `LiveCount()` and
`HeldBytes()` expose this, and both the unit tests and `smoke_glass` assert it. Switching the theme
off calls `Reset()`.

Helpers on top of the map:

* `Follow` glides to a moving target.
* `AnimateToggle` handles 0..1 openness around a flip.
* `HoverGlow` is the hover tween with the velocity gate.
* `Aux` stores per-entry scratch, such as a reveal's measured height.
* `MixOklab` and `EaseProgress` blend colours and ease progress with ImAnim's own evaluators.

### 6.3 Clipping guard during height animation

`Glass::BeginReveal(id, open, wasOpen)` and `EndReveal()`:

1. The body is laid out at its **full natural height**, which is measured every frame so that
   content growing mid-animation is followed rather than snapped.
2. The reveal reserves only the **animated height** in the parent layout. Everything below moves
   with the animation, so what you see is where it is.
3. A `Glass::ClipGuard` (RAII `ImGui::PushClipRect` / `PopClipRect`, always intersecting with the
   parent's rect) the size of the animated region wraps the body. Text and widgets never bleed
   past the edge or pop at the end.

A closed reveal at rest returns `false` and its body isn't submitted at all.

`RevealRow(index)` adds the stagger. It shifts the layout cursor (not just the drawing) and pushes
alpha, then undoes both after the row, so the rows after it are unaffected.

### 6.4 Input responsiveness policy

Elements stay clickable and hit-accurate while they animate:

* **Clipped means not hoverable.** ImGui hit-tests against the current clip rect, so the part of
  a revealing body outside the `ClipGuard` cannot be hovered or clicked. Nothing invisible ever
  takes a click.
* **Size-changing motion moves the real layout.** Reveals and staggered rows offset the cursor, so
  the hit rect *is* the drawn rect on every frame.
* **Transform-only motion uses a union hit rect.** For a lift, scale pop or shake, `MotionButton`
  registers `MotionHitRect(rest, drawn)`: the union of where the item rests and where it is drawn.
  A tile that grows on hover stays hovered at its edge instead of flickering as the hover changes
  its own size.
* **Clicks are never blocked or deferred.** In a layout that is moving this frame,
  `MotionButtonFlags` swaps the usual `PressedOnClickRelease` for
  `PressedOnClickReleaseAnywhere`. A press followed by a release still counts even if the button
  glided out from under the cursor between the two.
* **No input gating while animating.** There is no "busy" state. A row can be clicked mid-glide
  and a card edited mid-reveal.
* **Viewport picking.** The HUDs are real ImGui items, so the viewport's click-to-pick (which
  ignores clicks while `IsAnyItemHovered`) never selects through a HUD.

---

## 7. Testing

**Unit tests** (`--unit-tests`, `UnitTests_Glass.cpp`):

* Every style slot is written and the metrics sit on the 8 px grid.
* WCAG contrast of the text tokens on the composited surfaces.
* The linear-blend algebra round-trips.
* `ImAnimContext` holds 0 entries and 0 bytes at rest, before and after a hover, pulse or toggle,
  including under Reduce motion. Abandoned entries are swept.
* The velocity gate blocks a fast sweep and allows a slow one.
* Reveal: the layout follows the animated height, and the clipped last row is not hovered.
* `MotionButton`: the union rect, the press flags in motion, and a press that still lands after a
  glide.
* The springs settle within their durations, and the stagger delays cap.

**Smoke test** (`--smoke-test`, `tests/smoke-scenes/smoke_glass.json`), in order of frame:

| Frame | Action |
| --- | --- |
| 15 | Select an object and switch the editor to Glass |
| 20 | Fire a snap pop and a warning |
| 70 | Check that the theme is live and the frosted backdrop produced a texture whenever the viewport is visible |
| 70–80 | Switch back to Tartarus |
| 80 | Check that the animation map is empty and holds 0 bytes |

The test restores every preference it touched.

---

## Limitations

* **Module panel fills.** The reloadable editor module draws about 27 explicit `EditorTheme`
  fills. These keep the Tartarus palette inside an otherwise Glass style.
* **Objects panel.** It doesn't accept reparent drops yet. Use the classic Hierarchy for that.
* **Library panel.** It has no context menu, and follows the Asset Browser's current folder and
  search filter.
* **Blur cost.** The frosted blur re-runs every frame while a HUD shows. It doesn't yet skip frames
  where the scene didn't change.
* **Translucency.** Panel translucency reads against the wallpaper only. The editor doesn't use
  ImGui multi-viewports, so windows are not see-through to the OS desktop.
