<div align="center">

# Tartarus Engine — Bugs & Polish

**Editor QA sweep — 2026-08-29**

Every mouse-drivable subsystem of the editor was exercised against a Release build
(`build/Release/TartarusEngine.exe`). Keyboard shortcuts and Play-mode movement could
not be automated and are tracked separately under [Needs manual verification](#needs-manual-verification).

</div>

---

## Severity legend

| Tag | Meaning |
|---|---|
| 🔴 **Data loss** | Can silently destroy the user's scene or asset associations |
| 🟠 **Bug** | Wrong behaviour, high friction, or a dead end the user can hit in normal use |
| 🟡 **Polish** | Cosmetic, inconsistent, or mildly confusing — not blocking |
| 🟢 **Verified OK** | Tested and working; recorded so it isn't re-tested |

---

## 🔴 Data loss

### D1 — `File > New Scene` can silently overwrite `scene.json`
`New Scene` swaps in the default world, **clears the dirty flag**, and leaves the open-file
path as `scene.json`. `main.cpp` saves unconditionally on a clean exit. So:

> **New Scene → close the window → your real `scene.json` is overwritten with the default crates world.**

No prompt (scene isn't "dirty"), no recovery snapshot (only written while dirty, and auto-save
is off in this install anyway). The recovery-file work and the planned close-prompt both key
off `m_Dirty`, so neither catches this.

- **Fix ideas:** mark the scene dirty on New Scene; and/or force a `Save As` before a
  new/never-saved scene can be written over the previous file; and/or always prompt on New
  Scene when the current scene has unsaved history.

### D2 — Removing then re-adding a Mesh Renderer turns an imported model into a cube
Remove the **Mesh Renderer** component from an FBX-backed entity, then add it back from the
**Add Component** menu → the entity is given a fresh `cube#0` primitive mesh instead of its
original `*.FBX`. The mesh association is gone permanently and the **Mesh** field is read-only,
so there is no way to point it back at the FBX.

- Also spawns another `cube#0` library entry (see [B4](#b4--primitives-pile-up-in-the-asset-browser)).
- **Fix ideas:** keep the entity's `RenderableComponent` model reference (or its asset key)
  even while the component is "removed", so re-adding restores it; or make the Mesh field a
  drop target so a mesh can be reassigned.

### D3 — Undo/redo permanently reorders the Hierarchy
Any whole-scene snapshot round-trip — **undo, redo, or Play → Stop** — rewrites entity order:
every meshless entity (lights, empties) sinks below every mesh entity.

> Repro: default scene ends `… Point Light, Cube`. One **Undo** → `… Cube, Point Light`, and
> it never comes back.

- **Cause:** `SceneSerializer` writes `boxes[]`, then `models[]`, then `empties[]`; the loader
  recreates in that array order, discarding the original interleaved creation order.
- **Fix ideas:** write a single ordered entity list with an explicit `order` / creation index,
  or record the hierarchy's display order and restore it after load.

---

## 🟠 Bugs

### B1 — You can get trapped in Play mode
In in-panel Play, once you click the Game view to capture input the cursor is locked and
**every click becomes mouselook** — the on-screen **Stop** and **Fullscreen** buttons stop
receiving clicks. The only way out is the `Esc` key. During testing this required killing the
process.

- **Fix ideas:** keep the Play/Stop toolbar strip above the game-input capture (let clicks on
  just that strip through), or draw Stop as an always-on-top overlay that ignores the capture
  latch.

### B2 — Camera jumps when you click into the Game view
Clicking the Game view to engage input produces a large one-frame camera rotation (ended up
pitched/rolled hard into a wall). Classic cursor-lock delta spike — the first mouse delta
after `GLFW_CURSOR_DISABLED` is huge and gets fed straight into look.

- **Fix ideas:** swallow the first mouse delta for one frame after any cursor-lock transition
  (a `m_FirstMouseAfterLock` latch in `Input`).

### B3 — View presets / nav-gizmo axis clicks don't frame the scene
With nothing selected, **View → Front/Back/Left/Right/Top/Bottom** and the corner nav-gizmo's
axis cones snap the camera to a position where the scene is off-screen — you get a black
viewport or a sliver of geometry, with no easy recovery (scroll-zoom does nothing useful in
ortho). `View → Iso` happened to work; `Top` followed by a nav-gizmo click left the viewport
fully black.

- **Cause:** `ComputeViewPivot` falls back to "a fixed distance in front of the camera" when
  nothing is selected and the view ray misses all geometry.
- **Fix ideas:** fall back to the whole-scene bounding box (frame-all) instead of a fixed
  offset; give the nav gizmo an implicit frame-all.
- Works correctly **with** a selection.

### B4 — Primitives pile up in the Asset Browser
Every `Add > Cube` / `Sphere` / … and every **Duplicate** of a primitive registers a library
entry (`cube#0`, `cube#1`, …) that shows up as a browsable asset. These are per-instance
procedural meshes (`primitive://` paths), not importable assets. They accumulate and survive
`New Scene` (the AssetLibrary isn't reset with the world).

- **Fix ideas:** don't add `primitive://` models to `m_ModelList` / the browser listing; or
  filter them out of the Asset Browser view.

### B5 — Duplicate doesn't offset the copy
Hierarchy / context-menu **Duplicate** places the copy at the **exact** position of the
source (verified: original and copy both at `3.5 / 0.5 / -5.0`). **Paste** offsets by
`(1, 0, 1)`; Duplicate should match so the copy is visibly distinct.

### B6 — Import-Settings model preview can be lost with no way back
In the asset **Import Settings** preview, orbit-drag + scroll-zoom can send the model fully
out of frame (preview goes black). There's no reset/reframe control, and scrolling back
doesn't recover it — the only fix is to select a different asset and return. Zoom also looks
unclamped.

- **Fix ideas:** add a "reset view" button; clamp zoom; re-frame to bounds on any preview
  interaction that leaves the model outside the frustum.

### B7 — Asset click doesn't always open Import Settings
Clicking a model/texture asset in the browser didn't switch the Inspector to its Import
Settings — this happened right after a Hierarchy rename, and the Inspector stayed on the
renamed scene entity through several asset clicks. Clicking empty space first, or clicking the
asset again, fixed it. Likely stale text-input focus from the rename field. *(Needs a clean repro.)*

### B8 — History entry count appears to grow on undo-then-jump
After undoing and then jumping forward via the History panel, an `Edit Material` step appeared
to be listed one extra time (2 → 3). Possible duplicate-entry creation in `JumpToRedoEntry`,
or the colour picker emitting an entry on open **and** close. *(Needs a clean repro.)*

---

## 🟡 Polish

| # | Item |
|---|---|
| P1 | Gizmo **translate** drag writes float noise into **rotation** (`0.000 → -0.000`) via ImGuizmo's matrix decompose; accumulates over many drags. Same on re-parent. Fix: only write back the channel the gizmo actually moved, or snap near-zero euler components. |
| P2 | In **Wireframe** shading the **selected** object renders solid — its orange highlight-tint wash fills it, so the one object you're inspecting is the only one whose wireframe you can't see. |
| P3 | **Environment** Horizon/Zenith colours are hex-text fields (tiny swatch beside them); Material Base Color and Light Color get a full colour picker. Make them consistent. |
| P4 | **Import** file dialog opens in `build/Release/` (the working dir), never the project folder or a remembered last-used location. |
| P5 | Primitive Inspector, Material section: "Using material(s) imported from the source file" — a primitive has no source file. Same wording in the "Use Custom Material" tooltip. |
| P6 | **History** panel and the **Stats** overlay float in the same top-left corner and overlap. |
| P7 | Jumping to a **History** entry clears the scene selection even when that entity still exists at that point (name-based restore should have re-picked it). |
| P8 | History labels for equivalent edits are inconsistent: position edits logged as `Move`, `Move`, then `Transform`. |
| P9 | **Undo** collapses the Inspector's expanded component sections (Material re-collapses after an undo). |
| P10 | Every box in the default `World()` shows as **"(unnamed)"** in the Hierarchy — no positional/indexed fallback name, so 11 identical rows. This also feeds a name-based ambiguity in undo's selection-restore. |
| P11 | Colour-picker popup opens hard against the right screen edge; hue bar and Current/Original swatches are partly clipped at this window size. |
| P12 | After `Esc` closes a menu, the first click on another menu-bar item is consumed — needs a second click. *(Low confidence; may be automation timing.)* |
| P13 | New primitive spawns with Position X shown as `-0.000` (negative zero). |
| P14 | Nav-gizmo corner label only ever shows `Persp` / `Iso` — never updates to `Front` / `Top` / … for a named view. |
| P15 | Filtered Hierarchy still shows the **unfiltered** total count (`Objects (9)`), not `matches / total`. |
| P16 | An entity that has both a mesh and a Light shows **only** the light-bulb icon in the Hierarchy; after removing the Mesh Renderer it shows the "empty" icon — mesh indication is lost. |
| P17 | Asset right-click menu is only *Rename / Edit Labels / Remove from Library* — no **Reimport** (that lives only behind Import Settings → Apply). |
| P18 | Model/FBX assets in the icon grid show a generic cube icon, not a rendered thumbnail. |
| P19 | The spinning corner "mark" (bottom-left of the viewport) renders as an unreadable vertical-strokes glyph at its size. |
| P20 | The Console logged **nothing** all session — even D2's silent mesh→cube corruption and B3's lost camera produced no output. More diagnostic logging would help. |
| P21 | Group **Delete** of scene objects has no confirmation dialog (asset deletes do). It is undoable. Arguably fine — noting for consistency. |

---

## 🟢 Verified working

- **Toolbar:** Undo, Redo, Move/Rotate/Scale/Rect, World↔Local, Pivot↔Center, Toggle Grid,
  Toggle Snap-to-Grid, Snap-to-Ground, Shading cycle (Shaded → Wireframe → Unlit), Stats,
  Console, History, Lock Layout — all respond.
- **Gizmos:** all four modes draw; translate-arrow drag moves the object and grid-snaps;
  Snap-to-Ground drops the selection onto the surface below.
- **Hierarchy:** double-click rename, drag-to-parent (preserves world transform),
  drag-to-empty-space to un-parent, eye/active toggle, text filter, right-click context menu.
- **Selection:** click-pick, Ctrl-click multi-select, box/marquee select, group delete.
- **Inspector:** transform type-in and drag, Material section, colour picker (live-updates the
  mesh), Add Component (greys out components already present), Point↔Spot light switch
  (reveals Spot Angle), component remove (X).
- **Asset Browser:** folder tree + breadcrumb navigation, asset selection → Import Settings
  panel (model preview, scale, normals, optimise, animation/skeleton, material mode,
  Revert/Apply), drag an FBX into the viewport to place an instance (rests on the surface below).
- **Environment:** Horizon/Zenith colour edits live-update the sky.
- **Layout:** `Settings → Reset Layout` rebuilds the default dock tree cleanly; dock-border
  drag-resize is proportional.
- **Menus:** File, Import, Add, View, Window, Settings all open and their items fire.

---

## Needs manual verification

Automation can't send real key events to the GLFW window, and Play-mode movement can't be
driven, so these are **untested**:

- **All keyboard shortcuts** — `F` (frame), `W/E/R/T` (gizmo tools), `Ctrl+Z/Y/C/X/V/D/S`,
  numpad `1/3/7/0/5` (views + ortho), hold-`V` (vertex snap), hold-`Ctrl` (invert snap),
  `F1` (play), `F11` (fullscreen), `Esc` (release cursor).
  - Related engine note: `Input::Update()` polls `glfwGetKey()` once per frame and edge-detects
    "just pressed" off that poll, so a press+release inside one 16 ms frame is invisible. Low
    real-world impact for a human, but worth a look.
- **Play mode** — WASD / jump / mouselook, `Esc` reliably releasing the cursor, whether
  **Stop restores the on-entry snapshot**, Maximize-on-Play, `F11` during play.
- **Prefabs** — Save as Prefab → instantiate (drag / double-click).
- **Texture import** + R/G/B/A channel-isolation preview.
- **Asset Browser** — icon-size slider dragged to the far left → compact list view; the search
  box; the label-filter dropdown.
- **Orthographic toggle** (button / `5`).
- **OS drag-and-drop import** — single file and whole folder.
- **Audio Source** component playback / preview.
- **Copy / Paste** offset behaviour (context-menu Copy worked; Paste stays greyed until
  something is on the clipboard).
- **DPI scaling** on a monitor set to something other than 100 %.
