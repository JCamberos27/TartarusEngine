# Character Outfit audit

This audit covers the Character Outfit system (`CHARACTER_OUTFITS.md`): the runtime, the Inspector editor, and
the Quantum assets. It started on 2026-09-30 and runs as phased PRs:
1. tools and baseline (this document);
2. correctness;
3. runtime performance;
4. Quantum assets, then clipping (4b);
5. editor UI.

What's left is in [OUTFIT_TODO.md](OUTFIT_TODO.md).

## Tools
All of these run headless. Release builds have no console, so redirect stdout to a file.

| Command | What it checks |
|---|---|
| `--unit-tests` | Pure rules and coverage geometry (Wardrobe, OutfitCoverage, WardrobeQuantum, ...) |
| `--outfit-rules` | The 60 presets resolve unchanged; 5000 random outfits per gender come out whole |
| `--outfit-audit [wardrobe] [csv]` | The rules, plus clipping for every under/over pair the rules allow |
| `--outfit-selftest` | The OutfitSystem API end to end on the real wardrobe (new) |
| `--outfit-cost [scenes]` | Triangles, vertices, draws, and how much skin hiding throws away (new) |
| `--outfit-shots <dir> [scenes]` | Framed PNGs of the characters, for before/after comparisons (new) |
| `--perf-bench scenes/OutfitTest --perf-res 1920x1080` | Frame time per scene: edit, docked play, maximized play |
| `tools/quantum/audit_quantum_assets.py` | Missing, miswired or orphaned files, import settings, VRAM (new) |

The Quantum clothing PNGs are git-ignored, about 9 GB. Hard-link them in from a checkout that has them, rather
than copying.

## Baseline (main @ 6e89a00; GTX 1080 Ti, 1080p play-max)

| Scene | Characters | Pieces | Draws | Triangles | fps | GPU Scene Draw | GPU Sun Shadow |
|---|---|---|---|---|---|---|---|
| Presets | 60 | 575 | 1211 | 11.5M | 156 | 1.5 ms | 4.6 ms |
| Randomized | 72 | 701 | 2217 | 14.2M | 69 | 7.7 ms | 6.4 ms |
| Items | 134 | 1130 | 3848 | 24.1M | 33 | 17.6 ms | 12.5 ms |

In edit mode, "Asset Pump + Outfit Hiding" averages 4–10 ms on the CPU, with spikes to 45 ms. The Items scene
peaks at about 5.5 GB of VRAM.

### Where the triangles go
- **The heads are 46% of all triangles.** Each head is 81–86k triangles in 9–10 sub-meshes:
  - skin: 47k;
  - brows and lashes: 20k;
  - teeth: 8.4k;
  - eyes and cornea: 3k each.

  Every character wears a balaclava (on purpose). About 60% of the face skin is hidden under it, yet all of
  it is still drawn, in the camera pass and in every shadow pass.
- **About 25% of drawn triangles have every vertex hidden.** Items: 5.7M of 24M, and 122 sub-mesh draws are
  entirely hidden. The torso is 75% hidden, the head 37%, the feet 20%, and the first-person arms, used as
  the third-person arms, 10–18%.
- **Heavy items:**
  - Ushanka: 72k triangles.
  - Winter jackets: 64–67k.
  - Fur collar: 36k, with no skeleton.
  - Sport sneakers: 31k.
  - The male arms mesh is `FirstPerson/Quantum_Arms_FP.fbx`: 29k triangles and 263 bones.

### Textures
- 742 textures:
  - 606 at 4096², 132 at 2048², 4 at 1024²;
  - all block-compressed ("High Quality"), capped at 4096 on import.
- **About 9.1 GB of VRAM if all were resident. Clothing is 8.4 GB of that.**
  - Tops: 2.9 GB.
  - Outerwear: 1.9 GB.
  - Pants: 1.1 GB.
  - Hats: 0.8 GB.
- No texture is missing. Every FBX material has a remap to a `.mat` that exists. Import settings are right
  (colour maps sRGB, data maps linear, normal maps typed Normal Map).
- 33 FBX files carry embedded texture paths into the artist's drives (`D:/OneDrive/...`, `M:/Projects/...`).
  Their remaps override them, but every import logs a warning for each.
- **Orphans:**
  - 40 materials no model or colourway reaches. These are alternate looks in sub-folders:
    - `Hat_Classic/M_Hat_Classic` (the hat defaults to `Beach/M_Hat_Beach`, and colourways are only
      same-folder siblings);
    - the Leather coats and jackets;
    - the Fur_Collar jeans jackets;
    - the Print jeans vest;
    - ties.
  - 6 textures no material uses: the Puffer_Jacket "Clear" set and the Sport_Pants "Lines" set.
  - 24 texture slots with no `textureGuids` entry (eyes, brows, the balaclava).

### Rules and clipping
- **Rules:** the 60 presets resolve unchanged; 10,000 random outfits come out whole.
  - The balaclava excludes Hat and Headphones, and every style puts a balaclava on. So Randomize never
    gives a hat or headphones, although the styles give them 10–35% fill.
  - Female has no Formal style.
