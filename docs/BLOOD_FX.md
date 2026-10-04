# Volumetric blood

This doc covers the blood that flies when a round goes into a body: a soldier, a corpse or the player.
It is built on the KriptoFX **"Volumetric Blood Fluids"** Unity asset (v1.0.3). That asset is 11
Houdini fluid sims baked to vertex-animation textures (VAT), plus decal textures and 17 prefab setups.

This is also the **hand-off doc**: it records status, decisions and the next step, so another session
can pick the work up.

## Status

| Landmark | What | State |
|---|---|---|
| L1 | Importer, VAT spray renderer, hit wiring, BloodTest scene, `--npc-test blood`, unit tests | **done**: PR #523 |
| L2 | Projected world decals: floor splats under sprays, wall/ceiling spatter, corpse pools, drying | **done** (PR after #523) |
| L3 | Object-space splats on characters, ragdolls, guns (incl. the player's view model), moving props | planned |
| L4 | Tuning, a `BloodSettingsComponent`, perf pass, final docs | planned |

## Restoring the data (it is not in git)

The repo is public and the asset can't be redistributed, so the converted data is git-ignored under
`project/assets/Effects/Blood/`. Rebuild it on any machine with:

```
TartarusEngine.exe --project <project dir> --import-blood-fx "<path to the VolumetricBloodFX package folder>"
```

The source package lives at `C:\Users\jacob\OneDrive\Desktop\ASSETS TO IMPORT\VolumetricBloodFX`. The
import takes about 5 s and writes 94 MB:

- 11 `*.bvat` files
- `decals/*.png`
- a README

Without the data the game logs one warning and plays with no blood.

**Package check (2026-10-03).** All 358 files are present. The gaps, none of which matter:

- `blood13.prefab` lost one mesh. Its material is blood7's, so the generator uses blood7's mesh.
- `blood5` ships no decal.
- Every `Decal.mat` points at a missing `_Flowmap` that the shader never reads.
- The "URP and HDRP Patches" folder is empty.

## How it works

### Import (`src/Assets/BloodFxImport.*`)

- **Readers.** A minimal OpenEXR reader (scanline; HALF/FLOAT; NONE/ZIPS/ZIP) built on stb_image's
  zlib, and an ASCII-FBX UV reader. The sims' meshes are ASCII FBX triangle soups.
- **Topology.** Each sim's triangle topology changes every frame, which is Houdini's fluid mode. So
  frames are stepped, never blended.
- **No vertex buffer.** Vertex `v` reads texel `(3*(t%341) + 2 - v%3, frame*rowsPerFrame + t/341)` with
  `t = v/3`. The importer verifies every UV against that layout, so the shader derives the texel from
  `gl_VertexID` and needs no vertex buffer.
- **Decoding the textures.** They were authored to go through Unity's `LinearToGammaSpace`; that was
  verified: dead vertices decode to the origin only through it. Then come the Unity shader's swizzle
  `(-x, z, y) + _HeightOffset` and a z mirror into our right-handed axes.
- **Output format.** `RGBA16UI` texels, with:
  - xyz: position quantised into *that frame's* bounds, which gives sub-millimetre precision
  - w: octahedral normal, 8+8 bits
- **Winding.** The fluid's outside winds clockwise in our space. `BloodRenderer` sets `glFrontFace`
  from the header flag (verified visually in L1).

### Presets (`src/Game/Combat/BloodFxPresets.*`)

`tools/gen_blood_presets.py <package>` regenerates `BloodFxPresets.inc` from the 17 prefabs. For each
prefab it records:

- the sprays: sim, transform into the prefab, `TimeLimit`, `FramesCount`
- the floor decals: the `BFX_DecalSettings` height response, and the `BFX_ShaderProperies` reveal curve

These are Unity AnimationCurves, evaluated as Hermite segments (`BloodCurve`).

### Game side (`src/Game/Combat/BloodFx.*`)

**Where hits come from.** `NpcDirector` queues a `FleshHit` for every round into a body:

- player → soldier, living or corpse (`OnPlayerHit`)
- soldier → player (`HandleShots`)
- soldier → soldier, friendly fire (`HandleShots`)

`main.cpp` drains them into `BloodFx::OnFleshHit`.

**Placing the spray.** `BloodFx::Choose` picks a prefab and a size from the hit:

| Hit | Prefab |
|---|---|
| wound | blood3/4/5/6 |
| kill | blood1/9/2_left/2_right |
| head kill | blood7/8 |
| corpse | blood4/5 |
| player | blood3 |
| straight down | blood2 |

Damage and shotgun pellets scale it. The spray leaves from the exit point (hit + 13 cm along the round;
7 cm for a head), yawed onto the round's flattened line, since gravity is baked toward -Y.

**Physics.**

- Playback time is the authored `TimeLimit / AnimationSpeed x sqrt(size)`. A smaller splash falls a
  shorter way, and fall time goes with the square root of the distance.
- A `RaycastSolid` along the spray finds the first wall. Its plane becomes a `gl_ClipDistance`, so the
  fluid stops at the wall instead of passing through it (floors already hide it by depth).

**Caps.**

- Shotgun pellets on one body in one moment make one spray (an 80 ms window).
- At most 24 live sprays; the oldest is dropped.

### Rendering (`src/Renderer/BloodRenderer.*`, `shaders/BloodVat.vert.glsl`)

- **Loading.** The sims load once, at the first Play: 11 textures plus one SSBO of per-frame bounds.
- **Drawing.** Each frame `BloodFx::Submit` queues the live sprays. `SceneRenderer` draws them right
  after the opaque pass: one `glDrawArraysInstancedBaseInstance` per sim, frustum-culled per spray.
- **Shading.** The fragment stage is the engine's own `ModelFragment.glsl`, built with `_SUBSURFACE`, so
  the fluid gets every light the scene has: sun with CSM shadows, clustered point/spot lights, IBL, fog
  and aerial perspective. Its parameters:
  - albedo (0.32, 0.012, 0.009) linear
  - roughness 0.07
  - back-lit red transmission
  - the instance tint arrives as the vertex colour
- **GPU cost.** The `--npc-test blood` measurement is 0.05 ms/frame average for one to two live sprays.
  The 0.8 ms worst frame is the first draw, while the program warms up.

### World decals (L2)

**Game side** (`BloodFx`). Every stain is a box that projects along its +Y. There are four kinds:

- **Floor splat.** Each spray's own prefab decal, ported from `BFX_DecalSettings`:
  - It looks for the ground a step along the spray.
  - The fall height to that ground picks the decal's stretch and slide (`TimeScale*`, `TimeOffset*`) and
    when it lands (`TimeByHeight` / AnimationSpeed x sqrt(size)).
  - It lies on the ground and projects through 40 cm, so it covers steps and kerbs but not walls.
- **Wall spatter.** Wherever a spray's clip ray met a wall, two decals go down: a blot (`blood7` or
  `attached`) and a streak set (`blood1/3/9/2_right`). The streaks run out and down, and both land at
  dist / 4 m/s.
- **Ceiling.** A head shot with a ceiling within 2.2 m leaves a `blood7` spatter on it.
- **Pool.** A body killed here gets one 2 to 3 s later, once the ragdoll has settled:
  - Placed under the pelvis, found through `SetBodyLookup`, which `main.cpp` resolves through
    `NpcDirector::Npcs()`.
  - Uses `attached`, spreading over 20 to 30 s.

**Life of a stain.** The reveal follows `BFX_ShaderProperies`: the curve up to 0.3, a hold, then the
curve's tail over the last 0.7 x RevealSeconds, as the stain shrinks away. Pools spread instead. Stains
dry over about 90 s (pools 2.5x slower): darker, browner and matte. A stain lives 300 s, and the cap is
320 stains.

**Rendering** (`BloodRenderer::DrawDecals`, `shaders/BloodDecal.*`):

- **Atlas.** At load, the decal PNGs (normal/alpha + mask) are resampled to 512 px max and shelf-packed
  into two 2048x2048 RGBA8 atlases with mips. The 128x1 `lookup.png` fades the decal across the box's
  depth.
- **Pass order.** The opaque pass now draws **static first, then dynamic** (skinned or rigidbody;
  `DrawItem::Dynamic`). The decals draw in between:
  - The MSAA depth is resolved depth-only (`HdrTarget::ResolveDepthOnly`).
  - One instanced draw of the boxes' far faces, with no depth test.
  - Then characters, ragdolls and props draw over the stains instead of being painted.
- **Fragment.** It reconstructs world position, and a surface normal from the nearer of the neighbouring
  depths. It rejects pixels outside the box and surfaces not facing along the box (smoothstep 0.35 to
  0.65), then reads the atlas with Unity's uv, `(x, -z)`, flipped to top-first rows.
- **The blood layer.** It is lit exactly like a mesh through `ModelShading.glsl`:
  - The shared include holds `ModelFragment`'s uniforms and functions, moved verbatim.
  - Lights: sun with CSM and cloud shadow, clustered point/spot lights, IBL diffuse and specular.
  - Albedo: fresh (0.26, 0.010, 0.008), dried (0.075, 0.022, 0.016); the pooled core is 0.6x darker.
  - Roughness runs 0.06 to 0.55 as it dries.
- **Covering.** The layer covers the surface by thickness: 60% at the thin edges up to 97% at the core,
  and a thin layer lets the surface through tinted by sqrt(albedo).
- **Blending.** Dual-source: `dst = Add + dst * Mul`. Fog and aerial perspective fade the layer the same
  way they fade the surface.
- **Spray axis.** The blood7/8 prefabs spray along their own -X while the rest use +X.
  `BloodFx::PrefabAxis` works out each prefab's direction from where its sims' fluid ends up, and the
  spray is yawed so that axis follows the round.

## Testing

- `--unit-tests`: the `Blood::*` tests cover:
  - half floats and octahedral normals
  - EXR ZIPS decoding
  - building a VAT from a synthetic soup
  - FBX UVs
  - the presets and their curves
  - spray timing and transforms
  - flesh hits: clip plane, shotgun merge, expiry, cap, disabled
  - decals: the floor, wall and ceiling placements; no mirrored boxes; pool timing and placement; the
    reveal, hold and fade; lifetime; cap
- `--npc-test blood --smoke-shots <dir>` runs in `scenes/BloodTest.json`, which
  `tools/gen_blood_scene.py` regenerates. The scenario plays four cases: a wound, a chest kill in front
  of a wall, a head kill, and a corpse shot. It checks a spray was thrown each time and that the chest
  kill's spray is clipped by the wall, logs the blood's GPU time, and saves Scene + Game view PNGs at
  0.1, 0.3, 0.6, 1.0 and 1.8 s after each shot. 15 s after the last case it takes a `blood_overview`
  shot and checks that at least 6 stains and 2 pools were left (the last run had 14 and 2).
- In this worktree the Quantum clothing textures are missing (git-ignored), so the smoke summary reports
  log errors for them. That is not a blood failure; the `[NpcTest]` checks are what count.

## Decisions

- **Sizes.** The sizes are fractions of the prefabs' authored splash, which is a cinematic 2 to 6 m.
  The aim is bloody, not a fire hose. The values are in `BloodFx::Choose`.
- **Spray direction.** No spray goes back toward the shooter. The asset's demo sprays along the hit
  normal; we spray out of the exit wound, along the round.
- **VRAM.** The sims cost 94 MB. 16-bit positions cost 8 B/texel; a 10-bit-per-axis variant would halve
  that, at about 2 mm of jitter. Kept at 16-bit for quality.

## Next step (L3)

Blood on things that move: characters, ragdolls, guns, physics props.

- **Splat storage.** Splats are stored in each entity's bind-pose / mesh space, so they deform with
  skinning: up to 24 per entity, in an SSBO at binding 6.
- **Shading.** `ModelVertex.glsl` passes the pre-skin position and normal. `ModelFragment.glsl` loops
  over the splats before lighting and blends the albedo, roughness and normal toward blood. It uses the
  decal atlas.
- **Placing a splat.** Inverse-skin the hit point with the hit part's bone (`Model::FinalBoneMatrix`,
  the entity's world transform).
- **Which hits make splats.**
  - The entry wound on the victim.
  - Spatter on nearby soldiers caught in the exit cone.
  - Point-blank back-spatter onto the player's gun and arms.
  - The player's own wounds.

**Known gaps to revisit.**

- The atlas is 2 x 16 MB of RGBA8 plus mips; BC3 would cut it by 4x.
- The pools under corpses that lie against a wall merge into the floor splats.
- The scene-view ragdoll gizmos clutter the test shots.
