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
| A | `--import-knife-fx`, the texture-array library, flipbook sprite renderer; PBR decal kind | **done** (first v2 PR) |
| B | Energy model, real exit wounds, pellet puffs, instant impact puff and mist, headshot mist | **done** (first v2 PR); Knife wound decals on bodies moved to C |
| C | PBR pools, crawl/drag trails, footprints, handprints, wall drips (Leaks), screen blood, body wound decals | pools, drips, **footprints**, **screen blood** done; trails, handprints, wound decals todo |
| D | Headshot gore: hide the head, exploded-head stump, brain chunks; `Gore` setting; gore audio | **done** (hood-collar gap below) |
| E | Surface types, per-surface PRO bullet holes and impacts, muzzle flash and smoke | **done** (water splash todo) |
| F | Distance / screen-size cull, stain dedupe, perf A/B, DevPanel "Blood Lab" | **done** (spray triangle LOD not done) |

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
- **Knife decals** (`BloodRenderer::Decal::Knife`, `BloodDecal.frag.glsl`).
  - A decal can name a Knife library entry and a cell instead of a KriptoFX set.
  - The arrays are bound on units 21-24: the large library on 21/22, the small one on 23/24.
  - **Mask** entries take the film's fresh or dried colour. **Albedo** entries are decoded from sRGB.
  - Entries with smoothness over 0.85 are blood and dry darker. The rest, the bullet holes, keep their
    own material and fully cover what's under them.
  - Pools spread out from the middle of the cell, because the cutout eats the outer coverage first.
  - A flipbook decal (the Leaks drips) steps through its cells from landing, then holds the last one.
  - Users:
    - **Corpse pools.** `pool_smooth` / `pool_big`, falling back to KriptoFX `attached` when the library
      is missing.
    - **Wall drips.** `SpawnWallDrips` puts 1 to 3 Leaks under each wall spatter, running down over
      2 to 5 s.
- **Footprints** (`BloodFx::OnFootstep`). `FoleyAudio::SetStepListener` reports every footfall, heard or not:
  - the player's comes from the body's foot contacts, or from the distance stepper;
  - the soldiers' comes from `NpcWalk` / `NpcFeet`.

  A step into a fresh floor stain (under 60 s old, inside its middle) wets that walker's soles. The next 6
  steps leave `footprint` decals:
  - offset left or right by 11 cm, toe forward;
  - each fainter than the last;
  - one sole pattern per walker.
