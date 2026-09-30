# Character Outfit audit

This audit covers the Character Outfit system (`CHARACTER_OUTFITS.md`): the runtime, the Inspector editor, and
the Quantum assets. It started on 2026-09-30 and runs as phased PRs:
1. tools and baseline (this document);
2. correctness;
3. runtime performance;
4. Quantum assets;
5. editor UI.

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

### Bugs `--outfit-selftest` confirms (7 of 45 checks fail)
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
- **A failed re-model still records the new item**, and `Result.Ok` stays true.
- **Coverage for rigid headwear is worked out in the animated pose** when it's first computed during Play.
  The coverage cache key also has no world scale.
- **`AdoptExisting` puts every body alternate in "Feet".**
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
