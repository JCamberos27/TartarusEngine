# Setting up the player's body (First Person Body)

How to give the player a full body under the camera, what it needs, and what to change when
something looks wrong. The design and per-frame order are in `FPS_ANIMATION_SYSTEM.md` §8b; this
file is the hands-on guide. The reference setup is the Sandbox scene's **Player Spawn** and
`project/animations/fps_body_locomotion.controller`.

## 1. What you need

| Piece | Notes |
|---|---|
| A **First Person Controller** on the player object | The capsule, look, weapon. The body needs it. |
| A **First Person Body** component on the body's root object | Usually the same Player Spawn root. |
| **Rigged pieces** as children of that root | Head, torso, legs, feet (or clothing) - each a rigged model on the *same skeleton* (UE5 mannequin names). |
| One piece with an **Animator Controller** | The first piece that has one **drives**; the others follow it in Play. |
| A **locomotion controller** on that piece | Must follow the contract below. Copy `fps_body_locomotion.controller`. |
| Optional **arms piece** + a weapon | For Weapon Arms (the body's arms hold the gun). |

The **Setup** box at the top of the First Person Body Inspector checks all of this and names what is
missing (and which feature quietly turns off because of it). Clear every warning before tuning.

## 2. The controller contract

The body sets these parameters every frame and watches these states by name. The table is generated
from `src/Game/FirstPersonBodyContract.h` (`FPBody::Params()`, `States()`, `Bones()`): if the two
ever disagree, the header wins.

**Parameters** (Float unless noted): `MoveX`, `MoveY` (input in the body's frame, m/s, smoothed),
`Speed`, `Sprint` (Bool), `Grounded` (Bool), `Airborne` (Bool, off the ground > `Airborne Delay`),
`Jump` (Trigger), `Turning` (Bool), `TurnAngle` (degrees the view is off the body), `Moving` (Bool),
`Start` / `Stop` / `StopRun` (Triggers), `StartX` `StartY` `StopX` `StopY`, `Crouched` (Bool),
`CrouchDown` / `CrouchUp` (Triggers).

**States:** `Locomotion`, `Jump`, `Fall`, `Land` (always), `Turn`, `CrouchTurn` (Turn In Place),
`Start`, `Stop`, `StopRun` (Start/Stop Clips), `CrouchLoco`, `CrouchDown`, `CrouchUp` (Crouch).

**Tag:** `Airborne` on the states where the feet are off the ground (Jump, Fall) - foot IK reads it.

**Bones** (UE5 mannequin names): `pelvis`, `spine_01..05`, `clavicle/upperarm/lowerarm/hand _l/_r`,
`thigh/calf/foot _l/_r`, and the **Head Bone** (default `head`). Missing bones switch the feature that
needs them off; the Setup box says which.

Check any controller against this in the Animator: **Lint** tab > "Also check it as a first-person body
controller".

## 3. Step by step

1. Put the rigged pieces under one root object. Give the root a **First Person Controller** and a
   **First Person Body**.
2. Give one piece an **Animator Controller** pointing at a copy of `fps_body_locomotion.controller`.
   Set its Root Motion mode to **In Place** (the body forces it in Play anyway).
3. Open the Setup box and fix what it lists (missing states/parameters/bones, names that match
   nothing, a second body in the scene).
4. Press Play. The camera sits in the head. Turn on **Gizmos > Player body** to see what the body is
   doing (section 5).
5. Enable features one at a time, checking the Setup box after each: Turn Threshold, Start/Stop Clips,
   Crouch Height, Foot IK, Weapon Arms.

## 4. Settings and what to tune when

Everything in **(advanced)** groups has a tooltip; defaults are the values the body was tuned with.

| Symptom | Change |
|---|---|
| Body feels floaty / sluggish to start and stop | **Responsiveness** up (0 = the clips move you, 1 = the input does). |
| Feet slide when the view turns fast on the spot | Lower **Max Turn Rate**; check the turn clips with the Animator's clip analysis. |
| Body turns too early / too late while standing | **Turn Threshold** (degrees off before it steps round), **Turn Lag Floor**, **Turn End Angle**. |
| Turn clip keeps replaying | **Turn Min Time**, **Turn Timeout**. |
| Stop clip plays on a tap | **Stop Min Run Time**, **Stop Min Speed**, **Stop Debounce**. |
| Push-off plays when the move is short | **Start Idle Time**, **Start Max Move**. |
| Camera bounces on stairs | **Head Bob** down, **Camera Smoothing** up; **Stair Pop Rise/Rate/Ease** for the pop detector. |
| Left hand detaches from the gun | **Eye Slack**, **Reach Slack**, **Shrug Start / Max** (Weapon Arms). |
| Camera rises or drops on holster | **Arms Ease Out**; the body keeps the armed eye height unarmed. |
| Feet float over steps / sink into slopes | **Foot IK**, **Foot Ray Up/Length**, **Foot Max Raise**, **Pelvis Max Raise**, **Foot IK Max Drop**. |
| Foot slides while planted | **Foot Lock Drift**, **Foot Planted Height**, **Foot Lock Ease In/Out**. |
| Run/Sprint speed doesn't match the feet | Set **Run Speed** / **Sprint Speed** to the ground speed of the jog/run clip - the Animator's clip analysis measures it. |

## 5. Debugging tools

- **Setup box** (Inspector): contract check, live.
- **Live (Play)** section (Inspector): current state, `Turning`/`TurnAngle`/`Moving`, the last trigger fired.
- **Gizmos > Player body** (toolbar, Play): body vs view heading, turn wedge, root-motion arrow, stair
  detector line, foot rays with hit points and planted/locked markers, eye and shoulder cross.
- **Animator window**:
  - **Lint** tab: structure and contract checks, click an issue to select the node.
  - **History** tab (Play): every transition taken, and the conditions and parameter values behind it.
  - **Analyse** (state panel > Root Motion): travel, ground speed, foot-plant times, stride and loop
    seam of each clip - the numbers used for blend thresholds, Stop offsets and Start exit times.
  - Blend trees: **Thresholds from clip speed** and the 2D blend-space plot (drag it in Play).
