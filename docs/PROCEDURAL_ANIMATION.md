# Procedural Animation and IK

The first-person weapon plays its clips exactly as authored. A stack of procedural layers then moves the arms rig's **gun bone** (`ik_hand_gun` on the Manny rig), and **two-bone IK** keeps both hands on it. The weapon is socketed to that bone, so it follows. This is the approach Kinemation's CAS takes.

**The player's body** has its own procedural layers (spine aim, foot IK, the arms following the rig's hands):
see `BODY_SETUP.md` and `FPS_ANIMATION_SYSTEM.md` §8b. Their tuning fields are on the First Person Body component.

Everything is tuned per weapon in the weapon definition (`.fpsanim`). To open it, either:

- select the player object and press **Edit Weapon Definition** at the bottom of its First Person Controller, or
- select the `.fpsanim` in its **assets/Weapons/<Weapon>** folder in the Asset Browser.

Then open the **Recoil**, **Movement** and **IK** sections in the Inspector. Edits save immediately and apply **live in Play**: the running game re-reads the file within a quarter second, and that includes changes to the IK bone names.

Every field explains itself when you hover its label. Each section has a **Reset to Defaults** button, which asks before it resets anything. The header's **Undo** button steps back through every change to the weapon,
including a reset.

The shipped AKS74U and Remington870 use shared version 2 recoil profiles, the source sway
modifier, additive Animator locomotion and firing-shake assets. Their current contracts are
in [PROCEDURAL_RECOIL.md](PROCEDURAL_RECOIL.md), [WEAPON_SWAY.md](WEAPON_SWAY.md) and
[WEAPON_CAMERA.md](WEAPON_CAMERA.md). The older embedded recoil/bob/sway settings remain
compatibility paths; they do not describe the shipped weapons' active solvers.

## Axes and units

The layers work in **camera space**:

| Axis | Direction |
|---|---|
| +X | right |
| +Y | up |
| +Z | back toward the eye |

Positions are in metres and rotations in degrees, written as (pitch, yaw, roll). Positive pitch lifts the muzzle in the legacy camera-space layers. Their curves use normalized time 0..1. Version 2 recoil profiles instead use Unity axes (+Z forward, negative pitch raises the muzzle) and interpolation-alpha curves keyed in seconds; the host converts their output into the engine frame.

## Layers

The runtime composes procedural offsets, quaternion rotations, Animator layers and IK in their
defined order. Referenced recoil and firing-shake assets replace their older embedded channels:

| Layer | What it does |
|---|---|
| **Recoil** | The shipped weapons use the version 2 `.recoil` solver: semi/auto transitions, randomized targets, per-axis smoothing, noise/pushback/progression layers and compensated controller look recoil. It does not sum shot instances or use spring overshoot. The separate `.camerashake` asset supplies visual firing shake. See the recoil and camera guides. |
| **Sway** | The shipped weapons use `procedural.sway` source modifier settings: movement and raw look input feed independent position/rotation springs, with ADS scales, clamps, spaces and an additive-bone channel. Older embedded definitions can retain their legacy look-rate/velocity sway. See the sway guide. |
| **Bob** | Shipped weapon locomotion comes from additive Animator layers; legacy procedural bob is disabled in their playback. See the recoil guide's locomotion section. |
| **Breathing** | A slow idle loop (side, up, forward, pitch), calmer in ADS, with never-repeating noise **drift** on top. Sprinting winds the player: the breath and drift grow and quicken (**Exertion**), then recover. |
| **Jump & Land** | In the air the gun lags against vertical speed (rising pulls it down, falling floats it up). Landing kicks it down, dips the muzzle and rolls it a random way, sized by the fall speed; soft touchdowns under **Min Impact** don't kick. A spring brings it back. |
| **Camera Motion** | The view itself (never the aim): a head bob locked to the gun bob's stride (down on each footfall, a roll once per stride), a roll into strafes, and a dip and nod on landing. |
| **Walls** | Probes from the eye and along the barrel (**Probe Offset**); the nearer hit counts, so a wall on the gun's side or a door frame does too. In two stages: first the gun slides straight back as far as the wall is in (**Max Retract**), so the muzzle stops at the surface and you can still aim; then, over **Tuck Range**, it tucks into a low ready - a high ready when what's in the way faces up (a table, a low wall's top), or aside (muzzle turned away, gun shifted) when it's an edge beside the barrel, so peeking past a corner keeps the gun up. The probes run from where the eye actually is that frame - leaned and bobbed - so a peek isn't blocked by the corner the body is behind. The gun still fires and aims however tucked it is; set **Block At** above 0 to stop it firing (and drop the sights) once tucked that far. In the Sandbox, the **Shooting Range**'s bench, canopy posts and side walls are handy for trying it. |
| **Aim (ADS)** | A position/rotation offset blended in while a state tagged `ADS` plays, eased by the blend curve. Zero keeps the Aim clip's sight picture exactly. |
| **State Offsets** | A pose tweak per state name or tag, e.g. lower and roll the gun in `Sprint`, with blend in/out times. The drop-down next to the name lists the controller's states and tags. A name the controller doesn't have is shown in the warning colour. |
| **Locomotion Rate** | Sets the `WalkRate` / `SprintRate` parameters from the player's speed. Use them as the Walk and Sprint states' **speed parameter** so the clips step at the pace you move. |
| **Lean** | There's no lean key: the **corner peek** drives it. Aiming with cover within **Peek Range** straight ahead leans out around whichever side of it is open (the gun's side, right, on a tie) - only as far as clearing the edge takes, plus **Peek Margin** - and releasing the aim comes back behind cover. The side is picked when the aim starts and kept while it's held. The lean itself swings the head over on an arc about the waist - it rolls, slides sideways and drops a little - on a spring, so it eases out and settles. The gun rolls a little further into the lean on its own slower spring, so it lags behind the head (less with the sights up). The side step stops short of walls, so leaning can't put the eye through them, and the roll shrinks with it. Sprinting straightens up unless **While Sprinting** is on. |
| **IK** | Bone names (gun bone, both arm chains) and the **Off Tag** (`IKOff`). The drop-downs list the arms rig's bones; names it lacks are shown in the warning colour. States with the Off tag, and hidden states, fade every offset out over the blend time, so they play purely as authored. The AK tags Draw and Holster. Turning **Enabled** off keeps the motion but moves the whole view model instead of the gun bone. |

