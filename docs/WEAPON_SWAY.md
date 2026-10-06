# Weapon sway

Weapon definitions store the supplied KINEMATION `SwayModifierSettings.cs` settings under
`procedural.sway`. Edit them in the weapon Inspector's **Movement / Sway** section. They are
part of the `.fpsanim`, with its normal save, undo and Play hot reload; no separate asset is needed.

The new job ports `SwayModifierJob.cs` and its dependency `KSpringMath.FloatSpringInterp`.
Each position/rotation spring has Vector3 damping, stiffness, speed, scale and clamp.
For each axis, it clamps the target before scale, uses `min(dt * speed, 1)`, and maintains
velocity plus the previous error. This is the source's discrete spring, not the older
frequency-based engine spring. Stiffness must be nonnegative to keep its square root valid.

The input bindings are supplied by the host: raw player look deltas (horizontal, vertical)
before camera limits; movement action axes (right, forward) before speed and collision;
and the aiming input. Mouse sensitivity and stick integration are applied once by the
player controller. Scripted/NPC presentations without explicit movement input use local
velocity divided by walk speed, limited to unit length, as their input adapter.

Movement targets are `(right, forward, forward)` for position and
`(forward, right, right)` for rotation. Position is divided by 100, then both targets
exponentially follow their new values using dampingFactor before the springs run.
Aim accumulates look delta times 0.01, decays the accumulator, then feeds
`(horizontal, vertical, 0) / 100` to position and `(vertical, horizontal, horizontal)` to
rotation. ADS scales each job's targets before interpolation/clamping. Movement applies
first, aim second, then additive-bone animation; rotations compose as quaternions.

**Use Shooter Preset** restores the exact corresponding source preset. Both presets omit
adsScale, so it is 0. The movement preset also omits clamp, so its clamps are 0 and it
produces no motion until clamps are changed. These source defaults are intentional in the
port. DampingFactor 0 retains the smoothed target (aim can still accumulate input); spring
speed 0 freezes that axis. The two behaviors differ from the curve smoothing setting.

`weaponBone` defaults to the engine's equivalent, `ik_hand_gun`, of CAS's `IK weapon_bone`.
`weaponAdditiveBone` defaults to `weapon_bone_additive`. This channel reads the animated
bone's **local position and rotation**, not a delta from its first frame. Its rest local
transform should therefore be zero/identity for neutral output. Space Offset is a unit
quaternion stored as XYZW. It remaps the local translation and conjugates the rotation.
ADS Curve Scale interpolates from 1 while hip firing to the chosen ADS weight; ADS Curve
Smoothing 0 changes it immediately, otherwise it uses exponential interpolation.

Bone Space moves and rotates along the current gun axes; Parent Bone Space adds local
translation and postmultiplies local rotation; Component Space adds in the view rig's
component frame; World Space adds world translation and postmultiplies the target rotation,
including the source's unusual rotation convention. The host bridges Unity +Z-forward
positions/quaternions into the engine frame and scales metres into the rig's model units.
The additive channel is read from the engine's native animation pose and the remapping
quaternion is bridged into that frame.

Sway runs after ADS action carry but before the final procedural aiming/recoil offset and
arm IK, so the hands follow the moved gun and sight-alignment corrections apply afterward.
The persistent View Position still applies to the final first-person rig. A zero modifier
weight or invalid weapon bone freezes the job, as in the source. A missing additive bone
skips just its channel rather than reproducing Unity's invalid-handle error. Disabled sway
does not layer the old solver on top. Equip/weapon changes reset independent spring state.

AKS-74U and Remington870 now use the source presets. Older embedded definitions can still
load their legacy sway; **Use Unity Sway** replaces it with these presets. Their old look-rate
limits, free-aim zone and velocity-based fields do not appear in the new modifier. The
Animator's ADS Walking layer remains independent of additive-bone curve animation.

Run `TartarusEngine.exe --unit-tests` for formula, presets, serialization, frozen state,
ADS target scaling, curve smoothing, quaternion composition and all four space regressions.
