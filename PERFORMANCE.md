# Performance Pass — Sandbox scene

Goal: very high framerates in the Sandbox (targets: **144+ fps at 1440p**, **240+ fps at 1080p**) while
keeping visual quality. Test machine: GTX 1080 Ti, i9-7980XE, 3840x2160 window.

## How to measure

```
TartarusEngine.exe --project <project> --perf-bench <scenes dir | scene.json> [--perf-res 1920x1080]
```

Runs 600 frames each of **edit**, **play** (docked Game view) and **play-max** (maximized Game view),
skipping 100 warm-up frames per phase, with VSync / FPS cap / GL debug off. Prints average and worst
frame time and every CPU/GPU profiler scope. Release builds have no console: redirect stdout to a file.

Add `--perf-sample` for a statistical CPU profile of the main thread per phase (it suspends the thread
about every 1 ms and symbolizes the stack via dbghelp). It prints the top functions by self time,
inclusive time, and "external" time (which engine function was calling into a DLL such as the GL
driver or PhysX). `TARTARUS_SAMPLE_FOCUS=<function>` also prints the call chains that reach it.

## Phase 1 results (PR #490, 4K window)

| Phase | Before | After |
|---|---|---|
| Edit | 125 fps (8.0 ms) | ~133 fps (7.5 ms) |
| Play (docked) | 55 fps (18.2 ms) | ~96 fps (10.4 ms) |
| Play (maximized) | 66 fps (15.2 ms) | ~123 fps (8.1 ms) |

| CPU scope (play-max) | Before | After |
|---|---|---|
| First Person IK (body + arms + gun) | 7.5 ms | 2.4 ms |
| Animator Controllers | 1.7 ms | 0.75 ms |
| Spot Shadow Pass (CPU) | 1.05 ms | 0.08 ms |
| Sun Shadow Pass (CPU) | 1.1 ms | 0.8 ms |
| Scene Draw (CPU) | 1.45 ms | 1.2 ms |

Play is now roughly balanced: ~8 ms CPU vs ~4.4 ms GPU for the Game view at 4K.

## Done (Phase 0 + 1)

**Measurement**
- New profiler scopes: game module tick, animator controllers, FP body sub-stages (foot IK, spine and
  shoulders, arms on pieces and twins, world gun clearance, head/torso skinning, elbow search),
  outfits + transform cache, asset pump, probe gather, local shadow gather, whole Scene/Game view
  (CPU and GPU), ImGui (GPU), swap buffers.
- `--perf-bench` accepts a single scene file, `--perf-res WxH`, and adds a maximized-play phase.

**Pose math (bit-identical output)**
- `LocalTRS::ToMatrix` builds the matrix directly instead of three 4x4 products; `AffineMul` skips the
  exact-zero terms. Used by every pose rebuild and IK refresh.
- `Model::ApplyLocalPose` only computes globals for bones and their ancestors. Clothing pieces carry
  the full ~355-node skeleton but skin 10–54 bones. Any other node is derived on demand in
  `NodeTransform`.

**First-person body**
- The arm solve (pieces and world twins) skips pieces that skin nothing under the chest or clavicles:
  legs, feet, socks, shoes and the rigid balaclava. What's drawn is identical. The driver and arms
  pieces always solve.
- The spine and neck chain rotation likewise skips pieces that skin nothing under the chain, and only
  refreshes the chain's path, not all 355 globals.
- Gun keep-out head points are decimated on a 5 mm grid in bind pose (50.6k → 6.6k points; error
  ≤4.3 mm against a 5 cm clearance and 1 cm push steps).
- `SkinnedPoints` fetches each piece's bone palette once rather than per influence. The twins' torso
  points are reused within the frame when the gun keep-out already skinned them.
- The elbow-clearance swivel search stops scanning an angle as soon as it can't beat the best so far
  (`FirstPersonBodyElbowGapAbove`). The result is the same.

**Animator**
- State lengths are memoized per frame after `AdvanceAnimator`. Every rig's pose used to recompute
  them over every rig's clips (O(pieces²)).

**Shadows**
- Spot and point casters are gathered once per frame, not once per light and per cube face.
- Animated casters are now culled against spot/point frustums with padded bounds. Before, every
  skinned character was drawn into every spot map and all 6 faces of every point cube.
- Point lights first cull casters against the light's range.
- Spot and point maps are only redrawn when their light, map or any caster inside them changed
  (moved, swapped model/material, or animated). Static lights keep last frame's depth.
- `glIsEnabled` removed from the per-caster loops.

**Renderer CPU**
- `Shader` keeps each scalar/vector uniform's last value and skips unchanged `glUniform*` calls.
  It only trusts writes made while its own program is bound, and a write to another program makes
  every cache forget.

**Tried, not kept**
- AVX2 + LTO: no measurable gain (within the ±3% run-to-run noise).
- Sorting elbow-clearance points nearest-first: the sort cost more than the early exit saved.

