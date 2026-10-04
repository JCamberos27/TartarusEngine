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
| L2 | Projected world decals: floor splats under sprays, wall/ceiling spatter, corpse pools, drying | **done**: PR #523 |
| L3 | Splats on characters, ragdolls, guns (incl. the player's view model), moving props | **done** (PR after #523) |
| L4 | `BloodSettingsComponent`, the fluid's shading fixed (normals), perf A/B, final docs | **done** (same PR as L3) |

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
- **Winding and normals.** The sims' baked normals point *into* the fluid. The header flag records the
  corner order against those normals, so the outside winds the other way. `BloodRenderer` sets
  `glFrontFace` to match, and `BloodVat.vert` negates the normals; without the negation every droplet
  shades like its own back and reads white-pink.

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
  - albedo (0.2, 0.007, 0.005) linear
  - roughness 0.1
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

### Splats on bodies, guns and props (L3)

**What a splat is.** A splat is a decal box pinned in a mesh's **bind-pose space**, so it moves and
deforms with the mesh. The shader side:

- `ModelVertex.glsl` passes the pre-skin position and normal (`vBindPos` / `vBindNormal`; `BloodVat.vert`
  writes zeros).
- `ModelFragment.glsl` (`BloodSplatCover`) loops over the entity's splats before lighting, turning the
  albedo toward blood (fresh to dried, darker at the core), raising roughness as it dries and dropping
  metallic. Blood is therefore lit by the real material pipeline.
- Draws without splats pay one uniform branch (`uBloodSplatCount` 0).
- The splats live in an SSBO at binding 6. The atlas sits on texture **units 28 and 29**, which
  `ShaderAsset` now keeps out of the material units: its cap went from 30 to 28.
- The atlas is read with an explicit LOD from `fwidth(vBindPos)`. The reads happen inside a loop that
  skips splats per fragment, where implicit derivatives are undefined; the first version read the
  coarsest mip and painted solid boxes.

**Per piece.** Every mesh that draws a body gets its own copy of each splat, through its own copy of the
bone, as posed now: `world -> bind = inverse(world(piece) x FinalBoneMatrix(bone))`. That covers the
driver plus every clothing piece. Pieces don't have to share a bind pose. `BloodFx::SplatSpace` lists
the members.

- At most 24 splats per mesh.
- A body's splats go when the body does: `SetSplatMembers` reports whether it is alive.
- The hit point lies on the hitbox capsule, not the cloth, so splats project 30 cm deep. The facing
  test, `smoothstep(-0.2, 0.35, dot(bindNormal, splatNormal))`, keeps them on the side that faces the
  splat.

**What each hit leaves.**

- **On the victim:**
  - an entry blot (`attached`) that seeps out over 4 s
  - for the living, the exit's mess out of the back (`blood7` / `attached`) and a run of blood down from
    the wound (`char`, the asset's character drip)
- **In the spray's path:** three body-part rays through the exit cone find soldiers in the way. Up to
  2.5 m away, they get spatter (`blood9` / `blood3`), landing at dist / 4 m/s.
- **Loose props:** a host ray (`SetPropRay`) that hits only rigidbodies with a mesh. They get spatter in
  their mesh space.
- **The player:**
  - **Shot.** A wound and a run on their own body pieces near the hit, plus a 50% chance of drops on the
    gun and hands.
  - **Point blank (< 2.5 m).** Specks come back onto the gun and the hands the player sees. `SetPlayerGear`
    supplies the points: along the barrel from the muzzle, and the hands of the `ViewModelTag` arms piece.
  - **Member matching.** The player's members are matched by bone distance with tight reaches (16 cm
    for body pieces, 60 cm for the gun), so blood meant for the visible view-model arms doesn't land on
    the hidden world-body copies.

`--npc-test blood` adds a fifth case, `point_blank`, which checks that blood reached the player's gear,
and a `blood_wounded` close-up of the standing wounded soldier.

**Known gaps to revisit.**

- The atlas is 2 x 16 MB of RGBA8 plus mips; BC3 would cut it by 4x.
- The pools under corpses that lie against a wall merge into the floor splats.
- The scene-view ragdoll gizmos clutter the test shots.

### Settings and performance (L4)

**Blood Settings component** (`BloodSettingsComponent`, under Gameplay in the Add Component menu). The
first one in the scene is copied into `BloodFx::Settings` at Play start. It holds:

- Enabled
- Size
- Max Sprays, Max Stains
- Stain Lifetime, Dry Seconds
- Pools, Body Splats, Gear Spatter

A scene without one uses the same defaults.

**Performance A/B** (2026-10-04): `--perf-bench` on the Sandbox at 1920x1080, `origin/main` against this
branch, both built Release in separate directories and run back to back on the same project:

| | edit | play | play-max |
|---|---|---|---|
| main | 197.9 fps | 147.5 fps | 184.3 fps |
| blood | 209.3 fps | 146.4 fps | 187.4 fps |

GPU Scene Draw: 1.22 ms on main, 1.20 ms on this branch. So there is no regression from:

- the static/dynamic opaque split
- the extra varyings
- the splat branch in `ModelFragment`
- the per-draw splat lookup

Live blood itself costs about 0.05 ms per frame for the sprays. Decals and splats only cost where they
cover pixels. Both runs are below `docs/PERFORMANCE.md`'s 258 fps. That gap is environmental: main
itself measures 184 here, and this worktree lacks the git-ignored Quantum textures.

## v2: the Knife packs (branch `Claude/blood-v2-knife`)

The v2 pass brings the blood to AAA using two Knife Entertainment Asset Store packs the user owns:

- **Real Blood**: puddles, trails, prints, wall drips ("Leaks"), wound decals, blood particle sheets,
  screen damage, and the exploded head / brain parts.
- **PRO Effects FPS Muzzle Flashes & Impacts**: bullet holes for 13 surfaces, impact debris and smoke,
  and muzzle flashes.

These are blended with five improvements: real exit wounds, spray speed by energy, headshots that read
instantly, performance, and tooling. The approved plan has landmarks A to F.

| Landmark | What | State |
|---|---|---|
| A | `--import-knife-fx`, the texture-array library, flipbook sprite renderer; PBR decal kind | importer, library and sprites **done**; PBR decals next |
| B | Energy model, real exit wounds, pellet puffs, instant impact puff and mist, headshot mist | **done** except Knife wound decals on bodies |
| C | PBR pools, crawl/drag trails, footprints, handprints, wall drips (Leaks), screen blood | todo |
| D | Headshot gore: hide the head, exploded-head stump, brain chunks; `Gore` setting | setting only |
| E | Surface types, per-surface PRO bullet holes and impacts, muzzle flash and smoke | todo |
| F | Spray LOD and cull, stain dedupe, perf A/B, DevPanel "Blood Lab" | todo |

### Restoring the Knife data (not in git)

The packs are extracted as plain files into `C:\Users\jacob\OneDrive\Desktop\ASSETS TO IMPORT\`, one
folder each: "Knife Real Blood" and "Knife PRO Effects FPS Muzzle Flashes Impacts". Then run:

```
TartarusEngine.exe --project <project dir> --import-knife-fx "C:\Users\jacob\OneDrive\Desktop\ASSETS TO IMPORT"
```

The import takes about 5 s. It writes 131 MB into `project/assets/Effects/Knife/`:

| File | Layer size | What |
|---|---|---|
| `decals_large.kfx` | 2048 | pools and trails |
| `decals_small.kfx` | 1024 | splatter, prints, drips, leaks, 13 bullet-hole sets |
| `sprites.kfx` | 1024 | blood sheets, mist, smoke, debris, muzzle flashes |

Without these files the game logs one warning, and the effects that need them are skipped.

### How v2 works

- **Import** (`src/Assets/KnifeFxImport.*`).
  - The curated catalogue `kSources` names what to take from each pack, with its grid and smoothness.
  - Each entry becomes one array layer. Its grid cells are re-packed (flipbooks are thinned to 16 frames
    by a stride), area-filtered, mip-mapped, and compressed to BC3 colour plus BC5 normals.
  - Two kinds of colour data:
    - **Mask** entries (blood sheets, leaks, flashes) are white, with the shape in alpha.
    - **Albedo** entries (puddles, holes, debris) keep their colour, averaged by coverage.
  - The channel-packed smoke sheets are unpacked first; they hold 64 frames across r, g, b and a.
  - Empty trailing cells are trimmed, which sets the frame count.
- **GPU library** (`src/Renderer/KnifeFxLibrary.*`). Loads the `.kfx` files into texture arrays.
  `Find(name)` returns an id, and `At(id)` returns the layer, grid and frame count.
- **Sprites** (`src/Game/Combat/FxSprites.*`, `src/Renderer/FxSpriteRenderer.*`,
  `shaders/FxSprite.*.glsl`).
  - A pooled CPU sim with gravity, drag, a one-plane bounce, size, alpha, erosion over life, and the
    flipbook frame.
  - Everything draws in one sorted, premultiplied instanced draw after the particles, plus one in the
    view-model pass.
  - Modes:
    - **Blood**: Knife's erosion threshold, glossy, normal-mapped, back-lit.
    - **Lit**: smoke and dust get half-Lambert lighting; debris gets PBR.
    - **Additive**: written with zero alpha, so it adds light.
  - Soft against the resolved scene depth.
  - Units 17 (depth), 21 and 22 (arrays); SSBO binding 10.
- **Hits** (`BloodFx`).
  - **Energy.** `Energy(hit)` is about 1 for a rifle at combat range. It scales with damage, a
    shotgun's whole load, distance and headshots.
    - It drives spray size, and a horizontal stretch along the line of flight (`PrefabToWorld`'s
      `stretch`): faster, with the same baked fall.
    - Below `kExitEnergy` the round stays in. The spray is then a smaller spurt back out of the entry,
      and nothing exits the far side.
  - **Exit wounds.** `FindExit` casts a body-part ray back from 0.7 m past the entry, looking past
    anyone standing behind. The exit spray and the exit splat start where the body really ends.
  - **Puffs.** `SpawnPuffs` fires the frame the round goes in:
    - the entry burst (`blood_hit`) and mist (`blood_cloud`);
    - with an exit: the burst (`blood_burst`), the jet (`blood_jet`) and more mist;
    - without one: the fan back out (`blood_fan`);
    - velocity-stretched droplets (`blood_drop`);
    - headshots get denser mist and twice the droplets;
    - a shotgun's other pellets each puff, without another spray.
- **Settings.** `BloodSettingsComponent` gains:
  - `EnergyScale`
  - `ImpactPuffs`
  - `Gore`: 0 is off, 1 is mild (no headshot gore), 2 is full

### v2 testing

- **Unit tests**: `Blood::Energy`, `Blood::ExitWound`, `Blood::Puffs`, `FxSprites::Sim`,
  `KnifeFx::Library`, plus the BloodSettings round-trip.
- **`--npc-test blood`**: 12/12 checks pass. Its smoke status reads FAIL in this worktree only because
  of the missing git-ignored Quantum textures.
- **Tuning so far.** Flipbook cells start small inside the cell, so the sprites are sized large: the
  entry sheet is 0.45 m x sqrt(energy). The mist is a dark red haze (0.17, 0.014, 0.012) at 0.4 to 0.6
  alpha. Brighter read as pink fog in daylight.

### v2 next steps

Work in the plan's order: A (PBR decals) → C → D → E → F. Merge at landmarks.

## Next steps (v1)

1. **Tuning in the user's real scenes** (hands-on). Check spray sizes against "bloody, not over the top"
   (`BloodFx::Choose`), and how readable blood is on dark gear.
2. **The known gaps above**: the BC-compressed atlas, the pools by walls.
3. **Leftovers from the plan, if wanted.** Drips while the player is badly hurt; ceiling drips; exit
   wounds that follow the round's real path through the hitbox (the hit point is the hitbox centre for
   scripted hits).
