# Character Outfits: remaining work

What's left after the outfit audit (phases 1–4b, PRs #494–#498). The system itself is
described in [CHARACTER_OUTFITS.md](CHARACTER_OUTFITS.md). Items are in rough priority order within each
section. Each one says what's wrong, where to look, a suggested approach, and how to check it's done.

Standing decisions (don't undo these):
- Every character wears a balaclava, on purpose.
- Clothing textures are 2K; body, head and eyes are 4K.
- Clothing stays double-sided (single-sided measured no GPU gain).
- Only balaclavas, hoods and the black classic glasses are offered on the head. Hats, headphones and the
  aviators are out of the Quantum wardrobe, and not worth bringing back.

## How to test
All headless; the Release exe has no console, so redirect to a file. Build in a short path (PhysX breaks on
long worktree paths), e.g. `cmake --build C:/tb/perf --config Release`.

| Command | Use |
|---|---|
| `--unit-tests` | must report 0 failures |
| `--outfit-selftest` | OutfitSystem API end to end, 45 checks |
| `--outfit-rules` | presets and 10,000 random outfits |
| `--outfit-audit assets/Characters/Quantum/Quantum.wardrobe out.csv --outfit-posed` | clipping per pair, in bind pose and 32 animation poses (about 2.5 min) |
| `--outfit-shots <dir> scenes/OutfitTest` | PNGs, with `TARTARUS_SHOT_NAMES` and `TARTARUS_SHOT_POSE="<clip>@<fraction>"` |
| `--outfit-cost` | triangles, draws, hidden share |
| `--perf-bench scenes/OutfitTest --perf-res 1920x1080` | frame time per scene |
| `--smoke-test scenes/OutfitTest` and `scenes/Sandbox.json` | GL errors, load, undo round-trip |

Gotchas:
- After a `kCoverageVersion` bump (in `OutfitSystem.cpp`, 17 today), the first run re-works coverage in the
  background and draws pieces unhidden until it's done. Run shots and benches twice; use the second.
- Builds with different coverage versions overwrite each other's `Library/OutfitCoverage` files.
- The engine rewrites about 500 `.mat.meta` / `.png.meta` files on every run (a `"folder"` field). That's
  noise: stage only the files you meant to change.
- `--outfit-audit` exits 1 while any pair clips; that's expected today.

Baseline to beat (1080p play-max, GTX 1080 Ti): Presets 177 fps, Randomized 61 fps, Items 54 fps,
Sandbox 255 fps. Clipping: 211 of 2380 pairs in bind pose, 1265 in some animated pose.

---

## 1. Clipping in motion (highest priority)
Layer pull (4 mm per layer, up to 3 layers) handles grazing. What's left is cloth that goes 5 mm to 3 cm+
through the layer over it once the character moves. From `--outfit-audit --outfit-posed`:

| Under / over | Pairs clipping posed | Posed vertices | Where |
|---|---|---|---|
| Head / Outerwear | 129 of 148 | 13,512 | pickup: head bends into the collar |
| Shoes / Pants | 30 of 34 | 6,811 | crouch walk: sneaker tongue through the hem |
| Outerwear / Bag | 146 of 146 | 6,662 | run: jacket through the bag straps |
| Top / Outerwear | 65 of 65 | 4,575 | jump, arm flare |
| Pants / Outerwear | 151 of 152 | 3,636 | long coats over the hips |
| Top / Bag | 100 of 104 | 3,066 | run |
| Balaclava / Outerwear | 35 of 36 | 2,222 | pickup |
| Head / Balaclava | 8 of 8 | 1,803 | arm flare (face openings) |
| Arms / Outerwear | 34 of 36 | 1,629 | run (sleeve cuffs) |

Worst single pairs: sport sneakers under cargo, jeans and sport pants in a crouch walk (780–970 vertices,
3 cm); female heads under the puffer and open winter jackets in the pickup pose (650–800).

### 1a. Shoes under pants (crouch)
- **Why:** coverage is worked out in bind pose. In bind pose the pant hem clears the sneaker, so nothing is
  hidden. In a crouch the hem drops onto the shoe and the shoe's tongue goes through it.
- **Option A (cheap, try first):** swap to the `_Inboots`-style tuck for sport sneakers, or add a
  `"layers"` rule that puts high sneakers *over* pants (like boots). Check how it looks standing.
- **Option B:** posed coverage. Work out `OutfitCoverage` for the pair in the posed meshes too
  (`OutfitAudit::Posed` already skins them), and OR the posed hidden bits into the bind-pose ones for the
  under piece. Only for pairs the audit flags; cost is one more coverage job per pose. Needs care not to
  hide skin/cloth that shows in other poses (a shoe's top that's visible standing). Limit to vertices that
  are within `kEdgeBand` of already-covered ones.
- **Check:** `--outfit-posed` Shoes/Pants posed vertices down from 6,811; `TARTARUS_SHOT_POSE` with the
  crouch walk clip at 0.1 and 0.35 on a character in sport sneakers and cargo pants.

### 1b. Jackets and tops through bags
- **Why:** bags (layer 8) are rigid-ish straps skinned to the chest. The jacket under them swells when the
  arms swing, and the bag doesn't hide anything on the jacket because in bind pose the straps sit clear.
- **Options:**
  - raise the pull for bags only (a per-slot `pull` in the wardrobe, default `kLayerPull`); a bag strap
    8–12 mm nearer is fine because nothing rests on a bag;
  - or make bags hide the jacket under their straps (posed coverage, as in 1a).
- **Check:** Outerwear/Bag and Top/Bag posed counts; run clip at 0.35 and 0.85 in shots.

### 1c. Heads into collars (pickup, arm flare)
- **Why:** the head bends forward into a high collar. The head is exposed skin, so `Backed` rightly keeps
  most of it.
- **Option:** it's mostly the chin/neck going behind the collar, which is fine as long as the collar draws
  on top. Check shots first; this may be acceptable. If not, give collars (layer 7) and hooded jackets a
  bigger pull on the head only.

### 1d. Bind-pose head under tops (211 pairs)
- Mostly the neck through the neck opening at the 10 cm look-out cap (`kPokeReach`). In the shots it's
  behind the collar. Low priority: confirm with `_headback` shots and move on, or teach the audit to ignore
  vertices whose look-out passes through an opening (no cloth within the cap).

### 1e. Tools that would help
- A per-pose CSV (`--outfit-posed` writes only the deepest pose per pair).
- `--outfit-shots` with a list of poses in one run, not one `TARTARUS_SHOT_POSE` per run.
- The posed check caps at 3 cm (`kPosedReach`), so anything deeper reads as 3 cm. Raise it to 6 cm once, to
  see the real tail.

## 2. Asset problems
### 2a. Camo pants look wrong (reported by you, not investigated yet)
- Texture or UVs. The camo materials on pants are:
  - `Materials/Clothing/Pants/Pants_Cargo/M_Pants_Cargo_Camo.mat`, `M_Pants_Cargo_CamoDark.mat`,
    `M_Pants_Cargo_PixelCamo.mat`;
  - `Materials/Clothing/Pants/F_Pants_Jeans/M_F_Pants_Jeans_Camo.mat`;
  - `Materials/Clothing/Pants/Shorts/M_Shorts_Camo.mat`, `Shorts_Breeches/M_Shorts_Breeches_Camo.mat`.
- **Steps:**
  1. Find which pants you saw (take a shot of each camo colourway: `--outfit-shots` on the Items scene with
     `TARTARUS_SHOT_NAMES`).
  2. Compare with the pack's own preview (the Quantum preset FBXs / the artist's screenshots).
  3. If a map is wrong: check the `.mat`'s `_AlbedoMap`/`_NormalMap` point at the camo set, not another
     colourway's normal or mask; check sRGB on the albedo and linear on the normal (`audit_quantum_assets.py`
     reports both).
  4. If the UVs are wrong: open the FBX in Blender and look at the UV layout vs the texture. Check whether the
     importer picked the wrong UV channel (some packs put a lightmap UV in channel 0).
  5. Check the 2K cap didn't blur a fine pixel-camo pattern: set that texture's `.meta` back to 4096 and
     compare.