**Verification**: `--unit-tests` (1811 checks pass), `--weapon-test` (45 checks pass, no GL/log errors),
`--smoke-test` (19 scenes pass; one run flaked on `smoke_materials` with 0 draws and did not repeat in
three reruns).

## Phase 2 results

`--perf-bench` Sandbox, `--perf-res` as listed (window 1942x1136), average fps (worst frame):

| Resolution | Phase | Phase 1 | Phase 2 |
|---|---|---|---|
| 1920x1080 | Edit | 129 (12.2 ms) | 173 (10.2 ms) |
| 1920x1080 | Play (docked) | 101 (31.1 ms) | 142 (12.8 ms) |
| 1920x1080 | Play (maximized) | 132 (27.9 ms) | **203** (9.4 ms) |
| 2560x1440 | Edit | 135 (11.7 ms) | 177 (10.0 ms) |
| 2560x1440 | Play (docked) | 98 (31.0 ms) | 136 (13.1 ms) |
| 2560x1440 | Play (maximized) | 131 (31.4 ms) | **200** (8.8 ms) |
| 3840x2160 | Play (maximized) | 127 (28.5 ms) | 193 (8.8 ms) |

The 1440p target (144+) is met. 1080p reaches ~203 of the 240 target.

**Correction (found in phase 3):** the play rows above did not render at the listed resolution. The
Game view renders at the saved Game view preset, which was "1920x1080 FHD", so every play run drew
the game at 1080p and letterboxed it into the window. "GPU time barely changes from 1080p to 4K" was
this artifact. `--perf-bench` now forces the Game view to `--perf-res` (see Phase 3).

### Biggest finding: GL state reads stall the frame

NVIDIA's driver runs its own worker thread. Every `glGet*` / `glIsEnabled` / fence poll / buffer map
makes the engine thread wait until that worker has drained everything queued so far. The renderer's
save/restore reads were scattered across ~15 places (per-mesh render-state scopes, sky, particles,
tonemapper, crosshair, shadow passes), and the sampler put ~20% of the main thread in that wait.
Removing only some of them just moves the wait to the next one, so the fix had to be total:

- `extern/glloader` now shadows the render state (viewport, toggled caps, depth func/mask, cull mode,
  blend funcs, framebuffer bindings) as it is set. Reads are answered from the shadow and redundant
  sets are dropped. Code that changes that state behind the loader and leaves it changed must call
  `GLStateShadow_Invalidate()`. Dear ImGui's backend has its own loader but restores everything.
- The cluster-overflow fence poll and map run every 16th cull instead of every cull.

Result: play-max 1080p 6.5 → 4.95 ms.

### Done (Phase 2)

- **Sky after opaque**: `SkyAtmosphere::RenderView` only computes (LUTs, aerial, clouds). `DrawSky`
  composites at the far plane under `GL_LEQUAL` after the opaque pass (also HDRI and gradient skies).
  Sky composite 0.35 → 0.08 ms GPU.
- **Checkerboarded clouds**: a 2x2 Bayer pattern raymarches a quarter of the texels per frame. The
  temporal pass clamps against freshly marched neighbours and keeps history elsewhere. Resets march
  everything. Clouds 0.99 → 0.45 ms GPU; thin wisps are slightly grainier at 1:1.
- **Amortized IBL rebake**: the drifting-cloud rebake runs over 7 frames into staging cubes that are
  swapped in at the end. Forced and first bakes stay immediate. The 4 s spike went from 6.4 to 1.6 ms.
- **Clip warm-up**: the first time a rig meets a controller, every clip of every state is resolved.
  Play worst frame 33 → 16 ms.
- **Unresolvable clip refs** back off for 1 s instead of hitting the disk every frame.
- **ImGui GLFW backend** (build-tree patch): mouse passthrough is only re-applied when it changes, not
  per viewport per frame.
- **FP IK**: the two-bone solve refreshes the limb path once; the elbow gap search uses SSE with a
  block early-out. FP IK 2.34 → 1.78 ms.
- **GL state shadow** and **throttled cluster readback** (above).
- **`--perf-sample`** profiler.

**Tried, not kept**
- Sun CSM amortization (far cascades every 2nd/4th frame): only 4 → 3 refits per frame, ~0.09 ms GPU,
  no CPU gain.
- Per-pass declared state for `ShaderStateScope`: worked (0.3 ms), but each pass's one remaining
  query still drained the driver queue. Superseded by the loader shadow.

**Verification**: `--unit-tests` (7221 checks pass), `--weapon-test` (45 checks pass), Sandbox
`--smoke-test` passes with no GL errors; smoke screenshots match the previous build apart from
run-to-run noise in the animated shot.

## Phase 3 results

