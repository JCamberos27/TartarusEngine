# FPS First-Person Animation — System Reference

Branch: `feature/fps-first-person-animation`

This is the **how it works** document. It covers what the system is made of, how a frame
flows through it, the asset formats, the gameplay rules, and the invariants that keep it
from breaking again. Companion documents:

| Document | Read it when |
|---|---|
| **`ANIMATOR.md`** | You're editing a weapon's animation graph: states, transitions, layers, the Animator window |
| **`FPS_WEAPON_INTEGRATION.md`** | You're adding a new weapon (or re-exporting this one). Step-by-step workflow and checklist |
| **`FPS_ANIMATION_INVESTIGATION.md`** | You're about to form a new hypothesis about skinning or import. It lists everything already measured and ruled out, with numbers |
| `tools/assimp_patches/README.md` | You're bumping assimp, or a weapon's spare magazine vanished on import |

---

## 1. Overview

During Play, a player whose `FirstPersonController` has a **Weapon Definition**
(`.fpsanim`) in its Animation Set field gets a camera-bound pair of rigs:

- **arms**: a skinned FBX.
- **weapon**: a second skinned FBX.

If the field is empty, the controller behaves exactly as it did before this branch; the
Sandbox gravity gun is untouched.

The work is split three ways:

| Piece | Owns | Edited in |
|---|---|---|
| **Animator Controller** (`.controller`) | *Which animation plays when*: states, transitions, priorities, fades, events, tags | Animator window (`ANIMATOR.md`) |
| **Weapon definition** (`.fpsanim`) | Which rigs, which controller, how the gun mounts in the hands, and the gameplay numbers (magazine, rpm, recoil, bob…) | Inspector, with the file selected in the Asset Browser |
| **`FirstPersonPresentation`** (C++) | Placement on the camera, input → controller parameters, ammo/fire modes, the procedural ADS kick and bob | code (rarely needed per weapon) |

```
 .fpsanim ─ rigs · controller · socket · gameplay        FirstPersonController (scene)
            │                                            Camera Bone · View Model Offset/Rotation/Scale/FOV
            ▼                                                     │
 FirstPersonPresentation (src/Game, runtime only) ◄───────────────┘
   Start  ─ spawns arms + weapon entities, each with an AnimatorControllerComponent
            (arms: driver, track "arms"; weapon: follower, track "weapon", Driver = arms)
   Update ─ reads last frame's events/tags, drops unused triggers, hides/shows the rigs,
            places arms on the camera bone and the weapon on the arms' gun socket
   input  ─ Fire / Reload / MagCheck / Inspect / Melee / Equipped → controller parameters
   Tick   ─ Speed/Sprint/Aim/Equipped/Ammo parameters, Fidget timer, recoil + bob clocks
   Stop   ─ destroys both entities before the scene snapshot is restored
            │
            ▼
 UpdateAnimatorControllers (AnimatorController.cpp) ─ state machine → blended pose → Model::ApplyLocalPose
            │  ViewModelTag
            ▼
 SceneRenderer view-model sub-pass (own FOV, depth cleared)
```

Nothing the presentation creates is ever saved. The entities exist only between Play and
Stop, and `ViewModelTag` is runtime-only. The PhysX character stays the only physics
authority, and the view model has no collider.

### Per-frame order (`src/main.cpp`, Play loop)

1. `firstPersonPresentation.RemoveViewKick(player.Cam)`: takes last frame's view punch and lean
   off the camera, so the player only integrates its own look.
2. `firstPersonBody.BeforePlayerMove(player, player.Cam)`: takes the camera back out of the
   body's head and hands the Player last step's root motion (§8b). A no-op without a
   First Person Body in the scene.
3. `player.Update(...)`: movement and camera.
4. `firstPersonBody.Tick(...)`: stands the body at the capsule's feet and sets its locomotion
   parameters.
5. `firstPersonPresentation.Update(world, player.Cam)`:
   - consume the events the controller fired last frame (`Shot` spends a round, `Refill` fills the magazine);
   - reset the one-frame triggers;
   - apply hidden or shown;
   - pose both rigs from the camera.
6. Weapon input. This only runs when the game has input **and** the controller's gravity gun is off:
   - `FireMode` → `ToggleFireMode`
   - `Fire1` → `UpdateTrigger`
   - `Reload` → `UpdateReloadKey`
   - `Inspect` and `Melee` → `TriggerAction`
   - `Weapon1`, `Weapon2`, scroll and `Holster` → `SetEquipped`
7. `firstPersonPresentation.Tick(dt, planarSpeed, sprinting, aiming)`: sets the locomotion parameters. `aiming` means `Fire2` is held.
8. `UpdateAnimatorControllers(...)`: the arms controller runs, the weapon mirrors it, and both
   models are posed. The body's controller runs here too, its root motion In Place.
9. `firstPersonBody.LateUpdate(...)`: reads the body's travel for the next move and puts the
   camera in its head.
10. `firstPersonPresentation.LateUpdate(world, player.Cam)`: re-places the arms and weapon from
    this frame's finished pose (clips and IK), on the final camera.

Placement reads the pose from the previous frame. That one frame of latency is deliberate, so
a frame never mixes two placements.

### Code map