- **Related oddity:** `Materials/Clothing/Shoes/Flip_Flops/` holds four `M_Shorts_Breeches_*` materials whose
  textures are under `Textures/Clothing/Shoes/Flip_Flops/T_Shorts_Breeches_*`. Either the pack names the
  flip-flop textures after the breeches, or the male flip-flops are remapped to the breeches set. Confirm
  what `SKM_Flip_Flops.fbx.meta`'s `materialRemap` points at and whether the flip-flops look right.

### 2b. Material remaps to confirm by eye
- `Models/Clothing/Male/Outerwear/SKM_Jacket_Classic_Tie.fbx`: the tie's materials (`Accessories/Tie`) are
  among the 40 orphaned materials. Check the tie isn't drawn with the jacket's material.
- Male `SKM_Flip_Flops.fbx`: see 2a.

### 2c. Orphans and warnings (from phase 1, still open)
- 40 materials no model or colourway reaches: alternate looks in sub-folders (Leather coats and jackets, Fur
  collar jeans jackets, Print jeans vest, ties; `Hat_Classic/M_Hat_Classic` is moot now hats are out). Either
  offer them (let colourways include one sub-folder level) or leave them.
- 6 textures no material uses: the Puffer_Jacket "Clear" set and the Sport_Pants "Lines" set. Make materials
  for them (more colourways) or delete them from the Drive zip.
