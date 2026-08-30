<div align="center">

# Tartarus Engine — Bugs & Polish

**The live tracker is the [Issues tab](https://github.com/JCamberos27/TartarusEngine/issues).**
This page is the overview; each item below is a GitHub issue you close when it's fixed.

</div>

---

## How this is tracked

- Every finding is a **GitHub issue** with a stable ID in its title (`[D2]`, `[B4]`, `[P7]`…).
  Reference that ID in commit messages so the history ties together.
- Labels:
  | label | meaning |
  |---|---|
  | [`data-loss`](https://github.com/JCamberos27/TartarusEngine/issues?q=is%3Aissue+label%3Adata-loss) | can silently destroy scene data or asset associations |
  | [`bug`](https://github.com/JCamberos27/TartarusEngine/issues?q=is%3Aissue+label%3Abug) | wrong behaviour / dead end reachable in normal use |
  | [`polish`](https://github.com/JCamberos27/TartarusEngine/issues?q=is%3Aissue+label%3Apolish) | cosmetic / inconsistent / mildly confusing |
  | [`needs-repro`](https://github.com/JCamberos27/TartarusEngine/issues?q=is%3Aissue+label%3Aneeds-repro) | reported once; needs a clean reproduction |
  | [`needs-manual-verify`](https://github.com/JCamberos27/TartarusEngine/issues?q=is%3Aissue+label%3Aneeds-manual-verify) | couldn't be tested via automation |
- Fix an issue → close it, and add a `Fixed` line to [`CHANGELOG.md`](../CHANGELOG.md).
- The **Progress log** below is date-stamped each working session.

---

## Origin: editor QA sweep — 2026-08-29

Every mouse-drivable subsystem of the editor was exercised against a Release build. Keyboard
shortcuts and Play-mode movement can't be automated — those are issue **[#33 (MV)](https://github.com/JCamberos27/TartarusEngine/issues/33)**.

### 🔴 Data loss

| ID | Issue | Summary |
|---|---|---|
| D1 | [#1](https://github.com/JCamberos27/TartarusEngine/issues/1) | `New Scene` → close → real `scene.json` silently overwritten with the default world (not "dirty", no prompt, no recovery snapshot). |
| D2 | [#2](https://github.com/JCamberos27/TartarusEngine/issues/2) | Remove + re-add a Mesh Renderer → imported model becomes a `cube#0` primitive, permanently, no way to reassign. |
| D3 | [#3](https://github.com/JCamberos27/TartarusEngine/issues/3) | Undo/redo/Play-Stop permanently reorders the Hierarchy (lights/empties sink below meshes). |

### 🟠 Bugs

| ID | Issue | Summary |
|---|---|---|
| B1 | [#4](https://github.com/JCamberos27/TartarusEngine/issues/4) | Trapped in Play mode — Stop unclickable once game input is engaged; only `Esc` escapes. |
| B2 | [#5](https://github.com/JCamberos27/TartarusEngine/issues/5) | Camera jumps hard when you click into the Game view (cursor-lock delta spike). |
| B3 | [#6](https://github.com/JCamberos27/TartarusEngine/issues/6) | View presets / nav-gizmo axis clicks don't frame the scene → black viewport (with no selection). |
| B4 | [#7](https://github.com/JCamberos27/TartarusEngine/issues/7) | `Add > Cube` / Duplicate leak `cube#N` entries into the Asset Browser; they persist across New Scene. |
| B5 | [#8](https://github.com/JCamberos27/TartarusEngine/issues/8) | Duplicate places the copy exactly on top of the source (Paste offsets, Duplicate doesn't). |
| B6 | [#9](https://github.com/JCamberos27/TartarusEngine/issues/9) | Import-Settings model preview can be orbited/zoomed out of view with no reset. |
| B7 | [#10](https://github.com/JCamberos27/TartarusEngine/issues/10) | Asset click doesn't always open Import Settings in the Inspector *(needs repro)*. |
| B8 | [#11](https://github.com/JCamberos27/TartarusEngine/issues/11) | History entry count appears to grow on undo-then-jump *(needs repro)*. |

### 🟡 Polish — [#12–#32](https://github.com/JCamberos27/TartarusEngine/issues?q=is%3Aissue+label%3Apolish)

P1 gizmo drag adds rotation float-noise · P2 wireframe hides the selected object's wires ·
P3 environment colours are hex-text-only · P4 import dialog opens in `build/Release/` ·
P5 "imported from the source file" on primitives · P6 History/Stats overlays overlap ·
P7 History jump clears selection · P8 inconsistent History labels · P9 undo collapses
Inspector sections · P10 default boxes all "(unnamed)" · P11 colour picker clips off-screen ·
P12 two clicks to open a menu after Escape · P13 `-0.000` position · P14 nav-gizmo label
stuck on Persp/Iso · P15 filtered Hierarchy shows unfiltered count · P16 mesh+light shows
only the light icon · P17 no Reimport in the asset menu · P18 FBX assets have no thumbnail ·
P19 corner mark is an unreadable glyph · P20 Console logs nothing · P21 no confirm on group delete.

### 🟢 Verified working (this pass)

Toolbar (all buttons), all four gizmo modes + translate drag + snap-to-ground, Hierarchy
(rename / drag-parent / unparent / eye toggle / filter / context menu), selection (pick /
Ctrl-multi / box-select / group delete), Inspector (transform edit, Material + colour
picker, Add/Remove Component, Point↔Spot), Asset Browser (folder nav, Import Settings panel,
drag-FBX-to-place), Environment colour live-update, Reset Layout + dock resize, all menus.

---

## Progress log

Date-stamped each working session. See [`CHANGELOG.md`](../CHANGELOG.md) for what has shipped.

### 2026-08-29 — session 1
- Editor QA sweep: 33 findings filed as issues [#1–#33](https://github.com/JCamberos27/TartarusEngine/issues).
- Fixed: `Input` polled invalid GLFW key codes every frame (`301f865`).
- Landed alongside: GL/GLFW error diagnostics (`301f865`), recovery-file auto-save (`58641e5`).

### 2026-08-29 — session 2  (bug tier)
Fixed & pushed:

| Issue | Commit | Live-verified |
|---|---|---|
| [#1 D1] New Scene overwrites scene.json | `e5234e5` | yes |
| [#2 D2] Mesh Renderer re-add → cube      | `b04b320` | code review + build only |
| [#3 D3] undo reorders the Hierarchy      | `f2aafde` | yes |
| [#4 B1] trapped in Play mode             | `e309e81` | yes |
| [#5 B2] camera jump on Game-view click   | `039f781` | yes |
| [#6 B3] view presets don't frame scene   | `6291405` | code review + build only |
| [#7 B4] primitives clutter Asset Browser | `6f7a02a` | yes |
| [#8 B5] Duplicate doesn't offset         | `03be6ce` | yes |
| [#9 B6] model preview lost, no reset     | `65018b3` | yes |
| [#10 B7] asset click ignored after rename| `4426832` | yes |

- **Still open:** [#11 B8] history entry count grows on undo-then-jump — `needs-repro`,
  low confidence; leave for a hands-on session.
- **Not started:** all polish (#12–#32), #33 manual-verification checklist.

### 2026-08-30 — session 3  (polish + feature tier)

Polish and feature sweep over issues [#11–#50]. One uncommitted batch. Verified in a Release
build unless noted.

**Fixed & verified in-editor:** [#12 P1], [#13 P2] (+ a wireframe-mode outline tweak in
`main.cpp`), [#16 P5], [#17 P6], [#20 P9], [#25 P14], [#26 P15], [#27 P16], [#31 P20],
[#34 D4], [#39 P22], [#42 P25], [#44 P27], [#45 P28], [#47 P30], plus [#36 B10] and the
Add ▸ Camera flow.

**Fixed, build-clean, not individually eyeballed:** [#18 P7], [#19 P8], [#21 P10],
[#28 P17], [#43 P26], [#48 P31] (ctrl-click multi-select couldn't be driven by automation),
[#50 P33] (needs a CJK-named entity to see).

**Already fixed in code, just needs closing:** [#14 P3], [#49 P32].

**Cannot reproduce against this build (documented, not code-changed):**
[#11 B8], [#23 P12], [#37 B11], [#22 P11].

**Deliberately not changed:** [#32 P21] group-delete confirm (scene deletes are instant +
undoable by design); [#46 P29] near-black sky (per author — it's scene data, not the
engine default).

**Left for a follow-up:** [#29 P18] model thumbnails (needs a per-model render cache),
[#30 P19] corner mark (art asset), [#33 MV] manual keyboard/Play-mode checklist,
[#35 B9] restore-from-maximized ghost frame (hands-on check),
[#38 B12] rename length/sanitisation, [#40 P23] (largely covered by the no-op-edit dedup).

Side note: the color-picker *swatch* path can drop an undo step when the popup closes on a
frame the stale-staged-undo cleanup (`EditorLayer.cpp`, added in `5faa4de`) also fires —
predates this sweep; worth a hands-on look. The hex-field path records one entry per edit,
as intended.
