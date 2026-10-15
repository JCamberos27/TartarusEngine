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
  update. Today that is the Transform Controller and Light Flicker systems. It also carries the
  weapon and body rules (`WeaponRules.cpp`, `BodyLocomotion.cpp`): the engine calls the module's
  copies through `GameRules.h`, so tuning fire, reload or locomotion logic reloads mid-Play too.
* **TartarusEditor**: editor panels and tools (`src/Editor/EditorModule*.cpp`).

Edit their sources in the [Script IDE](SCRIPT_IDE.md) or any other editor, then save in the IDE,
press F6, or use **Rebuild Game & Editor Modules**. The host notices the new DLL, loads a copy
of it and swaps it in. A failed build leaves the running modules alone. Both modules compile with
`/W4 /WX`, so warnings fail the build.

The rules exchange `GameFrames.h` structs with the engine. Changing those structs means a full
rebuild: bump `kFramesRevision` in that header, and until the engine is rebuilt it keeps using
its own copies of the rules (the log says so) instead of calling a module built against other
frames.

To add a reloadable system: write `UpdateMySystem(World&, float)` in its own `.cpp`, add it to
`TARTARUS_GAME_MODULE_ONLY_SOURCES` in `CMakeLists.txt` and call it from `GameModule.cpp`'s
`Update`. CMake also writes that list to `TartarusModules.sources` in the build folder, which is
how the Script IDE knows saving the file (or the header beside it) should rebuild the modules.
Keep its state in components, since the module's globals are lost on reload. The module links
only glm and EnTT, so a system that needs the renderer, physics or audio goes through the host
callbacks in `GameModuleAPI.h`.

Code outside the two modules (renderer, physics, player movement, weapon presentation) needs a
normal rebuild and restart.

## Asset hot reload

| Asset | Reloads | Where |
| --- | --- | --- |
| Shaders and material shaders | on save, within a second | `ShaderLibrary::PollChangedFiles` |
| Textures, models, materials, prefabs | when changed outside the editor | `EditorLayer::OnExternalModify` |
| Preloaded audio clips | when changed outside the editor; playing voices finish on the old sound | `AudioEngine::Reload` |
| `settings.json` (Project Settings) | when changed outside the editor | `EditorLayer::OnExternalModify` |
| Animator Controllers | on the next lookup, at most twice a second | `GetAnimatorController` |
| `.fpsanim`, `.recoil`, `.camerashake`, weapon prefabs | while the weapon is out, four times a second | `FirstPersonPresentation::ReloadIfChanged` |

Scenes don't reload under you: the editor warns and **File > Revert Scene** loads the new
version. A shipped game turns all of this off at startup (`LiveReload.h`) so it never polls its
files; a development build keeps it.

## Adding a gameplay component

1. Add the struct to `Components.h`.
2. Register its fields in `ComponentRegistry.cpp`.
3. Write its system and call it from `GameModule.cpp` (reloadable) or the host's update.
4. Add a unit test in `UnitTests_Gameplay.cpp`.

`LightFlickerComponent` and `src/Game/LightFlickerSystem.cpp` are a small complete example.
