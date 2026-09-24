# Handoff: ADS animation system + weapon/Animator inspector overhaul

Work in progress. The next session should pick up here. Delete this file when the work is finished.

> **WARNING: THE EDITOR CODE HAS NOT BEEN COMPILED YET.**
>
> - The game-side half compiled and passed all 998 unit tests before the editor refactor began.
> - The editor files have never been built:
>   - `EditorLayer_WeaponInspector.cpp` (new)
>   - `EditorPropertyRows.h/.cpp` (new)
>   - the tag-chip edits in `EditorLayer_Animator.cpp`
>
> Build the project first and fix any errors before doing anything else.

## Goal (from the user)
> "set up the animation system in a way that handles these things correctly and will make it easy to implement the next set of ads Animations. We want it to be versatile, useful, and easy to use. Make sure the inspectors are useful and look good."

The user made two decisions:
- **Support both kinds of ADS animation:** authored `ADS_<action>` clips, and hip clips that get carried onto the sights.
- **Scope:** the weapon Inspector and the Animator window.

## Done (in code)
1. **Contract** (`src/Game/FirstPersonAnimation.h`):
   - New tags `kTagAdsCarry` ("ADSCarry") and `kTagBusy` ("Busy").
   - `kKnownTags[]` holds the tags with their descriptions, and `KnownTagDescription()` looks one up.
   - Fire is refused during states tagged Reload or Busy.
2. **`.fpsanim` `"ads"` block** (`FirstPersonAdsSettings`, member `Ads`) with these keys:
   - `zoom`, `viewModelZoom`, `zoomTime`
   - `referenceState`, `carryTag`
   - `matchElbows`, `matchTwist`
   - `aimHoldTime`

   The legacy keys `gameplay.adsZoom*` are still read. The AK `.fpsanim` has been migrated.
3. **Generator** (`BuildFirstPersonController`):
   - It sets the new tags.
   - When a clip list has optional `ADS_<TacReload|EmptyReload|MagCheck|Inspect>` clips, it creates "ADS <action>" states tagged ADS. Each gets an Any transition on the action plus aim, and an exit after 0.3 s.
   - The AK `.controller` has been retagged.
4. **Carry math extracted** to `src/Game/FirstPersonAdsCarry.h/.cpp`:
   - `BuildAdsCarry` finds carried states by tag.
   - `EvaluateAdsCarry` is a pure function.
   - A report is published per weapon path; look it up with `FindAdsCarryReport`.
5. **`FirstPersonPresentation`** now uses the new carry code (`SetupAdsCarry`/`SampleAdsCarry`/`AdsReport`). `ReloadIfChanged` now re-measures the carry when needed.
6. **`src/Editor/EditorPropertyRows.h/.cpp`:** a shared row toolkit (Float/Vec3/Range/Spring/Name rows, Section, Badge, ResetButton).
7. **New `src/Editor/EditorLayer_WeaponInspector.cpp`:** the redesigned weapon Inspector.
   - An overview card with badges.
   - Undo and redo.
   - Sections: Animation, Aim-Down-Sights (with the carry table and a how-to guide), Gameplay, Rigs & Mount, Recoil (every field exposed), Movement, IK.
   - The old weapon editor code was removed from `EditorLayer_Animator.cpp`.
8. **Animator window:**
   - State tags are chips, with a `+` picker listing the known tags.
   - Nodes show the state's tags under the state name.

## Remaining (in order)
1. **Build** with `taskkill //IM TartarusEngine.exe //F; cmake --build build --config Release` and fix the compile errors.
   - `PickFromList`/`ContainsNoCase` in `EditorLayer_Animator.cpp` are probably unused now. Delete them if the compiler warns.
2. **Unit tests** (`src/Tests/UnitTests.cpp`):
   - `ads` JSON round trip, and migration of the legacy `gameplay.adsZoom` keys.
   - Generator tags and ADS-variant routing in the FSM test. Also check that the existing FSM tests still pass with the new tags and exits.
   - `EvaluateAdsCarry` weights through a crossfade stack.
3. **Checks:**
   - `build/Release/TartarusEngine.exe --unit-tests`
   - `python tools/check_component_registration.py`
   - `python tools/check_button_styling.py` (no raw `ImGui::Button`; use `ActionButton`/`PrimaryButton`)
4. **Docs:**
   - `FPS_WEAPON_INTEGRATION.md`: add a new "Aim-down-sights animations" section covering authored vs carried animations, the tags, the `ads` settings and the Inspector workflow.
   - `FPS_ANIMATION_SYSTEM.md`: update the code map, the tags and the carry description.
5. **AK regression in Play** (Sandbox scene only). Use a temporary per-frame probe that forces an ADS tac reload and a mag check, then remove the probe. The numbers must still hold:
   - the gun stays within about 1 mm of the sight line;
   - the shoulders are 0.00 mm off Aim at the end of the clip;
   - the elbows stay within about 1 mm through the end blend;
   - the arm bones are within 0.06° of Aim on the first frame.
