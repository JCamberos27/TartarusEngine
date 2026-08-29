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

### 2026-08-29
- **Initial sweep** — 33 findings filed as issues [#1–#33](https://github.com/JCamberos27/TartarusEngine/issues).
- **Fixed:** `Input` was polling invalid GLFW key codes every frame (`301f865`).
- **Landed alongside (not from this list):** GL/GLFW error diagnostics (`301f865`),
  recovery-file auto-save (`58641e5`).
- **Open:** all of Data loss / Bugs / Polish — none triaged into fixes yet.