- 33 FBX files carry embedded texture paths to the artist's drives (`D:/OneDrive/...`). Remaps override them,
  but every import logs a warning each (seen in every self-test run). Either strip the paths in the FBXs, or
  have the importer skip the warning when a `materialRemap` covers that slot.

### 2d. Sandbox scene still has gold aviators
- One character in `project/scenes/Sandbox.json` wears `SKM_Glasses_Aviator` with `M_Glasses_Aviator_Gold`
  (around line 24010). The item is hidden in the wardrobe, so it isn't offered, but it's still worn there.
- **Fix:** open Sandbox, select that character, equip the classic glasses (or none), save. Or edit the JSON.
  Don't regenerate Sandbox.

## 3. Phase 5: editor UI (`src/Editor/EditorLayer_Outfit.cpp`)
Partly done: #500 replaced the thumbnail cards with a text list and a 3D preview (which also removed the
dangling-thumbnail problem), and #501 replaced the "None" row with a Remove button. Left from the audit plan:
- **Per-frame cost in the Inspector:**
  - `Catalog::Find` is a linear search that allocates strings: build a lowercase-path hash map once in
    `LoadCatalog`;
  - clash checks run for every card every frame: cache per `outfit.Version`;
  - `ColourGroups` is recomputed every frame: cache per `outfit.Version`;
  - with the "..." menu open it scans the whole asset tree for presets: throttle like `ProjectModelFiles` in
    `EditorLayer_Animator.cpp`, and give each preset a unique ImGui id;
  - the item list draws every row: use `ImGuiListClipper`;
  - `ContainsI` → the shared `MatchesFilter`.
- **Consistency and style:**
  - "Auto Hide Skin" shows twice (the generic field from `ComponentRegistry.cpp` and the outfit panel's
    checkbox): hide the generic one;
  - identity row (gender, skin, dice) at the top;
  - gender pills, slot tabs and the lock via `ActionButton(active=…)`, and remove the button-styling
    allowlist entry;
  - `AlignToColumn` in place of `SameLine(60)`; no hard-coded pixel sizes;
  - tooltips where missing;
  - label the colourway rows;
  - errors in the danger colour, notes dismissable; remove the early return that hides notes, the checkbox
    and the warning;
  - compare the selected colourway case-insensitively;
  - key UI state by entity and world, and prune it;
  - merge rapid equips into one undo entry.
- **New:** a "Fit" field for rigid head wear (writes the wardrobe's item override), so a balaclava can be
  sized without editing JSON; and a Slot list that no longer mentions Hat/Headphones.
- **Check:** an `--inspector-shot` harness (planned in phase 1, not built) to screenshot the Inspector before
  and after.

## 4. Runtime performance leftovers
Done in phase 3: no per-frame maps, pending coverage checked every 8 frames, bounded coverage jobs, covered
triangles not drawn. Not done:
- **Pieces run their own animation.** Pieces follow the body's driver (phase 2), but each still evaluates its
  own pose. Reuse the driver's evaluated pose when the rigs match. This is the biggest CPU item left for
  crowds.
- **Per-frame vector copies in `AnimatorController`** (noted in the plan around lines 975–980 and
  1076–1085; re-find them, lines move).
- **`fs::exists` in `OutfitSystem.cpp` (`Prefetch`/`DressPiece` path, line ~74)** runs on every submit. Cache
  it per path until a rescan.
- **Profiler scopes inside `OutfitSystem`** (only the two outer scopes exist), so the bench can split
  hiding, attachments and pending work.
- **Heads are still 46% of triangles** (81–86k each, brows and lashes 20k, teeth 8k). The balaclava covers
  most of the face, and covered triangles are skipped, but a head LOD (or dropping teeth and inner mouth
  under a balaclava) would cut shadow-pass cost. There are no LODs in the engine yet.
- Items at 54 fps is the stress case (134 characters, about 3000 draws). Draw count is the CPU limit there:
  instancing identical pieces is the next step if crowds matter.
