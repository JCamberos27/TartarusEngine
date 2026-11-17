# Motion Matching Overhaul: Session Handoff

**Read this first in every motion matching session.** It is the only context a new session needs. Update it at the end of
every session: status, files touched, exact commands and results, open issues, next steps. Then commit.

Reference: [`MOCAP_EXPORT_FINDINGS.md`](MOCAP_EXPORT_FINDINGS.md), the verified facts about the exported mocap pack.

## Status

| Phase | Branch | State |
|---|---|---|
| 0. Analysis and roadmap | `dev/mm-phase1` | **Done** (2026-11-17) |
| 1. Test infrastructure and legacy baseline | `dev/mm-phase1` | **Done** (2026-11-16) |
| 2. Motion bake pipeline and root generation | `dev/mm-phase2` | **Done** (2026-11-16; owner signed off on the `mmview` recording) |
| 3. Motion matching runtime | `dev/mm-phase3` | **Done** (2026-11-16) |
| 4. First-person player integration (unarmed) | `dev/mm-phase4` | **Next** |
| 5. NPC / dummy integration (unarmed) | `dev/mm-phase5` | Pending |
| 6. Legacy cleanup and final verification | `dev/mm-phase6` | Pending |

## Goal

Replace body locomotion for the first-person player and the NPC dummies with motion matching (MM), driven by the
Quantum Stage "Make Movement Feel Real" mocap pack (Fab, Standard License; 139 takes, 2.98 h, 60 fps, UE5 Manny
skeleton). Then delete the legacy locomotion graphs, logic and clips.

## Decisions (made with the project owner; don't re-open)

- **Scope.** MM replaces body locomotion only. These stay: the `AnimatorController` runtime and editor (the weapons
  use them), the weapon and arms rigs, procedural sway and recoil, foot IK and `FootLock`, spine aim, the world
  twins, ragdoll and flinch.
- **Unarmed first.** Weapons and the upper body come after the roadmap. Phases 4 and 5 are verified unarmed, with
  the weapon holstered.
- **Speeds.** Player and NPC walk, jog, run and crouch speeds are retuned to the measured mocap gait speeds; only
  small speed warping is kept.
- **Movement stays capsule-led.** MM picks the pose. Orientation and speed warping replace `LoopPlayRate`. The camera
  stays anchored to the capsule.
- **Airborne.** Jump, fall and land are label-locked MM segment playback from the baked jump frames, with
  inertialization. There is no `.controller` for it.
- **Exaggerated takes (`_S`) are excluded** from the database, but kept tagged for later.
- **Cleanup.** Only the legacy locomotion folders are deleted (`Mocap/RootMotion/{Locomotion,Locomotion_Mirror,
  Locomotion_V2,Turn,Idle,Crouch,Jump}`). The non-locomotion mocap stays.
