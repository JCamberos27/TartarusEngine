# Weapon camera animation and firing shake

Weapon Inspector > Camera contains the action camera channel and the firing shake asset slot.

## Setting up the camera in engine

No new Blender export is needed to follow the existing animated head. In the weapon's Camera
section, enable **Action Animation**, set **Camera Node = head**, and use **Reference State = Idle**.
This is already configured on the AKS74U and Remington870. **Rotation Scale = 1** follows the
head's rotation and **Position Scale = 1** follows its translation. Reduce either scale for
less motion, or set Position Scale to zero to keep the eye in place. **Action ADS Scale** reduces
the effect when aiming if needed. **Action Tags** defaults to Reload, Busy, Cycling. Use
**Extra States** for other actions, such as Draw or Holster, or add a shared tag to those states.
The original base-layer clip is sampled at the animator's current time, and camera motion
crossfades with its states. ADS holds, procedural arm IK and additive locomotion do not overwrite
the camera channel. Mouse look remains active. These effects apply only to the owning player.

Camera Node may also point to another animated node on the arms rig. The player's existing
Camera Bone still controls view-model placement. The weapon setting controls which node's
action animation contributes to the view. Idle/walk/sprint do not contribute to the camera
unless you explicitly add their states or tags.

## Firing shake

Create an asset from the Asset Browser's **Create Camera Shake** command, or **Create Firing
Shake** in the weapon's Camera section. Assign it to **Firing Shake**. **Open Shake** opens
its Inspector. The AKS74U and Remington870 already have assigned `.camerashake` profiles.

Author six independent curves: pitch/yaw/roll in degrees and right/up/back in metres.
Double-click a preview or click **Edit Curve** to open the [shared curve editor](CURVE_EDITOR.md)
in its own window, with tangent, interpolation, selection, clipboard and numeric tools.
Their signed values directly drive the camera; negative values move the opposite way.
**Rotation Min / Rotation Max** are two Vector3 scalar ranges for X pitch, Y yaw and Z roll.
**Location Min / Location Max** are two Vector3 scalar ranges for X right, Y up and Z back.
Each committed shot samples a uniform random multiplier independently for every axis, then
keeps all six sampled values for its entire playback. Equal min/max fixes an axis; negative
scalars reverse its direction. All components default to 1 to preserve existing assets.
Older shared `scalarMin` / `scalarMax` values load into every axis's range; subsequent saves
write the four Vector3 ranges.
Duration is seconds per shot, and ADS Scale additionally multiplies aimed shots. Each curve uses normalized
time 0..1 and must start and end at zero; an empty or flat zero curve disables that axis. The
Inspector previews the runtime solver for a single round or a ten-shot burst. **Resample Scalars**
picks another preview sequence; the preview otherwise stays stable while editing. Referenced
profile edits apply live in Play; profile GUIDs follow asset renames.

Only committed shots trigger shake, including procedural recoil fire. Dry firing adds none.
Overlapping rounds play the same authored curves independently and add together; all settle to zero. The shake
is a visual camera effect, independent of the controller recoil that modifies player look.
The profile replaces the older embedded camera punch, random roll and trauma noise on weapons
that reference it. There is no frequency, noise or random phase in curve playback; the only random
variation is the authored per-axis, per-shot scalar ranges. Player look recoil remains separate.

Profiles save as version 2. Version 1 profiles load by baking their envelope times each axis's
amplitude into the six curves, preserving authored keys and tangents and discarding frequency.
New profiles start with six flat zero curves ready to author.
