# Project asset organization

Third-party payloads remain local or in the private asset library. Git tracks their metadata,
materials, controllers and authored settings. Follow the root [README](../README.md) to fetch
the payloads and maintain `project/external_assets.csv`; never commit the models, textures,
animation files or recordings themselves.

The Asset Browser's `Assets` root represents `project/`. The lowercase `assets`
folder contains the authored content. Each asset is listed in its own folder;
the old virtual `Animation` collection has been retired because it displayed
the same files in a second location without their extensions.

## Weapons

`project/assets/Weapons/AKS74U/`, `project/assets/Weapons/Remington870/` and
`project/assets/Weapons/Revolver38/` ([.38 Special](WEAPON_REVOLVER38.md))
each contain the complete weapon setup:

| Extension / folder | Purpose |
|---|---|
| `.prefab` | Instantiable weapon model with its Weapon Definition component and Animator Controller |
| `.fpsanim` | First-person presentation settings, animation clips and recoil/shake references; gameplay data lives in the prefab's Weapon Definition |
| `.controller` | Animator state graph and transitions |
| `.recoil` | Recoil profile |
| `.camerashake` | Camera shake profile |
| `FirstPerson/` | First-person arm animation clips |
| `ThirdPerson/` | Third-person arm clips (`tools/weapons/author_3p.py`) |
| `Weapon/` | Weapon model and animation clips |
| `Materials/`, `Textures/` | Rendering assets |

The AKS74U and Remington870 assets previously shown under `Animation` are these
same files. The cleanup does not replace or regenerate any weapon settings,
graphs, clips, import settings, or prefab identities. File extensions now remain
visible so several assets named AKS74U are distinguishable.

Body controllers live under `assets/Animations/Controllers`, motion capture under
`assets/Animations/Mocap`, and NPC controllers under `assets/AI`. Engine, editor
and gameplay code is under `src/` (see [Gameplay code](GAMEPLAY_CODE.md)). Scenes, screenshots, and custom shaders
remain under `project/scenes`, `project/screenshots`, and `project/shaders`.

## Duplicate audit and consolidation

The Asset Browser already offers **Find References in Scene** and **Find References in Project**.
Project lookup scans supported files for the asset GUID or path and lists matching files in
Console; deletion from disk also shows reference warnings. This is not a complete typed
dependency graph.

The cleanup compared SHA-256 content hashes throughout `project`, excluding
generated `Library`, `bin`, `obj`, and asset metadata. No weapon asset duplicates
were found. Matching payloads with matching import settings were consolidated:

| Removed duplicate | Retained asset |
|---|---|
| `assets/Audio/Casings/shell/dirt_6.wav` | `assets/Audio/Casings/rifle/dirt_1.wav` |
| Female Afro body occlusion texture | Female European body occlusion texture |
| Female Afro arms occlusion texture | Female European arms occlusion texture |
| `screenshots/Sandbox_20261004_224845.png` | `screenshots/Sandbox_20261004_224853.png` |

Affected scene paths, material texture GUIDs, and the audio manifest were
updated. The rifle and shell sound banks retain their entries, keys, and
playback settings, sharing one identical sample. Both skin variants retain
their materials and share their identical occlusion maps. The retained image
is the later of the two identical screenshots.

The female and male Crime balaclava FBX files also have identical geometry but
remain separate **logical wardrobe assets**: the wardrobe classifies their
gender from their paths, and the female asset has an authored `fit: 1.06`
override. Removing one would change outfit selection and fit. Their identities
and settings are preserved.

Original removed files and edited reference files are backed up under
`build/project-cleanup-backup/`. Machine-readable audit and change manifests
are `build/project-duplicate-audit.json` and `build/project-cleanup-result.json`.
The backup is outside the project content and is not exported with the game.

## Asset Library

The Asset Browser's **Asset Library** root lists every placeable thing, sorted by kind. It covers prefabs, decals and HDRIs. Each category is split by pack, for example *Asset Library / Architecture / Walls / ModularCityAlleyPack*. Files never move to make this view; it is computed on every refresh from `project/asset_library.json`.

The JSON has three parts:

- **`categories`** has the shape `{id, name, parent?}`. A category without `parent` is a top-level folder.
- **`rules`** has the shape `{glob, category}`. Rules are tried in order and the first match wins. Each glob is matched case-insensitively against the project-relative path (`assets/...`), and `*` also crosses `/` (Enhancers `GlobMatch`). A path no rule matches goes to `other`.
- **`kits`** has the shape `{"<pack folder>": module metres}`. It lists the modular packs whose pieces snap together.

The Library root is a view, so it refuses folder creation, renames and moving assets into it. Drag from it into the Scene view as from any other folder.

To place items, drag a model or prefab into the Scene view:

- A tinted ghost shows the exact landing pose.
- The lowest point of the model or prefab rests on the surface under the cursor.
- With **Snap to surface** and **Align to surface normal** both on (grid-snap popover), the dropped object also turns its up axis to the face it lands on.

With **Snap kit pieces together** on (the default; hold Shift to invert), kit pieces behave as follows, whether moved with the Move gizmo or dropped:

- A piece's box comes flush against neighbouring pieces within a quarter of the module. That covers side-by-side faces and lined-up fronts.
- Axes with nothing near snap to the module grid. Height is kept.
- Rotation turns in quarter turns.
- A white outline marks the face the piece is flush against.

The code is in `src/Editor/ModularSnap.h`, with its tests in `UnitTests_AssetLibrary.cpp`.

A file listed in `external_assets.csv` that this checkout hasn't fetched shows as a greyed tile where it would be. Dragging it says to run `tools/assets/fetch-assets.ps1`.

`tools/assets/make_model_prefabs.py` gives every model in Furniture, Architecture, Props and Environments/Apartment a prefab beside it:

- Colliders are a box fitted to the bounds. Architecture gets a triangle-mesh collider, except for doors.
- Characters are skipped, because their parts are wardrobe pieces used through `.outfit` files.
- Run it with `--dry` to see counts. It skips prefabs that already exist unless given `--force`.
- It reads bounds through `TartarusEngine.exe --model-report` in a checkout that has the payloads.
