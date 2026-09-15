# Editor conventions

Short, load-bearing rules that aren't obvious from the code. Add to this file rather than
letting a convention live only in a commit message or a comment nobody reads.

## Ship components and controls fully wired, or not visible (#215)

The 2026-09-04 audit found the same problem five times, in both directions: controls the editor
shows that do nothing, and runtime behaviour with no controls. Both waste the user's time — a
dead control invites you to set it, save it, and build on an assumption that was never true; an
undiscoverable feature may as well not exist.

1. **A component or import setting is not shown in the editor until its runtime behaviour is
   implemented.** If the field must exist for serialization / forward-compat, keep it in the
   struct and the serializer but leave it out of the Inspector.

2. **A component that ships with runtime behaviour ships with an Inspector section and an Add
   Component menu entry in the same change** — not a follow-up.

3. **Where something genuinely must be visible before it works** (a user would reasonably expect
   the control to exist), show it **disabled** with an explicit `(not implemented)` label and a
   tooltip pointing at the tracking issue — never rely on a code comment the user never sees.

The native component registration system (#184 / #302) is the structural fix: registering a
component with `ComponentRegistry` (`src/Game/ComponentRegistry.cpp`) generates its
serialization, Inspector section and Add-Component entry together, so neither failure mode is
possible by construction. `tools/check_component_registration.py` (a CI step, and runnable
locally) enforces it — every struct in `src/Game/Components.h` must be registered there or
listed in `tools/component_registration_allowlist.txt` with a reason. When adding a component,
prefer registration; reach for the allow-list (and rule 3's disabled `(not implemented)`
control) only when the editor genuinely needs a bespoke widget.

A registered component may still opt out of one or both generic paths with
`ReflectComponent::GenericSerialize` / `GenericInspector` (both default true). It stays
"registered" for the guard rail and keeps its `Icon`/`Category`/`Tooltip` in one place, but a
hand-coded path owns its JSON and/or its Inspector section — for state that isn't plain
reflected fields. `RenderableComponent` ("Mesh Renderer") is the first: it holds a
`shared_ptr<Model>`, its scene form is the box `color`/`size` or model `path`/`material`
written by dedicated serializer code, and its add restores a stashed `DetachedMeshComponent`
via the editor-only `AssetLibrary`. Use both opt-outs sparingly; a plain reflected-field
component should never touch them.

### Current applications

- `RenderableComponent` ("Mesh Renderer") — both opt-outs; see above. **Settled (#6 item 3,
  2026-09-15): stays hand-coded, permanently — not a "build an editor-side ReflectComponent-
  metadata renderer" case.** Every other `ObjectField`-style reference (`ReflectFieldType::
  AssetRef`: Audio Source's Clip, etc.) is a `std::string` project-relative path that
  `AssetLibrary` resolves on demand. `RenderableComponent::ModelRef` is not that shape at all —
  it's a live `shared_ptr<Model>` already loaded, with its own instancing/undo lifecycle
  (`AssetLibrary::CreatePrimitive`/`InstantiateModel`, the `DetachedMeshComponent` stash on
  remove). Genuinely unifying it under `AssetRef` would mean rewriting `RenderableComponent` to
  store a path instead of a live pointer — a large, invasive, unrelated change to rendering and
  serialization — not a rendering-layer refactor. And since `ComponentRegistry` (`src/Game/`)
  architecturally cannot depend on `src/Editor/`'s `AssetLibrary`, a new reflected field type for
  it would carry no drawable metadata anyway; the hand-coded widget would just move, not
  disappear. The actual user complaint that motivated this item — raw `primitive://sphere#90` /
  bare `3` handles shown as the field's label — is already fixed (`b26e7c5`, Defect #5/#50): a
  stable friendly name, a tooltip with tri/vert counts, and a drag-drop target. The one real gap
  against `AssetRef`'s a7f038e upgrade (a "ping" button to jump the Asset Browser to the
  referenced asset) is closed directly on the existing hand-coded widget, same as any other
  `GenericInspector = false` component's editor-only affordance — no metadata renderer needed.
