# Black Glass: the editor design system

Black Glass is an opt-in look for the Tartarus editor with one rule: **things that float are glass,
things you work in are solid.** Docked panels, the menu bar and the status bar are solid black.
Everything that floats over them (menus, popups, combo lists, context menus, tooltips, modals,
undocked windows, the viewport toolbar, tool rail, overlays, HUDs and notifications) is real
blurred glass over whatever is behind it. The chrome is monochrome: white text on black, with colour
coming only from content (the scene) and from colours that carry meaning (axes, asset kinds, status).

It covers a palette, a spacing law, a motion language, a complete `ImGuiStyle`, a compositor that
puts glass under every floating ImGui window, three Glass Workspace panels and the viewport HUDs.
The default Tartarus theme is unchanged; nothing in this document runs until you switch to it.

**Turn it on:** *Preferences > General > Appearance > Editor theme > Black Glass*. Two more options
appear there:

* *Frosted blur* blurs what is behind every glass surface. With it off, glass is an opaque near-black.
* *Viewport HUDs* shows the selection card, stats pill and notifications over the Scene view.

*Window > Glass Workspace* toggles the Objects, Properties and Library panels and the HUDs.
The setting is stored as `glassTheme` in the editor preferences.

| Where | What |
| --- | --- |
| `src/Editor/Glass/GlassTokens.h` | Every number in this document: palette, glass material, spacing, radii, type, motion. |
| `src/Editor/Glass/GlassTheme.{h,cpp}` | `ConfigureMacEngineTheme`, the canvas, squircles, glass HUD plates. |
| `src/Editor/Glass/GlassCompositor.{h,cpp}`, `GlassSurfaces.cpp` | Glass under every floating window, and the arrival motion. |
| `src/Editor/Glass/GlassMotion.{h,cpp}` | `ImAnimContext`: the zero-at-rest animation map in front of ImAnim. |
| `src/Editor/Glass/GlassLayout.{h,cpp}` | Clip guard, animated-height reveals, staggered rows, the input policy, `MotionButton`. |
| `src/Editor/EditorLayer_Glass.cpp` | The panels, HUDs, notifications and theme lifecycle. |
| `src/Renderer/GlassBackdrop.{h,cpp}` and `shaders/Glass*.comp.glsl` | The blur compute pipeline. |
| `src/Tests/UnitTests_Glass.cpp`, `tests/smoke-scenes/smoke_glass.json` | Tests. |