### Curve editor

Double-click a preview or click **Edit Curve** to open the shared curve editor in its own window.
It includes selection, clipboard, snapping, transforms, cubic/linear/constant interpolation,
tangent controls, exact numeric fields and a scrubbable playhead. See [Curve editor](CURVE_EDITOR.md).

## The IK Rig component (any rig)

**Add Component → Rendering → IK Rig** works on any entity with an Animator Controller. It runs after the controller blends its layers and before the pose is drawn, and offers:

- **Limb A / Limb B:** two-bone chains (Upper, Lower, End) that reach for a **Target** bone. It keeps the bend the animation already has, and straightens toward targets out of reach.
  - **Keep Animated Offset:** the end reaches for where it sat relative to the target in the animated pose. This is how hands stay gripped while the gun moves.
  - **Match Rotation:** also turns the end bone to the goal.
  - **Pole Bone / Hint Weight / Hint Offset:** the elbow or knee points toward the pole bone's position (the animated elbow when empty) plus the offset. Hint Weight 0 (the default) keeps the animated bend plane; 1 puts the elbow in the plane of the pole and the root-target line, so it no longer flips when the target crosses the bend plane.
  - **Max Limb Scale:** how far the limb may stretch toward an out-of-reach target (1 = never, the default).
  - **Grip Position / Grip Rotation:** moves the end's grip in the target bone's own frame (degrees for rotation). The first-person weapon settings' `ik.rightHandPosition/Rotation` and `leftHandPosition/Rotation` (metres, degrees; default zero) write these.
- **Look At:** turns a bone's axis toward another bone, clamped to a maximum angle.
- **Weight:** blends the solved pose with the animated pose.

**Spine distribution** (First Person Body, group Spine Distribution; NPCs read the same fields once the director copies them): Spine Weight 1-5, Spine Max Angle 1-5 (degrees, 0 = no limit) and Pelvis Alpha share the view pitch, twist and shoulder turn over the spine bones. The default (all weights 1, no limits, alpha 0) is the old even spread, exactly.

The look-at runs before the limbs, so aiming a spine bone still leaves the hands on their targets.

The component's Inspector warns when:

- there's no Animator Controller,
- there's no rigged model, or
- a bone name isn't on the model.

**Presets** fill in the bone names for the UE5 Mannequin (arms or legs to its IK bones) and for Mixamo.

Code can also push rigid bone offsets through `IKRigComponent::Offsets` (runtime only); the first-person driver does this. The math is in `src/Game/IK.h` (`SolveTwoBone`, `OffsetBone`, `AimBone`) and works on any `LocalTRS` pose.

## For programmers

| Part | Where it lives |
|---|---|
| Procedural stack | `src/Game/FirstPersonProcedural.{h,cpp}`. `WeaponProceduralState::Update` takes an input (dt, look rate, camera-frame velocity, vertical speed, grounded, wall distance, tags, lean) and returns a `WeaponProceduralPose`. No world or model needed, so it's unit-tested directly (`TestWeaponProcedural`). |
| Wiring | `FirstPersonPresentation` feeds it each `Tick`, writes the gun offset into the arms' `IKRigComponent`, and puts the camera punch, head bob, landing dip and lean on the play camera (and the FOV pulse on the world FOV, not the mouse scale). `RemoveViewKick` takes them off before `Player::Update`. |
| Rigs without the bones | If the arms rig lacks the gun bone or arm chains, a warning is logged and the procedural pose moves the whole view model instead. |
| Self-check | When Play starts, the driver moves the gun 10% of an arm's length on the real rig and logs how far the hands land from their grip. The AK logs 0.000%. |
| File format | The `procedural` object in the `.fpsanim` (see `WeaponProceduralSettings::ToJson`). Missing keys keep their defaults, so `"procedural": {}` means the AK tuning. Files written before this existed migrate their old `gameplay.recoil` / `adsBob` numbers into curves of the same shape. |
