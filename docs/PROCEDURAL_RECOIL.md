# Procedural recoil

Shared `.recoil` assets are a behavioral port of the supplied KINEMATION `RecoilAnimData.cs`
and `RecoilAnimation.cs`. Version 2 stores the same field names, shapes and defaults as
`RecoilAnimData`; engine-only springs, camera shake, duration, aim recovery delay and shot
variety are not part of this asset. Bolt setup, RPM and fire mode remain on the weapon.

## Asset Inspector

The Inspector follows `RecoilAnimDataEditor.cs` with four tabs:

- **Recoil Targets**: pitch (Vector2), roll/yaw (Vector4), kickback/up/right, aim rotation/location multipliers.
- **Smoothing**: smoothRot/smoothLoc and extraRot/extraLoc, independently on each axis for auto/burst.
- **Layers**: noiseX/Y, noiseAccel/Damp (Vector2), noiseScalar; pushback; recoilSway; separate pitchProgress/upProgress; adsProgressAlpha.
- **Misc**: horizontal/vertical controller recoil and smoothing, damping, hip/aim pivots, smoothRoll, playRate, and semi/auto rotation/location vector curves.

Values use Unity local axes: +X right, +Y up, +Z forward. Negative pitch raises the muzzle;
negative kickback moves it toward the shoulder. The engine converts the final transform into
its camera frame before applying it to `ik_hand_gun`. The arm IK follows the moved gun.

Curves are interpolation **alphas**, keyed in **seconds**, rather than motion amplitudes over
normalized time. Each sampled alpha interpolates between the transition start and randomized
target. `playRate` scales the playback clock. `smoothRoll` reverses a new roll target when its
sign matches the previous target. ADS target multipliers are sampled when targets regenerate;
controller recoil does not receive these visual multipliers.

New assets reproduce Unity defaults: zero targets/rates, zero aiming multipliers, playRate 0,
noiseScalar/adsProgressAlpha 1, and all four vector curves flat at 0 from 0 to 1. To author motion,
set playRate, target ranges and curve shapes. Set aimRot/aimLoc for ADS. Controller smoothing 0
produces no controller movement, as in the source; auto/burst visual smoothing 0 follows its
raw output immediately, and extraRot/extraLoc 0 means a multiplier of 1.

## Runtime behavior

The first round after rest uses the semi transition. Rapid subsequent rounds in Burst/Auto
enter the looping auto transition at `60 / RPM` seconds. Play accumulates controller targets;
visual targets, noise, progression and sway regenerate when the playback clock is at zero.
The auto loop can regenerate targets between Play calls, exactly as the source does. Stop
releases looping and lets the active curve finish. Semi Play starts a transition from the
current smoothed output. Transition selection uses unscaled shot time and frame duration;
curve playback uses scaled frame time. The solver evaluates before advancing its timeline.

Noise and pushback damp their targets before following them. Progression and sway follow
before damping. Rotation/location auto smoothing is independent per axis. Main pivot position
is added before sway; sway rotation multiplies the main quaternion, and sway pivot translation
is added separately. The port does not sum shot instances or add spring overshoot.

Controller recoil accumulates raw player look input before camera limits while firing. Each axis follows its target with
`1 - exp(-smoothing * dt)`. While stopped, the target damps toward zero after this follow step.
Stop scales the current recoil by opposing accumulated input, then sets the controller target
and cached output to that compensated value. The emitted delta applies once to the player's
persistent pitch/yaw, independently of mouse sensitivity. The host maps Unity horizontal/
vertical ordering to its yaw/pitch input and supplies Stop on trigger release or cancellation.

The host uses its own random generator and coordinate conversion. It guards zero RPM and
malformed/missing curves rather than reproducing Unity indexing errors. Valid asset motion
and layer ordering otherwise follow the supplied solver.

## Authoring and migration

Right-click the Asset Browser and choose **Create Recoil Profile**. A weapon's Recoil section
can extract an asset, assign it with a picker/drop target, duplicate it, or open its Inspector.
Edits save on commit, support complete-asset Undo/Redo, and hot reload during Play. Runtime
state is independent for each presentation. Invalid files keep the last valid runtime data.

Version 1 profiles load through a legacy converter and save as version 2 on their next edit.
AKS-74U and Remington870 have already been upgraded, preserving their GUID sidecars and weapon
references. Migration rescales curve key times into seconds, normalizes curve amplitudes into
alphas and target ranges, and converts positions/pivots and rotation signs into Unity axes.
It maps legacy rates into the new fields; the old spring/summed-shot motion is replaced by the
Unity solver, so migrated profiles can need tuning. Embedded legacy weapon definitions remain
compatible. An explicit headless upgrade is `TartarusEngine.exe --migrate-recoil <file.recoil>`.

## Curve editor

