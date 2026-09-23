# Procedural Animation and IK

The first-person weapon plays its clips exactly as authored. A stack of procedural layers then moves the arms rig's **gun bone** (`ik_hand_gun` on the Manny rig), and **two-bone IK** keeps both hands on it. The weapon is socketed to that bone, so it follows. This is the approach Kinemation's CAS takes.

Everything is tuned per weapon in the weapon definition (`.fpsanim`): select it in the Asset Browser and open **Procedural** in the Inspector. Edits save immediately and apply **live in Play** (the running game re-reads the file within a quarter second).

## Axes and units

The layers work in **camera space**:

| Axis | Direction |
|---|---|
| +X | right |
| +Y | up |
| +Z | back toward the eye |

Positions are in metres and rotations in degrees, written as (pitch, yaw, roll). Positive pitch lifts the muzzle. Curves are keyed over 0..1 of their span, which is a shot's duration, a stride or a breath.

## Layers

All layers are summed:

| Layer | What it does |
|---|---|
| **Recoil** | Each shot starts its own curves: pitch, yaw, roll, side, up and kickback. Each channel is scaled by a random pick from its min/max range; a negative min kicks either way. Full-auto overlaps shots into a climb. A spring (frequency, damping) smooths the sum. Separate **Hip** and **ADS** scales apply; hip fire also plays the Fire clip. A **camera punch** (pitch/yaw curves) moves the view, not the aim. The Inspector plots a 10-round burst at the weapon's rpm. |
| **Sway** | Look sway: the gun lags behind turns (per 100 deg/s). Move sway: it trails movement and tilts into strafes (per m/s). Both run through a spring (damping under 1 overshoots) and are scaled down in ADS. |
| **Bob** | Walk and sprint cycles (side, up, roll) are phase-locked to distance travelled and blended by speed and sprint. Separate hip and ADS scales. |
| **Breathing** | A slow idle loop (side, up, forward, pitch), calmer in ADS. |
| **Aim (ADS)** | A position/rotation offset blended in while a state tagged `ADS` plays, eased by the blend curve. Zero keeps the Aim clip's sight picture exactly. |
| **State Offsets** | A pose tweak per state name or tag, e.g. lower and roll the gun in `Sprint`, with blend in/out times. |
| **Locomotion Rate** | Sets the `WalkRate` / `SprintRate` parameters from the player's speed. Use them as the Walk and Sprint states' **speed parameter** so the clips step at the pace you move. |
| **Lean** | `LeanLeft` / `LeanRight` (default **Z / C**) roll the camera, slide it sideways, and roll the gun a little further into the lean. |
| **IK** | Bone names (gun bone, both arm chains) and the **Off Tag** (`IKOff`). States carrying that tag, and hidden states, fade IK and every offset out over the blend time, so they play purely as authored. The AK tags Draw and Holster. |

### Curve editor

| Action | How |
|---|---|
| Move a key | Drag it |
| Change a key's slope | Drag the selected key's handles |
| Add a key | Double-click the curve's background |
| Delete a key | Right-click it |
| Presets and smoothing | Right-click the background: flat, line, ease, kick, sine, smooth tangents |
| Exact values | Edit the number row under the graph |

## The IK Rig component (any rig)

**Add Component → Rendering → IK Rig** works on any entity with an Animator Controller. It runs after the controller blends its layers and before the pose is drawn, and offers:

- **Limb A / Limb B:** two-bone chains (Upper, Lower, End) that reach for a **Target** bone. It keeps the bend the animation already has, and straightens toward targets out of reach.
  - **Keep Animated Offset:** the end reaches for where it sat relative to the target in the animated pose. This is how hands stay gripped while the gun moves.
  - **Match Rotation:** also turns the end bone to the goal.
- **Look At:** turns a bone's axis toward another bone, clamped to a maximum angle.
- **Weight:** blends the solved pose with the animated pose.

Code can also push rigid bone offsets through `IKRigComponent::Offsets` (runtime only); the first-person driver does this. The math is in `src/Game/IK.h` (`SolveTwoBone`, `OffsetBone`, `AimBone`) and works on any `LocalTRS` pose.

## For programmers

| Part | Where it lives |
|---|---|
| Procedural stack | `src/Game/FirstPersonProcedural.{h,cpp}`. `WeaponProceduralState::Update` takes an input (dt, look rate, camera-frame velocity, tags, lean) and returns a `WeaponProceduralPose`. No world or model needed, so it's unit-tested directly (`TestWeaponProcedural`). |
| Wiring | `FirstPersonPresentation` feeds it each `Tick`, writes the gun offset into the arms' `IKRigComponent`, and puts the camera punch and lean on the play camera. `RemoveViewKick` takes them off before `Player::Update`. |
| Rigs without the bones | If the arms rig lacks the gun bone or arm chains, a warning is logged and the procedural pose moves the whole view model instead. |
| Self-check | When Play starts, the driver moves the gun 10% of an arm's length on the real rig and logs how far the hands land from their grip. The AK logs 0.000%. |
| File format | The `procedural` object in the `.fpsanim` (see `WeaponProceduralSettings::ToJson`). Missing keys keep their defaults, so `"procedural": {}` means the AK tuning. Files written before this existed migrate their old `gameplay.recoil` / `adsBob` numbers into curves of the same shape. |