- **Clipping:** 1017 of 3195 pairs still poke through somewhere in bind pose (`--outfit-audit`).
  - The worst are heads under hats and hoods: 1.2k–6.1k vertices, 15 cm deep.
  - The head keeps skin that would leave a hole (`Backed`), so these may be expected. Check them visually
    with `--outfit-shots`.

### Bugs `--outfit-selftest` confirms (7 of 45 checks fail at baseline, all fixed in phase 2)
1. **`SetGender` drops items.**
   - `CutName` doesn't strip `SM_F_`, so the balaclava is lost.
   - Names differ between the genders (`SKM_Jeans` vs `SKM_F_Pants_Jeans`, and `_Inboots` variants), so the
     pants are lost too.
2. **`SetColourway` doesn't bump the outfit's version.** The first-person body's twins keep the old
   materials.
3. **`LoadPreset` throws on a malformed file** instead of returning an error.
4. **A waiting colourway is lost when you equip right after it.** `Submit` erases every pending step for the
   outfit.
5. **A duplicated piece in a slot is never removed.** `Pieces()` keeps the first match only.
6. **New pieces never follow the body's Animator Controller.** `follower.Driver` is never set, so each piece
   runs its own state machine.
7. **Editing a piece's Item or Slot in place doesn't re-run the hiding.** `HideSignature` hashes only
   entities and models.

### Bugs found by reading the code (not yet covered by a check)
- **A failed re-model still records the new item**, and `Result.Ok` stays true. (Fixed in phase 2: the old
  piece and its record stay.)
- **Coverage for rigid headwear is worked out in the animated pose** when it's first computed during Play.
  The coverage cache key also has no world scale. (Fixed in phase 2; coverage version 15, so every pair is
  worked out once again.)
- **`AdoptExisting` puts every body alternate in "Feet".** (Fixed in phase 2: the slot comes from the
  cover rule that uses the alternate.)
- **The first-person twins never refresh their materials or hiding.** (Fixed in phase 2: `SyncTwins`
  copies them when they change.)
- **"Rescan wardrobe" keeps the colourway folder cache and the layer wardrobe.** (Fixed in phase 2.)
- **A missing or broken wardrobe is read again on every call.** (Fixed in phase 2: failures are cached
  until a rescan.)
- **The editor's item thumbnails keep GL texture ids** that the thumbnail cache can delete, so they can dangle.
- **Every clothing piece draws double-sided** (`SceneRenderer.cpp`), whatever its material says.
- **Per-frame CPU work in `UpdateHiding` and `UpdateAttachments`:**
  - both build a `std::map<std::string, entity>` for every outfit, every frame;
  - `UpdateHiding` redoes its pair loop every frame while coverage is still computing;
  - coverage starts one `std::async` thread per pair.
- **The Inspector, every frame:**
  - it reloads a broken wardrobe from disk;
  - `Catalog::Find` searches linearly, allocating strings;
  - clash checks run for every card;
  - with the "..." menu open, it scans the whole asset tree.

## Phase 2: correctness
All 45 `--outfit-selftest` checks pass. `--unit-tests` (7222), `--outfit-rules`, the Sandbox `--smoke-test` and
`--weapon-test` (45 checks) pass too.
- **`SetGender`:** `CutName` strips `SM_F_` and variant suffixes. When no cut has the same name, the most alike
  name in the slot (shared words) is taken; Top, Pants and Shoes always get one.
- **`SetColourway`** bumps the outfit's version.
- **`LoadPreset`:** every read is inside the `try`. A preset made for another wardrobe adds a note.
- **`Submit`** keeps colourway-only steps that are waiting.
- **`Apply`:**
  - removes a second piece in a slot;
  - links every piece's Animator Controller to the driver's (follower mode). `UpdateAttachments` links a
    loaded scene again (`CharacterOutfitComponent::LinkedVersion`).
- **`HideSignature`** includes each piece's slot, item and flags.

## Phase 3: runtime performance
- **Covered triangles aren't drawn.** For each piece and its hide bits, the triangles with at least one
  visible vertex go into one element buffer (`VisibleIndexBuffer`, on `OutfitHideTag::Visible`). It's shared
  by every piece with the same model and bits, and drawn in place of the mesh's own indices in every pass:
  camera, sun cascades, spot/point shadows, and the SSAO pre-pass. Sub-meshes covered entirely are skipped.
  Vertex numbering is unchanged, so the hide bits and the bone palette still line up. The 57 `--outfit-shots`
  frames match the baseline: no pixel differs by more than 25 levels.
- **Blended sub-meshes at Opacity 0 are skipped** (the Quantum eye overlay and saliva: two draws per head
  that added nothing).
- **CPU:**
  - `UpdateHiding`'s per-frame check hashes the root's children directly, with no map built.
  - While coverage is being worked out, it looks again every 8 frames, not every frame.
  - `UpdateAttachments` builds no map.
  - Coverage runs at most (cores − 1) jobs at once, instead of a thread per pair.
  - Items play: "Outfits + Transform Cache" 0.57 → 0.35 ms, "Asset Pump + Outfit Hiding" 0.34 → 0.13 ms.

