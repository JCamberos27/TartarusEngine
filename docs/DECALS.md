# Decals

Clustered PBR decals: graffiti, grime, stains, signage and paint projected onto whatever surfaces
a box touches. A `DecalComponent` points at a `.decal` asset; the renderer bins visible decals
into the light clusters and applies them in the model fragment shader before lighting.

## Renderer

### Per frame

`DecalSystem` (`src/Renderer/DecalSystem.*`), owned by `main.cpp` and handed to `SceneRenderer`
through `SceneRenderInputs::decals`. For each view that runs the cluster cull (the Scene and Game
views; previews have no froxels and get no decals), `RenderScene`:

1. calls `Prepare`: walks the active `DecalComponent`s (plus `PushRuntime()` decals), builds each
   box as `world transform x seeded variation x Size`, frustum-culls it, drops it past Project
   Settings > Quality `decalMaxDistance` (60 m; it fades out over the last fifth), and skips it
   until its textures are resident. Past `Decals::MAX_VISIBLE` (4096) the nearest stay. The set is
   sorted on (SortOrder, entity, seed) with a stable sort, so the order is the same every frame,
   and uploaded: 128-byte `GpuDecal`s to SSBO 11, world-space boxes to SSBO 12;
2. dispatches `ClusterDecalCull.comp.glsl` from `ClusterGrid::Cull`, right after the light cull and
   over the same froxel AABBs. Per cluster it rejects on the box's view-space AABB, then on the
   box's own three axes (a conservative 6-axis SAT), counts, reserves one block of the shared index
   list (SSBO 14, 131072 entries behind a {cursor, overflow} header) with one `atomicAdd`, and
   writes. Past 256 hits a cluster keeps the top-most (the end of the sort order). Ranges go to
   SSBO 13. `ClusterGrid::DecalOverflow()` reports the cap (bit 1) or a full list (bit 2), a cull or
   two late. The view-model sub-pass re-runs the cull for its own projection, decals included;
3. binds SSBO 11 and the arrays on units 22-25 in `ApplyFrameState`, and again right after the
   blood decals draw (they and the FX sprites put their own arrays on 22-24).

`uDecalCount` is the view's count; 0 skips the whole decal block in one uniform branch.

### Shading

`ModelDecals.glsl`, included from `ModelFragment.glsl`. Order: material maps -> Grunge -> decals ->
blood -> lighting. The fragment walks its froxel's list bottom to top:

- **Receivers.** The decal's layer mask against the object's `uObjectLayerBit` (lighting layer),
  and Affect Static / Dynamic against `uDecalStatic` (static = `StaticTag`, or nothing skinned and
  no rigidbody). A material can opt out with **Receive Decals** (Standard shaders).
- **Projection.** World -> box through the 3x4 `WorldToBox`; outside the unit box is skipped. The
  uv gradients come from `dFdx/dFdy(vWorldPos)` taken before the loop (implicit derivatives are
  undefined in a loop that skips per fragment) and are chain-ruled through the box, so
  `textureGrad` picks the right mip with no seams. Past the slot's `maxLod` the gradients shrink to
  stay at it, and the uv is clamped half a texel (at that mip) inside the slot rect, so neighbours
  in the page never bleed in.
- **Fades.** Angle (receiver normal against box +Z, `angleFade` cos start / end) and edge (the last
  `edgeFade` of the box on every axis, depth included).
- **Opacity.** Albedo alpha, or luminance with `lumaMask` (the colour is then the tint alone),
  inverted with `invertMask`; then weathering: fade, peel (the aux map when the emissive channel is
  off, else noise), buff (scrubbed patches) and wear (chips), all in box space so they stick; then
  height blend, which thickens the decal in the receiver's cavities (low AO) and thins it on ridges.
- **Channels.** Albedo (tinted, sun-bleached by `bleach`), normal (whiteout blend in the box's
  frame: x along u, y up the image, z out of the surface; paint relief scales it), roughness and
  metal (the decal's values, or paint's, times the surface map), AO (multiplied), emissive (masked
  by aux, UV paint only under `uDecalBlacklight`, 0 until a UV light exists).
- **Grading.** The scene's Decal Grading folds in on the CPU: Age adds to fade and bleach, Wear to
  peel, buff and chipping.

`GpuDecal` (`DecalTypes.h`) is mirrored in `ModelDecals.glsl` and `GpuDecalBounds` in
`ClusterDecalCull.comp.glsl`; change all three together.

### Textures

`DecalTextureArrays` (`src/Renderer/DecalTextureArrays.*`): four `GL_TEXTURE_2D_ARRAY`s of 4096^2
pages, 11 mips (down to one BC block).

