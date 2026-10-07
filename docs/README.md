# Tartarus documentation

## External assets

The repository [README](../README.md) is authoritative for asset setup and Git policy.
Licensed third-party models, animations, textures and audio stay in the private asset library;
only their `.meta` sidecars, authored settings and `project/external_assets.csv` belong in Git.
This also applies to re-exported weapon FBXs and sounds derived from third-party recordings.
Run `python tools/assets/check_git_assets.py` before committing or pushing.

## C# gameplay and editor scripting

| Document | Use it for |
| --- | --- |
| [Scripting manual](SCRIPTING_MANUAL.md) | Start here: setup, tutorials, player/weapon customization, prefab workflow and troubleshooting. |
| [Complete API reference](SCRIPTING_API.md) | Implemented classes, methods, field types, lifecycle, native data access and limitations. |
| [Gameplay frame reference](SCRIPTING_FRAMES.md) | Every PlayerFrame, WeaponFrame and ShotFrame member, operation and flag. |
| [Implementation overview](CSHARP_SCRIPTING.md) | Native/managed ownership, build integration, AK/870 migration and research sources. |

The tutorials target the APIs implemented in this repository. The editor is C++; C# provides
gameplay and editor extensions. Editor tools and gameplay compile independently. See each
guide's boundaries before assuming Unity package or serialization compatibility.

## Related engine systems

* [Project assets](PROJECT_ASSETS.md): folder layout, weapon files, and duplicate consolidation.
* [Weapon reimport](WEAPON_REIMPORT.md): repeating the AKS74U and Remington 870 authored-export import.
* [Procedural recoil](PROCEDURAL_RECOIL.md), [weapon camera](WEAPON_CAMERA.md),
  [weapon sway](WEAPON_SWAY.md), [curve editor](CURVE_EDITOR.md).
* [Script IDE](SCRIPT_IDE.md), [editor history and inspectors](EDITOR_HISTORY_AND_INSPECTORS.md),
  [particle system](PARTICLE_SYSTEM.md).
* [Editor Enhancers](EDITOR_ENHANCERS.md): hierarchy/folder styling, bookmarks, hover keys, tabs,
  favorites and the ruler (vHierarchy / vFolders / vInspector / vTabs / vFavorites / vRuler).
