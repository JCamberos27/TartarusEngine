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

### Editor interaction stalls (2026-10-07)

The global undo input handler serialized the entire scene on mouse/key activity and again
when the interaction ended. Right-click camera movement, WASD and scrollbar activation could
therefore pay two full snapshot costs despite changing no authored content. This was a CPU
stall independent of the renderer's steady-state fps.

History now caches the last committed authored scene and starts scene transactions from
actual edit requests. This also preserves controls that request undo after changing a value.
Navigation performs no scene serialization; file-only and selection entries reuse the delta
chain anchor, and equal anchors no longer spawn a diff worker. Commits refresh the cached
scene; scene/history/prefab changes and external project updates refresh its context.

Regression check: `TartarusEngine.exe --unit-tests "Editor navigation undo"`.
On this machine, a synthetic 1,500-empty-object scene's two saves took **20–25 ms** across
repeated runs; the new navigation/history test averaged **0.0016–0.0018 ms** per frame over 120 frames with **zero scene
snapshots**. This measures the isolated input/history path, not full rendered frame time or
the user's scene. The check also covers post-change value edits, multi-frame drag coalescing,
unchanged activation, selection, file-only/mixed transactions and undo/redo. Release build
and 11 relevant tests passed (2,755 checks).

**Undo Scene Snapshot** profiler scopes now identify cache initialization and scene-edit
commit costs. Real edits can still serialize a large scene, and thumbnail generation,
first-use GPU resources and imports can still cause separate stalls. Recheck live scrolling,
selection and camera movement with the rebuilt editor before treating all hitches as resolved.

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
- **Play start (2026-10-04): 15-18 s -> ~1.2 s** (first Play of a session, two-gun player).
  - The texture cache keeps one entry per (texture, settings) (`TextureCache` v6). A weapon's 4K maps
    are loaded twice, by the model's own material (defaults: 2048, uncompressed) and by its material
    asset (.meta: 4096, high-quality BC). One shared entry evicted the other every session, so the maps
    were re-decoded and re-compressed on every Play (~7 s for the AK, ~2 s for the other gun's casing).
  - The first-person clips (one FBX per state and track, ~60 files) load clip-only
    (`ModelImportSettings::AnimationOnly`) on worker threads (`AssetLibrary::PreloadAnimationSources`) at
    `FirstPersonPresentation::Start`: ~0.7 s. The log line `Play start: first-person clips - ...` reports it.

## Rules these rely on (don't undo)

- **Texture cache variants.** An entry's filename carries its settings hash; a variant that isn't the
  `.meta`'s is pruned only after 14 days unused. Keying entries by GUID alone again would bring back the
  per-session re-encode.

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
5. **Static geometry** (~0.8 ms of Scene Draw at 1440p): primitives now share one GPU mesh per kind (done, #P2),
   but each is still its own draw. Instancing identical mesh+material runs was built for the main pass and
   the SSAO pre-pass (per-instance matrices in an SSBO, runs merged after sorting on material then mesh) and
   measured: no gain. CPU Scene Draw stayed at ~0.6 ms and the GPU SSAO pre-pass got ~15% slower, so it was
   dropped; the Sandbox is GPU-bound and its ~230 draws are not what costs. Note embedded materials are one
   asset per entity (batch on `Mat.Hash()`, not the pointer) and reflection probes are chosen per object.
   The sun and local shadow passes were not tried (the sun's instance index is the cascade layer).
6. ~~**Characters' bounds**: characters whose clips live in other files report `HasAnimations() == false`,
   so they are culled on unpadded bind-pose bounds in the main, SSAO and shadow passes. Harmless in the
   Sandbox, but a limb reaching out of the bind box could be culled. Use "has bones" for the padding.~~ **Done (#P1)**: Added `Model::HasBones()` and replaced the three padding checks with it (SceneRenderer L451, main L3095 and L3672).

**CPU** (worth it once the GPU is lighter, or on slower CPUs)
7. First-person body ~1.4 ms: the final bone matrices now use affine products (done, #P2: `AffineMul`, unit
   test vs the generic product within 1e-5; the bench's First Person IK timer is ~1.25 ms before and after,
   inside noise). Then (#A4): the per-piece arm-shape copy (`CopyArmShape`) looked up every node's rig twin by
   name each frame, per piece, twice; the pairs are now found once per (piece model, rig) and cached
   (`FirstPersonBody::CopyArmShapeCached`, unit-tested against the name walk). Bench at 1080p, base/new
   interleaved x3: First Person IK 1.17 -> 1.12 ms, 169.2 -> 171.2 fps (consistent in all three pairs; small).
   `--perf-sample` shows the rest is spread thin: IK refresh/compute ~9% of the frame (SolveTwoBone's final
   subtree refresh, foot IK, per-piece solves of the 5-6 upper-body pieces), `SkinnedPoints` ~3%, the rig's clip
   sampling. Each refresh already recomputes only the dirty subtree with affine products, so a further gain needs
   a structural change (share one solve across pieces, or cache local matrices) - not done: the pieces' bone
   lengths differ, and a cached local would go stale when callers write `pose[i]` directly.
8. PhysX `fetchResults` wait (~0.45 ms): overlap simulation with animation and render prep.
9. Animator controllers ~0.46 ms.

**Hitches**
10. First-use stalls, measured on Sandbox with the first 60 frames of each `--perf-bench` phase logged. Fixed by
    `warmRenderResources` (main.cpp, run behind the load): the physical sky's programs, LUT resources and cloud noise
    bake (`SkyAtmosphere::WarmUp`, GPU 268 ms on frame 3 down to ~6 ms), `Ssao::WarmUp` (compute+blur on a 16x16
    target) and the sun shadow array allocation. Edit-phase worst frame 12.2 ms down to 7.7 ms (A/B x2, noise
    ~0.1 ms). Still stalling on first use: frame 3 of the Scene view (~175 ms: SSAO depth pre-pass 20 ms, cluster
    cull 40 ms, model program variants), Play press (`Animator Clip Warm-up` ~1.1 s on the first Play frame, Enemy AI
    ~110 ms, asset pump ~87 ms on frame 3 of Play; not in the renderer).
    Round 4 moved more of it behind the load: `IblProbe::WarmUp` (programs + BRDF LUT), `Tonemapper::WarmUp`,
    `LightBuffer`/`ClusterGrid::WarmUp` (buffers + cull programs) and `SceneRenderer::WarmShaderVariants` (every
    material's ShaderAsset variant). First-frame CPU Scene Draw 64 -> 22 ms, IBL bake CPU 20.6 -> ~0, GPU Cluster
    Cull 47 -> under the 3 ms report line, SSAO pre-pass 21.7 -> 4.8 ms. The worst early frame (frame 3, ~175 ms wall)
    did NOT move (A/B x2: 174/175 base, 52/175 new): it is GPU-side first use - the urgent IBL convolve (~45 ms of real
    work, needs the first sky capture, so it cannot run at load), first-touch of textures/buffers by the driver, and
    NVIDIA's one-time "vertex shader recompiled based on GL state" for each shadow program (2 warnings; only a real
    mesh draw triggers it).
12. GL warnings, fixed: the periodic "texture object (0) bound to texture image unit 0 does not have a defined base
    level" burst (14 every ~4 s; 228 per weapon-test) was the IBL convolve (and the SSAO warm-up) leaving their
    program current with its cube/2D texture unbound, so the next draw/clear validated a program sampling texture 0.
    `BakeStateScope`, `IblProbe::Bake` and `Ssao::WarmUp` now `glUseProgram(0)` on exit; the spot/point shadow passes bind
    the white default to unit 0 before their first clear. Headless.log texture-state warnings: 228 -> 0. The
    MaterialPreview smoke pass still logs 7 (units 0/11-13), not in the game path.
11. An enemy soldier's respawn is ~1.8 ms (it was 6-7: the gun's clip matching and setup measurements are now shared
    between soldiers): ~1 ms of it is building the soldier's entities from Soldier.json. Pooling soldiers (reusing a
    dead one's entities and weapon rig) would take it to ~0.

Fixed in the enemy AI work: the first round fired in a Play stalled up to a second importing the spent case's FBX;
every weapon now warms its case's mesh when it starts (`FirstPersonPresentation::WarmEjectAssets`).

Character LODs are out of scope for this pass.