Double-click a preview or click **Edit Curve** to open its own shared curve editor window.
The channel list opens other recoil axes; windows remain editable after selecting another asset.
See [Curve editor](CURVE_EDITOR.md) for selection, clipboard, snapping, transforms, interpolation,
tangent, playback and numeric tools. View end sets the initial editable range in seconds.
Edits save on commit and use global Undo/Redo. Recoil key/tangent precision is preserved without
decimal rounding; curve data stays in the asset, never imgui.ini.

## Firing, bolt and validation

For a placement that survives every first-person animation, use the weapon Inspector's
**ADS / View Placement / View Position**. It moves the owner's arms and gun together in
camera-space metres (+X right, +Y up, +Z back), after animation and IK, and does not move
the world-view weapon. Hip Position remains a separate sights-down pose that fades out
on ADS. Sight Alignment is an extra ADS correction and remains active in carried actions.

The AK controller's **Weapon Locomotion** layer plays Idle, Walk, IdleToSprint,
Sprint and SprintToIdle additively, relative to the Idle state's first frame. This
shared reference preserves the sprint hold pose and the full exit motion. It runs
independently of base-layer reloads, melee, inspection and aiming. Layer weight
controls motion strength. Movement input selects the gait and WalkRate/SprintRate
control playback. Unequipping fades to Off. Base locomotion states retain their
contracts but hold the resting pose, preventing locomotion from being added twice.
The ADS pose hold for carried reloads is applied before locomotion layers, so
walking remains additive during ADS reloads. ADS offsets and arm IK run after
layer composition.

Walking is sampled once on the **Weapon Walk** additive layer, with a continuous
WalkRate clock across Walk, IdleToSprint and SprintToIdle. The locomotion layer's
WalkBlend state curve weights it through the same crossfades as the sprint clips.
Both transition clips keep their authored motion with walking added over it;
the walk fades out for full Sprint, Idle and Off. Walk on Weapon Locomotion holds
the resting frame so walking is never added twice. Weapon Walk uses the same
ADS Locomotion Scale as Weapon Locomotion.

Weapon Inspector / ADS / View Placement / ADS Locomotion Scale multiplies the
Weapon Locomotion layer while aiming (0 = none, 1 = unchanged). It blends over
the weapon's Blend Time and leaves the controller layer's hip weight intact.

ADS / View Placement / Blend Time controls both animated aim transitions and
procedural aiming offsets, including carried actions. The old separate Aim Hold
Time control is retired. Legacy procedural bob and sway are disabled in weapon
playback; locomotion comes from the additive layer and sway from the Unity modifier.

Aiming takes priority over held sprint input for player speed, weapon gait and
footstep audio. The AK additive layer fades straight out of sprint on aim, without
waiting for SprintToIdle. Releasing aim resumes sprint entry if sprint is still held.

The recoil Inspector has a separate bottom preview pane; drag its divider up or
down to resize it. The settings above scroll independently. Select a rotation,
translation or accumulated look-input channel, firing mode, aiming state and
pattern seed. The preview runs RecoilAnimation at 120 Hz using explicit shot
timestamps and includes recovery after release. Hover for time and output values.
It previews recoil alone, without player look compensation or other weapon motion.
The base weapon controller's `Speed` parameter also follows movement action input, so
hip walking begins blending to Idle immediately on release, including mid-loop while
the player is still decelerating. Its blend duration is configured on the controller's
Walk-to-Idle transition. Physical velocity
still controls `WalkRate` and procedural bob; presentations without action input retain
velocity-based state selection.

AK sprinting follows Idle/Walk → IdleToSprint → Sprint → SprintToIdle → Idle/Walk.
The entry and exit clips complete before advancing, unless sprint input is cancelled
or resumed respectively. Aim also enters through IdleToSprint and can resume after
SprintToIdle. These states use explicit destinations rather than Exit re-entry.

Referenced profiles make hip/ADS fire procedural. The Fire state, trigger, and Shot event are
optional. A successful round commits ammo, effects and one Play call through the shared shot
path. Empty, blocked, hidden, reloading or busy weapons reject it. Idle/Ready and IKOff gates
remain in place. Equip, reload, pump/bolt, movement, grips and muzzle configuration stay on the
weapon. Set Bolt Bone/Cycle/Travel to cycle without a Fire clip, or capture a reference clip's
travel with **Use Measured Bolt Travel**. Set an explicit muzzle when automatic detection
cannot follow the bolt stroke.

Build with `cmake --build build --config Release`; run `TartarusEngine.exe --unit-tests`.
The port regressions cover schema/defaults, lossless curves, atomic failures, migration,
semi/auto transitions, timeline order, ADS targets, alternating roll, independent layers,
pivot/quaternion composition, controller smoothing/compensation and once-only look deltas.
`CurveWheelCapture` checks zoom inside a scrollable Inspector and scrolling outside the canvas.

The isolated themed capture is `TartarusEngine.exe --editor-shot <directory> --editor-shot-curves
--editor-shot-default-layout`. It uses copied profile data and scratch preferences and captures
the curves and all four Inspector tabs. Live firing/IK still warrants checking in Play.