| Unit | Contents | Format |
|---|---|---|
| 22 | Albedo: rgb colour, a opacity | sRGB BC3 |
| 23 | Normal: tangent xy | BC5 |
| 24 | Surface: r rough, g metal, b AO, a height | BC3 |
| 25 | Aux: emissive or peel mask | BC4 |
| 21 | the material's Grunge map (not an array) | material's own |

- **Quadtree.** `DecalPageAllocator` splits pages: a 4096 slot fills a layer, 2048 a quadrant, then
  1024 and 512. A decal's maps share one rect in all four arrays. An exact free block is taken
  first, then the smallest larger one is split, then a new layer opens; four free siblings merge.
  Atlas frames are folded into `GpuDecal::Rect`, with `maxLod` stopping where a frame is still 4
  texels across.
- **Decode.** Each map goes through `Texture::DecodeFile` on a worker (`std::async`, three at
  once) with `TextureImportSettings::ForceFormat`, which resamples to the slot size and encodes
  exactly the array's BCn format, BC3 keeping its alpha block. The result is cached in
  `Library/Textures` like every other texture, so a decal decodes once per machine. Uploads happen
  on the main thread at the next `Prepare`; a decal draws from the frame after.
- **Budget and eviction.** Project Settings > Quality `decalTextureBudgetMB` (256). The normal,
  surface and aux arrays only exist once a resident decal has that map, so an albedo-only library
  gets about 12 layers and a fully mapped one 3. Every `Request` in a frame pins its slot (the
  refcount); when nothing fits, the least recently used unpinned slot is evicted. Growing a layer
  copies the resident pages on the GPU (`glCopyImageSubData`). An edited `.decal` is re-read within
  about a second and the arrays are refilled.
- **Units.** `ShaderAsset::BuildBindings` hands material maps 1-7 and 16-20 and pins `_GrungeMap`
  to 21, so 22-25 stay the decals'. The Standard shaders use exactly those twelve.

### Grunge

`_GrungeMap`, `_GrungeStrength` (0 = off) and `_GrungeScale` (tiles per metre) in `Standard.shader`
and `StandardAdvanced.shader` (`Material::GrungeMap` etc., saved in the `.mat`). Sampled triplanar
in world space with `SampleTriplanar`, so it ignores the mesh UVs: the map multiplies the albedo,
and where it's dark it roughens the surface and darkens its AO.

### Substrate

What shows where paint is missing (`Substrate.h`): albedo, normal, roughness and AO maps, metres
per repeat, tint, strength, a roughness range (the map, or white without one, remapped to it), a
mirrored-repeat switch, a crop rect (the part of the image that repeats) and `detail` (0..2: pits
and pores derived at load from the albedo's fine luminance - its high-pass as a height - whiteout-added
to the normal map and multiplied into the AO, for sets whose own normal / AO are flat).

Presets: `.substrate` files (that same JSON) under `assets/Decals/Substrates` - `SB_Wall_Concrete`
(1.5 m, crop `[0, 0.21, 1, 0.93]` to skip the sheet's trim bands, mirrored, tint 1.6, detail 1; its
roughness PNG is solid black, so roughness 0.9 from the range), `SB_Floor_Concrete` (2 m, tint 1.35,
detail 0.6) and `SB_Dark_Wooden_Planks`. A block names one as `{"preset": "<path>"}` (or the path as a
string); keys beside it override the preset's values, and it is written back as the reference plus
only the values that differ. Inline blocks (no preset) still load. Three places carry one:

- a `.decal`'s `"substrate"` block. Every `DA_PeelingPaint_*` references `SB_Wall_Concrete`;
- a material's `"substrate"` block (top level in a `.mat`, `{path, pathGuid}` refs in a scene's
  embedded material). The dress scripts give walls `SB_Wall_Concrete`, ceilings `SB_Floor_Concrete`
  and trim `SB_Dark_Wooden_Planks`, as preset references only;
- the Decal component's **Substrate** mode: **Auto** (the receiving material's substrate, else the
  asset's - only for an asset that has one, so graffiti never reveals the wall), **Asset**,
  **Custom** (the component's own `Substrate *` fields), **Off** or **Preset** (`Substrate Preset`,
  a `.substrate` path; the dress scripts put the ceiling decals on `SB_Floor_Concrete` this way).