6. **Inspector screenshots:** check the weapon Inspector sections and the Animator tag chips for alignment and readability.
7. **Fire gate:** check that firing during an ADS reload does nothing.
8. **Commit and merge only when the user says so.** Exclude `project/settings.json` from the commit.

## Full plan
See the approved plan below. It was copied from `~/.claude/plans/binary-floating-canyon.md`.

# ADS animation system + weapon/Animator inspector overhaul

## Context
Getting the AK's ADS reloads right took a pile of one-off fixes in `FirstPersonPresentation`:
- carrying hip clips onto the sights (gun-only IK offset);
- matching the elbow swivel and twist bones to Aim;
- keeping the grip during crossfades;
- predicting crossfade weights.

It works, but it's wired to the AK:
- **Hardcoded names:** the carried states are the literal list `{"TacReload","EmptyReload","MagCheck"}`, the reference pose is the literal `"Aim"`, and the zoom settings live under Gameplay.
- **Editor doesn't expose it:** nothing about ADS actions is editable, and ~25 recoil fields are hidden from the Inspector.
- **Inconsistent UI:** the two inspector files use different row and drag helpers, and state tags are typed as comma-separated text.

The goal is to make the next weapon's ADS animations a data and editor task, not code. Both kinds must work:
- real authored ADS clips play as-is while aiming;
- hip-only clips get carried onto the sights automatically.

## 1. Contract: tags drive behaviour (`src/Game/FirstPersonAnimation.h`)
- **New tags** in `FirstPersonAnimatorContract`:
  - `kTagAdsCarry = "ADSCarry"`: while aiming, this state's hip clip is carried onto the sights.
  - `kTagBusy = "Busy"`: the hands are busy, so no firing.
- **Tag registry:** add `KnownTags()` returning `{name, description}` for ADS, ADSCarry, Reload, Busy, Idle, Hidden and IKOff. The Animator and weapon inspectors both use it for pickers and tooltips.
- **Authored ADS clips work through the graph.** An "ADS TacReload" state is tagged `ADS`+`Reload` and reached by an Any transition with `Reload && Aim`. The driver already treats ADS-tagged states as sights-up (zoom, procedural ADS layer).
- **Fire gate:** `Fire()` refuses while the state is tagged `Reload` or `Busy`, which closes the procedural-fire hole during authored ADS actions.
- **Standard graph generator** (`BuildFirstPersonController`, `FirstPersonAnimation.cpp`):
  - tag TacReload/EmptyReload/MagCheck `ADSCarry`;
  - tag MagCheck/Inspect/Melee `Busy`;
  - tag Regrip `IKOff`;
  - when the clip list has `ADS_<name>` variants, add ADS states tagged `ADS`(+Reload) with Aim-conditioned transitions. The AK `.controller` gets the same tags.

## 2. `.fpsanim` ADS settings (new `WeaponAdsSettings`, `FirstPersonAnimation.h/.cpp`)
```
"ads": { "zoom", "viewModelZoom", "zoomTime",            // moved from gameplay (old keys still read)
         "referenceState": "Aim",                           // "" = first ADS-tagged state
         "carryTag": "ADSCarry", "matchElbows": true, "matchTwist": true,
         "aimHoldTime": 0.15 }
```
- The sight alignment settings (`procedural.aim` position, rotation, blend) are shown in the same Inspector section. Their JSON location doesn't change.
- Parsing merges partial files and ignores unknown keys, like the rest of the loader.

## 3. Extract the carry math (`src/Game/FirstPersonAdsCarry.h/.cpp`, new)
Move it out of `FirstPersonPresentation.cpp`:
- `AdsAction` becomes `AdsCarryAction`.
- `SetupAdsActions` becomes `BuildAdsCarry(model, rig, ctrl, assets, settings, socket, cameraBone) -> AdsCarryResult`.
  - It finds the states by `carryTag` and the reference by `referenceState`, not by name.
  - It returns per-state diagnostics: gun offset (cm, °), elbow swivel R/L (°), twist bones matched, and warnings (missing clip, reference not found, no IK so the whole-rig fallback applies).
- `AdsActionCorrection` becomes `EvaluateAdsCarry(stack, actions, dt, hold, out)`: a pure function, so it's unit-testable.

`FirstPersonPresentation` then only calls these two functions and writes the IK slots.
- **Bug fix:** live `.fpsanim` reloads that rebuild the IK (`ReloadIfChanged`) now also rebuild the carry data. They didn't before.
- **Report:** add `const AdsCarryResult& AdsReport() const`, which feeds the Inspector's live diagnostics during Play.

