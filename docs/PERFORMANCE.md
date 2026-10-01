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

## Results

`--perf-bench` Sandbox, play mode with the Game view maximized, average fps. The per-change numbers and
the full write-ups are in PRs #490–#493.

| | Before | Phase 1 (#490) | Phase 2 (#491) | Phase 3 (#492) | Phase 4 (#493) |
|---|---|---|---|---|---|
| 1920x1080 | — | — | 202 | 246 | **258** |
| 2560x1440 | — | — | 163 | 170 | **192** |
| 3840x2160 window | 66 | 123 | — | — | — |

Both targets are met. 1080p play-max is GPU-bound (GPU ≈ the whole 3.9 ms frame), so CPU savings no
longer show up as fps there. Phase 1's and phase 2's own tables were measured before the bench trap below
was found; the phase 2 row here is the re-measured one.

**Bench trap:** the Game view renders at its saved *preset* (e.g. "1920x1080 FHD"), not the window size.
`--perf-bench` now forces it to `--perf-res` (or Free Aspect) for the run only. Results taken before
phase 3 at "1440p" and "4K" were really 1080p, letterboxed.

### Render resolution (4K screens)

The 3D view renders at an internal height and is upscaled to the window: an FSR1-style RCAS sharpen, then a
bicubic resample (`src/Renderer/Upscaler.*`, 0.25 ms GPU at 4K). The HUD and ImGui draw after it, at native
resolution. Set it in Preferences â†’ Performance â†’ Render Resolution (Native / 2160 / 1440 / 1080 / 900 / 720, default
**1080**) with a sharpness slider, or with `--render-height N` (the bench defaults to native).

Sandbox with its enemy squad, 3840x2160 window, play-max:

| Internal | fps |
|---|---|
| Native (2160) | 72 |
| 1440 | 126 |
| 1080 | **163** |

The enemy squad (4 soldiers) costs ~0.3 ms CPU here; see `docs/ENEMY_AI.md` for its own numbers.

**Where the GPU time goes** (knockout tests, 1440p play-max): lighting and shading ~0.3 ms of Scene Draw,
skinning math ~0.03 ms (GPU pre-skinning isn't worth building), the player's body ~0, the NPCs ~0.3 ms,
the rest of the scene ~0.8 ms. Sun shadows are vertex-bound.

## What was done

- **Measurement:** profiler scopes throughout (game tick, animator, each first-person body stage,
  outfits, shadows, whole Scene/Game views, ImGui, swap); `--perf-bench` with a single scene and
  `--perf-res`; `--perf-sample`.
- **Pose math and first-person body:** direct TRS matrices, globals only for bones that skin, arm and
  spine solves skip pieces with nothing to move, decimated gun keep-out points, packed keep-out
  skinning, pruned elbow search (SSE), per-frame memoized state lengths, clip warm-up at first use.
- **Driver syncs:** no `glGet*` / `glIsEnabled` on the frame path (the render state shadow, below); the
  cluster-overflow flag read from a persistently mapped buffer.
- **Shadows:** casters gathered once per frame and culled per light; spot/point maps redrawn only when
  something in them changed; spot maps keep a static-caster layer; sun cascades skip casters whose shadow
  can't reach their slice.
- **GPU:** sky composited after opaque, checkerboarded clouds, amortized IBL rebake, cheaper SSAO taps,
  vertex-cache-ordered meshes (meshoptimizer).
- **CPU:** uniform value and location caches, cheaper material hash, `AABB::Transformed` in
  center-extent form, fewer ImGui GLFW platform queries.

## Rules these rely on (don't undo)

- **Render state shadow.** `extern/glloader` shadows viewport, caps, depth, cull, blend and framebuffer
  bindings, answers reads from the shadow and drops redundant sets. Code that changes that state behind
  the loader and leaves it changed must call `GLStateShadow_Invalidate()`. Any new `glGet*` on the frame
  path stalls the frame on NVIDIA's driver thread (it cost ~20% of the main thread before).
- **Dear ImGui backends are patched in the build tree.** The GL backend's state backups go through the
  engine's shadow (`src/Editor/ImGuiGLHooks.cpp`); the GLFW backend only re-applies mouse passthrough on
  change and skips the hover test while the cursor is captured.
- **`Shader` uniform cache** trusts only writes made while its own program is bound; a write to another
  program makes every cache forget.
- **Shadow maps are cached.** Spot and point maps redraw only when their light, map or a caster in them
  changed (moved, swapped model or material, animated). A new way to change a caster must mark it dirty.

## Tried, not kept

- AVX2 + LTO: within the ±3% noise.
- Sorting elbow points nearest-first: the sort cost more than it saved.
- Sun cascade amortization (far cascades every 2nd/4th frame): ~0.09 ms GPU, no CPU gain.
- Per-pass declared state for `ShaderStateScope`: superseded by the loader shadow.
- SSAO view-z mip pyramid: no gain (fetch-rate bound, not cache misses).
- Cascade fit fix (to-do 1): correct, but ~0.25 ms slower at 1080p with no visible change.

## To do

What is left, roughly by expected value. At 1080p the GPU is the limit, at 1440p more so.

**GPU**
1. **Sun cascade fit bug.** `CascadedShadowMap::Update` interpolates each slice's corners along the full
   camera frustum (to the 500 m far plane) with fractions taken against the 150 m shadow range, so each
   cascade is fitted ~3.3x deeper than the depths that select it (cascade 0 covers 0–33 m but serves
   0–10 m). Fixing it means taking the fractions against the far plane and fitting from 0.88x the previous
   split (the cross-fade band); the last cascade must keep reaching the far plane or shadows beyond the
   shadow distance vanish. The PCF radius and the depth/normal biases are in texels, so they must be
   scaled by old/new fit radius to keep the look (unscaled they turn to acne and 3x narrower penumbrae).
   Done that way, it looked identical and cost ~0.25 ms at 1080p (characters fill 3x more texels per
   axis). It is only worth doing together with a lower `ShadowResolution` (same texel density as now,
   less fill), or if sharper shadows are wanted.
2. **Depth prepass shared with SSAO.** The SSAO prepass (0.6 ms at 1080p incl. compute) draws depth for
   the same view the main pass then draws. Rendering it into the MSAA HDR target's depth and resolving
   for SSAO would give the main pass early-Z on everything, but shading is only ~0.3 ms total, so the
   saving is small unless the prepass itself gets cheaper. Needs `invariant gl_Position` and matching
   vertex math in both shaders, or holes appear.
3. **SSAO** (~0.55 ms at 1080p): 32 taps at full resolution, fetch-bound. Half resolution with a
   depth-aware upsample or fewer taps would trade some quality.
4. **Clouds** (~0.5 ms): already a quarter of texels per frame. Skipping texels behind opaque geometry
   needs history handling so moving objects don't leave holes.
5. **Static geometry** (~0.8 ms of Scene Draw at 1440p): primitives are one Model per entity (139
   spheres at 1152 triangles each, 56 cubes, 36 cylinders). Share one mesh per primitive kind and draw
   identical mesh+material runs instanced, in the main, SSAO and shadow passes. Also cuts CPU draw calls.
6. **Characters' bounds**: characters whose clips live in other files report `HasAnimations() == false`,
   so they are culled on unpadded bind-pose bounds in the main, SSAO and shadow passes. Harmless in the
   Sandbox, but a limb reaching out of the bind box could be culled. Use "has bones" for the padding.

**CPU** (worth it once the GPU is lighter, or on slower CPUs)
7. First-person body ~1.4 ms: `ApplyLocalPose` (the final bone matrices do two generic 4x4 products per
   bone; both are affine), IK global refreshes, clip sampling. Multithread the per-piece work, or sample
   the driver once and copy to followers through a node remap.
8. PhysX `fetchResults` wait (~0.45 ms): overlap simulation with animation and render prep.
9. Animator controllers ~0.46 ms.

**Hitches**
10. Edit-mode SSAO prepass ~117 ms on first use (shader compile); sun shadow pass spike on the first
    edit frames.
11. An enemy soldier's spawn is ~4 ms (5 ms of it was the weapon's clip matching, now shared between model
    instances). Pooling soldiers (reusing a dead one's entities and weapon rig) would take it to ~0.

Fixed in the enemy AI work: the first round fired in a Play stalled up to a second importing the spent case's FBX;
every weapon now warms its case's mesh when it starts (`FirstPersonPresentation::WarmEjectAssets`).

Character LODs are out of scope for this pass.
