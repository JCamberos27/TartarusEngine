# Character Outfits

A **Character Outfit** component dresses a modular character (body, gender, race, hair, clothes,
accessories) from a *wardrobe*. Put it on the character's root, for example "Player Body" next to First
Person Body.

## Pieces
The outfit is made of the root's children that carry an **Outfit Piece** component:
- body parts: Torso, Arms, Legs, Feet, UnderPants, Head;
- items: Balaclava, Glasses, Top, Pants, and so on.

Each piece is an ordinary model entity, so the scene saves it like any other. Its colourway is simply the
materials on its Mesh Renderer. The component itself holds only gender, race, Randomize locks and Auto Hide
Skin.

Pieces with an Animator Controller follow the body's driver (the root's controller, else the first piece's):
one state machine per character, so parameters set on the driver reach every piece and they stay in step.
New pieces get a follower copied from it; a loaded scene is linked again on its first frame. First Person Body picks up
head-attached pieces (balaclavas, glasses, and hair, hats or beards in a pack with them) as shadow-only, and it restarts its piece list when the
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
| `items` | Per-item overrides: `hidden`, `slot`, `name`, `gender`, and `fit` (rigid head wear drawn that much bigger about its own centre, 0.5–2) |

Rules that need no configuration:
- **Gender** comes from a `Female` folder or an `SKM_F_` prefix.
- **Hat-fitted haircuts:** with a hat on, the hair swaps to the cut named after it (`Bobcut` + `Cap` →
  `Bobcut_Cap`), for a wardrobe with Hair and Hat slots. The Quantum wardrobe has no hair (balaclavas stand
  in for it), so there this rule never fires. Fitted cuts and `_Inboots` pants count as *variants*: the
  rules pick them, so they aren't offered in the lists.
- **Colourways** are the `.mat` files in the same folder as an item's remapped material, when there are at
  least two. Materials under `Materials/Characters` (skin, eyes, teeth) and `Materials/Clothing/Generated`
  are nobody's colourway. A material in a sub-folder of the item's (`Hat_Classic/Beach`, `Coat/Leather`) is
  a separate set, not offered next to the default one.
- **Switching gender** swaps each item for the other gender's cut with the same name (`SKM_Hoodie` ↔
  `SKM_F_Hoodie`), else the most alike name in the slot; Top, Pants and Shoes always get one.

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
- **Heads.** Every style puts a balaclava on (on purpose). Hats and headphones are out of the pack: the Hat
  and Headphones slots are gone, `Hats` is in `excludeFolders`, and the aviators are hidden, so the only
  head accessories are the balaclavas, hoods, and the black classic glasses. A hood that's up goes over
  the balaclava. The female balaclava is modelled a little small for the female heads (the back of the head
  poked out), so its item override has `"fit": 1.06`.
- **Feet.** Socks (their own slot, filled by three item overrides - the Socks slot has no folder) go under
  every shoe but flip-flops and `Boots_Socks` (socks built in). Plain boots take the `_Inboots` pants;
  `Boots_Inboots` go under plain pants.
- **Style clashes** (soft): suit jackets or trousers with shorts, sport pants, flip-flops or sport sneakers;
  winter jackets with shorts or flip-flops.

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
- `TartarusEngine --outfit-selftest` drives the `OutfitSystem` API end to end on the real wardrobe (Apply,
  Submit, Equip, SetGender/SetRace, Randomize with locks, colourways, presets, AdoptExisting, hiding,
  CancelPending) and checks each result. Exit code 0 when every check passes.
- `TartarusEngine --outfit-cost [scene.json ...]` prints what the wardrobe's models cost to draw (triangles,
  vertices, sub-meshes and bones per model, by slot) and, per scene (default: the three OutfitTest scenes),
  pieces, draws, triangles and how much of that skin hiding throws away.
- `TartarusEngine --outfit-shots <dir> [scene.json|dir]` renders the smoke test with the Scene view framed on
  the characters: the first row, then picked characters full length front and back and their heads close up.
  `TARTARUS_SHOT_NAMES="Quantum_Male_01,Quantum_Female_11"` picks them by name. Each picked character also
  gets a `_headback` shot (behind and above the head, for balaclavas and hoods).
  `TARTARUS_SHOT_POSE="<clip>@<fraction>"` (for example
  `assets/Animations/Mocap/Locomotion_V2/AM_Crouch_Walk/AM_Crouch_Loco_Walk_Fwd.fbx@0.35`) holds every skinned piece in that pose, to see clipping in
  motion.
- `--outfit-audit ... --outfit-posed` also skins every pair into 32 poses (idle, walk, run, crouch walk,
  crouch, jump, pickup and arm flare, each at 10/35/60/85%) and reports what pokes through in any of them,
  with the deepest pose (`posed`, `posed_depth_cm`, `posed_where` in the CSV). It takes about a minute more.
- `python tools/quantum/audit_quantum_assets.py [--csv out.csv] [--clothing-max N] [--body-max N]` checks the
  files: every FBX material has a remap to a `.mat` that exists, every texture a material names exists, colour
  maps are sRGB and data maps linear, and nothing is orphaned; and totals VRAM by folder and model.

