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
outfit changes in Play.

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
  `Bobcut_Cap`). Those fitted cuts and `_Inboots` pants count as *variants*: the rules pick them, so they
  aren't offered in the lists.
- **Colourways** are the `.mat` files next to an item's remapped material.

## Skin hiding
With **Auto Hide Skin** on, each body part (and a top under outerwear) doesn't draw the vertices that
clothing covers, so skin can't poke through as the body moves:
- `OutfitCoverage` works out coverage once per (part, item) pair: in bind pose, each vertex looks 2 cm
  inward and 5 cm outward along its normal for the cloth. The covered area is then shrunk by one ring of
  vertices so hems don't open holes.
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

Female bodies play the male mocap clips as-is, without proportion retargeting.