- **Screen blood** and **headshot gore** (the stump, brain chunks, `GoreHideTag`, `tools/knife_gore_bake.py`) were
  built here and **removed in v3** (2026-10-04, the user's call): blood only. The gore sound stays on big headshot kills.
- **Gore and flesh audio.** From AE Master (the Unity project at `OneDrive\Desktop\AE Master`):
  - ColdBore's `FleshImpact_01-06` join `snd.impact.flesh`, making 13 variants.
  - The Free Pack's "Bloody punch" becomes `snd.impact.gore` (3 variants, pitch within 1.5 semitones).
  - `tools/audio/extract.py` copies the source takes to `C:\tb\audio-src\ae`, and `adsp` resolves `ae/...`
    sources.
  - Rebuild with `build_impacts.py`, then check with `check_audio.py` (PASS).
  - ColdBore also holds real impacts for 15 surfaces (brick, drywall, steel, water...), unused so far.
- **World impacts** (`Combat/ImpactFx`).
  - **Surface.** Read from the collider material, tag and name, using `ImpactFx::kSurfaceTable`. Words are
    plain substrings: "pane" was dropped because it matched "Panel".
  - **Holes on static entities** draw as PRO Effects' surface decal (4 variants) in the blood decal pass.
    `BulletHoleList` carries the decal id; moving props keep the procedural hole.
  - **Burst:** chips (concrete, rock, wood, glass). The PRO dust puff and streak were dropped (2026-10-04): they
    read badly.
  - **Metal** adds sparks and a flash.
- **Muzzle flash** (`CombatFx::MuzzleSpritesFor`, weapon prefab's `Weapon Definition` tabs).
  - Style 1, the default, layers PRO Effects' Shoot FX over the Tactical Shooter flame:
    - the star (rifle) or burst (shotgun);
    - side jets;
    - a glow;
    - the gas puff;
    - a thin afterfire wisp.
  - The player's own copy follows the muzzle (`FxSprites::Follow`) in the view-model pass, at 0.5x the size
    and 0.4x the sprite emission. `Player Flash Scale` independently controls the point light.
  - Style 0 uses the basic flame and smoke, without the extra PRO Effects layers.
  - Per-weapon light, flame texture/tint/ranges, flash entry, jets, glow and smoke controls are
    saved on `Weapon Definition`. Player and NPC shots use the same prefab. Older scene FX/HUD
    muzzle values are retained as a fallback for presentations using only an Animation Set.
- **Performance.**
  - Culling (`BloodRenderer::WorthDrawing`): sprays past 80 m, decals past 120 m, sprites past 150 m, and
    anything under about 2 px.
  - Stain dedupe (`BloodFx::DuplicateOf`): a fresh stain with the same image, size and facing merges.
  - **A/B** (2026-10-04), Sandbox at 1080p, `origin/main` vs this branch:

    | | play | play-max | GPU Scene Draw (play) |
    |---|---|---|---|
    | main | 147.9 fps | 189.4 fps | 2.61 ms |
    | v2 | 146.9 fps | 187.6 fps | 2.67 ms |

    That is within noise.
- **Blood Lab** (F7 dev panel → "Blood Lab", `Combat/BloodLab`).
  - Fire at the crosshair: rifle, 9-pellet shotgun, headshot kill, graze, surface impact, pool, wall
    spatter with drips, hurt me.
  - Live Blood Settings.
  - Live counts and GPU ms.
  - Clear all.
- **Settings.** `BloodSettingsComponent` gains:
  - `EnergyScale`
  - `ImpactPuffs`
  - `Gore`: 0 is off, 1 is mild (no headshot gore), 2 is full

### v2 testing

- **`--npc-test blood`**: 25/25 checks, adding the exit, puff and headshot-gore checks and a surface step.
  - The surface step shoots seven tagged panels on the BloodTest scene's west wall (metal, wood, brick, glass,
    mud, tile, concrete).
  - It saves `surface_*_burst` and `surface_*_hole` screenshots.
- **`--weapon-test`**: 81/81. It now saves the muzzle flash's first frames (`muzzle_*`).
- **More unit tests**: `Blood::KnifeDecals`, `Blood::Footprints`, `Blood::ScreenBlood`, `Blood::HeadGore`,
  `ImpactFx::Surfaces`, `Blood::Perf`, `Blood::LabActions`.

- **Unit tests**: `Blood::Energy`, `Blood::ExitWound`, `Blood::Puffs`, `FxSprites::Sim`,
  `KnifeFx::Library`, plus the BloodSettings round-trip.
- **`--npc-test blood`**: 12/12 checks pass. Its smoke status reads FAIL in this worktree only because
  of the missing git-ignored Quantum textures.
- **Tuning so far.** Flipbook cells start small inside the cell, so the sprites are sized large: the
  entry sheet is 0.45 m x sqrt(energy). The mist is a dark red haze (0.17, 0.014, 0.012) at 0.4 to 0.6
  alpha. Brighter read as pink fog in daylight.

## v3: one material, instant, persistent (branch `claude/blood-v3-polish`)

The polish pass after v2 (2026-10-04). Three packs' blood read as three materials, everything was mirror-glossy, the
sprays played at the packs' cinematic pace, and stains dried and vanished in front of you.

- **One palette** (`src/Renderer/BloodPalette.h`). Every piece takes its colour and roughness from it: the sprays
  (`BloodRenderer::FluidAlbedo`), the stains (`FilmFresh` / `FilmDried`, `uRough`), the splats on bodies
  (`ModelFragment.glsl` `uBloodFresh` / `uBloodDried`), and the flipbook bursts (`BloodFx::SpawnPuffs`, and the blood
  sprites' roughness in `FxSpriteRenderer`).

  | | colour (linear) | roughness |
  |---|---|---|
  | fresh film / pool | 0.16, 0.006, 0.005 | film 0.38, pool 0.32 |
  | dried | 0.045, 0.014, 0.010 | 0.75 |
  | thin sheet (bursts) | 0.20, 0.0075, 0.006 | 0.30 |
  | mist | 0.09, 0.0034, 0.0028 | |
  | spray fluid | fresh | 0.28 |
  | on cloth / skin | fresh | 0.35 |

  - **Knife albedo stains** (pools, prints) keep only their light/dark detail; the hue is the palette's
    (`BloodDecal.frag.glsl`). Pools are darker (deep), and solid through their body.
  - **Specular occlusion** at grazing views on blood decals, so a pool doesn't go milky pink with the sky across a room.
  - `pool_big` was dropped (baked highlights and holes); every pool is `pool_smooth`.
- **Speed** (`Blood Settings > Speed`, default 1.6, Blood Lab slider).
  - Sprays play 1.6x faster and start 6% in, so the blood is out of the wound on the hit's frame.
  - Floor splats land and spread by the same factor.
  - Wall, ceiling and body spatter land at about 12 m/s (was 4).
  - Body wounds show at once: entry 0.15 s, exit 0.2 s, the run below over 1.5 s.
  - Pools start 0.8-1.3 s after the death and spread over 10-14 s.
  - Puffs and mist are about 25% shorter.
- **Stains stay** (`BloodFx::SetViewer`, `InView`, `MakeRoom`).
  - Drying takes 15 min (`Dry Seconds` 900; pools 2.5x), linear: nothing visibly changes while you watch.
  - `Stain Lifetime` 0 = for good; no stain ever shrinks away.
  - A stain only leaves when it's out of view (outside a 75 degree cone from the camera, or past 60 m): over the cap
    (`Max Stains` 512) the farthest unseen one goes, else the oldest; footprints have their own cap (48).
  - Drawing cost stays bounded by `BloodRenderer::WorthDrawing` (distance and screen size).
- **Pools grow** from a small patch (8%) to full size over 20-28 s, fast then slow (`BloodFx::PoolScale`, `Decal::Spread`).
- **Thrown blood lands anywhere** (`SpawnGroundSplatter`, `ThrowArc`): every hit throws the bulk, drops and fine specks
  out of the wound on falling arcs; each splashes on the first surface it meets (floor, wall, crate), when it gets
  there, stretched the way it was going (longer when it skids in at a grazing angle); on a wall it runs. A few drops
  spill back toward the shooter. Head > body; a head kill the most.
- **Corpses** (`SpawnCorpseSplash`): a shot into a body lying down splashes round the wound onto what it lies on, and the
  new wound pools (a small spreading pool).
- **Wounded soldiers bleed** for 8 s after a hit: drops under them as they move (`m_Bleeds`).
- **Body stains:** smaller on the torso, wider on the head.
- **Variety** (`AddSplash`): Real Blood's `splat_small`, `splat_wide`, `splat_medium` (4 each) and the 16-cell `drops`
  sheet join the KriptoFX stains (round drops only on floors); every splash may be mirrored. All in the palette.
- **Footprints:** pool-coloured (an explicit `Blood` flag replaces the gloss test), point the way the player faces,
  14 prints a trail (128 kept).
- **v3.1 (play-test feedback):**
  - **Bullet holes by calibre:** AKS-74U 5.45 mm (`bulletHoleRadius` 0.0027), Remington 00 buck 8.4 mm (0.0042). The
    PRO decal is sized so its hole is `ImpactFx::kHoleScale` (2x) the calibre (`HoleFraction`, measured per surface off
    the albedos), and its chipped rim fades out at 3.5x the hole radius (`HoleRim`, `BloodRenderer::Decal::Rim`).
  - **No pixelated splatter:** thrown splashes are capped (head kill ~1.5 m, stretch <= 1.6x); Real Blood's splatters and
    drops live in the 2048 library (1024 px a splatter); KriptoFX's stains (512 px) only for small marks.
  - **One pool per body**, from the torso's middle (pelvis toward chest), once the body has stopped moving (two looks
    0.2 s apart within 3 cm, or 4 s); corpse shots splash but no longer pool.
  - **Faster, bloodier:** Speed 2.0; landed splashes pop in (0.04-0.08 s); throws 25% faster; ~1.3x drops, 1.5x specks.
  - **The player's gear:** their own wound never lands on the gun (`kBodyOnlyPart`); gun blood is small speckles, not
    stacked, at most 14, and never evicted or dried. Gun splats are keyed by the weapon model
    (`SplatSpace::Keys`, `SetMemberResolver`, `DrawnOn`), so a weapon swap brings them back.
  - **Drying only out of view** (`DryAge`), 30 min (pools 2.5x); body splats dry only while their body is out of view.
  - **Stain budget:** 1024; the farthest small unseen mark goes first; never a print or pool while anything else can.
  - **Footprints:** their own budget (200), kept 10 min, then a 3 min fade; over budget the oldest fades over 20 s.
- **Removed:** screen blood and headshot gore (see v2 above). `Gore` is now 0 off / 1 on.
- **Lighter import:** the Knife catalogue only holds what's drawn (dropped trails, handprints, the skin hole,
  `blood_side`, `blood_spurt`, `blood_blob`, `pool_big`).