`--perf-bench` Sandbox at `--perf-res`, the Game view rendering at that size. Paired runs: phase 2
(`main` at the #491 merge, run with the Game view preset set to the same size) and phase 3
back to back, two runs each, averaged. Average ms (fps):

| Resolution | Phase | Phase 2 | Phase 3 |
|---|---|---|---|
| 1920x1080 | Edit | 5.68 (176) | 4.67 (214) |
| 1920x1080 | Play (docked) | 7.27 (137) | 5.52 (181) |
| 1920x1080 | Play (maximized) | 4.95 (202) | **4.06 (246)** |
| 2560x1440 | Edit | 5.70 (176) | 4.55 (220) |
| 2560x1440 | Play (docked) | 6.89 (145) | 5.89 (170) |
| 2560x1440 | Play (maximized) | 6.14 (163) | **5.88 (170)** |

Both targets are met: 240+ fps at 1080p and 144+ fps at 1440p, maximized. Worst frames in play-max
are 7–8 ms. At 1080p the CPU and GPU are now about even (~3.9 ms each); 1440p is GPU-bound
(Game view GPU 5.2 ms: scene draw 2.2, SSAO 1.0, view model 0.7, clouds 0.7, sun shadows 0.7).

### Done (Phase 3)

**Driver syncs**
- **Dear ImGui's GL backend** (build-tree patch of `imgui_impl_opengl3.cpp`): its ~20 `glGet*` /
  `glIsEnabled` state backups go through the engine's state shadow (`src/Editor/ImGuiGLHooks.cpp`),
  which answers the ones it doesn't track with the values the engine always leaves (texture unit 0,
  nothing bound, fill mode, add blending). It keeps one VAO instead of creating one per render.
  Play-max 4.95 → 4.52 ms.
- **Cluster overflow flag** is read from a persistently mapped, coherent buffer that a copy fills: no
  fence poll or map, so no wait on the driver's worker.

**GPU**
- **Spot shadow static cache**: each spot keeps its static casters in a second layer that is only
  redrawn when they change. Every frame the live layer is restored from it and only the animated or
  recently moved casters are drawn. The restore covers only where those casters are and were (their
  bounds through the light); a whole 2048² layer is 16 MB. A readback check against a full restore was
  bit-identical over 685 play frames. Spot Shadow Pass 0.63 → 0.10 ms GPU.
- **SSAO** taps read view z in closed form instead of reconstructing a position through the inverse
  projection. 0.60 → 0.54 ms.

**CPU**
- **Uniform locations**: `Shader::Loc` checks a small table keyed by the name's pointer before the
  string map.
- **Keep-out skinning**: head and torso points are packed at first use (bind position, normalized
  weights, bones remapped to the few the region uses); each frame builds a palette of just those bones
  with the world transform folded in. Head 0.22 → 0.085 ms, torso 0.17 → 0.10 ms.
- **Elbow swivel search** only measures torso points inside the slab and ring the arm's swing can
  reach (plus the clearance). The picked angle is unchanged; a unit test checks it against a search
  over every point. 0.28 → 0.09 ms.
- **`AABB::Transformed`** in center-extent form instead of eight corner transforms.

**Measurement**
- `--perf-bench` renders the Game view at `--perf-res` (or Free Aspect without it) for the run only,
  without touching the saved preset.

**Tried, not kept**
- SSAO view-z mip pyramid (far taps read smaller levels, as in Scalable Ambient Obscurance): no gain.
  The pass is limited by texture fetch rate, not cache misses.

**Verification**: `--unit-tests` (7222 checks pass), `--weapon-test` (45 checks pass), Sandbox
`--smoke-test` passes with no GL errors. Smoke screenshots match phase 2 except the Scene-view shot
facing the sun, which differs between runs of the same build too (clouds drift in real time).

## To do

At 1080p the CPU (main thread) and GPU are about even; 1440p and up are GPU-bound.

**GPU**
1. Model fragment shader cost: the view model (arms + gun, a small part of the screen) takes
   0.5 ms at 1080p and scene draw 1.4 ms. Profile the shader (clustered lights, shadow taps, IBL).
2. Depth prepass into the MSAA HDR target reused by SSAO (the SSAO prepass excludes different
   entities than the main pass, so it needs its own exclusions); move the `discard`s into shader
   variants so ordinary opaque draws keep early-Z.
3. SSAO: 32 taps at full resolution, fetch-bound. Half resolution or fewer taps would trade quality.
4. Sun shadows 0.7 ms (vertex/submission-bound): skip sub-texel casters in far cascades.

**CPU**
5. FP body ~1.4 ms: `ApplyLocalPose` / IK globals / clip sampling. Multithread the per-piece work,
   or sample the driver once and copy to followers through a node remap.
6. PhysX `fetchResults` wait: overlap simulate with animation / render prep.
7. GL call volume: share one mesh per primitive kind and draw identical mesh+material runs instanced.
8. `Material::Hash` ~2.5%, `glfwWindowVisible` / `WindowFromPoint` ~1.7%.

**Hitches**
9. Edit-mode SSAO prepass ~117 ms on first use (shader compile); sun shadow pass spike on the first
   edit frames.

Character LODs are out of scope for this pass.
