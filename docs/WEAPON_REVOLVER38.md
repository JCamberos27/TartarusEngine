# .38 Special revolver

`project/assets/Weapons/Revolver38/` holds the third weapon, built to the AKS74U / Remington 870 layout
(see [Project assets](PROJECT_ASSETS.md)). The source is `38 Special 60fps.blend` (the same Manny/Quantum
arms rig as the other two); its textures are `Tartarus Assets/Raw/Weapons/38_Special`.

## Rebuilding

From the repository root, Blender 5.2 and Python with NumPy:

```text
blender --background "<38 Special 60fps.blend>" --python tools/weapons/export_revolver38.py -- build/revolver38-export save3p "build/revolver38-export/38 Special 60fps (3P).blend"
python -B tools/weapons/install_revolver38.py --staged build/revolver38-export --raw "<Drive>/Tartarus Assets/Raw/Weapons/38_Special"
blender --background "build/revolver38-export/38 Special 60fps (3P).blend" --python tools/weapons/author_3p.py -- project/assets/Weapons/Revolver38/ThirdPerson
python -B tools/weapons/install_revolver38.py --staged build/revolver38-export --raw "<Drive>/Tartarus Assets/Raw/Weapons/38_Special"
python -B tools/weapons/make_revolver38_controller.py
python -B tools/weapons/make_revolver38_fpsanim.py
python -B tools/weapons/make_revolver38_prefab.py
```

The second install picks up the ThirdPerson clips. `--used "<Drive>/Tartarus Assets/Used"` also mirrors the
payloads to the shared Drive. `REVOLVER38_RENDER=<dir>` makes the exporter render stills of every fix. The
source .blend is never written; `save3p` writes a separate copy for `author_3p.py`.

## What the exporter fixes

Approved in the November 2026 audit, applied in memory on every export:

- **Fire.** The weapon take only flicked the hammer. It is now a double-action pull: trigger back, hammer
  cocked and the cylinder indexing one chamber (60°, counter-clockwise from the rear) over six frames, the
  hammer falling on frame 7, the trigger resetting by frame 24 (`F_PULLED, F_FALL, F_HOLD, F_RESET`). Two
  frames turned the cylinder 30° a frame, where a six-chamber cylinder aliases (reads as still or backwards).
  The cylinder starts one chamber back, so the clip ends where Idle is; Any→Fire is a cut (duration 0) so
  that +60° start never blends back in sight. The round leaves on the hammer fall: the Fire state's Shot event
  is at frame 7 (0.117 s). The arms' trigger finger curls with the trigger.
- **Hip shots play the Fire clip.** A recoil profile normally makes every shot procedural (`WeaponRules.cpp`,
  Fire request). The .fpsanim sets `"hipFireClip": true`, so hip shots play the clip (hammer, trigger,
  cylinder) and sights-up shots stay procedural with the profile.
- **Reload.** `CB_ik_hand_r` was never keyed; it now holds the constant every other take holds. The six
  rounds that fall out are the same bones the left hand later loads. Each is scaled away between clearing its
  chamber and the fingers picking it up, and the engine throws a physics case per chamber instead.
- **Aim.** The file has no ADS pose. It is solved as the Remington's: Idle frame 0 with `CB_Gun` moved so
  the rear notch and front post lie on the AKS-74U's measured sight line, the notch 34 cm from the eye
  (`EYE_TO_NOTCH`; tune `SIGHT_CORRECTION` from Play as the 870's was).
- **Rig.** Bullet1..6 are re-parented under Cylinder with their constraint motion baked. The weapon root
  holds its bind pose in the weapon clips; its 0.64 cm bind offset is carried by `weaponMountOffset`, the
  870's scheme.
- **Rounds.** `Casing_38Special.fbx` (spent case, 120 tris) and `Round_38Special.fbx` (live round) are cut
  from the model's first chamber.

## Engine

`eject.origins` lists one origin per chamber; one batch of `Eject` events uses them in turn. `eject.liveModel`
is thrown for the chambers still loaded when the batch starts, so a partial reload dumps live rounds. Several
`Eject` events in one frame each throw a case.

The controller has one `Reload` state. It plays when `Reload` is set and `Ammo` < 6, fires six `Eject` at
frame 58 and `Refill` when the crane shuts (frame 322 of 372).

The player carries it in the First Person Controller's sidearm slot (`Sidearm Weapon Prefab` / `Sidearm Animation
Set`): 1 draws the primary (AK), 2 the secondary (870), 3 the sidearm and 4 goes unarmed (input action `Weapon4`).
Every scene with the AK/870 player loadout has it; the Drive-hosted forest-trail scene is unchanged.

## Headless test

```
TARTARUS_HIDDEN=1 build/Release/TartarusEngine.exe --npc-test revolver --smoke-shots <dir>
```

`src/Game/AI/NpcTest_Revolver.cpp`, 77 s in the WeaponLab: ADS (the sight line is logged), two sights-up
shots, a partial reload, six hip shots, a full reload, inspect, mag check, melee, walk and sprint, then the AK
and 870 fired and reloaded, the slots switched and one scroll notch. It saves 49 named frames of the Scene and
Game views and runs 19 checks: ammo, ejections by model, the cases at rest, hammer and trigger travel,
one chamber per hip shot with no backwards spin, the actions and sprint playing, each slot keeping its ammo.
Leave `TARTARUS_HIDDEN=1` on: a visible window that loses focus pauses Play and the run stalls. The run's
summary says FAIL only for 4 log errors that predate the revolver (`T_Spider_AO.png`, `worklight_hum_1.wav`).

## Open items

- **Audio.** The `snd.revolver38.*` events are authored, but no revolver recordings exist yet. The prefab names
  the firing profile `38`.
- **Play tuning.** `SIGHT_CORRECTION`, the 3P holds in `author_3p.py` (`REVOLVER`) and the recoil and damage
  values are first passes.
