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

`project/assets/Weapons/AKS74U/` and `project/assets/Weapons/Remington870/`
each contain the complete weapon setup:

| Extension / folder | Purpose |
|---|---|
| `.prefab` | Instantiable weapon model with the project C# WeaponDefinition script and native Animator Controller |
| `.fpsanim` | First-person presentation settings, animation clips and recoil/shake references; gameplay data lives in the prefab's C# WeaponDefinition |
| `.controller` | Animator state graph and transitions |
| `.recoil` | Recoil profile |
| `.camerashake` | Camera shake profile |
| `FirstPerson/` | First-person arm animation clips |
| `Weapon/` | Weapon model and animation clips |
| `Materials/`, `Textures/` | Rendering assets |

The AKS74U and Remington870 assets previously shown under `Animation` are these
same files. The cleanup does not replace or regenerate any weapon settings,
graphs, clips, import settings, or prefab identities. File extensions now remain
visible so several assets named AKS74U are distinguishable.

Body controllers live under `assets/Animations/Controllers`, motion capture under
`assets/Animations/Mocap`, and NPC controllers under `assets/AI`. Gameplay C# is
under `assets/Scripts`; editor C# is under `assets/Editor`; native engine and
editor implementation is under `src/`. Scenes, screenshots, and custom shaders
remain under `project/scenes`, `project/screenshots`, and `project/shaders`.

## Duplicate audit and consolidation

The Asset Browser already offers **Find References in Scene** and **Find References in Project**.
Project lookup scans supported files for the asset GUID or path and lists matching files in
Console; deletion from disk also shows reference warnings. This is not a complete typed
dependency graph. An indexed dependency browser is proposed in [EDITOR_UPGRADES.md](EDITOR_UPGRADES.md).

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
