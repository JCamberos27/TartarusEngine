# Character Outfits

A **Character Outfit** component dresses a modular character (body, gender, race, hair, clothes,
accessories) from a *wardrobe*. Put it on the character's root, for example "Player Body" next to First
Person Body.

## Pieces
The outfit is made of the root's children that carry an **Outfit Piece** component:
- body parts: Torso, Arms, Legs, Feet, UnderPants, Head;
- items: Hair, Hat, Top, Pants, and so on.

Each piece is an ordinary model entity, so the scene saves it like any other. Its colourway is simply the
materials on its Mesh Renderer. The component itself holds only gender, race, Randomize locks and Auto Hide
Skin.

New pieces get a follower Animator Controller copied from the body's driver. First Person Body picks up
head-attached pieces (hair, hats, glasses, beards) as shadow-only, and it restarts its piece list when the
outfit changes in Play. In the player's own view it also trims clothing around the camera, for any pack:
what sits around the neck in the garment's bind pose (a hood, a collar, the shoulder tops), and whatever
comes within **Clothing Near Hide** of the eye (wider to the sides). See [BODY_SETUP.md](BODY_SETUP.md).

## Wardrobes (`*.wardrobe`)
A JSON file describing one character pack
([`Characters/Quantum/Quantum.wardrobe`](project/assets/Characters/Quantum/Quantum.wardrobe)). It holds
only what the folders can't say:

| Key | Meaning |
|---|---|
| `itemFolders`, `excludeFolders` | Folders scanned for items (every model found is offered) |
| `slots` | Slot id, the folder names that fill it, an optional file-name suffix (`_L`), and `headAttached` |
| `bodies.Male/Female` | Body `parts`, `alternates` (e.g. `ShoeFeet`), and `races` (head, skin suffix, swatch tone, optional part overrides) |
| `skinMaterials` | Base skin `.mat` names. Any slot using one gets the race's variant (`M_Quantum_Body` → `M_Quantum_Body_Afro`) |
| `covers` | Body parts an item hides or swaps (pants hide Legs; shoes swap Feet to ShoeFeet unless the shoe carries skin) |
| `clears` | Slots an item empties (a `WithHair` hat clears Hair; a jacket with its own shirt clears Top) |
| `pairs` | Name-based swaps (boots → the pants' `_Inboots` cut, and back) |
| `items` | Per-item overrides: `hidden`, `slot`, `name`, `gender` |

Rules that need no configuration:
- **Gender** comes from a `Female` folder or an `SKM_F_` prefix.
- **Hat-fitted haircuts:** with a hat on, the hair swaps to the cut named after it (`Bobcut` + `Cap` →
  `Bobcut_Cap`). In the Quantum pack those cuts carry the hat in the mesh, so the separate hat comes off.
  Fitted cuts and `_Inboots` pants count as *variants*: the rules pick them, so they aren't offered in the
  lists.
- **Colourways** are the `.mat` files next to an item's remapped material.

## What goes together
The pack's own 60 preset characters are the reference: they were rebuilt as outfit presets
(`assets/Characters/Outfits/Quantum/*.outfit`, by `tools/quantum/import_quantum_presets.py`, which reads the
pack's preset FBXs for the item names - nothing is copied), and every rule below lets all 60 through.

| Key | What it does |
|---|---|
| `tags` | Tags items by slot, name (`nameHasAny`/`nameLacksAll`), material (`materialHasAny`) or earlier tags (`tags`/`lacksTags`), e.g. every jacket with a shirt material is `BuiltInTop`. |
| `clears` | An item empties another slot (`tags` or name/path): a jacket with its own top, a hat with its own hair, a hood that's up. |
| `excludes` | Two items that don't go together (`a`, `b` are matches); asking for both takes `b` off. `"soft": true` only keeps them apart in Randomize and warns in the Inspector ("Odd pairing"). |
| `styles` | Casual, Sport, Formal (male), Winter, Summer: a weight, and each slot's chance to be filled. Items tagged with a style's name only appear in that style; untagged ones (hair, glasses...) in any. |
| `randomOrder` | The order Randomize fills slots in (jackets before tops, shoes before pants). |

In the Quantum pack:
- **Jackets and tops.** Jackets with a top in the mesh (Leather Jacket, Jeans Jacket, Bombers, Coats with a
  shirt, Winter Open...) and closed ones (Puffer, Winter Closed, Jacket Classic) take the Top off. Open
  jackets (M65, female Bomber) keep a thin top but not a hoodie. Vests go over anything.
- **Heads.** A hood that's up takes the hair off and allows only small hats (caps, bandanas, beanies).
  Haircuts with a hat, headband or glasses built in take those slots off. Big hats leave no room for
  headphones.
- **Feet.** Socks (their own slot) go under sneakers and boots, not flip-flops or classic shoes. Plain boots
  take the `_Inboots` pants; `Boots_Inboots` go under plain pants.
- **Style clashes** (soft): suit jackets or trousers with shorts, sport pants, flip-flops or sport sneakers;
  winter jackets and hats with shorts or flip-flops.

`Wardrobe::Randomize` picks a style, then fills slots in `randomOrder`, each by the style's chance, from items
of that style that aren't variants and don't conflict with anything already chosen. The Inspector marks
item cards that clash with what's worn (amber: one comes off; grey: an odd pairing).

## Checking it
- `TartarusEngine --outfit-rules`: every preset goes through the rules unchanged, and 5000 Randomize outfits
  per gender are whole (top, pants, shoes) with nothing taken off and no clash. `--outfit-audit` adds the
  clipping check below over every pair the rules allow.
- `TartarusEngine --gen-outfit-scenes` rebuilds `scenes/OutfitTest/`: **Presets** (the artist's 60),
  **Items** (one character per item on a plain outfit, a row per slot) and **Randomized** (eight per style
  and gender). Select any character and use the Inspector to change or re-roll it.

## Skin hiding
With **Auto Hide Skin** on, each piece doesn't draw the vertices that poke through the layers worn over it,
so the body, the head, a shirt under a jacket or hair under a hood can't clip through as the character moves:
- Every slot has a `layer` (body parts and the head are 0): shoes 2, pants 3, tops 4, outerwear 6, collars 7,
  bags and wrists 8, hair/beards/glasses 9, hats 10. A piece hides what pokes through it from every lower layer.
- `"hides": false` on a slot (hair, beards, glasses) means its items never hide anything - they're see-through
  cards, and hiding the scalp under them would open holes.
- `"layers"` rules change that per item: `{"slot": "Top", "nameHasAny": ["Tucked"], "layer": 2}` puts tucked
  tops under the pants, boots go over them, and `"over": ["Hair"]` lets a hood that's up hide the hair.
- `OutfitCoverage` works out coverage once per (part, item) pair, in bind pose: each vertex looks 3 cm
  inward and 5 cm outward along its normal for the cloth, and that covered area is shrunk by one ring of
  vertices so hems don't open holes. Anything sitting outside the cloth (up to 10 cm, with the cloth
  right behind it and facing the same way) is hidden too.
- Coverage is stored as a per-vertex bit buffer (`OutfitHideTag` / `SkinHideBuffer`, SSBO binding 20), which
  `ModelVertex.glsl` reads.
- `OutfitSystem::UpdateHiding` runs every frame but only does work when an outfit's pieces change (an edit,
  an undo, or loading a scene).

## Code
- [`src/Game/Wardrobe.h`](src/Game/Wardrobe.h): parsing, classification and rule resolution (pure, unit tested).
- [`src/Game/OutfitSystem.h`](src/Game/OutfitSystem.h): the catalog scan, plus `Apply`, `Equip`, `SetGender`,
  `SetRace`, `Randomize`, colourways, `AdoptExisting`, presets and `UpdateHiding`. This is also the API for an
  in-game character creator.
- [`src/Game/OutfitCoverage.h`](src/Game/OutfitCoverage.h): coverage geometry (pure, unit tested).
- [`src/Editor/EditorLayer_Outfit.cpp`](src/Editor/EditorLayer_Outfit.cpp): the Inspector editor.
- [`src/Game/OutfitAudit.h`](src/Game/OutfitAudit.h), [`src/Game/OutfitTestScene.h`](src/Game/OutfitTestScene.h):
  `--outfit-audit` / `--outfit-rules` and `--gen-outfit-scenes`.

Female bodies play the male mocap clips as-is, without proportion retargeting.
