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

_None._ (`ColliderComponent::IsTrigger` was the last one — the PhysX 5 collision system shipped
in #185, and it is now a live checkbox.)

## Editor themes (#92, #234)

`EditorSettings::EditorTheme` is an int; `EditorLayer::ApplyThemeStyle()` is the single entry
point that applies the active theme — **both** its `style.Colors[]` (via `ApplyEditorTheme()`) and
its style *metrics* (rounding / padding / borders) — then DPI-scales once. Call `ApplyThemeStyle()`,
not `ApplyEditorTheme()`, on any theme change so metrics switch too.

Current lineup (#234): **0 = Bento** (default; `ApplyBentoPalette()` — the fall-through case in
`ApplyEditorTheme()`), **1 = Prism** (`ApplyBentoPalette()` then `ApplyPrismAnimation()` re-tints
the accent family every frame), **2 = Windows XP**. `EditorSettings::Load` clamps an out-of-range
value to 0.

**Layer-3 layout treatment is theme-gated.** The Bento hairline-card / tight-row look (#234
layer 3) only applies to the SaaS-dashboard themes. Gate host-side branches on
`EditorLayer::UseBentoLayout()` (`EditorTheme <= 1` — Bento or Prism); in a reloadable module,
`host.GetEditorTheme() <= 1`. Windows XP keeps the flat panels. The viewport HUDs (Stats,
History, status bar) stay bare contrast-adaptive text over the scene in every theme — a card
frame there occludes the view and defeats the adaptive tint.

**Adding a theme:**

1. New `int` value. Add a branch `if (EditorTheme == N) { … return; }` in `ApplyEditorTheme()`
   (`EditorLayer.cpp`) that sets `style.Colors[]` only. (Bento is the fall-through, so it needs no
   branch; a new theme does.)
2. If it needs non-default geometry, add `EditorTheme == N` to the metric block in
   `ApplyThemeStyle()` (Bento + Prism take it today). `ApplyThemeStyle()` resets to the shared
   baseline (`SetSharedMetrics`) first, so you only set what differs, and `ScaleAllSizes` runs
   after — use raw 96-DPI values.
3. Add the label to `kThemeLabels[]` and a line to the theme combo's tooltip (both in
   `EditorLayer.cpp`, Preferences ▸ General).
4. Extend the `EditorTheme` comment in `EditorSettings.h`; bump the `Load` clamp if you add a slot.

**Accent discipline (Bento, and any future multi-accent theme):** the cyan / blue / yellow triad
has a fixed semantic split — do not mix roles:

| Colour | Role | Where |
|---|---|---|
| **cyan** `#3DD6D0` | selection / "you are here" | `Header`, `CheckMark`, `SliderGrab`, `Separator{Hovered,Active}`, `TabSelectedOverline`, `NavCursor`, `DockingPreview`, `TextSelectedBg`, `ResizeGripHovered` |
| **blue** `#5B9DF9` | active / pressed / in-progress | `HeaderActive`, `SliderGrabActive`, `ResizeGripActive` |
| **yellow** `#E8C468` | warning / one data highlight | reserved for host-drawn warning text (the amber build-config label, `#204` overflow rows, "unsaved") — almost none of it is a `style.Colors[]` role, so it lives in hand-coded `ImVec4`s, not the palette |
