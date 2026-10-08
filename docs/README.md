# Tartarus documentation

## Editor authoring

* [Editor upgrade recommendations](EDITOR_UPGRADES.md): five ranked candidates, current implementation evidence, first deliverables and acceptance checks.
* [Editor UI](EDITOR_UI.md): theme, shared widgets, docking and screenshot checks.
* [History and custom Inspectors](EDITOR_HISTORY_AND_INSPECTORS.md): global undo and C# Inspector authoring.
* [Script IDE](SCRIPT_IDE.md): embedded C# editing, semantic navigation, compilation and recovery.
* [Curve editor](CURVE_EDITOR.md): shared curve authoring tools.

## C# gameplay and editor scripting

| Document | Use it for |
| --- | --- |
| [Scripting manual](SCRIPTING_MANUAL.md) | Start here: setup, tutorials, player/weapon customization, prefab workflow and troubleshooting. |
| [Complete API reference](SCRIPTING_API.md) | Implemented classes, methods, field types, lifecycle, native data access and limitations. |
| [Gameplay frame reference](SCRIPTING_FRAMES.md) | Every PlayerFrame, WeaponFrame and ShotFrame member, operation and flag. |
| [Implementation overview](CSHARP_SCRIPTING.md) | Native/managed ownership, build integration, AK/870 migration and research sources. |
| [Engine/game ownership audit](ENGINE_GAME_BOUNDARY.md) | Implemented game/engine owners, the five audited migrations, validation and remaining engine API limits. |

The tutorials target the APIs implemented in this repository. The editor is C++; C# provides
gameplay and editor extensions. Editor tools and gameplay compile independently. See each
guide's boundaries before assuming Unity package or serialization compatibility.

## Related engine systems

* [Project assets](PROJECT_ASSETS.md): folder layout, weapon files, and duplicate consolidation.
* [Weapon integration](FPS_WEAPON_INTEGRATION.md): imported weapon rigs and assets.
* [First-person animation](FPS_ANIMATION_SYSTEM.md): animator, first-person rig and procedural presentation.
* [Animator](ANIMATOR.md): states, transitions, parameters, tracks and animation events.
* [Player body setup](BODY_SETUP.md): first-person body, arms, IK and locomotion.
* [Procedural recoil](PROCEDURAL_RECOIL.md), [weapon camera](WEAPON_CAMERA.md),
  [weapon sway](WEAPON_SWAY.md), [procedural animation](PROCEDURAL_ANIMATION.md).
* [Audio](AUDIO.md): sources, weapon sound, foley, spatialization and mixing.
* [Editor UI](EDITOR_UI.md), [performance](PERFORMANCE.md), [enemy AI](ENEMY_AI.md),
  [character outfits](CHARACTER_OUTFITS.md), [sky](SKY.md), [blood effects](BLOOD_FX.md).