- `ColliderComponent`, `JointComponent` (Phase 4 / #6 item 1) — `GenericSerialize = false` only:
  each keeps its pre-existing hand-written `"collider"` / `"joint"` JSON block (predates this
  reflection layer, #185), but both are now fully registered with `GenericInspector` left at its
  default `true` — the generic field-list Inspector draws every field, with the handful that need
  a bespoke widget (Joint's per-type `Axis`/`UseLimit`/limit fields, Collider's `HalfExtents`)
  marked `EditorHidden` and drawn by `DrawReflectedComponentExtra` instead.

## Editor themes (#92, #234)

`EditorSettings::EditorTheme` is an int; `EditorLayer::ApplyThemeStyle()` is the single entry
point that applies the active theme — **both** its `style.Colors[]` (via `ApplyEditorTheme()`) and
its style *metrics* (rounding / padding / borders) — then DPI-scales once. Call `ApplyThemeStyle()`,
not `ApplyEditorTheme()`, on any theme change so metrics switch too.

Current lineup (Phase 1 item 9, collapsed from three themes down from #234's Bento/Prism/Windows
XP): **0 = Dark** (default; `ApplyBentoPalette()` — the fall-through case in `ApplyEditorTheme()`,
still named for its #234 "Bento" origin), **1 = Light** (`ApplyLightPalette()` — same role
structure and geometry, independently re-solved for a light background; see its function comment
in `EditorLayer.cpp` for why its FrameBg/Border land where they do). `EditorSettings::Load` clamps
an out-of-range value to 0. Every colour pair that carries a WCAG contrast floor (Text,
TextDisabled, FrameBg, Border/Separator composited) is checked automatically at every theme
switch by `EditorLayer::ApplyThemeStyle()`'s startup contrast assert
(`EditorUIPrimitives::AssertContrastFloor`) — a floor miss logs, it doesn't crash, but it should
never happen silently again the way #34's invisible checkboxes did.

**Layer-3 layout treatment.** The hairline-card / tight-row look (#234 layer 3) applies to both
current themes (`EditorLayer::UseBentoLayout()` / `host.GetEditorTheme() <= 1` in a reloadable
module — kept as a named call, always true today, in case a future flat-chrome theme needs the
old alternative back). The viewport HUDs (Stats, History, status bar) draw a fixed opaque plate
behind fixed light text in every theme (Defect #54) — not theme-adaptive, since a HUD only ever
needs to be legible over arbitrary scene content, not over the editor chrome.

**Adding a theme:**

1. New `int` value. Add a branch `if (EditorTheme == N) { … return; }` in `ApplyEditorTheme()`
   (`EditorLayer.cpp`) that sets `style.Colors[]` only. (Dark is the fall-through, so it needs no
   branch; a new theme does.) Compute Text/TextDisabled/FrameBg/Border against ITS OWN background
   via `EditorUIPrimitives::ContrastRatio`/`CompositeOver` — don't derive them from another
   theme's palette by inversion; `ApplyLightPalette`'s function comment walks through why that
   matters (the gamma-space-compositing correction Border/Separator needed).
2. If it needs non-default geometry, add an `EditorTheme == N` branch to `ApplyThemeStyle()`'s
   metric block. `ApplyThemeStyle()` resets to the shared baseline (`SetSharedMetrics`) first, so
   you only set what differs, and `ScaleAllSizes` runs after — use raw 96-DPI values.
3. Add the label to `kThemeLabels[]` and a line to the theme combo's tooltip (both in
   `EditorLayer.cpp`, Preferences ▸ General).
4. Extend the `EditorTheme` comment in `EditorSettings.h`; bump the `Load` clamp if you add a slot.

**Accent discipline (Dark, Light, and any future multi-accent theme):** the cyan / blue / yellow
triad has a fixed semantic split — do not mix roles:

| Colour | Role | Where |
|---|---|---|
| **cyan** `#3DD6D0` (Dark) / **teal** `#0E7C78` (Light) | selection / "you are here" | `Header`, `CheckMark`, `SliderGrab`, `Separator{Hovered,Active}`, `TabSelectedOverline`, `NavCursor`, `DockingPreview`, `TextSelectedBg`, `ResizeGripHovered` |
| **blue** `#5B9DF9` (Dark) / `#2563C7` (Light) | active / pressed / in-progress | `HeaderActive`, `SliderGrabActive`, `ResizeGripActive` |
| **yellow / warning** | warning / one data highlight | `EditorUIPrimitives::WarningColor()` (Phase 1 item 2) — a fixed, non-theme-derived accessor alongside `DangerColor()`/`SuccessColor()`/`InfoColor()`; a status colour has to mean the same thing regardless of which theme is active, unlike the selection/active accents above |

Light's accent hues are independently WCAG-checked against its own background, not derived from
Dark's by inversion — see `ApplyLightPalette()`'s function comment in `EditorLayer.cpp`.
