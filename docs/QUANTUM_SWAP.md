# Quantum swap: partial Unity-version pack → complete Modular Casual Bundle

Working log for replacing `project/assets/Characters/Quantum` with the complete Quantum Modular Casual Bundle
(male + female "Vivian"), exported from Unreal to a local `Quantum - Modular Casual Bundle\` folder
(`Export\`, `manifest.json`, `AUDIT_REPORT.md`). The job is finished (PR 831); the wardrobe itself is
documented in [CHARACTER_OUTFITS.md](CHARACTER_OUTFITS.md).
Branch: `dev/quantum-swap`.

## Phase 0 results (2026-11-18)

All checks ran through the engine's own importer (`TartarusEngine.exe --model-nodes` / `--model-report`, Assimp
5.4.3), comparing the current repo file with the new export.

### Rig: PASS. Body, heads and races are swapped, not kept

| Pair (old → new) | Bones | Worst world-space difference | Bounds / bind |
|---|---|---|---|
| `European/Quantum_Body_Full` → `SKM_Quantum_Body_Full` (the motion-matching target rig) | 353 = 353, same names | 0.003 mm, 0.0001° | identical; pelvis 0.959 m, hands ±0.478 |
| `European/Quantum_Torso` → `SKM_Quantum_Torso` | 353 = 353 | 0.000 mm | identical |
| `European/Quantum_Head` → `SKM_Quantum_Head` | same bones | 0.000 mm | identical |
| `Afro/Quantum_Torso_Afro` → `SKM_Quantum_Torso_Afro` | same | 0.000 mm | identical |
| `Female/.../SKM_F_Vivian_Body` → `SKM_Vivian_Body` | 163 bones, same | 0.001 mm | identical; pelvis 0.987 m |
| `Female/.../SKM_F_Vivian_Head` → `SKM_Vivian_Head` | same | 0.001 mm | identical |
| `SKM_Hoodie`, `SKM_Jeans` → `SKM_Hoodie_Black`, `SKM_Jeans` | same | 0.002 mm (`weapon_l/r` differ by 90°; unused, unweighted) | identical |
| Female pants, sneakers, legs vs new Vivian body | same | 0.001 mm | identical |

**Two differences to watch:**

1. **Node layout.** The UE export wraps the skeleton in an extra node: `RootNode → <MeshName> → root`, where the old
   file had `RootNode → root`. The 90° axis rotation is on `RootNode` instead of on `root`, so `root`'s local transform is identity now.
   World-space poses are identical. Phase 3 confirms motion matching is unaffected (`--unit-tests` rig diff,
   `--npc-test mmview`, `--npc-test mmdb`, `tools/mm_smoke.ps1`).
2. **Female hoodie.** The new `SKM_F_Hoodie_*` binds its *leg and finger* bones in a different rest pose than Vivian's body:
   up to 25 mm on the legs and 54 mm on the fingers. The hoodie isn't weighted to those bones, so it renders the same. Spine,
   arms and neck match. Female trousers and shoes match exactly. Any garment flagged by a bone-diff check gets a visual check
   in Phase 1.

### Axis and scale: no settings needed

New FBX files import with the same size, bounds and bind as the old ones with **no** `.meta` `upAxis`/`rootTransform`/`globalScale`
changes. Assimp's GlobalScale handles the cm units.

### Geometry differences (expected)

- The new body/head files carry the eye, teeth and lacrimal sub-meshes as separate materials (11 slots on `SKM_Quantum_Body_Full`).
  The old head already had them.
- Vertex counts are within a few vertices on clothing (UE splits seams slightly differently). Heads have +1-3% vertices.
- No morph targets are imported, same as today. The face-rig heads carry up to 852 in the FBX; the engine ignores them.

### Materials

- 465 exported material JSONs. **393 use `M_Quantum_Master_Material`** with exactly BaseColor + Normal + ORM (R = AO,
  G = roughness, B = metal). They map to `.mat` as `_AlbedoMap`, `_NormalMap` + `_NormalFlipY`, and `_MetallicRoughnessMap` + `_AOMap`.
- The master material is **Masked + two-sided**.
- `Brightness` varies from 0.09 to 5.0 (107 MIs; most 0.7-1.0). `Saturation` is set on 31 MIs and `Contrast` on 13; those
  need a baked albedo.
- 30 hair MIs (`M_Hair_Master`, `M_F_Hair_Master`) plus facial hair: colour comes from `RootColor`/`TipColor` **parameters**
  over a `T_Root` gradient, with an `Alpha` mask. They need a baked albedo-with-alpha.
- Eyes, teeth, lacrimal and cartilage use MetaHuman-style shaders. **The engine's existing eye/teeth/brow materials are kept.**
- Every exported BaseColor PNG is RGBA (Unreal always writes alpha). The importer keeps alpha only where it isn't fully opaque.

### FBX material names: every materialRemap is regenerated

The engine matches `materialRemap` keys on the FBX's material names (`AssetLibrary::ApplyMaterialRemap`).
- The old Unity-era FBX files used the **slot** names (`M_Hoodie1`, `M_Quantum_Body`).
- The UE exporter names each FBX material after the **material assigned** to the slot: `SKM_Hoodie_Black` gives `MI_Hoodie_Black`, `SKM_Quantum_Body_Full` gives `MI_Quantum_Body`.
- When two slots share one material, the later one gets `_<slot index>` (`MI_EyeRefractive_Inst_4`).
- A slot with no material gets `Fbx Default Material`. That's 55 meshes, mostly the headwear `SM_` ones, whose colour UE sets elsewhere, so their colourway is inferred by name.

The real names of all 1,075 exported FBX files are cached in `tools/quantum/casual_bundle/fbx_materials.json`.

### Other findings

- `SKM_Vivan_Head_FaceRig` has more than 512 bones (the engine's `MAX_BONES`). The face-rig heads are skipped; the regular heads replace the old ones.
- The `.gitignore` hats rule and `asset_manifest.py` `EXCLUDE_PREFIXES` drop `Textures/Clothing/Hats/` (hats were removed in #501). New headwear uses `Headwear/` folders instead.
- Wardrobe clothing is found by **folder scan**, so 26 kept items (Inboots pants, `SKM_Boots_Socks`, `Quantum_Arms.fbx`...) have no explicit reference anywhere. The map lists them as `keep-old` rows so the retirement scan can't drop them.
- Already broken today: the wardrobe's `SM_Glasses_Aviator` override and 6 `project/settings.json` `assetFolders` entries that don't exist.

### Skins: the pack is complete

The UE pack has male skins for European, Afro, Asian, **Old** (European/Old) and Young, and female skins for European, Afro and Old (head).
So no skin textures need keeping from the old copy.

## Phase 1: import done (2026-11-18)

`python tools/quantum/import_casual_bundle.py --apply`: **0 problems.**

- **Written:**
  - 211 meshes (105 replaced at their old paths, GUIDs kept; 106 new);
  - 411 materials (263 replaced, 148 new);
  - 639 textures (561 cooked, 78 baked: hair colours, plus saturation/contrast/colour-correction albedos).
- **Retired:** 502 old files (Unity-era maps, the misfiled `Boots_Classic`/`Flip_Flops` copies, sweater mats now `Hoodie_Sweater` colourways, duplicate colour names).
- **`project/external_assets.csv`:** 7,732 → 7,943 rows (480 removed, 691 added, 159 changed), +1.49 GB.
- **Kept** (`keep-old` in the map): the keep-list plus every Unity-only clothing item with no Casual Bundle equivalent (`SKM_Boots`, `SKM_F_Boots`, `SKM_F_Bomber`...) and its colourways.
- **Not importable** (9 headwear meshes): their UE materials point at another Quantum pack, and the export has no textures for them. They are the beret, cowboy hat, top goggles, bandana, headband, front headband, micro headphones, female panama (SM), and bobcut + beret. Listed in `last_import.json` `not_importable`.
- **Engine limits handled:**
  - Glasses lenses and the female scalp are translucent in UE; they are rebuilt opaque or cut-out from their textures, like the old mats.
  - Roughness Intensity above 1 is capped (2 headphone mats).
- **Pre-swap backup** (repo Quantum folder + CSV): kept off the repo.

`tools/quantum/audit_quantum_assets.py` after the import: 110 notes, none blocking.
- 15 "FBX material not remapped": FBX material entries with no geometry, which Assimp drops; every drawn material is remapped. The two team arm/hand exports carry them from before the swap.
- 55 "maxTextureSize" and 35 "texture no material uses": old textures still kept alive by scene `library*` lists. They go in Phase 3, when those lists are cleaned and the import is re-run.
- 5 "material nothing uses": Unity-only colourways of kept items.

## Phase 1: importer

`tools/quantum/import_casual_bundle.py` (driver) + `tools/quantum/casual_bundle/`:

| Module | Job |
|---|---|
| `common.py` | Paths, caps, GUID/JSON helpers, the mapping-table schema (`MapRow`) |
| `build_map.py` | Generates `tools/quantum/casual_bundle_map.csv`: every UE mesh/material/texture → item group → repo path → action |
| `materials.py` | UE material JSON → engine `.mat` |
| `textures.py` | Cook PNG (cap, alpha, BMP → PNG, saturation/contrast and hair bakes) + `.png.meta` |
| `metas.py` | Model `.meta`: GUID kept, `materialRemap` regenerated from the FBX's real material names |
| `refs.py` | Reference scan: what's retired, which references dangle |
| `csvrows.py` | `project/external_assets.csv` row swap + the retired-Used list for the Drive |

## Phase 2: engine (2026-11-18)

The outfit system uses the whole pack; `docs/CHARACTER_OUTFITS.md` describes it.

- `Quantum.wardrobe`: new slots Hair, FacialHair (male only), Headwear (`hatSlot`) and Headphones (Headwear models
  moved by item overrides), all head-attached (shadow-only in first person). Rules: fitted hat cuts picked and
  carrying their hat, cuts/hats with hair, hats or glasses of their own, hoods (incl. both winter jackets) clear the
  hair and keep only under-hood headwear, the balaclava takes the head items off, crests don't go under hats,
  `Shirt_Tie` formal/tucked/long-sleeved, rolled-up sleeves keep the arms. Races: Old's own torso/feet/underpants/
  shoe feet, Afro's shoe feet, Tattooed/Ornaments (male) and Make-up/Tattoo L/R/L+R (female), not random. Stale
  `SM_Glasses_Aviator` override removed; `SM_F_Haircut_Scalp` hidden (no material in the pack).
- Race-fitted items: not needed. The map folded every `_Afro` clothing variant into its base mesh as a colour
  variant: the meshes are identical and only the skin slot's material differs (`MI_Quantum_Body_Afro` vs
  `MI_Quantum_Body`), which the race's skin suffix already swaps (`Wardrobe::SkinMaterialFor`).
- Code: `Wardrobe::Resolve`/`MarkVariants` compare cuts without the mesh prefix and know `SM_F_` hats; a fitted cut
  that carries its hat stays when no hat is asked for (an outfit read back from its pieces); default layers for
  FacialHair/Headwear/Headphones. First person, coverage and the editor needed no change (data-driven).
- `--unit-tests`: 55212 checks, 3 failures, all `OutfitEdgeBandPosed` (the new `SKM_Shirt_Hawaii` over the kept
  `Quantum_Arms_FP` leaves 132/346/562 hidden arm vertices out from under the sleeve at 20/35/50 degrees). The
  pre-Phase-2 build fails the same way on the new assets: it comes from the import (Phase 3: check the shirt's
  sleeve weights, or the test's posing with the new `RootNode -> <Mesh> -> root` layout).

## Phase 3 (2026-11-18, second computer)

### The axis turn on RootNode: fixed with `rootNodeInModelSpace`

Phase 0's "world-space poses are identical" held for the bind pose only. The engine's model space is
`GlobalInverse * node global` with `GlobalInverse = inverse(RootNode)`, while skinned vertices are baked with the mesh
node's full global frame, RootNode included. The old Unity-era files carry the -90 X turn on the `root` bone, so both
agree. The Unreal exports carry it on RootNode, so the bones posed in a Z-up frame (`upperarm_l` at z 1.44) and the
vertices sat in the Y-up one. At bind every skin matrix is the identity and nothing shows. Once a bone turns, its delta
is applied in the wrong frame: limbs bend in the wrong plane. `OutfitEdgeBandPosed` caught it, because the new shirt's
sleeve swung backwards while the kept arms swung down. The old shirt passes the same test.

- New import setting `ModelImportSettings::RootNodeInModelSpace` (`.meta` `importer.rootNodeInModelSpace`, editor
  "Root Node In Model Space"). Model space keeps the root node's transform (`GlobalInverse` = identity), and every bone
  offset is re-based (`offset * GlobalInverse`). The bind is unchanged, and each bone's delta now turns the vertices in
  their own frame. Opt-in, so no other asset changes.
- `import_casual_bundle.py` sets it on every mesh it writes (211). The kept Unity-era files don't need it.
- Result: `OutfitEdgeBandPosed` passes (0 holes at 20/35/50 degrees). `--unit-tests`: 55212 checks, 0 failures.

### Re-running the import on another computer

- `old_slot_table` keys the remap of an imported meta by the FBX material name, not the slot name. The eye, teeth and
  brow fallback now tries both (it gave 102 "has no .mat" problems on a re-run).
- Retirement is case-blind against what the import writes. Two jersey metas were tracked as `T_Woman_Jersey_*` while
  the import writes `T_woman_jersey_*`; on Windows these are the same file, and the import deleted the texture it had
  just cooked. Both metas were renamed to the written spelling.
- `retired_used_paths.txt` merges with the list already there (a re-run only adds; the earlier files still go in Phase 4).
- The re-import gives byte-identical `.mat`/`.meta`/CSV output apart from line endings.

### Scenes

- The 7 scenes with Quantum `library*` entries lost 211 dead entries (files retired in Phase 1, plus 35 old Unity-era
  textures that only those lists kept alive). The 35 are retired now: CSV -35 rows, `retired_used_paths.txt` 480 -> 515.
  The entries were removed line by line, so the rest of each scene is byte-identical.
- All 60 `Outfits/Quantum/*.outfit` presets resolve (398 paths, 0 missing).

### Outfit rules for the complete pack

The wardrobe's rules were written for the old, partial pack (bald heads, no beards or hats). With the full bundle in:

- **Balaclava:** the everyday styles fill it at 0.05 (was 1.0, which kept every random head bald). A new **Masked**
  style (weight 0.4, any Casual or Winter piece) always wears it. Randomize now gives 265 / 400 heads with hair and
  114 with headwear.
- **Aviators** are offered again; a hard exclude keeps them off the balaclava (the classic glasses still go with it).
- **Hair and beards** stay in every style, except mohawks, irokez, braids and the long beards: they're kept out of Formal.
- **Long sleeves:** `SKM_Jacket_Tshirt` joins the sleeve-hands list (its sleeves reach the wrist like the other
  jackets, by mesh extent). Vests, jerseys, T-shirts, the Hawaii shirt and the rolled-ups keep the full arms.
- **Third-person arms:** unchanged. Every male body keeps the first-person arms, and races swap only their skin.
- Checked in data, no change: `SKM_Bobcut` is the pack's own male cut (`SKM_M_Bobcut`), `SKM_Hair_Skin` is real hair,
  and `SKM_Hat_Classic_Hairs` is a hat only (no hair mesh, so not `HairBuiltIn`).
- Dead `excludeFolders` (`WithHair`, `Hats`) are gone. `tools/quantum/wardrobe_audit.py` lists each slot's items and
  tags and flags rules that match nothing.

### Phase 3 leftovers

- `AI/Soldier.json` and `tools/scenes/sandbox_template.json` already resolve: 38 Quantum refs each, 0 missing paths,
  0 GUID mismatches. No regeneration needed (`gen_npc_soldier.py` was removed with the legacy locomotion).
- The importer's two dangling references are fixed: `build_master_scene.py` spells out the body-part paths (the
  `{model}` template hid them from the reference scan), and `asset_manifest.py` drops the exclusion for the Unity-era
  `Textures/Clothing/Hats` folder, which no checkout has any more.

### The player's head drew see-through

The scenes' saved head material lists were still in the old head's slot order. On a re-dress, `PickedColourways` read
each slot that differed from the remap as a picked colourway. The folder rule then spread `M_Eye_Overlay` (transparent)
to every material in `Materials/Characters`, skin included. Fixed in two places: body parts take no colourway picks
(`OutfitSystem.cpp`), and the 13 saved head lists now follow the new head's mesh order.

## Merged (2026-11-18)

Phases 0-3 merged in PR 831. The forest-lights PR 832 and the forest-trail scene removal PR 833 followed the same day.

## Phase 4: Drive (2026-11-18)

- **Upload:** the 850 payloads main lists but the Drive lacked went to `Used`: 691 added and 159 replaced (3.6 GB), each
  checked by sha256. `Used\project\external_assets.csv` is main's copy again.
- **Safety copy:** an off-repo backup holds the 515 retired `Used` files (1.9 GB)
  and both old Raw folders (225 files, 4 GB). The 159 replaced
  files went up before the backup existed, so their old versions are only in the Drive's version history (30 days).
- **Raw:** `Characters\Quantum - Extras (not in Casual Bundle)\` holds the 39 keep-list sources (3 GB): the team
  arm/hand exports, the male and female face rigs (`.dna`, `.mb`, FBX, beard combos), `SKM_F_Vivian_Parts` and
  `SKM_Hair_Back_2`. `Quantum - Characters (partial)` and `Quantum - Modular Basemesh` went to the Drive trash: their
  other 186 files have Casual Bundle equivalents. `Raw\SOURCES.md` and `Raw\CHANGELOG.md` are updated.
- **Used:** the 515 files in `tools/quantum/retired_used_paths.txt` went to the Drive trash. Separately, 293 unlisted
  mocap `RootMotion` clips and the unused forest-trail scene went too. `asset_manifest.py verify` on `Used`: 7908 listed,
  0 missing, 0 different, 0 unlisted.
- **Team, after pulling:** `python tools/assets/asset_manifest.py prune project/assets/Characters/Quantum`, then
  `tools/assets/fetch-assets.ps1`.
- **Still open:**
  - the Fab receipt and the Epic Starter Pack terms in `Licenses\`;
  - deleting `UE_Source\` on the export machine.
