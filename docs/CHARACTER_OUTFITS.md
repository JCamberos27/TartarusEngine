# Character outfits

Modular characters dressed from a pack's wardrobe: a body (parts + a race's head and skin) and one item per
slot (hair, a top, pants, a hat ...), with rules for what goes together, what hides what, and colourways.
The Quantum pack (`project/assets/Characters/Quantum`, the complete Quantum - Modular Casual Bundle) is the
one wardrobe in the project.

| Piece | Where |
|---|---|
| Wardrobe file format, parsing and every rule (pure, unit tested) | `src/Game/Wardrobe.h/.cpp` |
| Catalog scan, building pieces, colourways, presets, skin hiding, head wear | `src/Game/OutfitSystem.h/.cpp` |
| Which body vertices a garment covers (geometry only) | `src/Game/OutfitCoverage.h/.cpp` |
| Components | `CharacterOutfitComponent`, `OutfitPieceComponent`, `OutfitHideTag`, `OutfitLayerTag` (`src/Game/Components.h`) |
| Inspector outfit editor | `src/Editor/EditorLayer_Outfit.cpp` |
| First-person body (what the player's camera may see) | `src/Game/FirstPersonBody.cpp` |
| The Quantum wardrobe | `project/assets/Characters/Quantum/Quantum.wardrobe` |
| Presets | `project/assets/Characters/Outfits/Quantum/*.outfit` |
| Tests | `TestWardrobe`, `TestWardrobeQuantum`, `TestQuantumMaterialTextures`, `TestOutfitCoverage`, `TestOutfitRigidUnderHood`, `TestOutfitEdgeBandPosed` (`src/Tests/UnitTests.cpp`) |

## How a character is dressed

1. A **Character Outfit** component on the body's root names the `.wardrobe`, the gender, the race and the
   slots Randomize must leave alone (Locks). Its children tagged **Outfit Piece** are the outfit: each holds
   its slot (`Torso`, `Head`, `Hair`, `Top` ...), the model it was built from and flags (body part,
   head-attached). The pieces are the record of what is worn, colourways included (their materials).
2. `OutfitSystem::LoadCatalog` reads the wardrobe and scans its item folders (`Models/Clothing` for Quantum)
   for models. Each model is classified by `Wardrobe::Classify`: its **slot** from the nearest folder a slot
   lists (plus an optional file-name suffix, `_L`/`_R` for the wrists), its **gender** from a `Female`
   folder or the `SKM_F_` prefix, whether it **carries skin** (a skin material among its remapped
   materials), and its **tags** from the tag rules. A new model dropped into a slot folder shows up with no
   wardrobe edit. `Wardrobe::MarkVariants` then marks the cuts the rules pick (a haircut fitted to a hat,
   pants cut for boots): the editor hides them unless "Show fitted cuts" is on and Randomize never picks them.
3. A change is a **request** (gender, race, slot -> item). `Wardrobe::Resolve` applies the rules (below) and
   returns the pieces to build plus notes; `OutfitSystem::Submit` loads what they need in the background
   and applies it in one frame (`Apply` does it on the spot). The race's skin goes on every skin material
   (`Wardrobe::SkinMaterialFor`: `M_Quantum_Body` -> `M_Quantum_Body_Afro` when that file exists).
4. Every frame: `UpdatePending` lands waiting changes, `UpdateAttachments` moves rigid head wear (static
   meshes in head-attached slots) onto the head bone, and `UpdateHiding` works out which skin and under-layer
   vertices are covered (`OutfitHideTag`, cached per model pair in `Library/OutfitCoverage`) and how far each
   layer is pulled toward the camera (`OutfitLayerTag`).

Colourways are the sibling `.mat` files in the folder of a piece's remapped material, all on one mesh
(`Wardrobe::Colourways`, the Inspector's swatches); skin, eye and teeth materials have none.

## The .wardrobe file

JSON. Paths are under `root` unless they start with `assets/`. Rules match items by **substrings of the file
name** (case-insensitive), by remapped **material names**, and by **tags** other rules gave.

| Key | What it says |
|---|---|
| `itemFolders`, `excludeFolders` | Where items are scanned; folder names that hide a model (`StaticMesh`) |
| `skinMaterials` | Base skin material names; a race's `skin` suffix makes its variant |
| `hatSlot`, `hairSlot` | The slots the hat-takes-its-fitted-haircut rule works between (defaults `Hat` / `Hair`) |
| `slots` | `id`, `label`, `folders`, `nameSuffix`, `headAttached` (rides the head: shadow-only in first person), `layer` (how far out it's worn), `hides` (false for see-through items) |
| `bodies.Male/Female` | `parts` (Torso, Arms, Legs, Feet, UnderPants), `alternates` (ShoeFeet, SleeveHands), `races` (`head`, `skin` suffix, `tone`, own `parts`/`alternates`, `random`, skeleton grip settings) |
| `covers` | A body part hidden or swapped for an alternate while an item is worn (pants hide the legs, shoes swap the feet, long sleeves swap the arms for the hands) |
| `tags` | Tags put on matching items, in order (styles like `Winter`, kinds like `BuiltInTop`, `HoodUp`) |
| `clears` | An item that empties another slot (a jacket with its own shirt; a hat with its own hair) |
| `excludes` | Two items that don't go together: `b` comes off. `soft` ones (a style clash) only keep Randomize away |
| `layers` | A slot's layer changed for some items, or `over`: slots a piece hides whatever their layer (a raised hood over the balaclava and the hat) |
| `pairs` | Suffix swaps driven by another slot (boots -> the pants' `_Inboots` cut) |
| `randomOrder`, `styles` | Randomize: the order slots are filled in, and per style how likely each slot is filled |
| `items` | Per-model overrides: `hidden`, `slot`, `name`, `gender`, `fit` (rigid head wear drawn bigger) |

### Resolve order

Pairs, then the hat and the hair, then clears, then excludes, then the body (race parts and head), then covers.
Hat and hair: a hat asked for with a haircut takes the cut the pack fitted to it (`<cut>_<hat>`, compared without
the `SKM_`/`SKM_F_`/`SM_`/`SM_F_` prefix, so a static hat finds a skinned cut). When that cut carries the hat
itself (a hard exclude between the cut and the hat says so) the hat piece goes. With no hat asked for, a fitted
cut goes back to the plain one - unless it carries its hat: then it stays (that's how the outfit reads back from
its own pieces).

## Layers and hiding

Each piece has a layer (body parts 0, shoes 2, pants 3, tops 4, outerwear 6, bags and wrists 8, hair, beards
and glasses 9, hats 10, headphones 11; tucked tops 2, boots 5). A piece that `hides` hides what of a lower
layer pokes out through it: skin under clothes, a shirt under a jacket, hair under a hat. Head-attached items
(not body parts) are hidden only where they poke out (`OutfitCoverage::Hidden`'s `rigid`): hair under a cap
keeps everything below the brim. Hair, beards, glasses and wrist items hide nothing.

## First person

Every head-attached piece - hair, facial hair, headwear, headphones, the balaclava, glasses - is left out of the
player camera's own view and kept in its shadow and in every other view (`FirstPersonBody`, `PlayerBodyTag::
CameraHidden`), like the head. Clothing near the eye (a hood, the winter jacket's collar) is hidden by bones
(neck and up), by the collar mask and by Clothing Near Hide.

## The Quantum wardrobe

Slots: Hair, Facial hair (male only), Headwear, Headphones, Balaclava, Glasses, Tops, Outerwear, Pants, Shoes,
Socks, Bags, Left/Right wrist. Hair, facial hair, headwear and headphones live in `Models/Clothing/<Male|Female>/
Hair`, `FacialHair` and `Headwear` (headphones are Headwear models moved to their own slot by item overrides);
their materials and textures in `Materials|Textures/Hair`, `.../FacialHair` and `.../Clothing/Headwear`.

Rules for the head:

- **Hats and hair.** The female cuts made for a hat (`SKM_F_Haircut_Bobcut_Cap`, `_Hat_Warm`, `_Panama`,
  `SKM_F_Haircut_Ponytail_Cap`) are picked for Bobcut/Ponytail + that hat and carry it (tag `HatBuiltIn`).
  Cuts with a hat, headband or glasses of their own (`Ponytail_Cap_Glasses`, `Ponytail_Headband_Up`,
  `Headband_Haircut_Braids`) take the Headwear/Glasses pieces off. Without a fitted cut the hair stays and the hat
  hides what pokes through. Headwear with hair of its own (`SKM_Cap_Hair`, `SKM_Cap_Back_Hair`,
  `SKM_F_Hat_Warm_Braids`, `SKM_F_Headband_Haircut_Half_Up_Bandana`, `SM_F_Cap_02` with its scalp) clears the
  hair. A mohawk or irokez doesn't go under a hat (headbands, bandanas and goggles excepted).
- **Head_Scalp** (named "Shaved") is a hair item of its own: the pack's shaved head. The short cuts that need a
  scalp (`Pixie_Cut_Long`, `Undercut_Bun`) carry it.
- **Hoods.** A hood that's up (`Hoodie_Hood_Up`, `Hoodie_Zipper_Hood`, `Coat_Hoodie_Hood`, both winter
  jackets) clears the hair, takes headphones and hats off unless they fit under it (`UnderHood`: caps, bandanas,
  headbands, warm hats, berets), and is layered over the balaclava and the headwear.
- **The balaclava** takes hair, facial hair, headwear, headphones and the aviators off (the classic glasses stay).
  The everyday styles fill it at 0.05; the **Masked** style (weight 0.4, every Casual or Winter piece) always
  wears it.
- **Hair and beards** carry no style tags, so they go in every style, except the loud ones: mohawks, irokez,
  braids and the long beards are tagged with every style but Formal.
- Men's hats have no fitted cuts (only the female Bobcut/Ponytail ones), so a man's hat hides the hair that pokes
  through. `SKM_Hat_Classic_Hairs` is a hat only (no hair mesh), and `SKM_Bobcut` is the pack's own male cut.
- **Static (SM_) headwear** with a skinned twin (`SM_Cap`, `SM_Cap_Back`, `SM_Panama`, `SM_F_Cap`,
  `SM_F_Cap_Back`) is offered as "(static)": the same colourways, riding the head bone.
- Hidden on purpose: the body's own shoe feet and wrists, and
  `SM_F_Haircut_Scalp` (the pack gives it no material).

Other rules: backpacks and bags (layer 8) go over the outerwear (6); a suit jacket (`Jacket_Classic_*`) brings its
own shirt and tie and clears the top; `Shirt_Tie` is a formal, tucked, long-sleeved top (the arms become the
sleeve hands), a soft clash with shorts and flip-flops; rolled-up sleeves keep the arms. The male long sleeves
(hoodies, the classic and tie shirts, the bombers, coats and full jackets, `Jacket_Tshirt` included) swap the arms for
the sleeve hands; vests, jerseys, T-shirts, the Hawaii shirt and the rolled-ups keep them. The female body has no
sleeve hands, so she keeps her full arms under every sleeve.

Races: male European, Afro, Asian, Old, Young, plus Tattooed (`_Tatto` arm skin), Ornaments (`_Ornament`) and the
Realistic Skeleton; female European, Afro, Old, plus Make-up (its own head) and Tattoo L / R / L+R. Afro brings its
own torso, legs, feet, underpants and shoe feet, Old its torso, feet, underpants and shoe feet (its legs are the
European ones in the Old skin: the pack has no Old legs of their own). The
tattoo, ornament and make-up races are `random: false`, so Randomize's race draw is what it was. The pack's
`_Afro` clothing differs from the base only in its skin slot, so one mesh serves every race: the race's skin
suffix does the rest.

## Tools

- `TartarusEngine.exe --unit-tests Wardrobe` runs the wardrobe tests (`WardrobeQuantum` checks that every
  clothing model lands in a slot or is hidden on purpose); `--unit-tests QuantumMaterialTextures` checks that
  every texture a Quantum material names exists. Both skip when the pack isn't fetched.
- `--model-report`, `--model-nodes`: a model as the engine imports it.
- `tools/quantum/import_casual_bundle.py`: the Casual Bundle importer (`docs/QUANTUM_SWAP.md`).