ImAnim ([soufianekhiat/ImAnim](https://github.com/soufianekhiat/ImAnim), MIT) is fetched by CMake
at a pinned commit and compiled with the other vendored ImGui sources.

---

## 1. Palette: two materials, no in-between

All values are sRGB.

### Solid black

Where you work. The steps are the default Tartarus theme's (`EditorTheme.h`), so both themes share
one black. `Glass::DrawWallpaper()` paints `Base` behind everything each frame.

| Token | HEX | Use |
| --- | --- | --- |
| `Base` | `#000000` | Menu bar, status bar, dock gaps, unselected tabs, the canvas |
| `Panel` | `#050505` | A docked panel (`WindowBg`), the selected tab |
| `Card` | `#090909` | A card inside a panel (Inspector component bodies) |
| `Raised` | `#0E0E0E` | Headers, table headers |
| `Hover` / `Pressed` | `#1C1C1C` / `#2A2A2A` | Tab hover and press |
| `Hairline` | `#242424` | 1 px borders and rules on solid surfaces |

### Glass

What floats. Every glass surface is three layers:

| Layer | Value | Notes |
| --- | --- | --- |
| Backdrop | The blur of whatever is behind it | 6 half-resolution levels deep: no shape survives, only colour fields. Saturation ×0.85; highlights capped at encoded luminance 0.52 |
| `GlassTint` | `#000000` @ 0.25 | Over the backdrop |
| `GlassEdge` | `#FFFFFF` @ 0.15 | 1 px border |
| `GlassRadius` | 14 px | Corners of floating surfaces. Docked panels are square |
| `GlassFallback` | `#141414` | Opaque stand-in where there is no blur (*Frosted blur* off, a shader failure, a window dragged out to its own OS window) |

The highlight cap keeps white text legible on glass over anything: the brightest a backdrop can
be is 0.52, and under the tint that is `#636363`, which still gives `TextPrimary` 4.5:1.

Washes drawn on a surface are plain white at low alpha, so they lighten black and glass alike:

| Token | RGBA | Use |
| --- | --- | --- |
| `Field` / `FieldHover` / `FieldActive` | `#FFFFFF` @ 0.055 / 0.085 / 0.115 | Input boxes, sliders, combos |
| `WashHover` / `WashPressed` | `#FFFFFF` @ 0.060 / 0.100 | Row and button hover and press |
| `Separator` | `#FFFFFF` @ 0.100 | Separators drawn by the Glass panels |
| `Shadow` | `#000000` @ 0.450 | Soft shadows under floating surfaces |

### Text and the accent

| Token | HEX | Use |
| --- | --- | --- |
| `TextPrimary` | `#E6E8E6` | Labels and values |
| `TextSecondary` | `#9C9E9C` | Metadata, captions, unhovered rows |
| `TextTertiary` | `#6A6C6A` | Disabled and placeholder text |
| `Accent` | `#F4F6F4` | Selection, the active tab's line, focus, "on" |
| `AccentBright` | `#FFFFFF` | Hover or press on an accent fill; the active gizmo handle |
| `AccentWash` | `#FFFFFF` @ 0.12 | Selected row body |
| `AccentGlow` | `#FFFFFF` @ 0.40 | Focus ring, drop target |
| `OnAccent` | `#000000` | Text on an accent fill (inverse video) |

There is no accent hue. Status colours have fixed meanings: `Danger #E85C54`, `Warning #E6A450`,
`Success #7CD88C`. Axis colours are the editor's own: `X #E5484D`, `Y #4CC38A`, `Z #4C8DF6`.

While Black Glass is active, ImGuizmo's selection colour is `AccentBright`. It goes back to the
Tartarus accent when you switch away.

`ApplyThemeStyle` checks, whenever the theme is applied, that the text tokens clear WCAG contrast
on the solid surfaces and on `GlassFallback`. A failure is logged as a warning with the prefix
`[Glass theme]`. The unit tests enforce the same floors, plus the worst case over glass.

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

**Radii.** 14 px on floating surfaces, smaller and nested inside them:

| Token | px | Applies to |
| --- | --- | --- |
| `RadiusWindow` (`GlassRadius`) | 14 | Floating windows, popups, menus, tooltips, HUDs |
| `RadiusCard` | 10 | Child windows, Inspector cards, asset tiles |
| `RadiusControl` | 8 | Frames, buttons, tabs, scrollbars |
| `RadiusGrab` | 8 | Slider grabs |
| `RadiusRow` | 6 | A selection panel inside a list (4 px inset from a 10 px card) |

Docked panels are attached to an edge and stay square.

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
| | Selection panel glide | spring | **1 / 170 / 16** (ζ ≈ 0.61) | 380 | Panel slides row to row and lands with a ~9% overshoot; 3 px accent bar rides with it |
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
| **4. Arrivals** (every window, `GlassCompositor`) | Menu, combo list, context menu | `OutCubic` | n/a | 200 | Alpha 0 → 1; scale 0.96 → 1.00 from its top edge; settles down from 6 px above |
| | Tooltip | `OutCubic` | n/a | 120 | Alpha 0 → 1 |
| | Modal or undocked window | spring + `OutCubic` | **1 / 220 / 20** (ζ ≈ 0.67) | 460 (fade 180) | Scale 0.94 → 1.00 about its centre with a ~6% overshoot; alpha 0 → 1 |
| | Docked panel shown (tab switch, open) | `OutCubic` | n/a | 220 | Content alpha 0 → 1, rising 6 px; the panel's own background stays put |
| | Rim glint | `OutCubic` | n/a | 700 | A glass surface's edge starts 35% brighter and cools to 15% white |

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
* *Arrivals.* A surface arrives the first frame it is drawn after not being drawn: a menu opening,
  a tooltip appearing, a panel's tab being selected, every panel at launch.

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
  * Rounding: floating windows and popups 14; children 10; frames, tabs, scrollbars and grabs 8.
  * Borders: a 1 px hairline on windows and popups (15% white once they are glass); no frame
    borders and no tab-bar rule. A 2 px seam between docked panels.
  * `DisabledAlpha` 0.42.
* **The whole `ImGuiCol_` array.** The unit tests check that every slot was written, none is left
  at the sentinel, and every slot is a grey (no hue in the chrome). Docked panels are `Panel`; the
  canvas, menu bar and resting tabs are `Base`; the selected tab joins its panel and carries a 1 px
  white line. Floating windows and popups are written *solid* (`WindowBg` `Panel`, `PopupBg`
  `GlassFallback`, title bars the same colour as the body, borders `Hairline`), because the
  compositor finds exactly those colours on each floating window and replaces them with glass
  (see 6.1). Anywhere it doesn't run, they are the right fallback.

The reloadable editor module (`TartarusEditor.dll`) doesn't include any Glass headers. It inherits
the Glass style through the shared ImGui context. Its hand-coded `EditorTheme` fills stay in the
Tartarus palette (see [Limitations](#limitations)).

---

## 5. The Glass Workspace panels

All three panels are built on `ImGui::BeginTable` with an `ImGuiListClipper`. None uses legacy
columns. They share the editor's selection, undo, drag-and-drop payloads and context menus, so they
can be used next to, or instead of, the classic panels.

On first open each panel docks as a selected tab next to its classic counterpart (Objects with the
Hierarchy, Properties with the Inspector, Library with the Asset Browser); after that, wherever you
put it is remembered. A panel you undock becomes glass, like every other floating window.

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

These are glass plates over the Scene view, drawn inside it with the blurred scene behind them. They are hidden for viewports under
200×120 and in clean captures.

* **Selection card** (bottom right). Shows the glyph, name, component count (or how many are
  selected) and the position with axis-coloured X/Y/Z. It fades in and out with
  the selection; click it to frame the selection.
* **Stats pill.** Object count, fps and frame time, above the selection card.
* **Toasts** (bottom centre). Elastic pop, exit, restack glide and error shake.
* **Snap ring.** Played when a snapped gizmo drag ends.

---

## 6. Architecture

### 6.1 Glass on OpenGL 4.6

ImGui has no blur, so the glass needs two pieces: a blur, and something that puts it under each
floating window at the right moment.

**The blur (`GlassBackdrop`).** A compute pyramid, entirely on the GPU:

```
Source (RGBA8, display-encoded)
  │  1. Downsample  src → D0 → … → D(n−1)   13-tap (Jimenez) filter per level,
  │                                          sRGB decoded to linear on the first read  [RGBA16F mips, half res]
  │  2. Upsample    D(n−1) → U(n−2) → … → U0 3×3 tent per level, pure blur (no add-back) [RGBA16F]
  │  3. Resolve     U0 → Out                 linear → sRGB, saturation ×0.85,
  │                                          highlights capped at luminance 0.52         [RGBA8, half res]
  ▼
Out, sampled with UVs mapped from a surface's screen rect, so it shows exactly what is behind it.
```

* **Levels.** 6, clamped so the smallest level stays at least 4 px on its short side. That stops
  the blur "breathing" as the window resizes.
* **Cost.** Each level is a quarter of the one above, so the chain costs about 1.4× its first
  downsample. It shows as **"Glass Backdrop"** (and the copy before it as **"Glass Capture"**) in
  the Statistics GPU scopes. Not yet profiled on the 1080 Ti.
* **Barriers and state.** `glMemoryBarrier(TEXTURE_FETCH | SHADER_IMAGE_ACCESS)` between passes; no
  pass reads the mip level it writes. Afterwards the image and texture unit 0 are unbound and the
  GL state cache is invalidated.
* **Fallback.** If a shader fails to compile, one error is logged and glass falls back to
  `GlassFallback`.

**Floating windows (`GlassCompositor`).** In place of the single
`ImGui_ImplOpenGL3_RenderDrawData` call, `EditorLayer::EndFrame` hands the frame's draw data to the
compositor, which renders the draw lists back to front itself:

```
for each draw list (back to front):
  if it is a glass surface:
    render everything queued so far          → the framebuffer holds exactly what is behind it
    if anything drawn since the last capture overlaps this surface:
      copy the framebuffer, blur it          → GlassBackdrop
    draw the plate: soft shadow, the blur under the window's rect and rounding, the 25% tint
  queue the draw list
render the rest
```

* **What is glass.** A window whose dock tree root is a popup, menu, combo list or context menu; a
  tooltip; a modal; an undocked window or floating dock node. Also any child window drawn on the
  viewport HUD plate (the Scene view's tool rail). Not: docked panels, the menu bar and status bar
  (they span the window), and overlays with no background.
* **Clearing the solid look.** ImGui has already drawn each floating window solid. Its background is
  the first thing `Begin` draws, so the compositor reads its exact colour back from the first vertex
  of the window's draw list (checking it sits on the window's left edge) and makes every vertex of
  that colour, and of the title bar colours, transparent. Vertices in the `Hairline` border colour
  become the 15% white edge. No panel's code knows about glass.
* **Correct layering.** Because each surface captures what is under it at that point in the frame,
  a menu over the Scene view shows the scene, a menu over a panel shows the panel, and a submenu
  shows its parent menu blurred under it.
* **Shared captures.** The compositor tracks what has been drawn since the last capture (the union
  of each list's clip rects). A surface that doesn't overlap any of it reuses the last blur, so the
  viewport toolbar and the tool rail blur once between them. At most 6 captures a frame.
* **Arrival motion.** The same pass applies category 4 of the motion table to each surface's
  finished vertices and clip rects (`ApplyPose`): it covers every window without code changes, and
  input is live from the first frame. Timings are `Glass::Motion()` pulses, so nothing is held once
  a surface has settled.
* **Viewport HUDs** are drawn inside the Scene window, so they use the blurred scene image directly
  (`DrawFrostedPlate`) with the same tint, edge and radius.

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

* Every style slot is written, every one is a grey, and the metrics sit on the 8 px grid.
* WCAG contrast of the text tokens on the solid surfaces, on `GlassFallback` and over the
  brightest possible glass.
* The compositor finds a floating window's background (including alpha overrides), ignores a
  window with none, and clears it without touching its text; the arrival pose moves vertices and
  clip rects.
* The arrival curves start at 0 and land on 1; the window pop and selection glide overshoot and
  settle.
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
| 15 | Select an object and switch the editor to Black Glass |
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
* **Blur cost.** The blur re-runs every frame while glass shows. It doesn't yet skip frames where
  nothing behind it changed.
* **No desktop see-through.** Glass blurs the editor's own frame, not the Windows desktop. (Windows
  11 acrylic behind the whole GLFW window is possible, but would make the editor stop being solid
  black, so it isn't done.)
* **Closing has no motion.** ImGui stops drawing a window the frame it closes, so surfaces arrive
  with motion but leave at once.
