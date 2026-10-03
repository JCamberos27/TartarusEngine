# Volumetric blood

This doc covers the blood that flies when a round goes into a body: a soldier, a corpse or the player.
It is built on the KriptoFX **"Volumetric Blood Fluids"** Unity asset (v1.0.3). That asset is 11
Houdini fluid sims baked to vertex-animation textures (VAT), plus decal textures and 17 prefab setups.

This is also the **hand-off doc**: it records status, decisions and the next step, so another session
can pick the work up.

## Status

| Landmark | What | State |
|---|---|---|
| L1 | Importer, VAT spray renderer, hit wiring, BloodTest scene, `--npc-test blood`, unit tests | **done** (PR: see git log, "Volumetric blood L1") |
| L2 | Projected world decals: floor splats under sprays, wall/ceiling spatter, corpse pools, drying | next |
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

## Testing

- `--unit-tests`: the `Blood::*` tests cover:
  - half floats and octahedral normals
  - EXR ZIPS decoding
  - building a VAT from a synthetic soup
  - FBX UVs
  - the presets and their curves
  - spray timing and transforms
  - flesh hits: clip plane, shotgun merge, expiry, cap, disabled
- `--npc-test blood --smoke-shots <dir>` runs in `scenes/BloodTest.json`, which
  `tools/gen_blood_scene.py` regenerates. The scenario plays four cases: a wound, a chest kill in front
  of a wall, a head kill, and a corpse shot. It checks a spray was thrown each time and that the chest
  kill's spray is clipped by the wall, logs the blood's GPU time, and saves Scene + Game view PNGs at
  0.1, 0.3, 0.6, 1.0 and 1.8 s after each shot.
- In this worktree the Quantum clothing textures are missing (git-ignored), so the smoke summary reports
  log errors for them. That is not a blood failure; the `[NpcTest]` checks are what count.

## Decisions

- **Sizes.** The sizes are fractions of the prefabs' authored splash, which is a cinematic 2 to 6 m.
  The aim is bloody, not a fire hose. The values are in `BloodFx::Choose`.
- **Spray direction.** No spray goes back toward the shooter. The asset's demo sprays along the hit
  normal; we spray out of the exit wound, along the round.
- **VRAM.** The sims cost 94 MB. 16-bit positions cost 8 B/texel; a 10-bit-per-axis variant would halve
  that, at about 2 mm of jitter. Kept at 16-bit for quality.

## Next step (L2)

The world decals:

- A texture atlas built from `assets/Effects/Blood/decals/*` (normal/alpha + mask, plus `lookup.png`).
- **Rendering.** A box-projected decal pass with one instanced draw, which reconstructs position from
  depth (`HdrTarget::ResolvedDepthTexture`, resolved early). It needs the opaque pass split
  static-then-dynamic, so characters and props draw over the decals instead of being painted.
- **Look.** A multiplicative blood film plus a GGX wet coat. It reveals through the mask cutout and
  dries over about 90 s.
- **Placement.**
  - Spawned by the presets' `BloodDecalDef`: ground ray, delay by fall height, scale and offset.
  - Wall spatter where a spray's clip plane was hit.
  - A growing pool under each corpse.
