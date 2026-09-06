<div align="center">

<img src="docs/images/logo.png" alt="Tartarus Engine" width="520">

**A C++17 / OpenGL game engine and scene editor for first-person games.**

[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
[![OpenGL 4.6](https://img.shields.io/badge/OpenGL-4.6%20core-5586A4?style=flat-square&logo=opengl&logoColor=white)](https://www.khronos.org/opengl/)
[![Platform](https://img.shields.io/badge/platform-Windows-0078D6?style=flat-square&logo=windows&logoColor=white)](#building)
[![CMake](https://img.shields.io/badge/build-CMake-064F8C?style=flat-square&logo=cmake&logoColor=white)](#building)
[![CI](https://img.shields.io/github/actions/workflow/status/JCamberos27/TartarusEngine/build.yml?branch=main&style=flat-square&label=build)](https://github.com/JCamberos27/TartarusEngine/actions)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)

</div>

---

Tartarus is a from-scratch game engine with a full editor front-end — dockable panels,
transform gizmos, an asset browser, prefabs, undo/redo, and an in-editor play mode. It renders
imported FBX / glTF / OBJ models with PBR-style materials and skeletal animation through a
hand-rolled OpenGL 4.6 core loader — no glad / Python codegen step in the build.

Both the **gameplay module** and the **editor's panels** are compiled into hot-reloadable DLLs,
so game systems *and* editor UI can be rebuilt and swapped in without closing the editor or
losing the open scene. Engine-side components are **reflection-registered**: declaring one in a
single place gives it JSON serialization, an Inspector section, and an Add-Component menu entry
for free.

## Features

### Editor

| | |
|---|---|
| **Dockable layout** | Scene, Game, Hierarchy, Inspector, Asset Browser, and Console panels in a real ImGui dock tree — drag any border to resize neighbours, panels scale proportionally with the window, and the arrangement persists between sessions. Statistics and History sit as transparent, contrast-adaptive viewport overlays pinned to the corners |
| **Hot-reloadable UI** | The toolbar, Hierarchy, Inspector, Asset Browser, Console, and Stats panels live in `TartarusEditor.dll` — rebuild that target and the editor swaps the new panel code at the next safe frame with the scene still loaded. The gameplay module (`TartarusGame.dll`) hot-reloads the same way |
| **Themes** | **Bento** (dark-mode SaaS / bento-grid, the default), **Prism**, and a **Windows XP Luna** skin — swapped live from Preferences, colours *and* layout metrics |
| **Transform gizmos** | Translate / rotate / scale / rect tools with local vs. world space, pivot vs. bounds-center, a combined gizmo for multi-object selections, and a relative Batch Transform panel for nudging a whole selection at once. A **Gizmos** dropdown master-toggles every viewport gizmo and icon, or hides them per type |
| **Hierarchy** | Tree view with **drag-to-reparent** and **drag-between-rows to reorder siblings** (a full-row drop band, not a hairline), full **keyboard navigation** (arrows, `Home`/`End`, `←`/`→` to collapse-expand or step into children, type-to-select), per-kind icons (mesh / light / camera / folder) with a right-hand active-state eye column, and a right-click menu with Unity-style **GameObject** ops — Create ▸ (the full Create menu), Create Empty Child, Set as First / Last Sibling, Group / Unparent, Move To View, Align With View, Toggle Active State. Drop a model or prefab from the Asset Browser onto a row to instantiate it as a child |
| **Inspector** | Every component section has a right-click menu — **Reset**, **Copy Component**, **Paste Component Values** (across objects, creates it if missing), **Remove**. **Add Component** has a type-to-filter search. Reflection-registered components render their own sections with per-field drag speeds, clamps, and tooltips — no per-component editor code. Full multi-select editing with mixed-value dashes and tri-state checkboxes |
| **Lights** | Select a light and its shape draws in the viewport — a range sphere for point lights, an angle-and-range cone for spots, aim arrows for the sun, tinted by the light's colour — with drag handles to scale range, open or close the cone, or re-aim without leaving the viewport. A dockable **Lights panel** lists every light with solo / mute / frame; any light can be flown through (**look through light**) or **dropped onto the surface** below it. The Inspector picks colour directly or from a **colour temperature** (Kelvin) and exposes per-light shadow tuning; a whole multi-selection edits together |
| **Selection** | Click-to-pick, box / marquee select, `Ctrl`-click multi-select, `Shift`-click ranges, **Select All / Deselect / Invert**, hierarchy parenting for mesh-less and imported objects, per-entity active toggle |
| **Snapping** | **Grid & Snap popover** on the toolbar — grid cell size, major-line spacing, and independent Move / Rotate° / Scale increments for the gizmo. Hold `Ctrl` to invert snap for one drag; hold `V` for vertex snapping between unparented meshes; snap-to-ground |
| **Navigation** | Fly camera, Alt-orbit, pan, dolly, frame-selection, orthographic / perspective toggle, axis view presets with animated transitions, plus an on-screen orientation gizmo |
| **Undo / redo** | Whole-scene snapshots with a History panel you can jump around in; delta-compressed so history is cheap to keep |
| **Play mode** | Runs inside the docked Game panel with the editor still live; scene state is snapshotted on entry and restored on exit, so play never becomes an edit. **Play / Pause / single-frame Step** (`F1` / `F2` / `F3`), maximize the Game view (`F4`), click to capture input, `Esc` to release |
| **Game View** | Resolution and aspect-ratio presets with letterboxing, custom resolutions, maximize-on-play (Preferences), and an FPS / draw-call / triangle overlay. The aspect control and every overlay is **contrast-adaptive** — text and plates lighten or darken to stay legible over whatever's rendered. Add a **Camera** entity to frame a shot; the Game view previews through it while editing |
| **Quality of life** | Auto-save with crash recovery, copy / paste / duplicate, filtered console, statistics overlay, DPI-aware scaling, a searchable shortcut list in Preferences, persisted preferences |

### Assets

- **Import** FBX, OBJ, glTF / GLB models · PNG / JPG / TGA / BMP textures · WAV / MP3 / OGG / FLAC audio
- **Browse** with a folder tree, resizable icon grid or compact list, search, and labels
- **Drag and drop** — from the OS into the editor, from the browser into the viewport with a live translucent placement preview that grid-snaps and rests on the surface below, or from the browser onto a Hierarchy row to parent the new instance
- **Import settings** per asset (Unity-style): texture filtering, wrap, sRGB, mipmaps, max size; model scale, normals, tangents, skeleton, animation, and material handling — all re-importable in place
- **Previews** including live texture thumbnails, an orbitable 3D model preview, and R / G / B / A channel isolation for textures
- **Prefabs** — save an entity as a reusable asset and instantiate it by drag or double-click

### Renderer

- Forward PBR-style shading — albedo, normal, metallic, roughness, ambient occlusion, and emissive maps
- **Linear HDR pipeline** — the scene renders into a multisampled `RGBA16F` target and a single fullscreen pass applies exposure, a tone-mapping curve (**Reinhard / ACES / AgX**), and gamma
- **Directional, point, and spot lights** in one GPU light buffer (`std430` SSBO) — the sun is a placeable entity you aim with its rotation; point / spot have range and cone falloff. Each light's colour is set directly or from a **colour temperature** (Kelvin), and each carries its own shadow settings — cast on / off plus bias / normal-bias / softness / near-plane — layered on the global cascade config
- **Clustered-forward light culling** — a per-view compute pass bins point / spot lights into a 16 × 9 × 24 froxel grid, so a fragment loops only the lights that actually reach its cluster instead of every light in the scene
- **Cascaded shadow maps** for the directional sun — 2–4 texel-snapped cascades, soft rotated-Poisson PCF whose penumbra follows the sun's angular size, seam-blended between cascades, with per-cascade frustum culling
- **Point-light shadows** via a depth cube-map array, and **spot-light shadows** via a perspective depth array — both store linear distance-to-light so a shadow reaches the light's full range, and share the light SSBO's per-light shadow slot
- Skeletal animation with up to 100 bones per model
- Frustum culling, a redundant-state-change cache, and per-frame draw statistics
- Offscreen HDR targets per viewport, so Scene and Game render independently at their own resolutions and MSAA levels
- Procedural sky, distance-faded infinite grid, inverted-hull selection outlines, translucent drag previews
- Shaded / wireframe / unlit view modes
- Targets OpenGL 4.6 core through the hand-rolled loader — immutable texture and buffer storage, `std430` shader storage buffers, and direct state access throughout the mesh path (`glCreateBuffers` / `glNamedBufferStorage` / `glVertexArray*`), so building a mesh mid-frame never disturbs the bound render state

### Core

- **EnTT** entity-component system with a full transform hierarchy
- **Reflection-registered components** — a component listed once in `ComponentRegistry` gets its JSON serialization, its Inspector section (fields typed as bool / int / float / vec3 / string with per-field drag speed, min/max, tooltip), and its Add-Component entry generated from that one declaration. Per-frame behaviour lives in the hot-reloadable game module
- JSON scene serialization, including the asset library and per-asset import settings, with a format-version check
- First-person controller — WASD, sprint, jump, gravity, and sub-stepped AABB collision that won't tunnel through thin geometry
- AABB physics primitives with ray / box intersection, driving editor picking and the player's collision resolution
- Audio via miniaudio — in-editor preview, and Play-mode `AudioSourceComponent` playback (play-on-start, volume, loop) positioned from the cached world transform
- Decoded-texture disk cache (`Library/Textures`) keyed on source size, mtime and import settings — a warm scene load skips PNG decoding entirely, written atomically so a crash can't leave a corrupt entry
- Frame profiler with per-pass CPU timings, and a log that streams into the editor Console

## Performance

Measured on the bundled `Showcase` scene — a first-person hall lit entirely by moving,
colour-cycling point and spot lights.

| | |
|---|---|
| **142 FPS** (7.0 ms/frame) | uncapped, editor running |
| Resolution | 3840 × 2160, 4× MSAA |
| Shadows | 4096² maps, 4 cascades, 500 m distance |
| Scene | 83 entities · 65 renderers · **18 lights** · 19,380 triangles |
| Draw calls | 65 · 9 shader binds (2 redundant skipped) · 1 texture bind |
| CPU per frame | **≈ 2.2 ms** — the remainder is GPU |

Per-pass CPU timings from the built-in profiler: point shadows 0.47 ms · spot shadows 0.36 ms ·
scene draw 0.24 ms · editor UI build 0.26 ms · ImGui render 0.21 ms.

*One scene, one machine — indicative, not a benchmark suite. GPU-side timing is not yet
instrumented.*

## Project status

**Actively developed.** The engine has been through a full seven-round source audit — every file
in `src/` was read — tracked in
**[issue #187](https://github.com/JCamberos27/TartarusEngine/issues/187)** (now closed: all 54
findings resolved).

Current focus is **[issue #236](https://github.com/JCamberos27/TartarusEngine/issues/236)**, a
feature-gap audit of the editor against Unity — Hierarchy, Inspector, toolbars, gizmos,
shortcuts, and the Asset Browser. Much of it has shipped (sibling reordering, keyboard tree
navigation, drag-from-browser, the component context menu with copy/paste, the Add-Component
search, Pause & Step, the Grid & Snap and Gizmos popovers, GameObject-menu parity); the rest is
the open backlog on that issue.

Windows-only CI builds every push in **Debug and Release**.

## Tools

### TifSplitter

A standalone command-line utility (built alongside the engine as its own executable) that turns
high-bit-depth or multi-channel TIFFs into the 8-bit PNGs the engine's texture loader reads.
Decoding goes through libtiff, so any source layout it supports works — 8 / 16 / 32-bit integer
or float samples, tiled or stripped, palette, YCbCr, CMYK.

| | |
|---|---|
| **Channel splitting** | Unpack a combined sheet into `_R` / `_G` / `_B` / `_A` PNGs — e.g. a Metallic / Roughness / AO / Smoothness texture authored as one RGBA image |
| **Normal map conversion** | Flip green to convert a DirectX (+Y up) normal map to OpenGL (+Y down), which is what the engine's shaders expect |
| **Heightmap normalization** | Stretch a 16-bit heightmap's actual value range to fill 0–255, so terrain data that only occupied a narrow slice of its range doesn't decode to flat gray |
| **Tiling** | Split an oversized source into a grid of square PNGs, for terrain too large to load as a single texture |
| **Batch mode** | Convert a whole folder (optionally recursive, mirroring its structure) or an explicit file list, across multiple threads |

```bash
# One file, splitting a packed PBR sheet into its channels
TifSplitter -i packed_mrao.tif -o ./Output --split-channels

# A whole tree, converting DirectX normal maps as it goes
TifSplitter -d ./SourceTextures --recursive -o ./Output --flip-y

# A heightmap too big for one texture, cut into 1024px tiles
TifSplitter -i Terrain_Height.tif -o ./Output --tile-size 1024
```

Run it with no arguments for the full option list, or drag a `.tif` onto the executable to
convert it in place.

## Building

Requires **CMake 3.16+**, an **MSVC** toolset (Visual Studio 2022 or newer), and a GPU / driver
that can create an **OpenGL 4.6 core** context (any NVIDIA / AMD / Intel driver from the last
several years). GLFW, GLM, Assimp, EnTT, Dear ImGui, and ImGuizmo are fetched automatically by
CMake on first configure — that step needs an internet connection; later builds are offline.

```bash
cmake -S . -B build -A x64
cmake --build build --config Release
```

No `-G` on purpose — CMake picks whichever Visual Studio is installed (the CI does the same, so
a toolset bump doesn't break the build). Pass `-G "Visual Studio 17 2022"` etc. if you need a
specific one.

The executable lands at `build/Release/TartarusEngine.exe`; its reloadable modules,
`TartarusGame.dll` (gameplay) and `TartarusEditor.dll` (editor panels), are built beside it.
Icon fonts and branding are copied next to them automatically. On first launch it opens
`project/scenes/Showcase.json` — a first-person hall lit entirely by moving, colour-cycling
point and spot lights — and thereafter reopens whatever scene you last had open.

> **Note:** the engine must be closed before rebuilding `TartarusEngine.exe`, or the linker
> can't overwrite it. The two DLLs can be rebuilt while it runs.

### Hot reload

After launching a build that includes the hot-reload hosts, gameplay code and editor-panel code
can each be rebuilt without closing the editor:

```bash
cmake --build build --config Release --target TartarusGame     # gameplay systems
cmake --build build --config Release --target TartarusEditor    # toolbar / panels
```

The editor polls for a rebuilt DLL a few times a second, loads a private copy, and swaps it at
the next safe frame. The editor, renderer, scene, and OpenGL context stay in the host, so the
open scene is preserved. Rebuilding `TartarusEngine.exe` itself still requires closing it.

## Controls

| Input | Action |
|---|---|
| `Right-drag` | Look around · `WASD` `Q` `E` to fly while held · `Shift` to sprint |
| `Alt` + `Left-drag` | Orbit the selection |
| `Alt` + `Right-drag` / `Scroll` | Dolly / zoom |
| `Middle-drag` | Pan |
| `W` `E` `R` `T` | Translate / Rotate / Scale / Rect tool |
| `F` | Frame the current selection |
| `V` (hold) | Vertex snap — grab a vertex and snap it onto another mesh |
| `Ctrl` (hold) | Invert grid snapping for the duration of a drag |
| `Numpad 1` `3` `7` `0` | Front / Right / Top / Isometric views · `5` toggles orthographic |
| `Ctrl` + `Z` / `Y` | Undo / Redo |
| `Ctrl` + `C` / `X` / `V` / `D` | Copy / Cut / Paste / Duplicate |
| `Ctrl` + `A` / `Ctrl` + `Shift` + `A` / `Ctrl` + `I` | Select All / Deselect All / Invert Selection |
| `Ctrl` + `Shift` + `N` | Create Empty Child of the selection |
| `Alt` + `Shift` + `A` | Toggle active state of the selection |
| `Shift` + `A` | Quick Create menu at the cursor |
| `Ctrl` + `S` / `Ctrl` + `Shift` + `S` | Save · Save As |
| `F1` / `F2` / `F3` | Play–Stop · Pause–Resume · single-frame Step |
| `F4` / `F11` | Maximize the Game view · window fullscreen |
| `Esc` | Release the cursor from the running game |

The full list, searchable, is in **Preferences ▸ Shortcuts**.

## Project layout

```
src/
  Core/       Window, input, clock, logging, profiler, project paths
  Renderer/   GL loader, shaders, models, meshes, textures, camera, framebuffers
  Game/       World (EnTT), player controller, scene serialization, components,
              ComponentRegistry (reflection), the hot-reloadable game module
  Editor/     Editor layer, asset library, import pipeline, and the panel modules
              compiled into TartarusEditor.dll
  Physics/    AABB, frustum
  Audio/      miniaudio wrapper
extern/       Vendored single-header libraries, icon font, branding
tools/        TifSplitter — standalone TIFF-to-PNG channel splitter
project/      The scene and editor preferences being authored
```

## Roadmap

### Shipped

- Linear HDR pipeline with tone mapping (Reinhard / ACES / AgX)
- Cascaded shadow maps for the sun; point- and spot-light shadows
- GPU (SSBO) light buffer · clustered-forward light culling (16 × 9 × 24 froxel grid, compute-driven)
- Decoded-texture disk cache with source and settings invalidation
- **Hot-reloadable editor panels** — the toolbar and every dock panel swap without restarting
- **Reflection-registered components** — one declaration generates serialization + Inspector + Add-Component entry
- **Runtime audio** — `AudioSourceComponent` play-on-start with world-positioned sources
  ([#201](https://github.com/JCamberos27/TartarusEngine/issues/201))
- **Editor / Unity parity pass** — Hierarchy reordering & keyboard nav, Inspector component
  menu + copy/paste, Grid & Snap and Gizmos popovers, Pause & Step, GameObject-menu ops
  ([#236](https://github.com/JCamberos27/TartarusEngine/issues/236), ongoing)

### Next

- **Collision beyond AABB** — triggers, gameplay raycasts, capsule and mesh colliders
  ([#185](https://github.com/JCamberos27/TartarusEngine/issues/185))
- **GPU timer queries** — the CPU profiler accounts for only ~2 ms of a 7 ms frame; the rest is
  unmeasured ([#197](https://github.com/JCamberos27/TartarusEngine/issues/197))
- **Image-based lighting** — irradiance + prefiltered specular probes and a BRDF LUT, so
  surfaces out of direct light stop reading flat
  ([#196](https://github.com/JCamberos27/TartarusEngine/issues/196))
- **Screen-space effects** on the HDR buffer — SSAO, bloom
- **Standalone build export** — ship a scene as a runnable game without the editor
- Finish the remaining [#236](https://github.com/JCamberos27/TartarusEngine/issues/236) backlog —
  Asset Browser gaps, Inspector list/array fields, custom Hierarchy folders, tag / layer indicators

## License

Released under the [MIT License](LICENSE).

Bundled and fetched third-party components (miniaudio, stb, nlohmann/json, Font Awesome, GLFW,
GLM, Assimp, EnTT, Dear ImGui, ImGuizmo, libtiff) remain under their own licenses — see
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Branding assets under `extern/branding/` are
not covered by the MIT license.