## Skin hiding
With **Auto Hide Skin** on, each piece doesn't draw the vertices that poke through the layers worn over it,
so the body, the head, a shirt under a jacket or hair under a hood can't clip through as the character moves:
- Every slot has a `layer` (body parts and the head are 0): balaclavas 1 (their neck skirt tucks under
  every top and jacket, so collars lie over it), shoes 2, pants 3, tops 4, outerwear 6, collars 7,
  bags and wrists 8, glasses (and hair/beards in a wardrobe with them) 9. A piece hides what pokes through
  it from every lower layer.
- `"hides": false` on a slot (hair, beards, glasses, wrists) means its items never hide anything - they're see-through
  cards (or a watch strap / bead bracelet with gaps), and hiding the skin under them would open holes.
- `"layers"` rules change that per item: `{"slot": "Top", "nameHasAny": ["Tucked"], "layer": 2}` puts tucked
  tops under the pants, boots go over them, and `"over": ["Hair"]` lets a hood that's up hide the hair.
- `OutfitCoverage` works out coverage once per (part, item) pair, in bind pose: each vertex looks 3 cm
  inward and 5 cm outward along its normal for the cloth. The covered area is shrunk by one ring of
  vertices, then (on the body, not the head) by a 4 cm band of skin that's under the cloth (`kEdgeBand`):
  a loose sleeve or collar swings further off the skin than one ring once the character moves. Anything
  sitting outside the cloth (up to 10 cm, with the cloth right behind it and facing the same way) is hidden
  too - unless, 5 cm or more in, the look back reaches the body's own far side within 2 cm
  (`kFarWallDepth` / `kFarWallSlack`): that's cloth sunk into the far side of a limb, not skin outside it.
- The head keeps skin that would leave a hole if hidden (`Backed`). How far it looks back for the cloth
  follows how deep the vertex pokes out (a bun 9 cm out through a balaclava has the cloth 9 cm behind it). Headwear on the head bone (a balaclava,
  a hat under a hood) only hides what pokes out through the cloth over it: it can't deform out from under
  it, and its covered rest can be in view (a balaclava's sides through a hood's face opening).
- `OutfitEdgeBandPosed` and `OutfitRigidUnderHood` check these on the real Quantum meshes; `--outfit-audit`
  reports every pair's poke-through, to compare before and after a change.
- **Layer pull.** What hiding keeps can still graze the layer over it by a few millimetres once the
  character moves. Each piece is ranked by how many layers are worn over it (the longest chain of
  `Wardrobe::Hides`, at most `kMaxLayerRank` = 3), and `ModelVertex.glsl` draws it `kLayerPull` (4 mm) per
  rank nearer the camera (`OutfitLayerTag`, uniform `uLayerPull`). Vertices slide along the view ray, so
  nothing moves on screen and early-z still works; only depth changes. Deeper pokes (a sneaker through a
  pant leg in a crouch) are too far for it: see [OUTFIT_TODO.md](OUTFIT_TODO.md).
- A fitted piece's fit is part of its coverage key, and the coverage version (`kCoverageVersion`, 17) is
  bumped whenever coverage changes. The first run after a bump draws unhidden pieces until the background
  jobs finish.
- Coverage is stored as a per-vertex bit buffer (`OutfitHideTag` / `SkinHideBuffer`, SSBO binding 20), which
  `ModelVertex.glsl` reads.
- `OutfitSystem::UpdateHiding` runs every frame but only does work when an outfit's pieces change (an edit,
  an undo, or loading a scene).

## Code
- [`src/Game/Wardrobe.h`](src/Game/Wardrobe.h): parsing, classification and rule resolution (pure, unit tested).
- [`src/Game/OutfitSystem.h`](src/Game/OutfitSystem.h): the catalog scan, plus `Apply`, `Equip`, `SetGender`,
  `SetRace`, `Randomize`, colourways, `AdoptExisting`, presets, `UpdateHiding` and `UpdateAttachments`, and the
  background path the editor uses (`Submit`, `UpdatePending`, `IsPending`, `CancelPending`). This is also the
  API for an in-game character creator. The Inspector saves presets to `assets/Characters/Outfits/<name>.outfit`;
  the artist's are in `Outfits/Quantum/`.
- [`src/Game/OutfitCoverage.h`](src/Game/OutfitCoverage.h): coverage geometry (pure, unit tested).
- [`src/Editor/EditorLayer_Outfit.cpp`](src/Editor/EditorLayer_Outfit.cpp): the Inspector editor.
- [`src/Game/OutfitAudit.h`](src/Game/OutfitAudit.h), [`src/Game/OutfitTestScene.h`](src/Game/OutfitTestScene.h):
  `--outfit-audit` / `--outfit-rules` / `--outfit-selftest` / `--outfit-cost` and `--gen-outfit-scenes`.

Female bodies play the male mocap clips as-is, without proportion retargeting.
