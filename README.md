<div align="center">

<img src="docs/images/logo.png" alt="Tartarus Engine" width="520">

**A C++17 / OpenGL 4.6 game engine and scene editor for first-person games.**

[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
[![OpenGL 4.6](https://img.shields.io/badge/OpenGL-4.6%20core-5586A4?style=flat-square&logo=opengl&logoColor=white)](https://www.khronos.org/opengl/)
[![Platform](https://img.shields.io/badge/platform-Windows-0078D6?style=flat-square&logo=windows&logoColor=white)](#building)
[![CMake](https://img.shields.io/badge/build-CMake-064F8C?style=flat-square&logo=cmake&logoColor=white)](#building)
[![CI](https://img.shields.io/github/actions/workflow/status/JCamberos27/TartarusEngine/build.yml?branch=main&style=flat-square&label=build)](https://github.com/JCamberos27/TartarusEngine/actions)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)

</div>

---

Tartarus is a from-scratch game engine with a full editor front-end — dockable panels, transform
gizmos, an asset browser, prefabs, undo/redo, and an in-editor play mode. It renders imported
FBX / glTF / OBJ models with PBR materials and skeletal animation through a
[hand-rolled OpenGL 4.6 core loader](https://github.com/JCamberos27/TartarusEngine/discussions/282)
— no glad / codegen step in the build.

Two design choices shape the codebase. The **gameplay module** and the **editor's panels** are
each compiled into [hot-reloadable DLLs](https://github.com/JCamberos27/TartarusEngine/discussions/257),
so game systems *and* editor UI can be rebuilt and swapped in without closing the editor or
losing the open scene. And engine components are
[reflection-registered](https://github.com/JCamberos27/TartarusEngine/discussions/261) —
declaring one in a single place gives it JSON serialization, an Inspector section, and an
Add-Component menu entry, with no per-component editor code.

Every feature below links to a live discussion thread that tracks, in detail, exactly what that
feature can do today and what's still planned.

## Features

### Editor

| Feature | Summary |
|---|---|
| **[Dockable layout & panels](https://github.com/JCamberos27/TartarusEngine/discussions/256)** | Scene, Game, Hierarchy, Inspector, Asset Browser, Console in a real ImGui dock tree; the arrangement persists, and Statistics / History sit as transparent corner overlays |
| **[Hot-reloadable modules](https://github.com/JCamberos27/TartarusEngine/discussions/257)** | The toolbar and every dock panel live in `TartarusEditor.dll`; gameplay lives in `TartarusGame.dll`. Rebuild either and it swaps at the next safe frame, scene intact |
| **[Themes](https://github.com/JCamberos27/TartarusEngine/discussions/258)** | Bento (dark-mode SaaS, default), Prism, and a Windows XP Luna skin — colours and layout metrics, swapped live |
| **[Transform gizmos & Batch Transform](https://github.com/JCamberos27/TartarusEngine/discussions/259)** | Translate / rotate / scale / rect, local vs. world, pivot vs. bounds-center, a combined multi-select gizmo, and a relative Batch Transform panel |
| **[Scene Hierarchy](https://github.com/JCamberos27/TartarusEngine/discussions/260)** | Drag-to-reparent and drag-between-rows to reorder siblings, full keyboard navigation + type-to-select, per-kind icons with a right-hand active-eye column, and a Unity-style GameObject right-click menu |
| **[Inspector & component system](https://github.com/JCamberos27/TartarusEngine/discussions/261)** | Per-header Reset / Copy / Paste-Values / Remove menu, an Add-Component search box, full multi-select editing, and reflection-driven sections for registered components |
| **[Lights](https://github.com/JCamberos27/TartarusEngine/discussions/262)** | Editable viewport gizmos (range sphere / spot cone / aim arrows), a Lights panel with solo-mute-frame, look-through-light, drop-to-surface, Kelvin colour, and per-light shadow tuning |
| **[Selection, snapping & navigation](https://github.com/JCamberos27/TartarusEngine/discussions/263)** | Fly / orbit / pan / dolly, axis presets, box-select, Select All / Deselect / Invert, a Grid & Snap popover with per-operation increments, and hold-`V` vertex snapping |
| **[Play mode & Game view](https://github.com/JCamberos27/TartarusEngine/discussions/264)** | Play / Pause / single-frame Step inside the docked Game panel with the editor still live; scene state snapshotted on entry; resolution presets and a contrast-adaptive stats overlay |
| **[Undo / redo & History](https://github.com/JCamberos27/TartarusEngine/discussions/270)** | Delta-compressed whole-scene history with a jump-to-any-step History panel; covers asset-library operations too |
| **[Console & logging](https://github.com/JCamberos27/TartarusEngine/discussions/271)** | Streamed engine log with level filter, search, and collapsed repeats |
| **[Statistics & Profiler HUD](https://github.com/JCamberos27/TartarusEngine/discussions/272)** | FPS, draw calls, triangle / entity counts, GL bind counters, and per-pass CPU timings as a transparent overlay |
| **[Screenshot / Capture tool](https://github.com/JCamberos27/TartarusEngine/discussions/273)** | Editor / Scene / clean-Scene / Game modes, fixed-resolution and supersample options, PNG or JPG, `Print Screen` or a sentinel-file trigger |
| **[Preferences & settings](https://github.com/JCamberos27/TartarusEngine/discussions/274)** | Tabbed settings — theme, UI scale, viewport, light gizmos, grid & snap, capture, a searchable shortcut table — all persisted |
| **[Adaptive-contrast HUD system](https://github.com/JCamberos27/TartarusEngine/discussions/275)** | A shared async luminance sampler keeps every viewport overlay and the corner monogram legible over any render |
| **[Startup splash & frosted UI](https://github.com/JCamberos27/TartarusEngine/discussions/284)** | A Win32 layered-window splash that doesn't block a synchronous load, and a half-res Gaussian backdrop behind modal dialogs |

### Rendering

| Feature | Summary |
|---|---|
| **[PBR & HDR pipeline](https://github.com/JCamberos27/TartarusEngine/discussions/266)** | Forward PBR (albedo / normal / metallic / roughness / AO / emissive) into a multisampled `RGBA16F` target; exposure + Reinhard / ACES / AgX tone-mapping; per-viewport HDR targets |
| **[Lighting & shadows](https://github.com/JCamberos27/TartarusEngine/discussions/267)** | One `std430` GPU light buffer; clustered-forward culling on a 16 × 9 × 24 froxel grid; cascaded sun shadows, plus cube-map point and perspective spot shadows |
| **[Image-based lighting](https://github.com/JCamberos27/TartarusEngine/discussions/281)** | Split-sum IBL — irradiance + prefiltered-specular cubes and a BRDF LUT baked from the procedural sky, auto-rebaked when it changes |
| **[Sky & environment](https://github.com/JCamberos27/TartarusEngine/discussions/277)** | Per-scene procedural gradient sky feeding the IBL probes, a scene Ambient control, and a distance-faded infinite grid |
| **[Skeletal animation & the Animator](https://github.com/JCamberos27/TartarusEngine/discussions/276)** | Up to 100 bones per model from glTF / FBX rigs, ticked in the editor or the running game; an Animator component for procedural spin / orbit / bob / hue-cycle |
| **[OpenGL 4.6 layer](https://github.com/JCamberos27/TartarusEngine/discussions/282)** | Hand-rolled loader, direct-state-access mesh path, a redundant-bind cache with counters, and `KHR_debug` output routed to the Console |
| **[Primitive meshes & preview renderers](https://github.com/JCamberos27/TartarusEngine/discussions/283)** | Procedural cube / sphere / cylinder / capsule / cone / pyramid / torus / plane, an orbitable model preview, texture channel isolation, and the translucent drag-ghost + selection wash |

### Assets, core & gameplay

| Feature | Summary |
|---|---|
| **[Import, browser & prefabs](https://github.com/JCamberos27/TartarusEngine/discussions/265)** | FBX / OBJ / glTF models, common image and audio formats; a folder tree with search and labels; per-asset import settings; drag-in placement with a live preview; save-as-prefab |
| **[ECS, serialization, physics, audio & caching](https://github.com/JCamberos27/TartarusEngine/discussions/268)** | EnTT with a transform hierarchy; JSON scene format; a first-person controller with sub-stepped AABB collision; miniaudio; a decoded-texture disk cache; a frame profiler |
| **[Scene & project management](https://github.com/JCamberos27/TartarusEngine/discussions/279)** | New / Open / Save / Save As, a format-version guard, auto-save with a crash-recovery prompt, and last-scene-reopened-on-launch |
| **[Game module & built-in systems](https://github.com/JCamberos27/TartarusEngine/discussions/278)** | The hot-reloadable gameplay layer — Spin, Transform Controller, and Animator systems, all driven by reflection-registered components |

### Tools & build

| Feature | Summary |
|---|---|
| **[TifSplitter](https://github.com/JCamberos27/TartarusEngine/discussions/269)** | A standalone CLI that turns high-bit-depth / multi-channel TIFFs into engine-ready PNGs — channel splitting, normal-map green-flip, heightmap normalization, tiling, batch mode |
| **[CMake, dependencies & CI](https://github.com/JCamberos27/TartarusEngine/discussions/280)** | `-A x64` with no hardcoded generator; FetchContent for GLFW / GLM / Assimp / EnTT / ImGui / ImGuizmo; a headless `--smoke-test`; GitHub Actions builds every push in Debug and Release |

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
[issue #187](https://github.com/JCamberos27/TartarusEngine/issues/187) (closed: all 54 findings
resolved). The Unity feature-gap audit
([issue #236](https://github.com/JCamberos27/TartarusEngine/issues/236)) and the native
component-registration / prefab-override work
([#302](https://github.com/JCamberos27/TartarusEngine/issues/302),
[#315](https://github.com/JCamberos27/TartarusEngine/issues/315)) have all landed; the next
large piece is a real collision system
([#185](https://github.com/JCamberos27/TartarusEngine/issues/185)).
Windows-only CI builds every push in Debug and Release.

## Building

Requires **CMake 3.16+**, an **MSVC** toolset (Visual Studio 2022 or newer), and a GPU / driver
that can create an **OpenGL 4.6 core** context. GLFW, GLM, Assimp, EnTT, Dear ImGui, and
ImGuizmo are fetched by CMake on first configure — that step needs an internet connection; later
builds are offline.

```bash
cmake -S . -B build -A x64
cmake --build build --config Release
```

No `-G` on purpose — CMake picks whichever Visual Studio is installed, so a toolset bump doesn't
break the build. The executable lands at `build/Release/TartarusEngine.exe`, with the reloadable
`TartarusGame.dll` and `TartarusEditor.dll` beside it. First launch opens
`project/scenes/Showcase.json`, then reopens whatever scene you last had open.

> The engine must be closed before rebuilding `TartarusEngine.exe`; the two DLLs can be rebuilt
> while it runs — see [Hot-reloadable modules](https://github.com/JCamberos27/TartarusEngine/discussions/257).

## Controls

| Input | Action |
|---|---|
| `Right-drag` | Look · `WASD` `Q` `E` to fly while held · `Shift` to sprint |
| `Alt` + `Left-drag` / `Right-drag` / `Middle-drag` | Orbit selection · dolly · pan |
| `W` `E` `R` `T` | Translate / Rotate / Scale / Rect tool |
| `F` · `V` (hold) · `Ctrl` (hold) | Frame selection · vertex snap · invert grid snap for the drag |
| `Numpad 1` `3` `7` `0` · `5` | Front / Right / Top / Isometric · toggle orthographic |
| `Ctrl` + `Z` / `Y` · `Ctrl` + `C` / `X` / `V` / `D` | Undo / Redo · Copy / Cut / Paste / Duplicate |
| `Ctrl` + `A` / `Ctrl` + `Shift` + `A` / `Ctrl` + `I` | Select All / Deselect All / Invert |
| `Ctrl` + `Shift` + `N` · `Alt` + `Shift` + `A` · `Shift` + `A` | Create Empty Child · Toggle Active · Quick Create at cursor |
| `Ctrl` + `S` / `Ctrl` + `Shift` + `S` | Save · Save As |
| `F1` / `F2` / `F3` · `F4` / `F11` · `Esc` | Play–Stop · Pause · Step · maximize Game view · window fullscreen · release cursor |

The full, searchable list is in **Preferences ▸ Shortcuts**.

## Project layout

```
src/
  Core/       Window, input, clock, logging, profiler, splash, screenshot, project paths
  Renderer/   GL loader + state cache, shaders, models, meshes, textures, camera,
              framebuffers, shadows, clustered light grid, IBL probes, sky, tonemapper
  Game/       World (EnTT), player controller, scene serialization, components,
              ComponentRegistry (reflection), the hot-reloadable game module + systems
  Editor/     Editor layer, asset library, import pipeline, adaptive-contrast sampler,
              and the panel modules compiled into TartarusEditor.dll
  Physics/    AABB, frustum
  Audio/      miniaudio wrapper
extern/       Vendored single-header libraries, icon font, branding
tools/        TifSplitter — standalone TIFF-to-PNG channel splitter
project/      The scene and editor preferences being authored
```

## Roadmap

### Shipped

- Linear HDR pipeline with tone mapping (Reinhard / ACES / AgX)
- Cascaded sun shadows; cube-map point and perspective spot shadows
- GPU (`std430` SSBO) light buffer · clustered-forward light culling (16 × 9 × 24 froxel grid)
- Image-based lighting baked from the procedural sky ([#196](https://github.com/JCamberos27/TartarusEngine/issues/196))
- Hot-reloadable editor panels · reflection-registered components · runtime `AudioSourceComponent` playback
- Editor / Unity parity pass — Hierarchy reordering & keyboard nav, Inspector component menu + copy/paste, Grid & Snap and Gizmos popovers, Pause & Step, GameObject-menu ops ([#236](https://github.com/JCamberos27/TartarusEngine/issues/236))
- Live prefab instances with per-field & per-component overrides — accent-tinted labels, in-Inspector Revert / Apply to Prefab, Unpack ([#302](https://github.com/JCamberos27/TartarusEngine/issues/302), [#315](https://github.com/JCamberos27/TartarusEngine/issues/315))

### Next

- **Collision beyond AABB** — triggers, gameplay raycasts, capsule and mesh colliders ([#185](https://github.com/JCamberos27/TartarusEngine/issues/185))
- **GPU timer queries** — GPU-side pass timing is not yet instrumented ([#197](https://github.com/JCamberos27/TartarusEngine/issues/197))
- **Placed reflection probes** — local cubemaps and HDRI input, beyond today's single global sky probe
- **Screen-space effects** on the HDR buffer — SSAO, bloom
- **Standalone build export** — ship a scene as a runnable game without the editor
- Remaining editor polish — Inspector list/array fields, component reorder, an "Open Prefab" edit mode, nested prefabs / variants

## License

Released under the [MIT License](LICENSE). Bundled and fetched third-party components (miniaudio,
stb, nlohmann/json, Font Awesome, GLFW, GLM, Assimp, EnTT, Dear ImGui, ImGuizmo, libtiff) remain
under their own licenses — see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Branding assets
under `extern/branding/` are not covered by the MIT license.
