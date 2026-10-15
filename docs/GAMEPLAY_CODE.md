# Gameplay code

All gameplay is native C++. The C# runtime, the managed project scripts and the .NET build
dependency were removed in October 2026; scenes saved with the old C# slots convert to the
native components on read (`src/Game/ProjectComponentMigration.cpp`).

## Where things live

| Area | Files |
| --- | --- |
| Player movement and stance | `src/Game/Player.{h,cpp}` |
| Weapon fire, reload, ADS and inspect rules | `src/Game/WeaponRules.{h,cpp}` (pure functions over `WeaponFrame` / `WeaponLoadoutFrame`) |
| Weapon presentation, shots and effects | `src/Game/FirstPersonPresentation.cpp`, `MuzzleEffects.h`, `Combat/` |
| First-person body locomotion | `src/Game/BodyLocomotion.{h,cpp}` (pure function over `BodyMotionFrame`), `FirstPersonBody*.cpp` |
| Weapon prefabs and attachments | `src/Game/WeaponPrefab.{h,cpp}`, `WeaponAttachments.h` |
| Frame structs shared by the rules | `src/Game/GameFrames.h` |
| Reloadable systems | `src/Game/GameModule.cpp`, `TransformControllerSystem.cpp`, `LightFlickerSystem.cpp` |

`WeaponRules` and `BodyLocomotion` take one frame struct in and write their results back into it,
with no engine state of their own. That keeps them easy to unit test
(`src/Tests/UnitTests_Gameplay.cpp`) and easy to move into the reloadable module later.

## Components

Components are plain structs in `src/Game/Components.h`, registered once in
`src/Game/ComponentRegistry.cpp` with `ReflectComponent`. Registration gives the component its
scene/prefab JSON, its Inspector (groups, bounds, tooltips, enums, asset pickers), Add Component
menu entry, copy/paste and undo. The JSON key is the display name; enums are written as their
label. Fields left out of the registration are runtime scratch and never saved.

`python tools/check_component_registration.py` fails if a component in `Components.h` is neither
registered nor allow-listed with a reason.

## Hot reload

Two DLLs are rebuilt and reloaded while the editor runs:

* **TartarusGame**: gameplay systems ticked while playing. `GameModule.cpp` calls each system's
  update. Today that is the Transform Controller and Light Flicker systems.
* **TartarusEditor**: editor panels and tools (`src/Editor/EditorModule*.cpp`).

Edit their sources in the [Script IDE](SCRIPT_IDE.md) or any other editor, then save in the IDE,
press F6, or use **Rebuild Game & Editor Modules**. The host notices the new DLL, loads a copy
of it and swaps it in. A failed build leaves the running modules alone. Both modules compile with
`/W4 /WX`, so warnings fail the build.

To add a reloadable system: write `UpdateMySystem(World&, float)` in its own `.cpp`, add it to the
`TartarusGame` sources and the host's `REMOVE_ITEM` list in `CMakeLists.txt`, call it from
`GameModule.cpp`'s `Update`, and list the file in `kReloadable` in `src/Editor/GameModuleBuild.cpp`
so saving it from the IDE triggers a rebuild. Keep its state in components, since the module's
globals are lost on reload.

Code outside the two modules (renderer, physics, the player and weapon systems above) needs a
normal rebuild and restart.

## Adding a gameplay component

1. Add the struct to `Components.h`.
2. Register its fields in `ComponentRegistry.cpp`.
3. Write its system and call it from `GameModule.cpp` (reloadable) or the host's update.
4. Add a unit test in `UnitTests_Gameplay.cpp`.

`LightFlickerComponent` and `src/Game/LightFlickerSystem.cpp` are a small complete example.
