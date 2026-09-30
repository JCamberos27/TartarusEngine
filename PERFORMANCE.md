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

## Results so far (Phase 1, 4K window)

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

## To do

Ranked by expected payoff; the `--perf-bench` breakdown decides the order.

**GPU (4K Game view ≈ 4.4 ms)**
1. Draw the sky after opaque geometry (z = far, `GL_LEQUAL`). Today the full atmosphere/cloud/star
   shader runs under every pixel first (`SceneRenderer.cpp` `RenderScene`). Sky Atmosphere GPU is
   1.3 ms in play and 2.2 ms in edit.
2. Volumetric clouds: raymarch 1/4 of pixels per frame with reprojection (history already exists).
   Only redraw cloud shadows when the sun or wind moves a texel. Don't build sky-view LUTs twice for
   the main view.
3. Spread the environment capture and IBL rebake that fire every 4 s while clouds drift across frames.
   That's the periodic ~7 ms spike.
4. Depth prepass into the MSAA HDR target, reused by SSAO (drop SSAO's own geometry pass, 0.65 ms).
   Move the near-hide / hidden-vertex / alpha-clip `discard`s into shader variants so ordinary opaque
   draws keep early-Z.
5. SSAO at half resolution with depth-aware upsample.
6. Spot shadows still redraw every frame where an NPC stands in the spot (0.66 ms GPU). Keep a static
   layer and draw only dynamic casters over a copy of it. Drop the `gl_FragDepth` write in
   `ShadowDepthLocal.frag`.
7. Sun CSM: update far cascades every 2nd/4th frame; skip sub-texel casters in far cascades.

**CPU**
8. Animator followers: sample the driver once and copy to outfit pieces through a node remap.
9. Remaining FP body cost (~2.4 ms): elbow swivel search (0.5–0.6 ms, vectorize or coarse-to-fine),
   twins' arm solve, foot IK per piece.
10. Per-draw CPU: cached per-program uniform locations for `perDraw`, array uploads for
    `uBoneMask`/`uHideBones`, no string building in `BindMaterialDataDriven`, `ShaderStateScope`
    `glGet*` reads replaced with cached state.
11. Physics step (0.7–1.0 ms): sleeping bodies still get pose write-back; `SyncEntities` allocates.

**Hitches**
12. `Animator Controllers` max ~20 ms: a first-time clip load on entering a new state in Play. Pre-warm
    every clip a controller references at Start.
13. Sun shadow pass 126 ms spike on the first edit frames (one-off; investigate).

**Scene / assets**
14. Character LODs (meshoptimizer) for NPCs (109k-tri bodies, 81k-tri heads) and a lower LOD for
    shadow passes; the player's own view keeps LOD 0.
15. Share one mesh per primitive kind and instance identical mesh+material runs (~560 primitives are
    each their own Model today).
16. Review whether all 4 arena spots need shadows, and set small props to not cast into spot lights.
