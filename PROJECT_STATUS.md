# Tartarus Engine — Project Status

Updated 2026-10-07. Tartarus is a C++ / OpenGL engine and editor with a C# sample game.

| Area | Current state | Documentation |
| --- | --- | --- |
| Engine/game ownership | Project C# owns player/weapon definitions, health/damage, gravity, scoring, NPC decisions, HUD, audio policy, locomotion policy and content conventions. Native code executes rendering, physics, geometry, animation/IK and audio processing. | [Ownership and validation](docs/ENGINE_GAME_BOUNDARY.md) |
| Project independence | Generic C# bootstrap, project-local assemblies and exports work without an FPS integration. The sample build can be disabled with `TARTARUS_BUILD_SAMPLE_GAME=OFF`. | [Scripting manual](docs/SCRIPTING_MANUAL.md) |
| Editor authoring | Global undo, C# Inspectors, curve windows, embedded C# IDE, animation previews and scene recovery. Upstream adds Inspector attributes, component-level Keep Changes After Play, tabs, favorites, hierarchy/folder styles and ruler. | [Editor Enhancers](docs/EDITOR_ENHANCERS.md), [history and Inspectors](docs/EDITOR_HISTORY_AND_INSPECTORS.md), [Script IDE](docs/SCRIPT_IDE.md) |
| Responsiveness | Navigation no longer creates whole-scene undo snapshots. Remaining live-scene spikes need retained profiler evidence. | [Performance](docs/PERFORMANCE.md), [five upgrade candidates](docs/EDITOR_UPGRADES.md) |
| Sample weapons/body | Project WeaponDefinition data, `.fpsanim` presentation, Animator graphs and native recoil/sway/IK execution. Two existing AK ADS reload hand-anchor presentation checks remain unresolved. | [Ownership and validation](docs/ENGINE_GAME_BOUNDARY.md), [weapon reimport](docs/WEAPON_REIMPORT.md) |
| Private content | Licensed model, animation, texture and audio payloads stay in the private asset library. Git carries authored settings, metadata and the external asset manifest. | [Repository setup](README.md), [project assets](docs/PROJECT_ASSETS.md) |

The five editor proposals retain their original ranking. Keep Changes After Play now has a
component-level implementation; its proposed refinement is property-level review and edit
provenance. Public character/navigation/cover APIs and multiple simultaneous carry constraints
remain engine feature work, as documented in the ownership audit.

Build with `cmake --build build --config Release --target TartarusEngine`. The desktop
`run-editor.cmd` builds and launches its own checkout. Close engine/editor processes before
rebuilding. Verify with `build/Release/TartarusEngine.exe --unit-tests`, `--editor-tests`
and `--smoke-test tests/smoke-scenes`; GUI-subsystem processes must be waited for explicitly.

The editor may update asset metadata. Autosave writes a `<scene>.recovery.json` sidecar;
explicit Save serializes the authored scene. Review `git status project` after an editor
session and run `python tools/assets/check_git_assets.py` before committing.
