# UE5 Manny Mocap Export: Findings for the Motion Matching Locomotion Overhaul

Written 2026-11-17 by the "UE5 Manny export" session. Everything below was verified unless marked **UNVERIFIED**.

## 1. What we have

| | |
|---|---|
| Source | Fab: "Make Movement Feel Real - 3 Hours of AAA Mocap Data" by Quantum Stage (Swiss Mocap Animations), listing `8f3fb8e7-1b9a-4481-9079-475f6c003081`. Standard License. |
| UE project | A local UE 5.8.4 project (Third Person template). Pack lives at `/Game/QS_MM_Collection`. |
| Export folder | Local to the export machine; archived on the asset Drive under `Raw\Animations\Quantum Stage - Motion Matching Library` (Phase 2). Engine tools find it through an env var, never a hard-coded path. |
| Clips | **139 FBX** in `anims/`. 0 failed. 4.47 GB total; 8.7 MB to 98.6 MB each. |
| Total duration | **10,712 s = 2.98 h** (summed from per-clip lengths) |
| Frame rate | **60 fps** for all 139 (derived as num_frames / length; UE 5.8's AnimationLibrary has no frame-rate getter) |
| Clip length | 18.5 s (`Run_Loop_S_Take001`) to 233 s (`Walk_Loop_S_Take001`). These are **long continuous takes**, not short cycles. |
| Rig | `rig/SKM_Manny.fbx` (3 MB): mesh, skeleton, bind pose and skin weights, from `/Game/QS_MM_Collection/Demo/Characters/Manny/SKM_Manny`. |
| Manifest | `manifest.json`: per clip `name, file, ue_path, skeleton, length_seconds, num_frames, frame_rate, root_motion`. |
| Export script | `export_manny_mocap.py` in the UE project (re-runnable; settings at the top); archived alongside the export. |

## 2. Skeleton

- The clips target the pack's own **`SKM_Manny_Skeleton`**, not the engine's `SK_Mannequin`, but the bone layout is the **UE5 Manny** layout. These 22 key bones exist in both: `root, pelvis, spine_01..spine_05, neck_01, neck_02, head, clavicle_l, upperarm_l, lowerarm_l, hand_l, index_metacarpal_l, thigh_l, calf_l, foot_l, ball_l, ik_foot_root, ik_hand_root, ik_hand_gun`.
  `spine_04/05`, `neck_02` and the metacarpals exist only on the UE5 rig, not UE4.
- Blender's importer reports **162 bones**, hierarchy starting `root > pelvis > spine_01`.
- **UNVERIFIED:** a full bone-by-bone diff against `SK_Mannequin` (names, lengths, rest pose). Only the 22 bones above were checked.

## 3. Root motion: the big finding

**The `root` bone never moves. All locomotion travel is on `pelvis`.** This is how the pack is authored, not an export bug.

Evidence:

- **In Unreal** (read straight from the AnimSequence with `AnimationLibrary.get_bone_pose_for_frame`, no FBX involved): `root` is (0, 0, 0) at 0%, 25%, 50%, 75% and 100% of `Run_Loop_S_Take001` and `Walk_Loop_Take002`. `pelvis` Y swings from -409 cm to +449 cm in the run clip.
- **In the exported FBX** (Blender 5.2 headless import, about 400 samples per clip):

| Clip | root travel | pelvis path length | pelvis extent (X / Y / Z, m) |
|---|---|---|---|
| Run_Loop_S_Take001 | 0 | 23.6 m | 0.52 / 8.81 / 0.16 |
| Walk_Loop_Take002 | 0 | 23.0 m | 0.34 / 4.64 / 0.12 |
| Jog_Strafe_Diamond_Take002 | 0 | 23.3 m | 2.55 / 4.12 / 0.18 |
| Stand_Dodge_360_Take001 (control) | 0 | 7.1 m | 0.74 / 1.21 / 0.18 |

- The performer moves back and forth across the capture area, which is about 9 m long. Start-to-end displacement is small (for example 0.39 m for the run), but the distance actually covered is large.
- UE flags on these clips: `enable_root_motion = False`, `force_root_lock = False`, `root_motion_root_lock = REF_POSE`. All 139 show `root_motion = False`.

**What this means for motion matching:** the root trajectory and facing that the matching uses for queries **must be generated**. Typical approach:

1. Project the pelvis (hips) position onto the ground plane to get root position.
2. Derive facing from the hips' (or hips + shoulders') forward direction, flattened to the ground.
3. Smooth both, with a low-pass on position and stronger smoothing on facing so the root doesn't wobble with each stride.
4. Re-express pelvis (and the rest of the body) relative to the generated root.