- **Assets never go in git.** They follow the asset Drive's README:
  - cooked files go in `project/assets/...` and are exported to the Drive's `Used\` with
    `tools/assets/asset_manifest.py`;
  - only `.meta` files and `project/external_assets.csv` are committed, and `tools/assets/check_git_assets.py` must
    pass;
  - the whole download is archived on the Drive under `Raw\Animations\Quantum Stage - Motion Matching Library\`.
  - **Ask the owner** before any write to the Drive or deletion from it. It is a shared team folder.
- **CI has no licensed assets.** Unit tests use synthetic clips. Anything that needs the pack must SKIP, never FAIL,
  when the files are missing.

## Key technical decisions (the findings' open questions)

1. **Root motion comes from an offline bake in the engine.** A new mode, `--bake-motion <mmconfig>` (next to
   `--bake-gi` and `--bake-impostors`), does the work:
   - reads the untouched raw FBX files through the existing assimp `Model` path;
   - generates the root, keeps the core bone set, trims and quantizes;
   - writes `project/assets/Animations/MotionMatching/*.mmanim` (pose stream) and `*.mmdb` (features).

   The pack's `root` bone never moves; all travel is on `pelvis`. `Model::FindRootMotionNode` (`Model.h:351`) picks
   `root` and would read zero travel, so root generation is required.
2. **`RootSynth`** generates the root:
   - **position:** the pelvis projected to the ground, with a Gaussian of σ ≈ 0.10 s;
   - **facing:** 0.5 hips forward + 0.5 shoulder-line forward (`clavicle_l` → `clavicle_r`, perpendicular), flattened,
     as an unwrapped yaw with σ ≈ 0.25 s;
   - mirror-padded at the clip edges, and tunable per tag in `.mmconfig`;
   - **validation:** yaw jitter, a root-to-hips facing angle under 20° on forward takes, and foot slide in root space.
3. **Skeleton.** Bones are matched by name onto Quantum, with no retargeting. Quantum's 352 nodes include every Manny
   bone except `weapon_l` and `weapon_r`.
   - The bake keeps about 65 core bones: the spine, limbs, twist and IK bones.
   - It drops the fingers (a static relaxed hand pose replaces them) and the corrective/helper bones.
   - It keeps rotations only, plus the pelvis translation scaled by the ratio of pelvis heights.
   - Phase 2 checks rest poses and bone lengths, and acts if a core bone differs by more than 2%.
4. **Tagging.** `tools/mm/takes.csv` is built from the pack's info sheet and decides which takes are included.
   Per-frame labels are computed:
   - gait from root speed;
   - crouch from pelvis height;
   - strafe from facing versus travel direction;
   - airborne from foot contacts;
   - prone frames are excluded.
5. **Mirroring is not in v1.** Phase 3 reports left/right coverage. If mirroring is needed, it is done as feature
   rows plus mirroring at sample time.
6. **Budgets.** At most 150 MB of pose memory (smallest-three 16-bit rotations, 60 fps), at most 40 MB of features, and
   at most 2 s to load. The curated database is about 1.2 to 1.5 h. The bake fails when a budget is exceeded.

## Phase plan

### Phase 1: Test infrastructure and legacy baseline (done)
- **Unlocked FPS.** Add `TARTARUS_UNLOCKED_FPS` to the `src/Core/Env.h` table and read it through `Env::`. It forces
  VSync off and the FPS cap to 0 whatever the editor prefs say, at `src/App/Application.cpp` ~1076 (VSync) and ~2354
  (FPS cap).
- **Focus loss.** Headless runs (smoke, npc and perf) must not pause Play when the visible window loses focus. See
  `Application.cpp` ~1229 and `src/Core/Input.cpp` ~92; `docs/WEAPON_REVOLVER38.md:84` describes the stall.
- **Test script.** `tools/mm_smoke.ps1` builds Release, then runs `--unit-tests`, `--smoke-test tests\smoke-scenes`,
  and the npc-test scenarios given as arguments, in that order.
  - Every run is visible, with unlocked FPS and no `TARTARUS_HIDDEN`.
  - Each run is launched with `Start-Process -PassThru` + `WaitForExit()`, because Release is a GUI-subsystem build.
  - It prints a pass/fail table and exits non-zero on any failure.
- **Baseline.** Run the legacy `testbed`, `npcloco`, `locomotion`, `stopsteps`, `feet` and `labloco` scenarios, plus
  `--perf-bench tests\perf-scenes\perf_stress.json`.
  - Save the metrics and traces (`NPC_TEST_TRACE`) to `docs/mm_baseline/`: foot slide, clip speed against capsule
    speed, frame cost.
  - Later phases must match or beat them.

**Done when:** the infrastructure works visibly at unlocked FPS and the baseline is saved.

**What landed (2026-11-16):**
- `TARTARUS_UNLOCKED_FPS` (`Env.h`), read in `Application.cpp`. It turns VSync off, sets the FPS cap to 0 and drops
  the background cap, whatever the prefs say. `tools/npc_test_runs.py` sets it too.
- Focus. Smoke, npc and perf runs (`opt.smokeTestMode`) call `Input::SetIgnoreDevices(true)`: keyboard, mouse and
  gamepad read nothing, focused or not (`SimulateKeyTap` still works). Focus loss no longer releases the game input, and
  a click in the Game view no longer captures the cursor. Scripted input (`Player::ScriptMove`, `NpcTest` triggers)
  never needed focus.
  - Code review found no other path where focus pauses Play in a harness run: the iconify sleep, the background FPS
    cap and the reimport-on-focus are all `!headless`.
  - The stall `WEAPON_REVOLVER38.md` describes was most likely VSync and the driver throttling an unfocused window,
    which unlocked FPS removes.
  - Measured: the smoke run lost focus for 16 of its 21 scenes (`unfocused=100`), and they all passed, except
    `smoke_play_controller` on the known bad wav. The final pass's `testbed` (it drives the player) spent 1642 frames
    unfocused and passed 29/29.
- The `[SmokeTest] PASS|FAIL` per-scene line gained `rendered=N wall=Xs fps=N unfocused=N` (`SmokeHarness.cpp`): the
  real frame count, wall time, frame rate, and frames spent without focus.
- `tools/mm_smoke.ps1`: build, then `--unit-tests`, `--smoke-test tests\smoke-scenes`, the npc scenarios given, and an
  optional `--perf-bench`. Every run is visible, with unlocked FPS, `Start-Process -PassThru` + `WaitForExit()`,
  stdout and stderr per run, a pass/fail table, and exit 1 on any failure.
  - Options: `-Runs`, `-TraceDir`, `-PerfBench`, `-UnitFilter`, `-SkipBuild/-SkipUnit/-SkipSmoke`, `-Sound`,
    `-TimeoutMinutes`, `-LogDir`.
  - `-AllowErrors <regex>` turns a run that failed only on matching log errors into `PASS*`. It is opt-in, for a local
    asset known to be broken.
  - Logs go to `work\mm_smoke\<timestamp>\`. Traces always go to `<LogDir>\traces` unless `-TraceDir` says
    otherwise: `labloco`'s one check counts traced frames, so it fails without a trace folder.
- `tools/mm/baseline_summary.py`: turns an `mm_smoke` log folder into `logs/`, `traces/`, `metrics.json` and
  `SUMMARY.md`. Run it on a later phase's output to compare with the baseline.
- `docs/mm_baseline/`: the legacy baseline. See its `README.md` and `SUMMARY.md`. Headline numbers:
  - **Checks:** testbed 29/29, npcloco 33/33, locomotion 20/20, stopsteps 10/10, labloco 1/1, feet 4/6 (a test bug, fixed since: 6/6).
  - **Clip vs capsule speed** (testbed A): walk 0.9986, jog 1.0000.
  - **Foot slide:**
    - player: ≤ 0.1 cm worst on testbed's legs; labloco's stops after a strafe right or a jog back are 5.3 and 26.5 cm;
    - dummy: worst 19–22 cm (labloco idle, strafe left, jog back; npcloco jog back 21.5 cm).
  - **Frame cost** (`perf_stress`, p50): edit 2.87 ms, play 5.65 ms, play-max 7.44 ms.

### Phase 2: Motion bake pipeline and root generation
New `src/Game/MotionMatching/`:
- `RootSynth`, `MotionBake` and `MotionClipStream`.
- The `--bake-motion` mode and the `.mmconfig` asset. The source directory comes from an env var listed in `Env.h`.
- `tools/mm/takes.csv` and its generator.
- The rest-pose and bone-length diff (pack `rig/SKM_Manny.fbx` against `Quantum_Body_Full.fbx`).
- `--npc-test mmview`: plays cooked takes with the generated root drawn, and runs PASS/FAIL validation metrics.
- Unit tests on synthetic clips.
- The Drive archive and license entry, after the owner confirms.

**Done when:** the bake fits its budgets, `mmview` passes visibly, and the owner has signed off on a recording.

**What landed (2026-11-16):**
- **`src/Game/MotionMatching/`** (no engine dependencies beyond `Model`; the math is header-only and unit-tested):
  - `MotionMath.h`: Gaussian smoothing with edge padding, yaw unwrap, smallest-three 48-bit quaternions (worst error
    1.0e-4 rad), a float-safe `QuatAngle`.
  - `RootSynth.{h,cpp}`: the root, as decided (pelvis on the ground, σ 0.10 s; 0.5 hips + 0.5 shoulder-line facing,
    σ 0.25 s), plus `Validate` (yaw jitter, facing vs hips, path wobble, foot slide, largest frame step) and `Contacts`.
    - Edges: **mirror-padded through the edge's trend value** (a point mirror, `v[-k] = 2 E - v[k]`, with E the trend
      line through two 1 s window means at the edge). A plain mirror flattens the speed at a cut; a point mirror
      through the edge sample leaves the edge unsmoothed (the unit tests caught both).
    - **The bake generates the root over 2 s more of the take on each side of the trim, then cuts.** Padding at a
      budget cut through a turnaround ran the root 1.3 m off the pelvis (a 0.22 m frame step in
      `Run_CardLong_noStops_R`); now padding only stands in at a take's own ends, inside the 1 s trims.
  - `MotionClipStream.{h,cpp}`: the `.mmanim` pose stream (format in the header): per frame the root (x, z, yaw),
    the pelvis translation under it and one packed rotation per bone; one static rotation per finger bone; the clip
    table (name, tags, first frame, count, source start). `Bind` (by name, pelvis height ratio), `SamplePose` (nlerp
    between frames), `RootAt`, `RootMatrixAt`.
  - `MotionFeatures.{h,cpp}`: the `.mmdb` (format in the header): 27 features per frame in the frame's root space
    (both ankles' position and velocity, pelvis velocity, the root at +20/+40/+60 frames and its facing there) with
    their mean and standard deviation, and per-frame labels: gait, `Crouch`, `Strafe`, `Airborne`, `Excluded` (prone),
    `NoSearch` (the last 60 frames of a take), `ContactL/R`.
  - `MotionBake.{h,cpp}`: `--bake-motion <file.mmconfig> [--match <text>] [--out <prefix>]`, an early CLI tool (no
    window or GL). Takes are imported clip-only through `Model` on 4 worker threads; the rig diff runs first; the
    feet are rebuilt from the quantized stream on the *target* rig and compared with the source (worst 0.01 cm); the
    files are read back and timed. It fails on a budget overrun or a failed take. Report: `work/mm_bake/<name>.report.json`.
- **`TARTARUS_MM_SOURCE`** (`Env.h`): the pack folder (`anims\`, `rig\`, `manifest.json`). Nothing else reads the pack.
- **`project/assets/Animations/MotionMatching/Manny.mmconfig`** (committed: our own config, not licensed data): the
  bones, statics, RootSynth params (`byTag: snakes → yawSigma 0.15`), label thresholds, budgets. Paths are
  project-relative. The cooked `Manny.mmanim` / `Manny.mmdb` are git-ignored (`.gitignore`), not yet on the Drive
  (`Used\` registration is Phase 3).
- **Drive archive** (owner-approved, 2026-11-16): `Raw\Animations\Quantum Stage - Motion Matching Library\` (10.4 GB,
  692 files, sizes and sample SHA-256s checked): `Manny Export\` (`anims\`, `rig\`, `manifest.json`, the findings
  and the UE export script - point `TARTARUS_MM_SOURCE` here on another machine), `Raw Source Data\` (the pack's own
  data, with fingers), `Docs\` (info sheet, readme). `_INFO.md`, a `Raw\SOURCES.md` row, a `Raw\CHANGELOG.md`
  entry and a "Fab Standard License" section in `Licenses\README.md` were added. The Drive has ~144 GB free (the
  "nearly full" note was C:'s space); its local cache is on D:.
- **`tools/mm/gen_takes.py` → `tools/mm/takes.csv`** (139 rows, 82 included, 1.27 h): `_S` left out (tagged
  `exaggerated`), dodges and strolls left out of v1, 1 s trimmed off each end, and a seconds budget per pattern
  (Take001 first): walk/jog/run 90 s, crouch 70 s, stand 75 s, idles/steps/jumps 60 s, transitions 120 s, accel 240 s.
  Tags: the group, the pattern kind, `forward` (loop, circles, snakes: the facing check), and the suffixes (`fo`,
  `ml`, `fixed`). Tagging is from the names plus the pack's **info sheet** (a PDF, `Docs\` in the Drive archive;
  its facts are copied into the script's `SHEET` table): `_S` = exaggerated, `_M_L` = medium and large jumps,
  `Stand_to_Walk_Take002` is exaggerated though not named `_S` (now left out), `Crouch_Strafe_Star_Take001` is an
  incomplete pattern (kept). `_FO` it doesn't explain.
- **Rig diff** (pack `rig/SKM_Manny.fbx` vs `Quantum_Body_Full.fbx`, in each rig's `root` frame): **the 41 core bones
  match** - worst length 0.002 %, worst rest rotation 0.04°, nothing missing; pelvis 0.9590 m on both (scale 1.0000).
  No action needed. (Quantum carries the Z-up → Y-up turn on its `root` node; the pack's animation files carry it
  nowhere - identity `RootNode`. The stream lives in the `root` node's frame, so both play the same.)
- **Core bones: 41, not ~65.** Pelvis, spine 01-05, neck 01-02, head, and per side clavicle, upper arm + 2 twists,
  lower arm + 2 twists, hand, thigh + 2 twists, calf + 2 twists, foot, ball. Dropped: the UE IK bones (`ik_foot_*`,
  `ik_hand_*` hang off `root`, which the bake replaces, and nothing in the engine reads them), `interaction`,
  `center_of_mass`, the correctives/helpers (`*_in/_out/_fwd/_bck`, `twistCor`, `bicep/tricep`, `pec`, `latissimus`,
  `wrist_*`, `scap`). Fingers: 38 static bones posed from the middle of `Stand_Idle_Neutral_Take001`.
- **Labels, calibrated on the pack** (`Manny.mmconfig` `labels`):
  - gait from root speed: idle < 0.3, walk < 1.6, jog < 2.9, run above (m/s). The pack is slow - a 9 m capture
    area: straight walks ~1.25 m/s (p90 1.5), straight jogs ~1.7-1.8 (p90 2.0-2.3), runs peak 4.5-5.2.
  - crouch: pelvis under 0.84 × standing (0.806 m) and slower than a jog (a run's lean drops the pelvis to ~0.81).
  - prone (`Excluded`): pelvis under 0.20 m (only `Transitions_stationary`, down to 0.08 m; the deepest crouches sit
    at 0.30).
  - airborne: the lower ankle over 0.18 m **and** the pelvis 0.02 m over standing, 4+ frames. The ankles alone can't
    tell a hop from a run's flight (both ~0.25 m); the pelvis can (runs ≤ 0.97 m, jumps ≥ 1.00 m).
- **`--npc-test mmview`** (`NpcTest_MmView.cpp`, LocomotionLab's south-west quarter, no squad needed):
  - Loads `Manny.mmanim`/`.mmdb` (SKIP when not baked) and validates **every** frame on the Quantum rig (0.6 s): the
    budgets; all 41 + 38 bones bind; the root never steps over 0.15 m; forward takes' facing p95 under 20°; yaw
    jitter under 300; the planted feet rebuilt from the stream slide within 0.5 cm/s of the take's own (the
    `.mmdb`'s source feet); the `.mmdb` foot features match the rebuilt feet within 1 cm; labels fit (runs have run
    or jog frames, both big jumps airborne frames, crouch takes a third crouched, idles 95 % idle).
  - Then plays 10 takes × 7 s on a bare Quantum body, re-centred, the player unarmed and turning to follow it, the
    Scene view following from three-quarters. Drawn (Game view): the root's path (grey behind, yellow 1 s ahead), its
    facing (green), the hips' facing (red), the ankles (blue planted, white not), a cyan tick while airborne.
    `TARTARUS_MMVIEW_CLIPS='Name[@start];...'` picks the takes. Records every frame with `NPC_TEST_RECORD`.
  - The Game view draws `NpcTest::DebugLines` through the combat HUD's line pass (`Application.cpp`).
- **`tools/mm/frames_to_video.py`**: an `NPC_TEST_RECORD` folder → one MP4 (both views side by side; OpenCV, no ffmpeg).
- **Unit tests** (`UnitTests_MotionMatching.cpp`, filter `"Motion matching"`, 9 tests): smoothing/unwrap/packing; RootSynth on
  a swaying diagonal walk and a 720° turn in place (through ±180°); contacts and slide; the stream and feature files
  round-tripped (corrupt files refused) and bound by name; labels and features on synthetic takes; `takes.csv`.
  Two need licensed files and SKIP without them: the rig diff (`TARTARUS_MM_SOURCE`) and the cooked budgets.

**Results (2026-11-16):**
- Bake: 82 takes, 275,269 frames (1.27 h), 41 bones + 38 static; **poses 70.9 MB (150), features 28.9 MB (40), load
  0.15 s (2)**; 30 s on 4 workers (import ~100 s of CPU in all). Foot rebuild worst 0.01 cm. Forward takes' facing p95:
  median 8.7°, worst 14.2° (`Run_Loop_Take001`). Yaw jitter: median 72, worst 250 (`Run_Snakes`, on its σ 0.15 s).
  Frame steps: worst 0.086 m (a 5.2 m/s run). Labels: idle 113k / walk 139k / jog 19k / run 4.6k frames; crouch
  40k, strafe 32k, airborne 739, excluded (prone) 1.1k.
- `mmview`: 12/12 (budgets; 41/41 + 38/38 bones bound; step 0.086 m; facing 14.2°; jitter 250; slide gap 0.18 cm/s
  vs the take's own, whose worst is 15.6 cm/s of heel-to-toe roll; features vs rebuilt feet 0.012 cm; labels).
- Final full pass (`mm_smoke.ps1 testbed npcloco locomotion stopsteps feet labloco mmview -PerfBench
  tests\perf-scenes\perf_stress.json`, `work\mm_smoke\20261116-223524`): **all PASS** - unit 51638 checks, smoke
  21/21, testbed 29/29, npcloco 33/33, locomotion 20/20, stopsteps 10/10, feet 6/6, labloco 1/1, mmview 12/12, perf
  edit 3.65 ms / play 6.01 ms (baseline 2.87 / 5.65 p50; Phase 1's own final pass was 3.18 / 6.03 - legacy
  locomotion is untouched, so this is run-to-run noise, not a regression to chase).
- Recording: `work\mm_view\mmview_phase2.mp4` (Game view left, Scene view right; `frames_to_video.py`). **The owner
  signed off on it (2026-11-16)**, which closes Phase 2.

### Phase 3: Motion matching runtime
- `MotionDatabase`.
- `MotionSearch`: 16- and 64-frame AABB boxes, label filters, searches every ~0.1 s or on input changes,
  deterministic ties.
- `Inertialization`: dead blending.
- `TrajectoryPredictor`: spring-based and collision-aware.
- Debug draw.
- `--npc-test mmdb`: search time and left/right coverage.
- The gait speed table.
- Unit tests.
- Register the cooked assets in `Used\` and `external_assets.csv`.

**Done when:** the runtime pieces and their unit tests exist, `--npc-test mmdb` passes visibly, and the cooked assets are
registered.

**What landed (2026-11-16):**
- **`src/Game/MotionMatching/`**, all pure (no Model, no GL) and unit-tested on synthetic takes:
  - `MotionDatabase.{h,cpp}`: loads `Manny.mmanim` + `Manny.mmdb` together, checks them against each other (frames,
    clips), normalizes the 27 features per group (`(x - mean) / groupStd`, the group's std = the mean of its dims'
    std), builds the 16- and 64-frame AABBs (with each box's flag OR/AND and gait range) and the frame → clip map. It
    keeps only the normalized features (`Raw()` denormalizes; `MotionFeatureSet::ReleaseFeatureData`): poses 70.9 MB,
    features 34.4 MB, load 0.2 s.
  - `MotionSearch.{h,cpp}`: cost = Σ groupWeight × (q − x)² (weights foot pos 0.75, foot vel 1, pelvis vel 1, traj
    pos 1, traj dir 1.5, applied at search time) + a soft gait cost (0.5 per gait step from the desired one: the gait
    labels overlap) + an optional transition cost. Filters: `Require`/`Forbid` flags; `Excluded` and `NoSearch` always
    forbidden, `Airborne` by default (jumps will be label-locked playback), `Crouch` required or forbidden by the
    host. The boxes skip on their lower bound and their flags; a frame's sum stops once it can't win. **The answer is
    exactly brute force's** (`SearchBrute`; same per-dim terms in the same order, so the bound never exceeds a
    frame's cost even in float). Ties: frames in ascending order, strictly lower cost wins, the playing frame wins a
    tie. Frames of the playing clip within `IgnoreNear` (20) of the playing frame are skipped.
    `MotionSearchSchedule`: every 0.1 s, at once on an input change (desired velocity by 0.5 m/s, facing by 20°, the
    filter), or when the playing frame can't go on.
  - `Inertialization.{h,cpp}`: **dead blending** (Holden 2023). At a jump the shown pose and its velocity (per bone
    angular velocity, the pelvis's linear velocity, from the last two output poses; clamped at 4π rad/s and 6 m/s)
    are extrapolated with the velocity decaying (half-life 0.05 s), and the output cross-fades into the new animation
    over 0.2 s with a smoothstep. A jump mid-blend starts from the screen.
  - `TrajectoryPredictor.{h,cpp}`: closed-form critically damped springs (Holden's "simple spring damper"): velocity
    toward the desired one (half-life 0.2 s), facing toward the desired yaw (0.15 s, the short way round).
    **Collision-aware**: the path is swept 20 steps per second through a host callback; a hit stops the move at the
    contact, slides the rest along the surface, and drops the velocity, acceleration and desired velocity's part into
    it for the rest of the prediction. `Predict(..., goalTurnRate)` keeps the goal turning through the prediction (a
    stick held into a curve; Phase 5's navmesh corners). `Update` is the simulated character (`mmdb`'s capsule).
  - `MotionMatcher.{h,cpp}`: the loop on one character - play on, search on the schedule (query = the playing frame's
    pose features + the desired trajectory turned into root space with `RootSynth::RootMatrix`), jump, dead-blend,
    sample the pose (`MotionPose`: stream-bone rotations + pelvis). **A playing frame that fails the filter forces a
    search before it is shown** (an actor standing up out of a crouch, a jump's flight, `NoSearch` near a take's end).
    `WritePose` binds it onto a rig.
  - `MotionGait.{h,cpp}`: **the gait speed table**, measured from the database by leg plateaus (below).
  - `MotionDebugDraw.{h,cpp}`: HUD lines (`NpcTest::DebugLines` format): a root-space trajectory placed at a character
    (desired or matched), predictor states, the root, the feet (blue planted).
- **`--npc-test mmdb`** (`src/Game/AI/NpcTest_MmDb.cpp`, LocomotionLab, ~77 s, SKIP when unbaked):
  - Offline: load and budgets; 1000 searches timed, the first 200 against brute force; 200 self-matches (cost 0) and
    repeats (same frame); the gait table; left/right coverage (frames per gait turning/strafing each way, and
    trajectory-only queries for left and right turns at 45 and 90 °/s and strafes, per gait).
  - Live (visible): a bare Quantum body driven by `MotionMatcher`, `TrajectoryPredictor` as its capsule (sphere casts
    against the solid world), through idle, walk, walk circles left/right, stop, strafes left/right, jog circles, run
    circles, stop, a crouch circle, crouch idle, stand, and a walk at the 1 m ledge ~39° off its normal. Drawn: the
    predicted path (grey), the desired trajectory (yellow), the matched frame's (magenta), the root (green), the feet;
    a magenta tick over the head at each jump. Checked: ~10 searches/s and their time, jumps blended, the crouch filter
    held, never airborne, the trajectory error per segment, the slide along the ledge (no penetration).
- **Unit tests** (`src/Tests/UnitTests_MotionMatchingRuntime.cpp`, filter `"Motion matching"`, 10 tests on a synthetic
  13-take database with a non-zero rest yaw): database build and boxes; box search = brute force over 400 mixed
  queries (and prunes > 2x); filters and ties (duplicate takes, Excluded/NoSearch/Airborne never matched, crouch
  require/forbid, the gait cost's pull, IgnoreNear); the schedule; dead blending (continuity, ends on the target, no
  pops, chained jumps, the velocity clamp); springs (prediction = simulation, no overshoot, the short way round, a
  wall: the prediction runs along it); the gait table (synthetic legs); the query's root-space convention (a frame's
  own future gives back its features exactly); the matcher end to end (walk, then a left turn); the cooked database
  (box = brute, SKIPs unbaked).
- **The cooked assets are registered**: `Manny.mmanim` / `Manny.mmdb` rows in `project/external_assets.csv` (only
  those two rows - see Open issues on `build`), both files copied to the Drive's `Used\project\assets\Animations\
  MotionMatching\` and hash-checked, the CSV copied there, a line in `Raw\CHANGELOG.md`. `asset_manifest.py verify`
  on `Used`: 8025 listed, 0 missing, 0 different. `check_git_assets.py` passes.

**The gait speed table** (`GaitSpeedTable::Measure`, the current bake; `kMeasured` defaults in `MotionGait.h`):
- The pack's takes cross a 9 m capture area, so a take is legs (speed up, cruise, slow down, turn round). A leg: at
  least 1 s moving forward over 0.5 m/s, turning under 20 °/s (over ±0.25 s), not strafing or airborne, crouched only
  in crouch takes. Its plateau: its best 0.5 s mean speed. The table: the median plateau per take group; for run,
  only legs whose plateau is labelled Run (the rest are the walk-backs between sprints).
- **walk 1.27 m/s** (68 legs, p25-p75 1.16-1.40), **jog 1.85** (61 legs, 1.53-2.11), **run 3.91** (17 legs,
  3.71-4.25, max 4.99), **crouch-walk 0.79** (27 legs, 0.74-0.87).
- **Strafes are slower** (same rule on Strafe frames): walk 1.14 (26 legs), jog 1.32 (37), crouch 0.76 (8); run has no
  strafe takes (4 stray legs, 1.11).
- Per-frame percentiles don't work: the runs barely cruise (43 steady frames above 2.9 m/s in the whole bake), so the
  handoff's earlier "run 4.5-5" was the peaks, not a cruise.

**Left/right coverage** (`mmdb`; + = left):
- Searchable moving frames turning each way (facing at +1 s over 15° off): walk 30869 / 29867 (0.97), jog 5997 / 5794
  (0.97), run 1177 / 1549 (0.76), crouch 3652 / 3521 (0.96). Strafing: walk 6336 / 8159 (0.78), jog 707 / 353 (0.50),
  run 85 / 39, crouch 1030 / 1052 (0.98).
- Trajectory-only queries, error of the best match at +1 s, left / right: walk turn 45 0.07 / 0.04 m, turn 90 0.08 /
  0.14, strafe 0.18 / 0.09; jog 0.07 / 0.15, 0.21 / 0.25, strafe 0.09 / 0.13; crouch 0.10 / 0.13, 0.20 / 0.16, strafe
  0.26 / 0.22; run 0.57 / 0.44, 0.92 / 0.81 (no data that fast on a curve). Worst left/right gap on walk/jog/crouch:
  0.09 m (walk strafe).
- **Verdict: no mirroring needed for v1.** Turns are balanced within 4 % on walk, jog and crouch, and left and right
  match equally well. The thin spots (jog strafes 2:1, run anything) are thin on both sides, which mirroring wouldn't fix.

**Results (2026-11-16):**
- Search over 275,269 frames: **avg 0.41 ms, p99 0.96, max 2.0** (brute force 5.3 ms; 1410 frames scored per search);
  0 of 200 differ from brute force; 200/200 self-matches; 200/200 repeats identical. Live: 742 searches in 74 s (10.0
  per second; 2 on input changes, 2 at a take's end), avg 0.33 ms, p99 1.06, max 2.6.
- Jumps: 145 in the course; the pose's step at a jump avg 4.9° (worst 10.6°) against the raw jump's 27.9° (worst
  124°); a normal frame's p99 is 20.3°.
- Trajectory error at +1 s, median per segment: walk 0.15 m, walk circles 0.15, strafes 0.22-0.27, jog circles
  0.09-0.19, crouch circle 0.21, idles 0.00-0.06, the ledge walk 0.07; **run circles 0.87-1.06 m and 20-24°** (see Open
  issues).
- The ledge: 154 sweep hits, the 0.3 m sphere never inside it (closest gap 0.000 m) while it slides along the face.
- Final full pass (`mm_smoke.ps1 testbed npcloco locomotion stopsteps feet labloco mmview mmdb -PerfBench
  tests\perf-scenes\perf_stress.json`, `work\mm_smoke\20261116-232610`): **all PASS** - unit 52240 checks, smoke 21/21,
  testbed 29/29, npcloco 33/33, locomotion 20/20, stopsteps 10/10, feet 6/6, labloco 1/1, mmview 12/12, mmdb 18/18
  (171 fps), perf edit 3.15 ms / play 6.02 ms (Phase 2: 3.65 / 6.01; legacy locomotion untouched).

### Phase 4: First-person player (unarmed)
- MM becomes the pose source in `FirstPersonBody`, with retuned speeds, warping and a root clamped to the capsule.
- Turn-in-place feeds `MaxYawRate`. Crouch and sprint are label filters. The airborne segment is added.
- These consumers keep being fed:
  - gait phase (camera bob and sway, `PlayFrame.cpp:257`);
  - per-foot heights (footsteps, footprints);
  - `FootIK`.
- A temporary `LocomotionSource {Legacy, MotionMatching}` setting.
- `--npc-test mmplayer`, plus testbed parts A and C. The owner signs off on recordings.

### Phase 5: NPCs (unarmed)
- MM in `NpcBody::Tick` (~195–380), with one shared database.
- Trajectories from navmesh corners, plus LOD.
- Port `npcloco`, `locomotion`, `stopsteps`, `roam`, `labloco`, and testbed parts E and F.
- Perf compared against the baseline. The owner signs off on recordings.

### Phase 6: Legacy cleanup and final verification
- Delete only after a reference grep, and only after the owner approves the delete list (repo and Drive `Used\`).
- Remove the legacy body/NPC controllers, their generators, the legacy `BodyLocomotion` triggers and `LoopPlayRate`,
  `LocomotionSource`, and the legacy locomotion mocap.
- Docs: `docs/MOTION_MATCHING.md`.
- Final full run of `mm_smoke.ps1`.

## Test protocol (every phase)

- Tests always run **visible**: never set `TARTARUS_HIDDEN`, and keep `--visible` on `tools/npc_test_runs.py`.
- Tests always run with **unlocked FPS**: `TARTARUS_UNLOCKED_FPS=1`, from Phase 1 on. Game time stays on the fixed
  1/60 s step, so results don't depend on frame rate.
- **Test budget** (the owner's rule: don't over-test):
  - While working, run only the scenario or unit filter that the change touches.
  - Run the full `tools/mm_smoke.ps1` pass once, at the end of the phase. Use 3 runs (`-Runs 3`, or
    `npc_test_runs.py --runs 3`) only for scenarios known to be noisy, such as `npcloco`.
  - Re-capture `docs/mm_baseline/` only if legacy locomotion code changed.
  - Never re-run a failure whose cause is known and that a re-run can't fix (a broken local asset, a stale scenario).
    Record it under Open issues instead.
  - When a run fails, diagnose it before running again. Repeat a run only to check for flakiness.
- Commands (PowerShell):
  - Everything: `powershell -ExecutionPolicy Bypass -File tools\mm_smoke.ps1 testbed npcloco locomotion stopsteps feet
    labloco mmview mmdb -PerfBench tests\perf-scenes\perf_stress.json`. It should be all PASS. Use `-AllowErrors` only for a local
    asset you know is broken, and say so under Open issues.
  - Compare with the baseline: add `-TraceDir <dir>` to `mm_smoke.ps1`, then
    `python tools/mm/baseline_summary.py <log dir> <out dir> --traces <dir>`; diff `SUMMARY.md` and `metrics.json`
    against `docs/mm_baseline/`.
  - Build: `cmake --build build --config Release --parallel`
  - Unit tests: `build\Release\TartarusEngine.exe --unit-tests "<filter>"`
  - Smoke test: `build\Release\TartarusEngine.exe --smoke-test tests\smoke-scenes`
  - Scenarios: `python tools/npc_test_runs.py build\Release\TartarusEngine.exe <scenarios> --runs 3 --visible`
- How results are reported:
  - `[SmokeTest] PASS|FAIL` lines (with `rendered= wall= fps= unfocused=`) and `[NpcTest] PASS|FAIL` lines;
  - the engine's `[error]` lines go to stderr (`<run>.stderr.log` under `mm_smoke`);
  - the logs are in `Headless.log`;
  - exit code 0 means everything passed.

## Code map (where things are)

**Animation core**
- `src/Renderer/Animation.h` and `src/Renderer/Model.{h,cpp}` (`SampleLocalPose`, `AttachClip`,
  `FindRootMotionNode`, `ClipRootMotion`).
- `src/Game/RootMotion.h`, `src/Game/AnimationSystem.cpp`.
- `src/Game/AnimatorController.{h,cpp}`: the state machine, kept for the weapons.
- `src/Game/ClipAnalysis.{h,cpp}`: clip speed and foot contacts.
- `src/Game/IK.{h,cpp}` (`FootLock`, `SolveTwoBone`, `FindTwistBones`).

**Player**
- `src/Game/FirstPersonBody.{h,cpp}`: `Tick`, the parameter block at ~1162, `ApplyFootIK`, `GaitPhase`.
- `src/Game/BodyLocomotion.{h,cpp}`: the triggers and `LoopPlayRate`.
- `src/Game/Player.cpp`, `src/Physics/CharacterMover.cpp`.

**NPCs**
- `src/Game/Npc/NpcBody.{h,cpp}` and `src/Game/AI/NpcDirector.cpp`.
- `project/assets/AI/Soldier.json`, `tools/gen_npc_soldier.py`.

**Legacy graphs**
- `project/assets/Animations/Controllers/fps_body_locomotion.controller` (+ `.template.json`), generated by
  `tools/gen_body_locomotion.py`.
- `project/assets/AI/npc_soldier.controller`.

**Frame order** (`src/App/PlayFrame.cpp`): `Player::Update` → `firstPersonBody.Tick` → `npcDirector.Think` →
`UpdateAnimatorControllers` → `firstPersonBody.LateUpdate` → presentation → `npcDirector.LateUpdate`.

**Motion matching** (Phase 2+)
- `src/Game/MotionMatching/`: `MotionMath.h`, `RootSynth`, `MotionClipStream` (`.mmanim`), `MotionFeatures` (`.mmdb`),
  `MotionBake` (`--bake-motion`, wired in `src/App/CliTools.cpp` and `LaunchOptions.cpp`).
- `project/assets/Animations/MotionMatching/Manny.mmconfig`; cooked `Manny.mmanim` / `Manny.mmdb` beside it (git-ignored).
- `tools/mm/gen_takes.py` → `tools/mm/takes.csv`; `tools/mm/frames_to_video.py`.
- `src/Game/AI/NpcTest_MmView.cpp` (`--npc-test mmview`); `src/Tests/UnitTests_MotionMatching.cpp`.
- Runtime (Phase 3): `MotionDatabase`, `MotionSearch` (+ `MotionSearchSchedule`), `Inertialization`, `TrajectoryPredictor`,
  `MotionMatcher`, `MotionGait` (`GaitSpeedTable`), `MotionDebugDraw`; `src/Game/AI/NpcTest_MmDb.cpp` (`--npc-test mmdb`;
  `NpcTest::MmDbLines` feeds `DebugLines`); `src/Tests/UnitTests_MotionMatchingRuntime.cpp`.
- Run the runtime: `tools\mm_smoke.ps1 mmdb -SkipSmoke -UnitFilter "Motion matching"` (build, the MM unit tests,
  `mmdb`: ~2 min).
- Bake: `set TARTARUS_MM_SOURCE=D:\Jacob\JCamberos\Manny_Mocap_Export` then
  `build\Release\TartarusEngine.exe --bake-motion assets/Animations/MotionMatching/Manny.mmconfig` (30 s, 4 workers;
  `--match <text>` bakes some takes, `--out <prefix>` writes elsewhere - use D: for scratch).
- Takes list: `python tools/mm/gen_takes.py` (reads `TARTARUS_MM_SOURCE`'s `manifest.json`).

**Tests**
- `src/Tests/SmokeHarness.cpp`: the scenario-to-scene map at ~86–101.
- `src/Game/AI/NpcTest*.cpp`.
- `src/Tests/UnitTests_Animation.cpp`.
- Launch flags: `src/App/LaunchOptions.cpp`.
- Env vars: `src/Core/Env.h`. Every env var must be listed there.
- Tools: `tools/mm_smoke.ps1`, `tools/mm/baseline_summary.py`, `tools/npc_test_runs.py`; baseline in `docs/mm_baseline/`.

## Open issues

- `_FO` is still unexplained (the info sheet just calls those takes idle variations); `_M_L` = medium and large jumps.
  Both are in the `mmview` playlist for the owner to look at.
- **The root keeps most of the stride's side-to-side sway.** The decided σ 0.10 s passes ~82 % of a 1 Hz sway
  (`exp(-(2π f σ)²/2)`; the unit test measures 3.3 of 4 cm). `RootValidation::PathWobble` (stride band, RMS) reads
  walk 4.5, jog 6.3, run 10.5 cm median on the pack (sharp card turns are in that band too). Phase 3: if the
  trajectory features or the drawn path look noisy, raise `posSigma` per tag in `Manny.mmconfig` and rebake (30 s).
- Yaw jitter has no spec number: `mmview` fails over 300 (the worst take is 250). A regression guard, not a quality bar.
- Gait labels from speed alone overlap: a jog through a turn is slower than a straight walk's peak. Phase 3 should use
  them as soft costs or for reporting, not hard filters (speed is in the features anyway).
- The UE IK bones are not streamed (see "Core bones: 41"). If a later phase needs `ik_foot_*`, they must be rebuilt
  under the generated root, not copied.
- Foot contacts use the ankle (low and under 0.6 m/s), so they include heel-to-toe roll: absolute "slide" reads
  5-15 cm/s on clean mocap. Phase 4's foot lock may want ball-of-foot contacts too.
- `Crouch_Strafe_Star_Take001` is an incomplete pattern (info sheet); it's in, at 28 s.
- *Resolved 2026-11-16:* the cooked `Manny.mmanim` / `.mmdb` are in `Used\` and `project/external_assets.csv`.
- **`asset_manifest.py build` picks up local, uncommitted work.** In this checkout it also listed the owner's ignored
  `AKS74U/Attachments/` binaries and `scenes/SeedMesh_Mossy_Cedrus_Forest_Mossy_forest_trail.json` (111 MB), and moved
  one row (`Light_Switches.fbx`, the committed file isn't strictly sorted there). Phase 3 added only its two rows to the
  committed CSV, and exported only those two files (the full `export` would also hash all 15 GB on the Drive). That
  SeedMesh scene is already on the Drive's `Used\scenes\` but not in the CSV (`verify`: 1 unlisted) - the owner's.
- **Run:** the pack's runs barely cruise (17 sprint legs; 43 steady frames over 2.9 m/s) and never run fast on a curve
  (its run circles are 2-2.75 m/s). At the table's 3.91 m/s, `mmdb`'s run circles (60 °/s) match 0.9-1.1 m and 20-24°
  off at +1 s. Phase 4: cap the player's turn rate at run speed (or slow the run in turns), and choose run 3.9 m/s
  (the plateau) or something nearer the 4.5-5 peaks with speed warping.
- **Strafes are slower than forward** (walk 1.14 vs 1.27, jog 1.32 vs 1.85, crouch 0.76 vs 0.79), there are no run
  strafes, and jog strafes are 2:1 left (707 / 353 frames). Crouch strafes (8 legs) match 0.22-0.26 m off at +1 s, both
  sides. Phase 4 retunes strafe speeds from `GaitSpeedTable::Strafe` and doesn't strafe at run.
- The search weights and the dead-blend times are Holden's defaults, not tuned on this pack. Tune them in Phase 4 on
  the player with recordings, not on numbers alone.
- The query's pose part is the playing frame's own features, not the blended output's. While a dead blend runs they
  differ for 0.2 s; standard practice, but a candidate if Phase 4 sees jumps chain.
- One crouched frame (`Stand_Jump_360_M_L_Take001` @ 11.4 s, a landing) is the best crouch-strafe-right match: the
  crouch label also covers deep landings. Harmless while jumps are label-locked; watch it in Phase 4.
- *Resolved 2026-11-16:* pack-Manny vs Quantum rest pose and bone lengths: the 41 core bones match (worst 0.002 %).
- *Resolved 2026-11-16:* two zero-filled local assets, `project/assets/Audio/Ambience/worklight_hum_1.wav` and
  `project/assets/Characters/Spider/Textures/T_Spider_AO.png`.
  - Every byte was 0, so every run logged `[error]`s; they were what made the baseline runs `PASS*`.
  - Restored from the Drive's `Used\project\assets\...`, after checking the sha256 against `external_assets.csv`.
  - A scan of all 11,632 asset files found no others.
- *Resolved 2026-11-16:* `feet` failed its two ramp checks. The cause was a test bug, not the scene.
  - `feet` teleported the dummy with `SetNpcFootPosition` but left the director's `Feet` and `Velocity` alone.
  - The director read the jump as a speed of hundreds of m/s and slid the dummy off the ramp and across the Arena.
  - `NpcTest::Feet` now resets that state; the run is 6/6.
  - The baseline's `feet` numbers in `docs/mm_baseline/` predate this fix.
- Npc-scenario frame cost (`fps=` / ms per frame) includes scene loading and the model imports during the run, so it
  is only a rough figure. `perf_stress` is the frame-cost baseline. If Phase 5 needs per-scenario frame cost, add a
  steady-state window (for example, after the first 2 s of Play) to the summary line.
- Only `testbed` and `labloco` write `NPC_TEST_TRACE`. `npcloco`, `locomotion` and `stopsteps` log their metrics
  (`[NpcLoco]`, `[Locomotion]`, `[StopSteps]`, `[Slide]`).
- The baseline is one run per scenario. Re-runs were cut short on purpose. For noise bands, use
  `tools/npc_test_runs.py --runs 3`, but only if a later comparison is borderline.
- A forced focus steal (Notepad launched from a background job) did not take focus: Windows' foreground lock. Focus
  loss was checked through natural focus changes during the smoke run instead.

## Phase 4: first steps

1. Branch `dev/mm-phase4` from `dev/mm-phase3`, in the main checkout (licensed assets and `build\` only exist there).
   The cooked database is on the Drive's `Used\` now (`fetch-assets.ps1`), or rebake (30 s).
2. Read `FirstPersonBody::Tick` and the parameter block (~1162), `BodyLocomotion` (`LoopPlayRate`), `Player.cpp` and
   `CharacterMover.cpp`: where the legacy controller's pose enters, and what reads the gait phase, foot heights and
   `FootIK`.
3. Add `LocomotionSource {Legacy, MotionMatching}` (temporary setting). In MM mode, one `MotionDatabase` (shared, loaded
   once) and a `MotionMatcher` on the body; the capsule is the simulated character: feed `TrajectoryPredictor` from the
   player's input (the stick as the desired velocity at `GaitSpeedTable` speeds, strafes at `StrafeFor`), with its
   `Sweep` on the capsule's own cast, and keep `TrajectoryPredictor::State` in step with the capsule (or predict from
   the capsule's position and velocity each frame).
4. Retune the player's walk / jog / run / crouch speeds to the table (walk 1.27, jog 1.85, run 3.91, crouch 0.79;
   strafes 1.14 / 1.32 / 0.76) and decide the run's turn-rate cap (Open issues: Run).
5. Orientation and speed warping in place of `LoopPlayRate`; the root clamped to the capsule; turn-in-place feeding
   `MaxYawRate`; crouch as `Require`/`Forbid Crouch`; sprint as the run gait.
6. Keep feeding gait phase (camera bob, `PlayFrame.cpp:257`), per-foot heights and `FootIK` - the feet's contact flags
   and positions are in the matched frame's features (`MotionDatabase::Raw`: dims 0-5, flags ContactL/R).
7. The airborne segment: label-locked playback of the baked jump frames (`Require Airborne` is how to find them) with
   inertialization in and out.
8. `--npc-test mmplayer`, plus testbed parts A and C; reuse `MotionDebugDraw` and `mmdb`'s course. Record for the owner.

## Phase 3: first steps (done)

1. Branch `dev/mm-phase3` from `dev/mm-phase2`, in the main checkout (licensed assets and `build\` only exist there).
   Big scratch output on D:.
2. Bake first if the cooked files are missing: `set TARTARUS_MM_SOURCE=D:\Jacob\JCamberos\Manny_Mocap_Export`, then
   `build\Release\TartarusEngine.exe --bake-motion assets/Animations/MotionMatching/Manny.mmconfig` (30 s). The pack
   is also on the Drive (`Raw\Animations\Quantum Stage - Motion Matching Library\Manny Export`).
3. `MotionDatabase` loads `Manny.mmanim` + `Manny.mmdb` (`MotionClipStream::Load`, `MotionFeatureSet::Load`); normalize
   with the stored mean/std; never match `NoSearch` or `Excluded` frames.
4. `MotionSearch` over the 27 features (layout in `MotionFeatures.h`), with feature-group weights; the gait labels as
   soft costs (they overlap); `Airborne` frames only through the label-locked jump segments.
5. The gait speed table from this bake (`work/mm_bake/Manny.report.json`, `speedP50` / the takes' straight parts): walk
   ~1.25 m/s, jog ~1.8-2.0, run 4.5-5.
6. `Inertialization` and `TrajectoryPredictor`, then `--npc-test mmdb`. Reuse `mmview`'s FK and line drawing
   (`NpcTest_MmView.cpp`, `NpcTest::DebugLines`) for the debug draw.
7. Register `Manny.mmanim` / `.mmdb` in `Used\` and `project/external_assets.csv` (`tools/assets/asset_manifest.py`).

## Phase 2: first steps (done)

1. Branch `dev/mm-phase2` from `dev/mm-phase1`, in the main checkout. The licensed assets and `build\` exist only
   there, so a separate worktree can't run the engine. C: has little free space; put big scratch output on D:.
2. The pack is already on this machine. Read it through a new `Env.h` variable (for example `TARTARUS_MM_SOURCE`); never
   hard-code the path.
   - `D:\Jacob\JCamberos\Manny_Mocap_Export\`: `anims\` (139 FBX, 4.47 GB), `rig\SKM_Manny.fbx`, `manifest.json`.
   - The pack's raw source data, with fingers, is in `D:\Jacob\JCamberos\Downloads\raw_source__data\`.
   - The owner has OK'd Drive access. The Drive (`G:\My Drive\Tartarus Assets`) is nearly full, so ask before writing
     the archive there.
3. Diff the rest pose and bone lengths: pack `rig/SKM_Manny.fbx` against `Quantum_Body_Full.fbx`, through `Model`.
   Report any core bone that differs by more than 2%.
4. Write `RootSynth` with unit tests on synthetic clips (a pelvis on a sine path, turning in place) before touching
   real takes.
5. Add the `--bake-motion` mode next to `--bake-gi` in `LaunchOptions.cpp`, then `MotionBake` and `MotionClipStream`.
6. Add the `mmview` scenario. Verify with `mm_smoke.ps1 mmview -SkipSmoke` plus the unit tests, and one full pass at the end.

## Session log

- **2026-11-17, Phase 0:**
  - Analyzed the project, the pack's readme and info sheet, and the UE5 export findings.
  - Settled the roadmap and the decisions above.
  - Created this doc and `docs/MOCAP_EXPORT_FINDINGS.md`.
  - No code changes.
- **2026-11-16, Phase 1:**
  - Added `TARTARUS_UNLOCKED_FPS`, harness input that ignores focus (`Input::SetIgnoreDevices`), and per-scene
    timing on the `[SmokeTest]` line.
  - Added `tools/mm_smoke.ps1` and `tools/mm/baseline_summary.py`; `npc_test_runs.py` now uses unlocked FPS.
  - Captured the legacy baseline into `docs/mm_baseline/`.
  - Verified with visible runs at unlocked FPS: unit 51375 checks, 0 failures; smoke 20/21 (the bad wav); all npc
    checks pass except `feet`'s stale two; up to 433 fps on light scenes.
  - Final full `mm_smoke.ps1` pass:
    - build and unit tests PASS; perf PASS (edit 3.18 ms, play 6.03 ms);
    - smoke, testbed, npcloco, locomotion and stopsteps `PASS*` (only the two broken assets' log errors);
    - feet FAIL (stale);
    - labloco failed for lack of a trace folder. Fixed in the script (traces now default to `<LogDir>\traces`), and
      its one re-run was `PASS*`.
    - testbed spent 1642 frames without focus and still passed 29/29.
  - Afterwards:
    - restored the two zero-filled assets from the Drive (hash-checked);
    - fixed the `feet` teleport bug (6/6, no log errors);
    - found the pack on D:.
  - Found the two zero-filled assets and the `feet` failure; both are resolved (see Open issues).
- **2026-11-16, Phase 2:**
  - Added `src/Game/MotionMatching/` (`MotionMath.h`, `RootSynth`, `MotionClipStream`, `MotionFeatures`, `MotionBake`),
    `--bake-motion`, `TARTARUS_MM_SOURCE`, `Manny.mmconfig`, `tools/mm/gen_takes.py` + `takes.csv`,
    `tools/mm/frames_to_video.py`, `--npc-test mmview` (+ `TARTARUS_MMVIEW_CLIPS`, `NpcTest::DebugLines` drawn by the
    combat HUD), 9 unit tests.
  - The rig diff: the 41 core bones match. The bake fits every budget by a wide margin (71 / 29 MB, 0.15 s).
  - The unit tests caught three things before the real takes did: `acos`-based quaternion angles can't resolve
    under 7e-4 rad in float; a point mirror through the edge sample leaves the edge unsmoothed; σ 0.10 s keeps most
    of the stride sway (kept as decided, reported).
  - `mmview` caught two: a root running 1.3 m off the pelvis at budget cuts (fixed: the root is generated over 2 s
    past each trim), and a slide limit set on the mocap's own heel roll (replaced by rebuilt vs source).
  - Calibrated the labels on the pack (gait speeds, crouch with speed, prone at 0.20 m, airborne with the pelvis).
  - Read the pack's info sheet: `_M_L`, and `Stand_to_Walk_Take002` is exaggerated (now left out).
  - Archived the pack on the Drive (owner-approved), with its `_INFO.md`, index, changelog and license entries.
  - Visible runs only, through `mm_smoke.ps1`: the unit filter and `mmview` while working, then one full pass, all
    PASS. One `mmview` re-run was a recording overlapped by a Drive copy (frames dropped; re-recorded for the owner).
- **2026-11-16, Phase 3:**
  - Added the runtime (`MotionDatabase`, `MotionSearch` + schedule, `Inertialization`, `TrajectoryPredictor`,
    `MotionMatcher`, `MotionGait`, `MotionDebugDraw`), `--npc-test mmdb`, 10 unit tests; registered the cooked files.
  - Box search: exactly brute force's answer, 0.41 ms avg on 275k frames (brute 5.3 ms). Dead blending cuts a jump's
    pop from 28° to 5° avg. Left/right turns are balanced (ratios 0.96-0.97): no mirroring for v1.
  - Gait table by leg plateaus: walk 1.27, jog 1.85, run 3.91, crouch 0.79 m/s; strafes slower. Per-frame percentiles
    were tried first and gave run 2.8 m/s (the runs' walk-backs).
  - `mmdb`'s first run (14/18) caught: the matcher playing into frames the filter refuses between searches (crouch 5,
    airborne 15; fixed: such a frame forces a search), strafe queries at forward speed (the pack's strafes are slower;
    the table measures them now), and a wall metric that ignored the ledge's corners (now sphere-to-box). It also
    showed the runtime holding the features twice (63 MB > 40; now normalized only, 34 MB). The crouch-strafe miss
    (both sides, 1-5 cm over) became a reported row, left/right symmetry stays gated.
  - Visible runs only, through `mm_smoke.ps1`: the unit filter and `mmdb` while working (4 runs), then one full pass,
    all PASS.