`DecalSystem` keeps a table of the substrates in use (SSBO 16, 80-byte `GpuSubstrate`, up to 255),
keyed by the resolved files plus the values, so a material's and a decal's copy of the same set share
one entry. Each entry's maps take one 2048 slot of the decal texture arrays - albedo, normal, and a
surface map packed on load from the separate roughness (r) and AO (b) files - so they count against
the decal texture budget. Substrates used in the last ~600 Prepare()s stay pinned. A decal names one
as index + 1 in `Ids.x`'s top byte (`FLAG_SUBSTRATE_AUTO` lets the receiver's win), a material
through `uMaterialSubstrate` (set in `SetSurfaceOptions`, same encoding); `uSubstrateOn` gates both
to the views that prepared decals.

Shading (`ModelDecals.glsl`): sampled in world space on the receiver's dominant axis (so overlapping
decals and the Grunge reveal show one continuous wall), with explicit gradients and the same maxLod /
half-texel clamp as the decals. Its AO is baked into its colour (lamps, not ambient, light these
walls). Under a decal it replaces the decal's colour (keeping a little of the mask photo's grain) by
its strength, recessed: darkened where the decal's height is below its ~2 cm neighbourhood (three
mips down) and by the decal's cavity AO. Its normal goes on first and the decal's relief (the paint's
step and lips) on top at full strength; its roughness and AO follow under the coverage. In the Grunge
slot the map's alpha - the peel coverage of the cooked `T_Grunge_*` maps - reveals the material's
substrate past `1 - Grunge Strength`, read on the dominant plane 2.5 mips down (hairline cracks stay
dirt) and gated by the coverage 5.5 mips down (isolated specks on clean wall stay shut, clusters
open), with a thin lighter paint rim around each chip and, on walls, a hairline shadow inside its
upper edge.

`TARTARUS_NO_SUBSTRATE=1` turns every substrate off (before / after comparisons).

### Tests

`src/Tests/UnitTests_DecalRender.cpp` (`--unit-tests "Decal render"`): the 6-axis test against an
exact 15-axis SAT and a sampled brute force (never a false reject), box bounds, the deterministic
stable sort, the per-cluster cap keeping the top-most, the quadtree's invariants under random churn
(no overlaps, merging, layer limits), and seed-reproducible variation and atlas frames.

## Editor & assets

### The `.decal` asset

`src/Renderer/DecalAsset.*`. A JSON file (`decalVersion` 1, schema in the header comment of
`DecalAsset.h`) holding the textures (project-relative; only the albedo is required), the mask
mode (`lumaMask` for white-on-black alpha packs, `invertMask`), the slot `tier` (512 / 1024 /
2048 / 4096), the atlas grid, a default box size, per-channel opacities, the base surface values,
the angle and edge fades, height blend, the paint response, the asset's own weathering, and a
library category and tags.

- **Load is forgiving.** A missing key keeps its default; a wrong type keeps its default with a
  Console warning; numbers clamp to their ranges; the tier snaps to the nearest valid size;
  `tags` may be an array or one comma-separated string; unknown keys (a newer file) are ignored.
  An unreadable file or broken JSON returns an asset with `Missing` set rather than throwing.
- **Save is stable.** Keys are written in schema order (`ordered_json`), atomically
  (`AtomicFile`), so a load followed by a save is byte-identical and diffs stay clean.
- `.decal` is a known asset type (`AssetDatabase`), so it gets a GUID `.meta` and moves/renames
  keep references.

### Components

Both are generic-registered in `ComponentRegistry.cpp` (Inspector, Add Component, scene and
prefab serialization, presets), like Reflection Probe. `tools/check_component_registration.py`
picks them up from their `Register<>` calls.