**Recommendation from this session:** do this in the TartarusEngine animation importer and keep the FBXs as untouched raw capture. Smoothing windows can then be tuned without re-exporting 4.5 GB. Alternative: bake it into the FBXs with a Blender or Unreal script ahead of time. **Not decided yet; the overhaul plan should decide.**

## 4. Units and axes

- The FBX is written by Unreal's exporter with `force_front_x_axis = False`, so it uses Unreal's native conventions: **centimeters, Z-up**. Pelvis rest height is about **95 cm**.
- After Blender's FBX import (which converts to meters and keeps Z up), the locomotion in the sampled clips runs mainly along **Y**.
- Other export options used: binary FBX, `export_local_time = True` (clips start at t = 0), `map_skeletal_motion_to_root = False`, no preview mesh in the animation files, morph targets on.
- **UNVERIFIED:** the exact axis/unit metadata in the FBX header (GlobalSettings UpAxis, FrontAxis, UnitScaleFactor). Read it in the importer rather than assuming.

## 5. Clip catalogue (139 clips, grouped by name prefix)

| Group | Clips | Minutes | Contents |
|---|---|---|---|
| Stand | 52 | 73.4 | Idles (Neutral, Fidgets, LookAround, Variations, FO), Turn_360, Step_Fixed/Turned_360, Dodge_360, Jump_360 / InPlace, Repositions_Star, Stand_to_{Walk, Jog, Run} (Fixed, 360, Interrupted_360) |
| Walk | 22 | 26.6 | Card (with/without stops, Freestyle), Circles, Loop, Snakes, Strafe (Diamond, Square L/R, Star) |
| Jog | 20 + 1 | 21.3 | Same patterns as Walk. One file is spelled `Jogg_Card_withStops`. |
| Run | 12 | 17.0 | Card, CardLong (incl. `_R`), Circles, Loop, Snakes |
| Crouch | 14 | 16.6 | Idles, Card, Circles, Loop, Snakes, Strafe, Repositions_Star, Crouch_to_CrouchWalk_Interrupted_360 |
| Stroll | 10 | 10.9 | Card, Snakes, Strafe |
| Transitions | 4 | 8.0 | Transitions_Movement (+ Jogs), Transitions_stationary |
| AccelerationDeceleration | 4 | 4.9 | Starts, stops, speed changes |

- **UNVERIFIED** meaning of name suffixes: `_S`, `_FO`, `_M_L`, `_Fixed`. Many clips exist both with and without `_S`. Check the pack's documentation (linked from the Fab page) before tagging the database.
- The pack is shot for motion matching: long takes covering cardinal directions, strafes, circles, snakes, starts, stops, pivots and interruptions.

## 6. Also in the pack (not exported yet)

- `/Game/QS_MM_Collection/Animations/Metahuman_m_med_nrw`: **the same 139 clips** (names match one for one) on a MetaHuman skeleton, 3.16 GB in UE. The user wants these exported later. Rerun `export_manny_mocap.py` with a different `SOURCE_FOLDER`, `REQUIRED_SKELETON`, `OUTPUT_DIR` and `RIG_MESH_PATH`.
- `/Game/QS_MM_Collection/Demo` (Manny + MetaHuman characters and materials) and `/Maps`.

## 7. Export machine notes

Machine-specific setup (drive junctions, DDC location) is kept out of the repo. Unreal can re-run the export headless:
`UnrealEditor-Cmd.exe "<project>.uproject" -run=pythonscript -script="export_manny_mocap.py" -unattended -nullrhi`. Downloading from Fab is the only step that needs the GUI. UE 5.8 Python has no `AnimationLibrary.get_frame_rate`; `get_num_frames`, `get_sequence_length` and `get_bone_pose_for_frame` exist.

## 8. Open questions for the overhaul plan

1. Where is root motion generated: in the TartarusEngine importer at load or bake time, or pre-baked into the FBXs?
2. Smoothing parameters and facing source for root generation (hips only, or hips + shoulders).
3. Is the TartarusEngine runtime skeleton exactly this 162-bone Manny, or a subset with retargeting?
4. Clip tagging and labels (gait, stops, strafe, idle) from file names, given the unknown suffixes.
5. Mirroring, which would double the database for left/right coverage. Needs a left/right bone map; UE names use `_l` / `_r` suffixes.
6. Memory budget: 2.98 h at 60 fps × 162 bones is large uncompressed; decide on pose-feature extraction and compression.