- **Tests:**
  - New unit tests: `Blood::Persist`, `Blood::Palette`, `Blood::Speed`, `Blood::GroundSplatter`, `Blood::CorpseSplash`,
    `Blood::SplashVariety`.
  - `--npc-test blood`: 25/25, the head kill now checks the big-headshot sound.
  - Its screenshots were checked for the pools' colour at a grazing view.

### Next steps

1. Hands-on: Speed, mist size, pool darkness in the user's scenes.
2. Crawl and drag trails, handprints, wound decals (re-add their catalogue entries).
3. Water impacts; spray LOD by triangle count.

### Restoring the Knife data in the Work clone

```
TartarusEngine.exe --project project --import-knife-fx "C:\Users\jacob\OneDrive\Desktop\ASSETS TO IMPORT"
```

The gore and flesh sounds are committed, so they need no step. `assets/Effects/Knife/Gore` (v2's baked gore) can be
deleted.

## Next steps (v1)

1. **Tuning in the user's real scenes** (hands-on). Check spray sizes against "bloody, not over the top"
   (`BloodFx::Choose`), and how readable blood is on dark gear.
2. **The known gaps above**: the BC-compressed atlas, the pools by walls.
3. **Leftovers from the plan, if wanted.** Drips while the player is badly hurt; ceiling drips; exit
   wounds that follow the round's real path through the hitbox (the hit point is the hitbox centre for
   scripted hits).
