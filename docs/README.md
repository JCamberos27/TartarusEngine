# Tartarus documentation

## Editor authoring

The repository [README](../README.md) is authoritative for asset setup and Git policy.
Licensed third-party models, animations, textures and audio stay in the private asset library;
only their `.meta` sidecars, authored settings and `project/external_assets.csv` belong in Git.
This also applies to re-exported weapon FBXs and sounds derived from third-party recordings.
Run `python tools/assets/check_git_assets.py` before committing or pushing.

* [Editor Enhancers](EDITOR_ENHANCERS.md): hierarchy and folder styles, Inspector attributes, tabs, favorites and ruler.
* [History and Inspectors](EDITOR_HISTORY_AND_INSPECTORS.md): global undo and gameplay Inspectors.
* [Script IDE](SCRIPT_IDE.md): embedded C++, shader and .tescript editing, module rebuilds and recovery.
* [Curve editor](CURVE_EDITOR.md): shared curve authoring tools.
* [Black Glass design system](EDITOR_DESIGN_SYSTEM.md): the opt-in editor theme (solid black panels, real blurred glass under everything that floats), its motion language, Glass Workspace panels and viewport HUDs.

## Gameplay code

[Gameplay code](GAMEPLAY_CODE.md): where player, weapon and body code lives, components, and
hot-reloading the game and editor modules. Everything is native C++.

## Related engine systems

* [Project assets](PROJECT_ASSETS.md): folder layout, weapon files, and duplicate consolidation.
* [Weapon reimport](WEAPON_REIMPORT.md): repeating the AKS74U and Remington 870 authored-export import.
* [Procedural recoil](PROCEDURAL_RECOIL.md), [weapon camera](WEAPON_CAMERA.md),
  [weapon sway](WEAPON_SWAY.md), [curve editor](CURVE_EDITOR.md).
* [Script IDE](SCRIPT_IDE.md), [editor history and inspectors](EDITOR_HISTORY_AND_INSPECTORS.md),
  [particle system](PARTICLE_SYSTEM.md).
* [Editor Enhancers](EDITOR_ENHANCERS.md): hierarchy/folder styling, bookmarks, hover keys, tabs,
  favorites and the ruler (vHierarchy / vFolders / vInspector / vTabs / vFavorites / vRuler).
* [Editor Enhancers handoff](EDITOR_ENHANCERS_HANDOFF.md): implementation design and code map for the integrated tools.