| File | Role |
|------|------|
| `src/Game/AnimatorController.{h,cpp}` | The general animator: `.controller` v2 format, `AdvanceAnimator` (state machine), sampling and blending, the tracks/driver group, `BuildFirstPersonController`'s output format |
| `src/Game/FirstPersonAnimation.{h,cpp}` | The weapon definition (`.fpsanim` v1/v2), `FirstPersonAnimatorContract` (the parameter, tag and event names), `BuildFirstPersonController` (the standard FPS graph), `FirstPersonReloadButton`, `FirstPersonRegripDelay` |
| `src/Game/FirstPersonPresentation.{h,cpp}` | The runtime driver: `Start`/`Stop`/`Update`/`Tick`, fire, reload, equip, ADS recoil and walk bob |
| `src/Game/FirstPersonBody.{h,cpp}` | The player's body (§8b): stands it at the capsule's feet, feeds its locomotion controller, hands its root motion to the Player and puts the camera in its head |
| `src/Game/AnimationSystem.{h,cpp}` | `ApplyRootMotion`: a clip's travel moves the object (a dynamic Rigidbody's velocity, or the Transform), or is only reported (In Place) |
| `src/Game/FirstPersonAdsCarry.{h,cpp}` | Carrying hip clips onto the sights: `BuildAdsCarry` (measures each `ADSCarry` state against the aim pose), `EvaluateAdsCarry` (how much of the crossfade stack a carried action owns), and the per-weapon report the Inspector shows |
| `src/Game/BodyDebugDraw.h` | What the body shows about itself: `BodyDebug::Info()` (the Inspector's Live readout on the First Person Body component) and world-space debug lines (Gizmos > Player body: foot IK rays and pinned feet, body heading vs view with the turn-threshold wedge, root-motion arrow, stair easing, the camera on its shoulders) drawn by `ColliderGizmo` in the Scene view while playing |
| `src/Game/FirstPersonBodyContract.{h,cpp}` | The body's contract with its controller: the parameter / state / tag / bone names `FirstPersonBody` uses, which body option needs each, and `FPBody::Validate` (drives the Setup box on the First Person Body component) |
| `src/Game/Components.h` | `AnimatorControllerComponent` (params, tags, events, `Track`, `Driver`, `RootMotion`), `FirstPersonControllerComponent`, `FirstPersonBodyComponent`, `ViewModelTag`, `PoseSourceTag` |
| `src/Editor/EditorLayer_Animator.cpp` | The Animator window (state tags as chips with a known-tag picker), the component's Inspector section, and the `.controller` asset inspector |
| `src/Editor/EditorLayer_WeaponInspector.cpp` | The `.fpsanim` weapon Inspector: overview badges, undo/redo, and the Animation / Aim-Down-Sights / Gameplay / Rigs / Recoil / Movement / IK sections |
| `src/Editor/EditorPropertyRows.{h,cpp}` | `PropertyRows`: the shared label-column rows, sections, badges and reset buttons the weapon Inspector is built from |
| `src/Renderer/Model.{h,cpp}` | Import, skinning, `SampleLocalPose` / `ApplyLocalPose`, `NodeTransform` |
| `src/Renderer/SceneRenderer.cpp`, `RenderFrameContext.h` | The view-model sub-pass (`ViewModelFov`) |
| `src/Core/InputMap.{h,cpp}` | Default bindings and `MergeDefaults` |
| `src/Tests/UnitTests.cpp` | `TestAnimatorController`, `TestFirstPersonAnimationSet`, `TestFirstPersonAnimationFSM` (drives the AK graph through the real runtime), `TestFirstPersonAds` (the `ads` block, ADS variants, carry weights), `TestBlendTree2D` (2D weights, JSON, the body's frame maths), `TestInputMap` |
| `src/main.cpp` | Play-loop wiring; `--upgrade-fpsanim` (v1 → controller) |
| `project/animations/fps_body_locomotion.controller` | The body's locomotion: a 2D blend of idle, walk and jog in eight directions and run, plus Jump / Fall / Land (MC Core Motion clips, `project/assets/animations/mc_core_motion/`) |
| `tools/assimp_patches/` + `tools/apply_assimp_patches.cmake` | Local assimp fix, applied at configure time |
| `tools/component_registration_allowlist.txt` | `ViewModelTag` and `PoseSourceTag` are allow-listed (runtime-only). CI fails without them |

---

## 2. The weapon definition (`.fpsanim`)

The shipped definition is `project/assets/fps/AKS74U/AKS74U.fpsanim`. Select it in the
Asset Browser's **Animation** folder to edit it in the Inspector.

```jsonc
{
  "armsModel":   "assets/fps/AKS74U/FirstPerson/AKS-74U_A_FP_ADS.fbx",  // required: mesh + skeleton + REST bind pose
  "weaponModel": "assets/fps/AKS74U/Weapon/AKS-74U_A_W_ADS.fbx",        // required
  "controller":  "assets/fps/AKS74U/AKS74U.controller",                 // the Animator Controller (tracks "arms" + "weapon")
  "viewRotation": [0, 180, 0],        // Y-X-Z degrees: the FBXs' axis convention (§6)
  "weaponSocket": "ik_hand_gun",      // bone on the ARMS rig the gun rides
  "weaponRoot":   "root",             // bone on the WEAPON rig that lands on the socket
  "weaponMountRotation": [0, 90, 90], // fixed socket -> weaponRoot rotation (rotation only)
  "gameplay": {
    "magazine": 30, "rpm": 700, "allowFullAuto": true, "reloadHoldSeconds": 0.35,
    "regripMin": 10, "regripMax": 20,
    "recoil": { "pitch": 1.2, "offset": [0, 0.002, 0.014], "rise": 0.035, "settle": 0.08 },
    "adsBob": { "stride": 2.4, "side": 0.003, "vertical": 0.0015, "fullSpeed": 3.5, "ease": 8 }
  }
}
```

**Validation.** `FirstPersonAnimationSet::FromJsonString` fails the load, with a specific
message, on any of these:
- missing `armsModel` or `weaponModel`
- non-finite rotations
- `weaponSocket` given without `weaponRoot`, or the other way round
- neither a `controller` nor a v1 `clips` list
- a non-positive `rpm`, recoil rise or settle, bob stride or full speed, or reload hold time

**Attach check.** `Start()` then attaches **every** clip in the controller, the arms track on
the arms model and the weapon track on the weapon model. It fails Play if any clip can't be
attached, so a bad path shows up when Play starts, not in the middle of a reload.

**v1 files.** An older file with a flat `clips` list and no `controller` still runs: the
standard graph is built in memory. To turn one into a real controller, use the Inspector's
**Create Controller from Clips** button, or run:

```powershell
build\Release\TartarusEngine.exe --upgrade-fpsanim <abs .fpsanim> <abs out.controller> <project-relative controller ref>
```

That's how `AKS74U.controller` was produced.

### The contract between the driver and a weapon's controller

The driver refers to **names**, not states. A weapon's controller can have any states and
any transitions, as long as it uses these names (`FirstPersonAnimatorContract`):

| Kind | Name | Meaning |
|---|---|---|
| Float param | `Speed` | Planar speed, m/s |
| Bool params | `Sprint`, `Aim`, `Equipped` | Sprint held, aim held, weapon wanted in hand |
| Int param | `Ammo` | Rounds in the magazine |
| Triggers (one frame) | `Fire`, `Reload`, `MagCheck`, `Inspect`, `Melee`, `Fidget` | Set on input and dropped the next frame if no transition took them. `Fidget` comes after 10–20 s in a state tagged `Idle` |
| Tag | `ADS` | Sights are up: fire is a procedural kick (no `Fire` trigger), walking bobs, the view zooms. An authored ADS action ("ADS TacReload") carries it too |
| Tag | `ADSCarry` | While aim is held, this state's hip clip is carried onto the sights (see below). The tag name is `ads.carryTag` |
| Tag | `Reload` | A reload is running: R does nothing, and the gun can't fire |
| Tag | `Busy` | The hands are busy (mag check, inspect, melee): the gun can't fire |
| Tag | `IKOff` | The arm IK is off: the clip plays untouched (Draw, Holster, Regrip) |
| Tag | `Hidden` | Unarmed: both rigs are hidden. The controllers keep running |
| Tag | `Idle` | Settled idle: counts toward `Fidget` |
| Event | `Shot` | A round leaves the gun (hip fire). Put it at time 0 on the fire state |
| Event | `Refill` | The magazine is full again. Put it at time 1 on each reload state, so a reload cut short doesn't count |

### ADS actions: authored clips or carried hip clips

A reload, mag check or inspect while aiming can play in one of two ways, per action:

- **Authored ADS clip.** Add a clip named `ADS_<action>` (`ADS_TacReload`, `ADS_EmptyReload`,
  `ADS_MagCheck`, `ADS_Inspect`). The standard graph adds an `ADS <action>` state tagged `ADS`
  (plus the hip state's other tags, minus `ADSCarry`), reached with Aim held before the hip
  state can take the trigger, and leaving through Exit after 0.3 s. It plays as authored.
- **Carried hip clip.** With no ADS clip, the hip state plays, and if it's tagged `ADSCarry`
  it is carried onto the sights. `BuildAdsCarry` measures, once at Play start, the clip's
  first frame against the reference state's (`ads.referenceState`, default `Aim`; empty = the
  first `ADS`-tagged state):
  - the rigid gun correction about the camera bone, applied to the gun only through the IK
    `kAdsOffset` slot, so the arms follow it by IK and the shoulders stay put;
  - per arm, the elbow swivel onto the aim pose's elbow (`ads.matchElbows`);
  - the twist-helper bone locals the IK doesn't solve (`ads.matchTwist`).

  With `ads.actionBones` (default `clavicle_l`) the gun correction isn't needed: every other
  bone is held in the aim pose through `IKRigComponent::HoldPose` (the aim clip, still playing
  from where the aim state was), fully while the action is in the blend and eased off over
  0.3 s after, and the action's hand keeps its grip relative to the gun, so only the left arm
  plays the clip. `WriteAdsHold` in `FirstPersonPresentation` drives it. Per state,
  `ads.gunMotion` adds back a share of the clip's own gun turn / movement on top
  (`AdsGunMotion`, about the rear sight `ads.sightPivot` ahead of the eye; by default 18% / 25%
  for the reloads and 60% / 20% for the mag check) - the mag check's
  tip that brings the magazine into view.

  With all three the solved first frame *is* the aim pose, so the action starts and ends on
  the sights and moves like the hip clip in between. `EvaluateAdsCarry` weights the correction
  by how much of the crossfade stack the action owns, times how long aim has been held
  (`ads.aimHoldTime`). Without IK the whole view model is carried instead (the shoulders move).

The weapon Inspector's **Aim-Down-Sights** section lists each action's mode and, after a Play
session, what was measured (gun offset, turn, elbow swivel, matched bones) and any warnings.

---

## 3. The AK's graph (the standard first-person graph)

`AKS74U.controller` is exactly `BuildFirstPersonController` applied to the original 15 clips.
Open it in the Animator to see it. It reproduces the behaviour of the old hard-coded state
machine, and `TestFirstPersonAnimationFSM` pins that behaviour.

| State | Priority | Tags | Reached by |
|---|---|---|---|
| `Idle`, `Walk`, `Sprint`, `Aim` | 0 | `Idle` / – / – / `ADS` | Entry, and locomotion transitions between them |
| `IdleToSprint`, `SprintToIdle` | 1 | | Idle → Sprint; Sprint → not-Aim |
| `Regrip`, `Fire`, `Inspect`, `MagCheck` | 2 | | Idle + `Fidget`; Any + `Fire` / `Inspect` / `MagCheck` |
| `TacReload`, `EmptyReload`, `Melee` | 3 | `Reload` (reloads) | Any + `Reload` (`Ammo` > 0 / = 0); Any + `Melee` |
| `Draw`, `Holster` | 4 | | Holstered + `Equipped`; Any + not `Equipped` |
| `Holstered` | 5 | `Hidden` | Holster, when it ends. Holds Holster's last frame |

- **Locomotion.**
  - Sprinting (held **and** moving) beats aiming, and aiming beats walking.
  - Idle → Sprint plays `IdleToSprint`.
  - Leaving Sprint plays `SprintToIdle`, unless you're aiming: aiming out of a sprint goes straight to `Aim`.
- **Any State transitions.** All of them have **Respect Priority** on, so a request only takes over a state of strictly lower priority:
  - A fire, inspect or mag check can cut a sprint transition short.
  - Nothing below 4 interrupts a reload or melee.
  - Holster can't cut Draw short. Pressing 1, 2 or scroll mid-swap queues instead.
  - Fire, Inspect, MagCheck and Melee also have **To Self** on, so pressing again restarts them.
- **One-shots.** Every one-shot leaves through **Exit** at exit time 1.0. Entry then picks the resting state from the current input (Sprint, Aim or Walk, else Idle). That's why a reload started in ADS hands back to `Aim`.
- **Clips missing on the weapon track.** Idle, Walk, Aim, Draw and Regrip have no weapon clip, so the weapon crossfades to its bind pose, which for this set *is* the ADS pose. §8 item 5 explains why that must be a fade, not a cut.

To change behaviour, edit the graph: fades, priorities, extra states and new transitions
need no code. §9 lists what does still need code.

---

## 4. Weapon gameplay

This lives in `FirstPersonPresentation`, and its numbers come from the definition's `gameplay` block.

| Input (action name) | Default key | Does |
|---|---|---|
| `Fire1` | LMB / L-Ctrl | Fires one round per shot. At the hip it sets the `Fire` trigger, and the round is spent on the `Shot` event. In a state tagged `ADS` it's a procedural kick instead. A dry trigger does nothing |
| `Fire2` | RMB / L-Alt | Hold to aim (the `Aim` parameter) |
| `FireMode` | B | Toggles Semi-Auto (the default) and Full-Auto (60/rpm s between rounds while held). Logged to the Console. Does nothing if `allowFullAuto` is false |
| `Reload` tap | R | Sets the `Reload` trigger when the magazine isn't full and no `Reload`-tagged state is playing |
| `Reload` hold ≥ `reloadHoldSeconds` | R | Sets the `MagCheck` trigger |
| `Inspect` / `Melee` | I / Q | Set those triggers |
| `Weapon1` / `Weapon2` | 1 / 2 | Set `Equipped` true / false |
| scroll wheel, `Holster` | wheel / H | Toggle `Equipped` |
| `Sprint` | L-Shift | The `Sprint` parameter. Sprinting drops ADS |

Defaults live in `InputMap::Defaults()`. `project/settings.json` holds the saved list and
**wins** per action, and `InputMap::MergeDefaults` tops it up with any default it lacks. To
change a default key, edit both places, or delete that action from `settings.json`.

- **Magazine.** Refilled on the controller's `Refill` event. Anything that cuts a reload short, such as Holster, leaves the count unchanged. There is no reserve ammo, and running dry never auto-reloads.
- **Triggers last one controller update.** `Update()` resets them each frame, so fire pressed during a reload is dropped rather than firing when the reload ends. The same happened in the old code.
- **Fidget.** After `regripMin`–`regripMax` s (uniformly random, re-rolled each time) in a state tagged `Idle`, with no crossfade running.
- **Unarmed.** While the playing state is tagged `Hidden`, both rigs get `DeactivatedTag` and `InactiveTag`, so they aren't drawn. Their animators have `UpdateWhenInactive` set, so the controller can still leave the state.
- **ADS fire.** The set has no ADS fire clip, and the hip `Fire` clip would pull the sights off centre on every shot. So in an `ADS` state the pose is kept and the whole view model kicks about the eye, in camera space:
  - `recoil.pitch` degrees of muzzle rise, plus `recoil.offset`.
  - A `rise` linear ramp, then a `settle` exponential return to the §5 sight picture.
  - Each shot re-enters the rise at the current kick, so full-auto chains into a shake instead of snapping between rounds.
- **ADS while walking.** There is no aim-walk clip, so an `ADS` state gets a procedural camera-plane figure-eight bob from `adsBob`. Firing while walking in ADS adds the kick on top of the bob.

---

## 5. Placement: camera bone, weapon socket, ADS centring

### Camera-bone anchor

The rigs are authored **standing in their own scene** (feet at `y = 0`, head at
`y ≈ 1.557`). Parking the model's root on the camera floats the arms ~1 m overhead, so
`Update()` solves for the root that puts a **rig bone** exactly on the camera:

```
world(local) = position + rotation * (scale * local)
⇒ position   = camera.Position - rotation * (scale * boneLocal)
rotation     = cameraRotation * recoil * viewRotation(asset) * ViewModelRotation(scene)
position    += cameraRotation * (ViewModelOffset + recoilOffset + bob)
```

`boneLocal` comes from `Model::NodeTransform(CameraBone)` in the **current pose**, so the
bone stays pinned through the animation. `head` is a node, not one of the 52 skinned
bones — `BoneInfoMap` won't find it; only `NodeTransform` will. A missing bone logs one
warning and falls back to root-anchored placement.

### Weapon socket

The weapon is **not** given the arms' pose. It rides the arms rig's gun socket:

```
weaponEntity = armsEntity * (socket * mount * weaponRoot⁻¹)
  socket     = arms NodeTransform(weaponSocket)        posed
  weaponRoot = weapon NodeTransform(weaponRoot)         posed
  mount      = rotation-only weaponMountRotation        (0, 90, 90) for this set
```

Only the weapon's **root** is overridden; its own channels (bolt, trigger, magazine…)
still animate. Without the socket the gun barely follows the hands (measured
socket-vs-weapon error: Sprint 14.5 cm, Draw 18.2, Holster 21.7, Aim 67.8). The mount is
**rotation-only on purpose**: a translated mount (af4ad15, reverted in c01f451) pulled
the AK out of the hands.

### ADS sight centring — scene config, not code

With the rotation-only mount, `Aim` leaves the sight line ~5 cm left of and ~3 cm above
the `head` camera. That is corrected on the scene's controller:

```jsonc
// project/scenes/Sandbox.json, Player Spawn > First Person Controller
"View Model Offset":   [0.0562, -0.032, 0.0]   // camera frame: +x right, +y up, +z back
"View Model Rotation": [-0.24, 0.39, 0.0]      // Y-X-Z degrees, pivots on the head bone = the camera
"View Model FOV":      50.0
"Camera Bone":         "head"
```

"Aligned" means the front post's tip sits centred in the rear U-notch, flush with its
top edge, exactly on screen centre. Both sights were located from the mesh
(`work/sight_align_probe.cpp`, 0.5 mm heightmaps), not from bones:

| sight point (raw weapon frame) | x | y | z |
|---|---|---|---|
| rear notch, centre at shoulder height | −0.0682 | 1.559 | 0.4865 |
| front post, centre of tip | −0.0698 | 1.558 | 0.7235 |

That line is 0.39° right and 0.24° down of camera forward — hence the small rotation;
offset alone leaves the aligned sights ~4 px right / 2 px low. With both, the sights land
within 0.1 px of centre (confirmed in Play at View Model FOV 20). The offset/rotation
apply in every state, so hip fire shifts by the same ~6 cm / ~0.5°, which reads as
normal. All view-model fields are read in `Start()` — **Stop and Play again** after
changing them.

To re-measure (e.g. after re-exporting the rig):
`work\build_probe.bat sight_align_probe`, then
`work\sight_align_probe.exe <A_FP_ADS> <A_FP_Aim> <A_W_ADS> - 0 90 90 <offset xyz> <rotation xyz>`;
re-read the two sight points off the heightmaps first.
Trap: `ads_sight_probe.cpp` (in af4ad15) picks the highest mid-plane vertex as the front
sight — that's the top of the right-hand protective ear, ~5 mm off. Use `sight_align_probe`.

### Scene fields (`FirstPersonControllerComponent`)

| Inspector field | Default | Meaning |
|---|---|---|
| Animation Set | empty | `.fpsanim` path; empty = no view model |
| Camera Bone | `head` | arms-rig node pinned to the camera; empty = root-anchored |
| View Model Offset | 0,0,0 | residual nudge, camera frame |
| View Model Rotation | 0,0,0 | Y-X-Z degrees, applied after the asset's `viewRotation` |
| View Model Scale | 1 | must be finite and > 0 |
| View Model FOV | 60 | vertical FOV of the view-model sub-pass, clamped 20–150 in the Inspector |
| Gravity Gun | — | must be **off**: weapon input is only read when it is |

### View-model render pass

Entities with `ViewModelTag` are drawn last in `SceneRenderer::RenderScene`, after a
depth clear, with their own perspective at `ViewModelFov` (near/far shared with the
world pass), so world geometry can't clip hands 0.3 m from the eye and the weapon
doesn't stretch with the world FOV. They're skipped by the SSAO depth prepass and cast/
receive no shadows. The editor **Scene** tab passes no FOV, so there they draw as
ordinary geometry.

---

## 6. Orientation — `viewRotation`

The Manny rig comes out of Blender facing `-Y`; Blender's FBX axis conversion
(`(x, y, z) → (x, z, -y)`) makes that model **`+Z`**, while the engine camera looks down
**`-Z`** — exactly backwards. The correction is `"viewRotation": [0, 180, 0]` on the
**asset**, because it describes the axis convention of the two FBXs it names — not in
`ModelImportSettings` (per-FBX, and the two files must agree) or in every scene.

Axis reference used throughout: Blender (Z-up, cm) → engine (Y-up, m) is
`(x_b, y_b, z_b) → (x_b, z_b, -y_b) / 100`; a bone's world position in Blender is
`arm.matrix_world @ pose_bone.matrix`.

---

## 7. The asset pipeline

```
C:\Users\jacob\OneDrive\Desktop\AKS-74U 60fps (Revised).blend      (read-only ground truth)
        │  work/export_clip.py (headless Blender), ranges in export_manifest.json
        ▼
project/assets/fps/AKS74U/
├── AKS74U.fpsanim                       the state contract (15 states)
├── export_manifest.json                 which .blend, which frame range per clip
├── verification_report.json             per-FBX import check ("ok": true for all)
├── FirstPerson/
│   ├── AKS-74U_A_FP_ADS.fbx             armsModel: mesh + skeleton + REST bind pose
│   └── AKS-74U_A_FP_<State>.fbx         one per state: channels only (meshes=0)
└── Weapon/
    ├── AKS-74U_A_W_ADS.fbx              weaponModel: mesh + `AK` armature (bind = ADS pose)
    └── AKS-74U_A_W_<State>.fbx          one per state that has weapon motion (11 files)
```

### Export rules

1. **The base arms file is exported at REST** (`pose_position = REST`,
   `bake_anim = False`). Exporting it from a posed frame made the node tree disagree with
   the skin's bind data (`FPS_ANIMATION_INVESTIGATION.md`, fix 1).
2. **Clip FBXs are channel-only: `meshes=0`.** `Model::AttachClip` reads only channels;
   a meshed export adds stray meshes (`Mesh.001`, 267 nodes vs 265), doubles the file,
   and dragged in a material referencing a missing texture.
3. **Pair the weapon action with the arms action during the bake.** `CB_ik_hand_l`
   carries `CHILD_OF → AK:magazine` at influence 1.0 — the left hand is glued to the
   weapon's magazine bone, so the bake samples the weapon too:

   | arms clip | weapon action during the bake |
   |---|---|
   | `A_FP_<x>` | `A_W_<x>`, when that action exists |
   | Idle / Walk / Aim / Draw / Regrip (no weapon clip) | `A_W_ADS` — the weapon's bind pose *is* the ADS export |

   `work/export_clip.py` applies this automatically and prints `weapon action paired = …`.
   Getting it wrong is not subtle: the old Idle had the left hand chase a magazine
   being pulled out (223.6 mm of hand travel → **2.1 mm** after re-export), and
   `Mag_Check` shipped 22.8 mm off (`FPS_ANIMATION_INVESTIGATION.md` UPDATE 6). Check a
   clip with `work/hand_probe.cpp` or `work/bl_pair_check.py`.
4. **`export_manifest.json` is the frame-range record.** Its `frameStart`/`frameEnd` per
   action are what `export_clip.py` should be given. Nothing regenerates it or
   `verification_report.json`. Both date from the original export, so their `meshes` /
   `meshCount` fields predate rule 2. Keep the frame ranges accurate by hand when adding
   or re-cutting a clip.

### The `.blend` standing rule

**Always ask before changing anything in `AKS-74U 60fps (Revised).blend`.** Read-only
inspection is fine. Drive it headless — never through an interactive Blender holding a
dirty scene:

```powershell
& "C:\Program Files\Blender Foundation\Blender 5.1\blender.exe" --background `
    "C:\Users\jacob\OneDrive\Desktop\AKS-74U 60fps (Revised).blend" --python work\bl_inspect.py
```

The file is saved on the neutral pairing `Armature` → `A_FP_Idle`, `AK` → `A_W_ADS`
(it used to be saved on `A_FP_Tac_Reload`/`A_W_Tac_Reload`, which baked the broken
batch). `export_clip.py` never saves it; `work/set_neutral_action.py` is the only
script that writes it, and `AKS-74U 60fps (Revised).blend.pre-neutral.bak` is the
rollback.

---

## 8. Invariants — the fixes this branch landed, and how to not undo them

| # | Symptom | Root cause | Fix (keep it) |
|---|---------|-----------|-----|
| 1 | Arms fine at bind pose, "fan of blades" the instant a clip played | `Model::ProcessMesh` skipped the node's world transform for skinned meshes, while Assimp's `mOffsetMatrix` expects root-space vertices | `bake = nodeTransform` for **every** mesh |
| 2 | Arms + weapon ~1 m above the camera | Root parked on the camera; rig is authored standing | Camera-bone anchor (§5) |
| 3 | Arms rendered behind the camera | Rig faces model `+Z`, camera looks `-Z` | `viewRotation [0,180,0]` (§6) |
| 4 | Arms + gun blinked to a T-pose at the end of every one-shot | `Once` clears the clip → bind pose (the Manny T-pose) | Non-looping states hold their last frame (`ClampForever` sampling); `Holstered` holds Holster's end |
| 5 | Spare magazine hard-cut in/out around weapon clips | A cut to bind; `mag2` is 121.8 mm off bind at both clip ends | A state with no weapon clip is an *empty motion*, which the animator crossfades to bind like any other pose |
| 6 | Spare magazine invisible in the engine | assimp's `JoinIdenticalVertices` ignored bone weights, merged the two coincident magazine islands | Local assimp patch (`tools/assimp_patches/0001-…`) |
| 7 | Gun lagged the hands by up to 68 cm | Weapon clips never move the gun's root | Weapon socket (§5) |
| 8 | New default keys did nothing | Saved `settings.json` input list replaced `Defaults()` wholesale | `InputMap::MergeDefaults` |

### 1. The bake rule (the invariant most likely to be broken by accident)

```cpp
const glm::mat4& bake = nodeTransform;   // ALWAYS — skinned or not
palette[i] = GlobalInverseTransform * PosedGlobal[i] * BoneOffset[i]
```

The palette never mentions the mesh node's own frame, so the bake is the only thing
that puts vertices into root space before skinning. At bind pose the whole palette
collapses to one shared matrix, so a missing node transform just reorients the mesh;
only when a clip plays does each bone apply its own delta and tear it apart. The
winding-flip correction for negative-determinant nodes applies to skinned meshes too.

**`C = RestGlobal · BoneOffset` is not an error term — do not cancel it.** Assimp builds
`mOffsetMatrix = inverse(TransformLink) * absolute_transform`, so `C` is the node tree's
**pose-versus-bind delta**: `I` for the rest-exported arms base, varying per bone for
clip FBXs (frame-0 pose), and `≈ translate(-0.069, 1.503, 0.446)` for the weapon base
(the ADS pose — the gun-socket delta). Applying `C⁻¹` was tried and dropped the weapon
to the floor. `mag2`'s `C` is an outlier on purpose.

### 3. Hold the last frame, never drop to bind

`Once` clears the clip at the end, so the model falls back to the base FBX's bind pose, the
Manny T-pose (`ik_hand_gun` 678 mm off). That lasts a frame, and the next crossfade travels
through it. The animator samples non-looping states with `ClampForever`, so their last frame
holds until a transition takes over. For the same reason `Holstered` plays Holster's clips
held at their end: Draw then fades out of the holstered pose, not out of the T-pose. The
model's own `PlayAnimation` / `Once` contract is untouched for other callers.

### 5. Fade to bind, don't cut

A state with no weapon clip is an empty motion on the weapon track. On the base layer that
means "bind pose", and it goes through the same crossfade stack as any clip, so the spare
magazine fades rather than pops. The clip files were not touched.

### 6. The assimp patch — a fresh clone depends on it

assimp comes from GitHub at a pinned `GIT_TAG`. Stock v5.4.3's dedup key ignored bone
weights, so the two coincident magazine islands merged and the losing bone kept 17,594
stale vertex ids (193,534 dropped weights over 27 files; all 11 weapon FBXs affected,
arms clean). `tools/apply_assimp_patches.cmake` applies the patch at configure time.
Consequences: (a) `build/_deps` must be configured before probes link against it;
(b) bumping `GIT_TAG` fails configure loudly until the patch is rebased; (c) don't swap
it for `importer.optimizeGraph=false` — that costs +348% verts vs +18.7%. `*.patch` is
checked out LF (`.gitattributes`) because `git apply` needs it.

---

## 8b. The player's body (true first person, #405)

The player can have a full body under the camera: the MC Core Motion locomotion clips walk it by
root motion, and the camera rides in its head. It shares the UE5 mannequin skeleton with the AK's
first-person clips (down to the fingers and `ik_hand_gun`), which phase 2 builds on.

**Setup (the Sandbox's Player Spawn has one).** A root object with a **First Person Body**
component, its children the body pieces - `Quantum_Head`, `_Torso`, `_UnderPants`, `_Legs`,
`_Feet` today, clothing later - each a rigged model with an Animator Controller on
`animations/fps_body_locomotion.controller`. The first piece with an Animator Controller drives;
the others follow it (`AnimatorControllerComponent::Driver`, set in Play). Pieces whose names
match **Hidden Parts** (default `Head`) cast shadows but aren't drawn.

Step-by-step setup, the controller contract and tuning: `BODY_SETUP.md`.

**Per frame** (`FirstPersonBody`, `src/Game/FirstPersonBody.h`), around `Player::Update`:

1. `BeforePlayerMove` takes the camera back out of the head and hands the Player last step's root
   motion (`Player::RootMotionVelocity`, weighted `1 - Responsiveness`; none while airborne).
2. `Player::Update` looks and sweeps the capsule with the blend of root motion and input.
3. `Tick` stands the body at the capsule's feet facing the camera's yaw and sets the controller's
   `MoveX` / `MoveY` (the input in the body's frame, m/s, smoothed), `Speed`, `Sprint`,
   `Grounded`, `Airborne` (off the ground > 0.15 s) and `Jump`.
4. The animators run: the driver's root motion is **In Place** (stripped from the pose, reported).
5. `LateUpdate` takes the reported travel for the next move and puts the camera in the head: the
   head bone plus **Camera Offset**, **Head Bob** of its motion, smoothed in the body's frame.

**The controller.** A 2D blend tree of idle plus walk and jog in eight directions and run forward,
each child at its clip's measured velocity (walk forward 1.53 m/s, jog forward 3.26, jog right
2.95, jog backward 2.26, run 4.72), plus Jump / Fall / Land. The Player's move speeds become
**Run Speed** / **Sprint Speed** so the input asks for what the clips have.

**Phases 2 and 3.** The body has arms (the `ArmsPiece`, an arms-only Quantum piece). It copies the
arms rig's arm shapes and solves its hands to the rig's hands (`FirstPersonBody::ArmsLateUpdate`),
the camera is anchored to the shoulders while armed (`kEyeSlack`, `kReachSlack`, the shrug), and it
keeps that height unarmed. Drawn, the arms take the rig's hands from the first frame; holstered
they leave the view-model pass at once. Phase 3 added turn in place with a turn-rate cap, start /
stop clips, crouch, foot IK with foot lock, stair easing and jump / land polish.

**The arms rig is a hidden pose source.** It still runs the weapon's controller, the IK and the
ADS carry (the sights must stay camera-locked, which the body's swaying arms cannot do), but it
carries `PoseSourceTag`: never drawn, no shadow, no SSAO depth, no search tint. The body's arms
are what is seen and what casts the arm shadow (before, both cast: a double shadow). Retiring it
fully (the weapon controller on the body's arms, the gun on the body's `ik_hand_gun`) was looked
at and set aside - see issue #424 for why.

## 9. Known gaps / next steps

1. **One weapon per player.** The two slots are "1 = the definition, 2 = unarmed". A real
   inventory means several definitions on the controller and swapping them on
   Holster → Draw. `FPS_WEAPON_INTEGRATION.md` §6 lists what that needs. Per-weapon
   numbers and animation logic are already data (the `.fpsanim` gameplay block and the
   `.controller`).
2. **`Idle`, `Walk`, `Aim`, `Draw`, `Regrip` have no weapon clips** — the `A_W_Idle` /
   `A_W_Walk` actions don't exist in the `.blend` (verified with
   `work/bl_idle_probe2.py`), so closing the gap means authoring animation. Ask first.
3. **No ADS fire / aim-walk clips.** Both are procedural (§4) and tied to the `ADS` tag.
   If they're authored, add them to the graph and drop the tag from that state.
4. **View-model fields are captured at `Start()`**, not live-tunable — Stop/Play.
5. **The camera sits on the `head` bone origin** (skull base, 1.557 m) rather than the
   eyes; the ADS offset absorbs it. There is no eye bone on this rig.
6. **No HUD** — ammo and fire mode are Console-only (`Ammo()`, `IsFullAuto()` are there
   for one).
7. **The FPS scene is not in CI.** CI's smoke test runs `tests/smoke-scenes/`; the FPS
   scene lives in `project/scenes/` and is only smoke-tested locally. Adding an FPS smoke
   scene would gate `.fpsanim` loading and clip attachment on every push.
8. **Materials/textures are out of scope.** Untextured rendering is expected;
   `Texture: failed to load ...` and `Y Bot.fbx` import errors are known noise.
9. **The true-FPS body has its phases 1-3 merged** (#405): body, weapon arms, turn in place, start/stop,
   crouch, foot IK. See §8b, `BODY_SETUP.md` (setup and tuning) and issue #426 (authoring tools).

---

## 10. Verifying changes

```powershell
cmake --build build --config Release -- /m /v:minimal        # close the editor first (LNK1104)
$p = Start-Process build\Release\TartarusEngine.exe -ArgumentList "--unit-tests" -Wait -PassThru `
       -RedirectStandardOutput build\probe\ut.txt; $p.ExitCode   # 0 = all checks passed
$p = Start-Process build\Release\TartarusEngine.exe -ArgumentList "--smoke-test","project\scenes" -Wait -PassThru `
       -RedirectStandardOutput build\probe\smoke.txt
```

Release is a GUI-subsystem binary: run it through `Start-Process -Wait` with redirected
output or you get no output and no exit code.

Expected: unit tests exit 0. The AK is tested in `Sandbox.json` (its Player Spawn carries
it); Sandbox and `Apartment` need the gitignored Mixamo characters and HDRI sky locally, or
their smoke runs fail on the missing assets.

CI (`.github/workflows/build.yml`) additionally runs
`tools/check_component_registration.py`: any new `*Tag`/`*Component` struct in
`Components.h` must be registered or allow-listed.

Open `AKS74U.controller` in the Animator during Play to watch states change live.
Manual pass in the Game tab: hip fire, ADS fire (semi + full), walk while aiming, sprint
out of ADS, tap/hold R at 30 / 15 / 0 rounds, F, Q, 1/2/scroll mid-action, idle for 20 s.

### Editor / screenshot gotchas

- The editor reopens the last scene from `%LOCALAPPDATA%\TartarusEngine\editor_prefs.json`
  (`lastScenePath`); back it up before letting a script drive the editor.
- Play-mode edits revert on Stop — write tuned values into the scene file.
- A full screenshot read back is a **downscaled preview**; derive click coordinates from
  full-resolution crops.
- Use `Window → Console`, not the notification bell (it truncates).
- The Game tab's `cam`/`yaw`/`pitch` overlay shows what the player camera is really doing.

---

## 11. Standalone probes (`work/`)

C++ probes link directly against the **prebuilt, patched** Assimp + glm in
`build\_deps\` — no engine rebuild, no editor. Preferred for any import/skinning
hypothesis. Build one with `cmd /c "work\build_probe.bat <name>"` (vcvars, `/MD`,
include/lib paths). **Must be `/MD`**: the prebuilt lib is `MD_DynamicRelease` despite
the `-mt` in its name. Binaries, logs and re-exported FBXs under `work/` are gitignored;
only sources are tracked.

| Probe | What it answers |
|-------|-----------------|
| `pose_probe.cpp` | Arms/weapon bind + posed bounding boxes — found the `C` misunderstanding, verified the bake fix |
| `bake_audit_probe.cpp` | Per-skinned-mesh: what transform `ProcessMesh` actually bakes |
| `forward_skin_probe.cpp`, `full_skin_probe.cpp` | CPU forward skinning of a mesh |
| `clip_coverage_probe.cpp` | Which bones a clip drives |
| `bone_dump.cpp` | Per-bone `|A−I|`, `|C−I|`, `C`'s translation |
| `nodes.cpp` | Node-name search; `--chain <node>` prints an ancestor chain (how `head` was found) |
| `hand_probe.cpp` | Per-bone motion through a clip: left-vs-right path, loop seam, undriven bones. Found the left-hand pairing bug |
| `bind_probe.cpp` | Bind pose vs each clip, node by node. Found the T-pose blink |
| `socket_probe.cpp` | Socket-vs-weapon-root error per clip; derived `weaponMountRotation` |
| `sight_align_probe.cpp` | Sight heightmaps + solved ADS offset/rotation (§5) |
| `fbx_info.cpp` | Nodes, meshes, takes, per-bone weights inside an FBX |
| `mag_probe.cpp` | Out-of-range weight audit, post-process flag bisect. Proved the assimp vertex-join bug |
| `align_probe.cpp`, `diag_probe.cpp`, `assimp_probe.cpp`, `bone_match_probe.cpp`, `dup_name_probe.cpp` | Earlier investigations — see `FPS_ANIMATION_INVESTIGATION.md` |
| `bl_inspect*.py`, `bl_idle_probe*.py`, `bl_pair_check.py`, `bl_mag_probe.py` | Read-only Blender ground truth |
| `export_clip.py` | Clip export with the pairing rule: `blender -b "<blend>" --python work\export_clip.py -- <action> <out.fbx> <start> <end> <meshes 0\|1> [weapon\|auto]` |
| `inspect_blend.py`, `preflight_neutral.py`, `set_neutral_action.py` | Read the `.blend`'s saved state / (only with approval) park it on the neutral pairing |
| `extract_frames.py` | Frames from a Play recording via Blender's VSE (no ffmpeg here) |

**Gotcha:** `AiToGlm` must be a *direct element copy*; `glm::make_mat4(&m.a1)` silently
transposes Assimp's row-major matrix. Copy the one in `work/pose_probe.cpp`. And the
probes replicate `Model.cpp`, they are not it — they omitted `ProcessMesh`'s vertex
placement, which is exactly where the original bug was.

---

## 12. Things that will bite you

- **The weapon is socket-parented, not co-posed.** Arms get the camera-derived pose; the
  weapon's root is solved onto `weaponSocket` every frame. A gun that drifts from the
  hands is a socket/mount/pairing problem, not placement math.
- **Parameter, tag and event names are the API** (`FirstPersonAnimatorContract`). State
  names are free, but renaming the `Fire` *parameter* or dropping the `ADS` tag silently
  unhooks that behaviour.
- **Parameters the driver sets every frame** (Speed, Aim, …) overwrite whatever you type
  into the Animator's live Parameters panel.
- **Input defaults vs `settings.json`.** The saved list wins per action.
- **View-model fields don't hot-reload** — Stop/Play.
- **Don't re-check what `FPS_ANIMATION_INVESTIGATION.md` ruled out** without new evidence.
- **Never touch the `.blend` without asking.**