## 4. Shared property-row helpers (`src/Editor/EditorPropertyRows.h/.cpp`, new)
Two inconsistent lambda sets (`DrawWeaponDefinitionEditor`, `DrawWeaponProcedural` in `EditorLayer_Animator.cpp`) become one shared toolkit. It follows `AlignToColumn`/`DrawVec3Row` from `EditorLayer_Inspector.cpp` and uses `EditorUIPrimitives` colours and buttons:
- `Row(label, tip)` and `FloatRow`: clamped, with the tooltip on both the label and the widget, and an optional units suffix.
- `Vec2Row`/`RangeRow`/`Vec3Row` with axis-coloured labels, `CheckRow`, `SpringRow`, `NamePickRow` (wraps `PickFromList` and flags unknown names in the warning colour).
- `Section(icon, title, summary, onReset)`: a collapsing header with a one-line summary and a reset button that reuses the existing `resetSection` confirm popup.
- `StatusBadge(ok/warn/error, text)`.

## 5. Weapon Inspector redesign (`EditorLayer_Animator.cpp`)
- **Overview card:** models, controller, and badges for controller loaded, socket/root found, IK bones OK, and ADS carry states measured.
- **Sections:**

| Section | Contents |
|---|---|
| Animation | Controller and "Open in Animator" (as today) |
| Rigs & Mount | Arms and weapon models, view rotation, socket, root, mount rotation |
| Gameplay | Magazine, RPM, fire mode, reload hold, fidget range, impact |
| **Aim-Down-Sights** (new) | See below |
| Recoil | Sub-groups: Kick, Variety, Camera Punch, Camera Shake, Aim Climb, Bolt & Hip. Exposes every hidden field (spread, bias, jitter, first shot, wander, burst, shake, aim climb/recovery, camera smoothing, hip procedural, bolt). Keeps the burst preview plot. |
| Movement | Sway, Bob, Breathing, Locomotion Rate, Lean, State Offsets |
| IK | As today, with the bone check shown as a badge |

- **The Aim-Down-Sights section** contains:
  - zoom (world, gun, time) and sight alignment;
  - a reference-state picker and a carry-tag picker, plus match-elbows and match-twist toggles;
  - a **table of the controller's actions:** State | Mode (Authored ADS / Carried hip clip / Not ADS) | Status | measured values. The values come from `AdsReport()` while playing; otherwise a "measured at Play" hint shows.
  - a collapsible "How to add ADS animations" guide covering both paths.
- **Undo/redo** for weapon definitions: JSON snapshots like `AnimatorWindowState::Commit/Step`, with Ctrl+Z/Y when the panel is focused and small undo/redo buttons. It still autosaves.

## 6. Animator window tags (`EditorLayer_Animator.cpp` ~1632)
- Replace the comma-separated tags text field with chips: each tag is a coloured chip with ✕ to remove, and the tooltip comes from `KnownTags()`.
- An "+ Tag" picker lists the known tags, with descriptions and custom entry.
- Nodes show small tag chips under the state name.

## 7. Docs
- `FPS_WEAPON_INTEGRATION.md`: new "Aim-down-sights animations" section covering authored clips versus the carried-hip path, the tags, `ads` settings and the Inspector workflow.
- `FPS_ANIMATION_SYSTEM.md`: update the code map, contract tags and the ADS carry description.

## Files
- **Game:**
  - `src/Game/FirstPersonAnimation.h/.cpp`
  - `src/Game/FirstPersonPresentation.h/.cpp`
  - new `src/Game/FirstPersonAdsCarry.h/.cpp`
  - `project/assets/fps/AKS74U/AKS74U.fpsanim` / `.controller` (tags, `ads` block)
- **Editor:**
  - `src/Editor/EditorLayer_Animator.cpp`
  - new `src/Editor/EditorPropertyRows.h/.cpp`
- **Tests:** `src/Tests/UnitTests.cpp`
- **Build:** CMake source lists
- **Reused, not rewritten:** `IK::ApplyRig`/`SolveTwoBone` (swivel, gripPose, LocalRotations), `AnimatorCrossfadeWeight`, `PickFromList`, `CurveEditor::Draw`, `EditorUIPrimitives::*`

## Verification
- **Unit tests:**
  - `ads` JSON round trip and legacy `gameplay.adsZoom` migration;
  - generator tags and ADS-variant routing in the FSM test;
  - `EvaluateAdsCarry` weights through a crossfade stack.
- **Full build and checks:** full `--unit-tests`, plus `tools/check_component_registration.py` and `tools/check_button_styling.py`.
- **AK regression in Play:** use the temporary per-frame probe as before (forced ADS tac reload and mag check), then remove it. The numbers must match today's:
  - gun within ~1 mm of the sight line;
  - shoulders 0.00 mm off Aim at clip end;
  - elbows within ~1 mm through the end blend;
  - arm bones within ≤0.06° of Aim at the first frame.
- **Inspector check:** screenshots of the weapon Inspector (each section) and of Animator tag chips, checked for alignment and readability.
- **Fire gate:** confirm in Play that firing during a (carried) ADS reload does nothing.