1080p play-max, two runs after a warm-up run:

| Scene | Baseline | Phase 3 | GPU Sun Shadow |
|---|---|---|---|
| Presets | 156 fps | 168–174 fps | 4.6 → 4.3 ms |
| Randomized | 69 fps | 63–70 fps | 6.4 → 6.0 ms |
| Items | 33 fps | 37–41 fps | 12.5 → 11.3 ms |

GPU Scene Draw moves 1.5–2.5 ms between runs of the same build in the textured scenes (Randomized, Items),
while the shadow pass, which samples no textures, is steady. That points at texture bandwidth, which is phase
4's work. Sandbox is unchanged within run-to-run noise: at 1440p, alternating runs of main and this branch
averaged 5.39 and 5.47 ms, each spread over about 0.3 ms.

Two things to know when benchmarking:
- **Builds share the coverage cache.** Builds with different coverage versions (main is 14, this is 15)
  overwrite each other's `Library/OutfitCoverage` files. Alternating them makes each re-work its coverage
  during the bench.
- **Draw calls cost CPU.** Items has 3848 of them, and CPU Scene Draw is 5.5 ms.

## Phase 4: Quantum assets
- **Clothing textures are capped at 2K** (656 `.png.meta` files, `maxTextureSize` 2048). Body, head and eyes
  stay 4K. Worst-case resident VRAM: 9.1 GB → 3.4 GB.
- **15 materials got the `textureGuids` they were missing** (eyes, brows and lashes, balaclavas, fur).
- **No texture loads twice:** `--outfit-cost` counts the models' own embedded textures, and none load (0 MB).
- **Double-sided clothing stays on.** Drawing it single-sided measured no GPU gain, and would open holes in
  collars, hoods and hems.

## Phase 4b: clipping
- **Head accessories trimmed.** Only balaclavas, hoods and the black classic glasses stay:
  - the Hat and Headphones slots, their tag rules, excludes, clashes and style fills are gone;
  - `Hats` is in `excludeFolders`, and the aviators are hidden;
  - the 14 presets that wore aviators wear the classic glasses, and the ones with hats or headphones lost
    them;
  - the OutfitTest scenes were regenerated.
- **Female balaclava fit.** It's modelled a little small for the female heads, and the back of the head
  poked out. Its item override has `"fit": 1.06`, applied about the model's centre everywhere it's placed:
  `UpdateAttachments`, the first-person twins, coverage geometry, and the audit. Head poke-through dropped
  from about 300 vertices to about 21, all at the face openings.
- **`Backed` looks back as far as the vertex pokes out.** A bun 9 cm out through a balaclava used to read as
  a hole at the 4 cm look-back and stayed drawn over it.
- **Layer pull** (`OutfitLayerTag`, `uLayerPull` in `ModelVertex.glsl`): each piece draws 4 mm nearer the
  camera per layer worn over it (up to 3), along the view ray. It covers the grazing cases (under 4 mm, the
  largest group in the posed depth histogram). Pushing vertices out along their normals was tried first and
  made clipping worse (it moves the silhouette), so it was dropped.
- **The audit covers every race's head** (the heads differ), and `--outfit-posed` checks 32 poses.
- **Coverage version 17.**

`--outfit-audit --outfit-posed` now (2380 pairs):
- **Bind pose:** 211 pairs clip. Most are the head under tops and jackets, through the neck opening at the
  10 cm look-out cap. They're behind the collar in the shots.
- **Animated:** 1265 pairs clip in some pose. Poke depth over all vertex-poses: ≤4 mm 268k, 4–5 mm 38k,
  5–10 mm 135k, 10–15 mm 125k, 15–20 mm 89k, 20–25 mm 75k, 25–30 mm 71k (the check stops at 3 cm).

| Under / over | Pairs | Clip in bind | Clip posed | Posed vertices |
|---|---|---|---|---|
| Head / Outerwear | 148 | 121 | 129 | 13,512 |
| Shoes / Pants | 34 | 0 | 30 | 6,811 |
| Outerwear / Bag | 146 | 0 | 146 | 6,662 |
| Top / Outerwear | 65 | 0 | 65 | 4,575 |
| Pants / Outerwear | 152 | 0 | 151 | 3,636 |
| Top / Bag | 104 | 0 | 100 | 3,066 |
| Head / Top | 93 | 81 | 74 | 3,006 |
| Balaclava / Outerwear | 36 | 0 | 35 | 2,222 |
| Head / Balaclava | 8 | 8 | 8 | 1,803 |
| Arms / Outerwear | 36 | 0 | 34 | 1,629 |

The worst single pairs are sport sneakers under cargo, jeans and sport pants in a crouch walk (780–970
vertices, 3 cm), and the female heads under the puffer and open winter jackets in the pickup pose (650–800).

Verification: `--unit-tests` 7349 checks, `--outfit-selftest` 45, `--outfit-rules` clean, `--smoke-test` on
OutfitTest and Sandbox with no GL errors. 1080p play-max: Presets 177 fps, Randomized 61 fps, Items 54 fps
(fewer pieces now that hats are gone), Sandbox 255 fps (unchanged).
