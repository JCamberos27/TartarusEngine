# CAS parity: gap matrix

Goal: our IK and procedural animation matches Kinemation's Character Animation System (CAS) feature for feature.
Rule that holds for every row: authored clips play untouched; every correction is a layer on top, toggleable and weighted 0..1.

Version tags: **CAS** = Character Animation System (Unity asset, current, docs at kinemation.gitbook.io/character-animation-system-docs).
**FPSAF** = FPS Animation Framework 4 (its predecessor, now "Legacy", kinemation.gitbook.io/fps-animation-framework). CAS absorbed FPSAF's
layers as modifiers (see its "FPSAF -> CAS guide"). A feature tagged FPSAF is described in the legacy docs.

Sources (public docs; inspector fields below are as the docs list them, magnitudes are not published):
- CAS: [architecture](https://kinemation.gitbook.io/character-animation-system-docs/character-animation-system/architecture),
  [layered blending](https://kinemation.gitbook.io/character-animation-system-docs/character-animation-system/layered-blending),
  [look](https://kinemation.gitbook.io/character-animation-system-docs/character-animation-system/animation-modifiers/look-modifier),
  [foot IK](https://kinemation.gitbook.io/character-animation-system-docs/character-animation-system/animation-modifiers/foot-ik),
  [two bone IK](https://kinemation.gitbook.io/character-animation-system-docs/character-animation-system/animation-modifiers/two-bone-ik),
  [stride](https://kinemation.gitbook.io/character-animation-system-docs/character-animation-system/animation-modifiers/stride-modifier),
  [step](https://kinemation.gitbook.io/character-animation-system-docs/character-animation-system/animation-modifiers/step-modifier),
  [pivot](https://kinemation.gitbook.io/character-animation-system-docs/character-animation-system/animation-modifiers/pivot-modifier),
  [IK motions](https://kinemation.gitbook.io/character-animation-system-docs/fps-addon/modifiers/ik-motions),
  [ADS](https://kinemation.gitbook.io/character-animation-system-docs/fps-addon/modifiers/ads-modifier),
  [sway](https://kinemation.gitbook.io/character-animation-system-docs/fps-addon/modifiers/sway-modifier),
  [weapon collision](https://kinemation.gitbook.io/character-animation-system-docs/fps-addon/modifiers/weapon-collision),
  [FPS offset](https://kinemation.gitbook.io/character-animation-system-docs/fps-addon/modifiers/fps-offset),
  [attach hand](https://kinemation.gitbook.io/character-animation-system-docs/fps-addon/modifiers/attach-hand),
  [recoil](https://kinemation.gitbook.io/character-animation-system-docs/fps-addon/recoil-animation).
- FPSAF: [look layer](https://kinemation.gitbook.io/fps-animation-framework/fundamentals/animation-layers/look-layer),
  [leg IK](https://kinemation.gitbook.io/fps-animation-framework/fundamentals/animation-layers/leg-ik),
  [sway layer](https://kinemation.gitbook.io/fps-animation-framework/fundamentals/animation-layers/sway-layer),
  [ADS layer](https://kinemation.gitbook.io/fps-animation-framework/fundamentals/animation-layers/ads-layer),
  [left hand IK layer](https://kinemation.gitbook.io/fps-animation-framework/fundamentals/animation-layers/left-hand-ik-layer),
  [pose blending](https://kinemation.gitbook.io/fps-animation-framework/fundamentals/animation-layers/pose-blending),
  [weapon collision](https://kinemation.gitbook.io/fps-animation-framework/fundamentals/animation-layers/weapon-collision),
  [recoil layer](https://kinemation.gitbook.io/fps-animation-framework/fundamentals/animation-layers/recoil-layer),
  [locomotion layer](https://kinemation.gitbook.io/fps-animation-framework/fundamentals/animation-layers/locomotion-layer),
  [curve blending](https://kinemation.gitbook.io/fps-animation-framework/fundamentals/animation-system/curve-blending).
- Not read: the Asset Store page, videos, forum threads. The docs do not publish CAS's default numbers or the exact
  turn-in-place and stride maths, so those rows describe behaviour only.

Our side: `IK.cpp` (`SolveTwoBone`, `AimBone`, `ApplyRig`), `IKRigComponent` (`Components.h`), `FirstPersonProcedural.{h,cpp}`,
`FirstPersonBody.cpp`, `Npc/NpcBody.cpp`, `AnimatorController.cpp`; docs `PROCEDURAL_ANIMATION.md`, `FPS_ANIMATION_SYSTEM.md`, `BODY_SETUP.md`.
Line numbers are from commit 32d7e727 and drift; search by symbol.

## Matrix

Parity: full / partial / missing. "Hardcoded" = a constant in code a designer cannot reach from the Inspector.

| CAS feature (version) | What CAS does | Ours | Parity | Hardcoded in ours |
|---|---|---|---|---|
| Layered Blending: per-chain Base / Additive / Local weight (CAS) | Bone chains (lower body, spine, head, arms, fingers); each overlay layer has three weights; chain weights authorable as clip curves (`Layering_*`) or constant. | `AnimatorController::Layer` (`AnimatorController.h` ~L110): Override or Additive, one Weight, include/exclude bone mask; `ApplyAdditive` (`.cpp` ~L908). | partial | No per-layer Local weight, no named bone chains, no per-chain curve-driven weight. |
| Curve-driven layer weights (CAS Mask_* / Enable_* curves; FPSAF MaskLookLayer, MaskLeftHand, WeaponBone, Overlay) | Clip curves toggle look, left-hand IK, foot IK, procedural layer and weapon-bone follow per frame, so each clip declares which corrections it allows. | Weapon: `IKOff` state tag fades IK (`WeaponIKSettings`); state offsets by name/tag. Clip float curves as weights: missing. | partial | Per-state tags only; no per-clip, per-frame weight. |
| Look layer: pitch/yaw spread over spine bones (CAS Look Modifier, FPSAF Look Layer) | Look angles distributed over a bone list (enter one value, the rest follow proportionally); per-bone max angle; pelvis alpha and lerp speed; per-weapon aim-offset assets. | Player: `SpineAim`, `SpineAimDown`, `SpineTwist` spread evenly over spine_01..05 (`FirstPersonBody.cpp` ~L1478). NPC: spine twist + head yaw/pitch (`NpcBody.cpp`). `IK::AimBone` (IK.cpp ~L220). | partial | Even spread (no per-bone weight or max angle); no pelvis alpha; no per-weapon aim offset; NPC kMaxTwist 1.2 rad, kHeadMaxYaw 1.2, kHeadMaxPitch 0.6, kAimLean 0.1/0.22/0.4. |
| Lean via look roll (CAS roll input; FPSAF Leaning tab: angle + interp speed) | Roll input leans the spine; leaning reuses the look-right aim offsets. | `WeaponLeanSettings` (camera roll/offset, gun roll, corner peek) drives the camera; NPC `m_Lean` eased. Spine lean from the lean key on the player body: missing. | partial | NPC lean ease 0.12 s (`NpcBody.cpp` ~L354). |
| Turn in place (CAS Look Modifier) | Yaw past a threshold (45-90 deg) plays Turn L/R animator states while the upper body keeps its pose. | Player: `TurnThreshold`, `MaxTurnRate`, `TurnLagFloor`, ... (`FirstPersonBodyComponent`). NPC `NpcShouldTurn`. | full (clip-driven) | NPC kTurnThreshold 1.15 rad. |
| Stride warping (CAS Stride Modifier) | Warps leg yaw and stride length to the velocity (needs generated stride curves), pelvis roll, hip rotation. | none (Responsiveness only scales clip play rate). | missing | n/a |
| Foot pinning (CAS Stride pinning) | Locks feet in world during contact to stop sliding. | Foot lock in the player body (`FootLockDrift`, `FootLockEaseIn/Out`, `FootPlantedHeight`). | partial | No yaw/angular pin thresholds, no leg stretch limit. |
| Foot IK (CAS Foot IK, FPSAF Leg IK) | Ray per foot (layer mask, foot height/radius, ray offset), pelvis corrected, interp speed. FPSAF: trace length, height offset, interp speed. | Player `FootIK` + ray, raise, drop, tilt, ease fields (`Components.h` ~L650-665). NPC `FootPass` (`NpcBody.cpp` ~L245). Ground-normal tilt: yes. | full (player), partial (NPC) | NPC: kMaxDrop .35, kMaxRaise .35, kPelvisRaise .08, kTiltMax .5 rad; no foot radius/height or sphere cast. |
| Step modifier (CAS) | Procedural step (lead/follow foot + pelvis curves) on stance change, stop and turn; docs suggest max stride 0.3-0.4, 45 deg, cooldown 0.3-0.4 s. | Clip-based start/stop/turn clips only. | missing | n/a |
| Pivot modifier (CAS) | Spine rotation + pelvis translation on start/stop, lean toward velocity (max angle, intensity). | none | missing | n/a |
| Two-bone IK with hint/pole (CAS Two Bone IK) | Tip, target (bone or world), hint weight, pole target, hint offset, max limb length scale. | `IK::SolveTwoBone` (IK.cpp ~L160) keeps the animated bend plane and straightens when out of reach; `IKLimb`: Weight, KeepAnimatedOffset, MatchRotation, runtime Swivel and GoalMove. | done (round 2) | `TwoBoneHint` (pole bone, HintWeight, HintOffset, MaxLimbScale) on `SolveTwoBone` and `IKLimb`; defaults identical to before. |
| Per-limb IK weights, curve-driven (CAS Enable_HandR/L_IK, Enable_FootR/L_IK) | Each limb's IK weight authored per clip. | `IKLimb::Weight` static; `IKOff` tag; per-state weight curves (`IK`, `IK_RightHand`, `IK_LeftHand`, `Look`) on the weapon arms. | partial | `FootIK` curve and FBX curve import. |
| Attach hand + grip pose (CAS Attach Hand) | Left hand to a weapon attach transform, finger-chain pose clip, fallback default hand. | Hands follow the gun via KeepAnimatedOffset; the grip is whatever the clip authored. No separate grip/finger pose. | partial | n/a |
| FPS Offset (CAS) | Component-space offsets for weapon bone and each hand relative to weapon; several allowed, weight-overridable (e.g. first-person only). | `WeaponStateOffset`, `WeaponAimSettings` offsets. Separate hand-vs-gun offsets: missing. | partial | n/a |
| Weapon bone follow (FPSAF WeaponBone curve) | Weapon follows the right hand or the IK weapon bone, switchable by clip curve. | Gun bone (`ik_hand_gun`) driven by the procedural stack. Per-clip switch: missing. | partial | n/a |
| ADS: aim point, hip target, aim target, absolute/additive blend, camera blend (CAS, FPSAF ADS layer) | Aligns sights to the camera aim target; position/rotation blend between absolute and additive; ease modes; aim point speed; crouch pose. | `WeaponAimSettings`: offset blended by the `ADS` tag, `BlendTime`, curve; keeps the Aim clip's sight picture. | partial (round 3: separate blends, crouch pose, camera share) | No aim-point transform. |
| IK motions: additive bone curves on demand (CAS IK Motions) | Rotation/translation curves, scales, blend time, play rate, auto blend out; aim-in, equip jiggle, etc. | `WeaponStateOffset` (static offset, blend in/out); recoil curves per shot. Triggered curve motions: missing. | partial | n/a |
| Recoil animation (CAS, FPSAF Recoil Layer) | Six curves (pitch/roll/yaw + translation) starting and ending at zero; auto curves with a fire-rate delay point; min/max targets, aiming multiplier, auto/burst smoothing, noise layer, pushback layer, play rate, smooth roll. | `WeaponRecoilSettings` (`FirstPersonProcedural.h` L42-119): per-shot curves, spread, bias, wander, burst growth, ADS/hip scale, camera scale, aim recovery, shake, FOV punch, bolt. | full (superset) | n/a |
| Sway: spring look + move sway, free aim dead zone (CAS Sway, FPSAF Sway Layer) | Position and rotation springs (damping, stiffness, speed, scale, space); free-aim dead zone; curve additive with ADS scale. | `WeaponSwaySettings` (look, move, `FirstPersonSpring`, ADS scale). Free-aim dead zone: `FreeAimZone`. | full | n/a |
| Bob / breathing / jump-land / camera motion | Not named layers in CAS; clips or user code. | `WeaponBobSettings`, `WeaponBreathSettings` (exertion, drift), `WeaponJumpSettings`, `WeaponCameraMotionSettings`. | ours exceeds | n/a |
| Weapon collision (CAS, FPSAF) | Sphere trace along weapon length (start offset, trace radius, layer mask); blends to primary pose (looking up) or secondary pose; interp speed. FPSAF: threshold + rest pose. | `WeaponObstructionSettings` (reach, retract, tuck, side rotation, corner peek, optional block). | full | Probe radius and layer mask not per-weapon (only `Reach`). |
| Pose blending / default pose (FPSAF Pose Blending, CAS layered blending) | Blend asset with per-bone base weight and anim weight so a pose blends over the animation (zero anim weight during reload). | Layer bone masks give a coarse version; no per-bone weight asset or named pose. | partial | n/a |
| Mirroring (CAS) | Mirror poses and animations. | none found in `IK` or `AnimatorController`. | missing | n/a |
| Free look (decoupled head/aim from body) | Look modifier yaw/pitch inputs, delta mode. | NPC head independent of chest (kHeadMax*). Player free look: none. | partial | n/a |
| Dynamic bones (CAS) | Spring bones for secondary motion. | Not found in the files audited (cloth/ragdoll lanes not checked). | missing | n/a |
| Full-body IK (CAS) | Full-body solver for hand/foot targets. | none | missing | n/a |
| Property bindings (CAS) | Bind gameplay values to modifier fields. | Fields set from code each frame; no bindable data layer. | partial | n/a |

## Ranked gaps

Impact on visible realism, then effort (S under a day, M a few days, L a week or more).

### Little wins

1. **DONE (round 2) - Pole/hint on two-bone IK (S-M).** Optional `Pole` bone, `HintWeight`, `HintOffset`, `MaxLimbScale` on `IKLimb` and `SolveTwoBone`; defaults reproduce today's keep-animated-bend. Fixes elbow/knee flips.
2. **DONE (round 2) - Per-bone look weights (S).** Replace the even spread (`FirstPersonBody.cpp` ~L1478, `NpcBody`) with a per-bone weight list plus max angle; default = even.
3. **Expose NPC hardcodes (S).** kMaxTwist, kHeadMax*, kAimLean*, kTurnThreshold and FootPass kMax* into a reflected NPC body component. Best tunability for the effort.
4. **DONE (round 3) - ADS pieces (S).** Separate position/rotation absolute-vs-additive blend, crouch pose, camera blend. `WeaponAimSettings::PositionAdditive/RotationAdditive` (1 = old), `CrouchPosition/Rotation/BlendTime` (fed by `FirstPersonPresentation::SetCrouch`), `CameraShare`.
5. **DONE (round 2) - Hand-vs-gun offsets (S).** Right/left hand offset fields on `WeaponIKSettings`.
6. **DONE (round 3) - Free-aim dead zone (S).** Sway option: the view turns inside a zone before the gun follows. `WeaponSwaySettings::FreeAimZone` (yaw, pitch degrees; 0 = off), `FreeAimReturn`, `FreeAimAdsScale`.
7. **DONE (round 3) - Per-clip weight curves (M, high value).** Named float curves authored on Animator Controller states (`State::Curves`, JSON `curves`, keys over normalized state time) are sampled by `AnimatorSampleCurve` (crossfade-blended; none = 1) and scale the weapon IK: `IK` (all), `IK_RightHand`, `IK_LeftHand`, `Look` (names in `WeaponIKSettings`, `UseClipCurves`). `IKOff` still works and multiplies in. Not done: curve import from FBX custom properties (Model importer is outside lane A) and a `FootIK` consumer (NPC/body feet); the curve editor UI.

### Big items

8. **Stride warping + foot pinning (L).** Biggest realism gain for sprint/strafe sliding; needs stride-curve preprocessing of the Quantum clips and a trajectory component.
9. **Step and pivot modifiers (M-L).** Procedural steps on stance change/turn/stop; spine and pelvis response to start/stop. Today only clip-based.
10. **Triggered IK motions (M).** Curve-asset additive bone motions on demand (equip, aim-in jiggle, hit reactions).
11. **Mirroring (M-L).** Pose mirror table per skeleton; low priority unless left-handed play matters.
12. **Layered blending chains (M).** Named bone chains, per-layer Base/Additive/Local weights.
13. **Full-body IK / dynamic bones (L).** Only if hit reactions and cloth need them.

Unaudited: player free look and dynamic bones; those rows say "none" only because nothing was found in the files listed above.