- **Decal** (`DecalComponent`, Rendering): the asset (`ReflectAssetKind::Decal`, drawing from the
  project's `.decal` files), the box `Size` in metres, tint and overall opacity, per-channel
  multipliers (Channels), sort order, receiver layer mask and static/dynamic receivers
  (Receivers), seed, atlas frame and rotation/scale/flip jitter (Variation), and weathering added
  on top of the asset's. In memory the asset path is absolute, like every reflected asset field;
  the scene file stores it project-relative plus its GUID.
- **Decal Grading** (`DecalGradingComponent`): scene-wide Age and Wear. The first active one in
  the scene is used. Lighting > Environment shows the same two sliders ("Decal age", "Decal
  wear"); in a scene without one, moving a slider creates a "Decal Grading" object.

The box is the decal's `Size`, never the Transform's scale (keep that at 1). +Z points out of the
surface; the decal projects along -Z.

### Asset Browser

- `.decal` files list as their own kind (stamp glyph, "Decal" label, `t:decal` search). A tile
  shows its albedo's image thumbnail; the tile re-reads the `.decal` when it changes on disk
  (checked at most once a second), so retargeting the albedo updates the tile.
- Drag payload `ASSET_DECAL_PATH`. Dropped in the viewport on a surface it places a new decal
  entity (named after the file without `DA_`), facing out of the surface and upright from the
  view, sized to the asset's default size and given a random seed; one undo step. Dropped on a
  placed decal (its box is entered before the surface) or on a Hierarchy row with a Decal, it
  swaps that decal's asset and keeps its placement. Folders, tabs, favourites and bookmarks
  accept the payload like other files.
- **Create Decal**: the background menu writes `DA_NewDecal.decal` into the folder being viewed
  (`assets/Decals` otherwise). On a texture's context menu it writes `DA_<name>.decal` beside the
  texture with that texture as the albedo (a leading `T_` is dropped): Luma Mask is turned on
  when the image has no real alpha, the default size keeps the image's aspect, and the tier
  follows its resolution.

### Inspector

- Selecting a `.decal` opens its editor (`EditorLayer_Decals.cpp`): the preview with the atlas
  grid, the fade curves, then Textures, Placement, Channels, Paint, Weathering and Library
  sections. An edit saves when it commits (a drag released, a value typed), as one undo step that
  writes the file back; the renderer hot-reloads the file. Duplicate writes `<name>_Copy.decal`.
- The Decal component adds, below its fields:
  - **Fit to Surface**: casts along the decal's -Z and moves it onto the surface it hits, turning
    +Z out of it and keeping its spin. **Asset Size** resets Size to the asset's default.
  - The **preview** (albedo tinted by base colour x tint) with the **live atlas-frame picker**:
    hover a cell, click to use it; **From Seed** sets Frame back to -1.
  - The **channel matrix**: per channel, the asset's opacity x this decal's multiplier x Opacity
    = what lands, as a bar, with an on/off toggle for this decal (greyed out when the asset
    doesn't write the channel; Normal counts as off without a normal map).
  - The **fade curves**: strength by angle (head-on to edge-on, from the asset's angle fade) and
    through the box's depth (its edge fade). The preview assumes a linear ramp between the fade
    start and end.
  - **Randomize Seed** (a reflected button): a new seed, one undo step.
  - **Edit <asset>**: the shared asset's settings in place.

### Viewport

- The transform gizmo works on the box: Size is folded into the matrix it manipulates, so Scale
  (and the Rect/Universal bounds) drags write `Size`, while Move and Rotate write the Transform.
  The drag readout shows Size. Surface snap with align on a decal turns +Z out of the surface.
- Every decal draws its box: the +Z (surface-out) face solid, the far face and sides faint. A
  selected decal also shows an arrow down its projection axis and a tick on its top edge.

### Tests

`src/Tests/UnitTests_DecalAssets.cpp` (`--unit-tests Decals`): asset and component defaults and
registration, the `.decal` round trip (every field, byte-stable), the forgiving load (wrong
types, clamps, tier snapping, broken and missing files), and the Decal / Decal Grading scene
round trip.

## Editor tools

Phase 2. Code: `EditorLayer_DecalLibrary.cpp` (panel, scan, radial slots), `EditorLayer_DecalTools.cpp`
(Place, Brush, overlay, auto-grime), `DecalTools.*` (brush stroke and grime planner, no editor
dependencies, unit-testable), `DecalEditorState.h` (per-editor state), `Renderer/DecalPalette.*`.

- **Decal Library** (Window menu, or opens itself when a tool has nothing to paint with). Indexes
  every `.decal` under `assets/Decals` a slice per frame. Category tree, search (name, category,
  tags), Favourites and Recent, a wall-tone swatch (dark, mid, light) behind the previews, and a hover
  card with a live preview. Tiles cascade in with the library's motion spec and lift on hover;
  hearts pop. Dragging a tile into the viewport carries an `ASSET_DECAL_PATH` payload (the Asset
  Browser's decal cells use the same one).
- **Place Decal** (`P`, tool rail). A ghost follows the cursor on the surface under it: +Z points out
  of the surface, a decal on a skinned or non-static rigidbody receiver is parented to it. Scroll
  turns the ghost, Shift+scroll sizes it, click stamps (scale-in pop plus a ring flash). One undo
  step per stamp.
- **Decal Brush** (`B`). Source is one decal, a category, or a palette. Settings: radius, spacing,
  density, size and size jitter, rotation jitter, opacity, align to surface, follow stroke. Scroll
  sizes the radius, Shift+scroll the decals. Ctrl-drag erases decals whose centre is under the brush.
  A stroke is one undo step ("Paint Decals (n)" / "Erase Decals (n)"). New decals go under a
  `Decal Paint` group.
- **Auto-grime.** Select a room (a floor or wall group works), pick a palette, press Auto-grime.
  Rays from the room centre find floor and ceiling, then for each wall a depth profile per column:
  open to the floor is a door, open higher up a window (grime goes under the sill), a shallow bump a
  pipe (a drip below it). Wall bases, open wall, ceiling edge, corners and floor patches follow.
  Sites go to the palette entries that list that site kind. Everything lands under one
  `Grime (auto)` group, replaced on the next run (or the Clear button). The same seed gives the same
  result. It is meant for one room: a whole floor's bounds is a poor input.
- **Palettes** (`DP_*.decalpalette`, JSON, `paletteVersion` 1): weighted entries with scale and
  opacity ranges, a tint, and the sites an entry suits (`wall`, `base`, `corner`, `window`, `door`,
  `pipe`, `ceiling`, `floor`; none means anywhere). Shipped: `DP_Squat`, `DP_WaterDamage`,
  `DP_GangTurf`. Load / save is forgiving and byte-stable.
- **Radial.** With the Brush active, holding Space opens the decal ring: palettes, top-level
  categories, recent decals. Labels are cut to fit their wedge.
- **Sounds.** Preferences group "Decals & Graffiti": DecalPlace, DecalBrushTick (rate-capped),
  DecalErase, PaletteSwap, AutoGrimeStart, AutoGrimeDone, LibraryFavourite. The shipped wavs are
  synthesised placeholders, to be replaced by cuts from real recordings.
- **Test.** `--editor-tests "decals:"` opens the Apartment Hallway and drives all of the above, then
  saves and reloads.

## Corridor wear

The Apartment Hallway corridor is dressed as a derelict squat by `tools/scenes/dress_corridor.py`. It patches
`project/scenes/Apartment Hallway.json` in place (never rebuild that scene with `build_apartment_hallway.py`; it has
hand edits):

```
python -I tools/scenes/dress_corridor.py
```

- **Materials:** the corridor walls get the `PH_Beige_Wall` albedo and AO plus the `T_Grunge_PeelingPaint_036`
  grunge (strength 0.35, scale 0.25); the ceiling and baseboards get `_085` (0.45 and 0.5). The builder's
  `corridor_plaster` / `corridor_ceiling` / `corridor_trim` materials carry the same values, so a rebuild matches.
- **Decals:** a seeded, layered set under a `Corridor Wear` root (a soft base, a peel layer, then flakes and a crack
  on top): a heavy skirting band, door surrounds, two hero patches a side (one `_4K`), a water-damaged ceiling round
  the dead lamp and in the corners, the end walls round the windows, and dark `DA_LiquidSplatter_*` floor splashes by
  the doorways. A `Decal Grading` entity (Age 0.35, Wear 0.4) sits under the root.
- **Limits** (asserted by the script): at most 80 decals, 22 unique 2K `.decal` maps and one 4K map, to stay inside
  the decal texture budget. Only the `DA_PeelingPaint_*` sets that aren't tile or board grids are used.
- **Reruns:** the script first removes the `Decal Lab` and `Corridor Wear` subtrees, so running it again leaves the
  scene byte-identical.

## Apartment wear

`tools/scenes/dress_apartments.py` gives units 301-306 the same derelict look; run it after `dress_corridor.py` (which now
also removes the `Apartment Wear` subtree, so the pair is reproducible in that order):

```
python -I tools/scenes/dress_corridor.py
python -I tools/scenes/dress_apartments.py
```

The walls (and the window / arch `Reveal` pieces inside a unit) stay white: they keep their `baseColor`, normal and
roughness maps and gain only the `PH_Beige_Wall` AO map and the `_036` grunge (0.3); ceilings get `_085` (0.4) and the
apartments' baseboards `_085` (0.45). Decals sit under `Apartment Wear`, one `30x Wear` group per unit: two seeded units
are trashed (~60 decals), the rest moderate (~30-40), built from door-frame cracks and flakes, a skirting band, hero
patches (two or three sharp flakes each, for paint chipping) and ceiling clusters. Bathrooms are dressed like the hall:
one ceiling cluster, chipped lower walls and one dark `DA_LiquidSplatter_*` splash on the floor at 0.3 opacity. Placement is read from the scene (wall planes, which side faces into
the room, doors, windows, ceiling footprints) and a built-in self-check fails if a decal is off its wall, faces out of
the room or covers an opening. Limits asserted: at most 330 decals, and across corridor + apartments at most 30 unique
2K maps and one 4K map (the apartments add eight flake and crack maps to the corridor's pool, `APT_POOL`). 301 is the hand-edited NPC room: it only gains
materials and decals.

