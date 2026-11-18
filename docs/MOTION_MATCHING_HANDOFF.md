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
| 4. First-person player integration (unarmed) | `dev/mm-phase4` | **In progress** (the strip-back and the full-library plan, Steps 0-4: the whole pack baked with styles, fingers, repairs and mirrors; the query asks for a style; open: the foot lock's release-and-drag when the body turns faster than the clip - "Phase 4: next steps") |
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
5. **Mirroring where the audit finds the pack lopsided** (revised 2026-11-17, the full-library plan): the jog's and the
   stroll's strafes (left / right steady seconds 1.40 and 1.85 apart; `docs/mm_audit.md`) are baked a second time
   mirrored, as takes of their own (`MotionBake::MirrorPose`, rest-corrected) - real pose data, not sample-time mirroring:
   ~7 min, and no runtime path for virtual frames.
6. **Budgets** (revised 2026-11-17, the owner: compress, then raise the caps): at most 200 MB of pose memory (smallest-three
   16-bit body rotations, 32-bit finger tracks, 60 fps), at most 40 MB of features (16-bit), at most 2 s to load - the
   caps live in `Manny.mmconfig` and every check reads them there. The database is the whole pack but the exaggerated
   takes and the dodges: 2.23 h (174.9 MB poses, 51 of it fingers; 26.2 MB features). The bake fails over a cap.

## Asset gaps (the pack doesn't have it; not faked)

Measured on the full-pack audit (`docs/mm_audit.md`, 2026-11-17). Each is a capture to buy or shoot, not a runtime trick:
- **A sustained run / sprint (in the pack).** The pack was shot in a 9 m room: the longest time anywhere at >= 3.5 m/s is
  1.6 s / 5 steps, always a dash (it rises to a peak and brakes); the gate (6 steady strides at 3.5 m/s) fails 0 of 12.
  Two attempts on the pack's own data, both measured and retired (2026-11-17):
  - **Phase A, a held sprint** (the run style at 3.93 m/s, searched like any gait): pose step p99 22.4 deg (gate 12.6), 40 %
    of frames cruising (gate 70), the longest take 0.58 s - stitched dash fragments. Not a gait.
  - **Phase B, the sprint burst** (one of the pack's 77 dashes played whole, locked, the capsule at the clip's own speed and
    heading): 0 jumps inside a dash, slide 0.0-1.8 cm, but 2-3 s a burst at best (the dashes run 1.1-4.1 s). The owner, after
    the longest-dash preference: "still extremely short". Removed entirely (the dashes, `StartBurst`/`EndBurst`, `DashSpeed`,
    `Player::RunAllowed` / `RunFollowsExactly` / `RunHeading`, the cooldown, the `[MmBurst]` logs and their MmTune keys).
  - **The sprint now: the old locomotion's jog set** (the owner's call, 2026-11-17; "the sprint works great"). The legacy
    `assets/Animations/Mocap/RootMotion/Locomotion_V2/AM_Jog/AM_Loco_Jog_*` clips (3.26 m/s forward; a looped set with starts,
    stops, pivots and turning starts - the "full blast run" set was not used, the owner judged it poor) are baked into the
    database as their own style, **`sprint`** (`MotionLabel::StyleSprint`, gait table group 5). `gen_takes.py` lists them
    (`LEGACY_*`): 28 clips, the 8 direction loops played 12 times over (`loop` column; the root's travel carried cycle to
    cycle), tagged `rootmotion` (their travel is on the `root` bone: the bake takes the body against the root's place at the
    clip's start, `MotionTake::RootMotion`). The side loops differ - Left 2.30 m/s, Right 2.96 - so a strafe either way had one
    of them out of its warp range (strafe right slid 31 cm, 3.3 jumps/s): both are also baked mirrored (`LEGACY_MIRROR`; the
    bake now mirrors a root motion clip's travel too, `M R M`). 30 rows; the database 2.29 h, poses 178.6 / 200 MB, load 0.32 s.
    Pelvis height 0.915 against the pack's 0.928; feet rebuilt within 0.23 cm.
  - **Controls (Left Shift):** held from standing, the pack's jog; **pressed while moving, the sprint** - held while Shift is
    held and the stick moves (`Player::SprintHeld`, gait slot 4 at the sprint style's measured speeds); letting go, stopping
    or crouching ends it.
  - **The authored root** (after the owner's play test, from the full pass): the set's clips carry their own root (the
    capsule an animator moved), so the bake takes it (`MotionBake.cpp`, `RootMotion`) instead of generating one from the hips.
    Generated, the side clips faced the hips' twist toward the travel - the strafe loops 44 and 57 deg off ahead, the diagonals
    21 - and wobbled 9-173 deg/s^2 (authored: 0); the authored root bone starts facing the rig's rest ahead (the straight
    clips' hips agree within 2 deg). The turning starts' sharp authored turns read 200-530 on mmview's wobble measure, a
    deliberate turn: that check now covers generated roots only and reports the authored ones.
  - **Turning is steered** (`Settings::SprintCharacterFacingHalflife` 0.08 s while a sprint frame plays; 0.2 otherwise): the
    set has no curved runs, so the forward loop plays on while the facing follows. (Before the authored root, the turn "held"
    by playing the right-strafe loop whose facing the hips had rotated 57 deg - a sideways run standing in for a curve.)
    `M sprint turning` (60 deg/s): 2.25 jumps/s, 17 % cruising, 16.7 cm slide at 0.2 -> 1.0 jumps/s, 91 %, 2.2 cm.
  - **Measured** (full `mmplayer`, `[MmSprint]`; each leg against its jog counterpart: jumps within 1.5x + 0.5, pose step p99
    within 1.15x + 1 of the leg's own away from jumps - the sprint's limbs move further a frame than the jog's - feet <= 5 cm):
    straight 0.4 jumps/s (one loop 4.5 s), 0.9 cm; from a jog 0.5, 0.0 cm; turning 1.0, 2.2 cm; strafes 0.7 / 1.0, 0.1 / 0.5
    cm, 100 % cruising (were 2.7-3.3 jumps/s); back 0.7 jumps/s, **5.0 cm** (just over: fails). Sprint style 100 % on every
    leg; 3.27 m/s forward, 2.53 sideways, 2.18 back.
  - **The full pass after it** (2026-11-17): unit, smoke, testbed, npcloco, locomotion, stopsteps, feet, labloco, mmview,
    mmdb, perf pass. Still failing: `mmplayer` 8 checks - `M sprint back` (5.0 cm), the jog's jump rates (`A jog` 2.6/s),
    `M strafe right (walk)` clip/capsule speed 0.72, and the looking-about legs (walk and jog jumps/s and pose steps; feet
    7.8-22.6 cm on `M walk looking about`, `M jog looking about`, `M jog random look` in the full course - 1.4-3.8 cm when run
    on their own); `mmstress` one pop (114.5 deg, `S stop-start walk`). Not yet split into new and old; the looking-about and
    jog-rate checks failed before this session too. Runs are deterministic: the same legs alone and two at once give
    identical logs, so parallel runs are safe.
- **Long straight jogs.** The jog's steady forward stretches last at most 1.4 s (3 steps): a long straight jog re-matches
  between takes 2.6-2.7 times a second (pose steps stay smooth: p99 10.6 deg).
- **Tight turns at a walk.** Sweeping the view +-50 deg at 0.5 Hz while walking (a 157 deg/s turn) finds no walk take turning
  that hard (best costs 4-5); the jog takes' card turnarounds covered it before the style label.
- **Running jumps.** Every jump in the pack starts from standing (`Stand_Jump_*`): a jump on the move plays a standing jump's
  flight under the moving capsule, and lands with the take's in-place landing skipped.
- **Prone.** Only inside `Transitions_stationary` (on the way down and up); prone frames are excluded.
- **Dodges.** In the pack (`Stand_Dodge_360`), left out by the owner's choice.
- **Exaggerated styles** (`_S`, 0.75 h): kept tagged for a later style variant.

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

**What landed so far (2026-11-17, session 1; committed on `dev/mm-phase4` as work in progress):**
- **`src/Game/FirstPersonBodyMotion.{h,cpp}`**: the MM pose source.
  - `SharedMotionDatabase::Get`: one database per process (Phase 5's NPCs share it), with the gait table, the
    turn-in-place rate (p75 root yaw rate of the idle frames that turn: 96 deg/s) and the idle / crouch-idle frames.
  - `FirstPersonBodyMotion` runs a `MotionMatcher` on the capsule. Each frame the `TrajectoryPredictor` is put where the
    capsule is (`SetState`) and predicts from the wish velocity and the facing wanted.
  - Pose onto every piece by name (`Bind` per piece, the driver's pelvis scale shared). Writes `MmPlayerDebug::Info()`.
- **`FirstPersonBody`**: `LocomotionSource {Legacy, MotionMatching}`.
  - The field is `FirstPersonBodyComponent::LocomotionSource`, "Locomotion Source" in the Inspector (0/1);
    `TARTARUS_LOCOMOTION_SOURCE=legacy|mm` overrides it (`Env.h`). The MM path is `TickMotionMatching`.
  - The driver's AnimatorController is held: `SkipUpdate` skips the whole follower group, and `Stop` restores it.
  - The root is clamped to the capsule (the body stands at the capsule's feet; the clip's travel is never applied;
    `RootMotionWeight` 0).
  - Speed warping replaces `LoopPlayRate`: the matcher plays at capsule speed over the matched frame's root speed, within
    0.8-1.25, smoothed over 0.08 s.
  - Orientation warping:
    - moving: the heading follows the view on a 0.03 s spring, and *nothing* of the clip's turn;
    - standing: the clip's root turn plus a spring, with Turn Threshold / Turn End Angle;
    - turn in place feeds `Player::MaxYawRate` (the database rate, min Max Turn Rate).
  - Crouch is `Require`/`Forbid Crouch`; sprint is the Run gait while Shift is held and the move heads along the view
    (gait cost x2 at run pace).
  - The airborne segment: a jump or a fall (0.12 s off the ground) searches `Require Airborne`, then plays label-locked
    from 0.12 s before the airborne run (`Locked`, `HoldFrame`, `ForceFrame`).
    - It is time-warped to the jump's flight and holds its last airborne frame.
    - On landing: 0.25 s of the take's landing, locked - but none when the capsule moves faster than 1 m/s (the pack's
      jumps land in place).
  - The search schedule watches the move in the view's frame, at the gait's nominal speed, so a mouse sweep or the run
    slowing in a turn is not a new input.
  - TransitionCost 0.25, MinRunway 8, ForcedPoseWeight 0.25 (below).
  - Still fed: the gait phase (`UpdateGaitPhase`, shared with legacy), per-foot heights (`FootHeights` reads the posed
    driver), foot IK (`ApplyFootIK`: airborne from MM).
  - Spine Stability also runs under MM, with its reference from the database's idle / crouch-idle frames
    (`FirstPersonBodyMotion::ReferencePose`), eased out through jumps.
  - **Arm Hang** (legacy and MM, unarmed): `CaptureArms` / `HangArms`.
    - The upper arms' model-space rotations are taken before the spine's pitch at the view and turned back after it, so
      the arms hang instead of swinging back with the chest.
- **Player speeds = the gait table** (`Player::DirectionalSpeeds`, `PlayerGaitSpeed`, unit-tested):
  - walk 1.27 (default) / run 3.91 (Shift) / crouch 0.79 m/s ahead;
  - strafes and backwards (from 50 deg off the view, smoothstep): 1.14 / 1.32 (the jog strafe: no run strafes in the
    pack) / 0.76.
  - **The jog (1.85) has no key.**
  - The run slows in turns: at most 3 m/s^2 sideways off the view's turn rate, down to 1.32 (`m_MmRunScale`). The view
    itself is never capped while moving.
- **Runtime additions** (all default off - Phase 3's behaviour and tests unchanged):
  - `MotionMatcher::PlayRate`, `Locked` / `HoldFrame` / `ForceFrame`;
  - `ForcedPoseWeight`: a forced search (filter change or the frame can't go on) weighs the pose features x this;
  - `MotionSearchParams::MinRunway` with `MotionDatabase::Runway`: frames the take keeps its Crouch / Airborne /
    Excluded / NoSearch labels from here - no dead ends;
  - `TrajectoryPredictor::SetState`.
- **`MotionDatabase::Build` now excludes** (for everyone, NPCs included):
  - **capture glitches:** a stream bone turning > 35 deg in one 60 fps frame, +-3 frames. That's 1018 frames: twist flips
    up to 180 deg on thighs and feet, and a forearm swinging 63 deg in a frame in `AccelerationDeceleration_Take002`. Real
    motion's p99.9 is ~30 deg/frame (the runs).
  - **the way to and from prone:** 1 s either side of the baked Excluded frames (480 frames).
- **The predictor's sweep** (player): a sphere just over the step height *and* one at head height, both hitting within
  0.5 m.
  - One sphere at knee height read the steep stairs' risers as walls, and one over step height read a flight's later
    risers a second ahead: the matcher played idle or slow frames on stairs.
  - A ~1 m ledge is no longer predicted (the capsule still stops there).
- **`--npc-test mmplayer`** (`NpcTest_Testbed.cpp`, AnimTestbed, `TESTBED_PARTS=ACM` and `TARTARUS_LOCOMOTION_SOURCE=mm`
  set by the `NpcTest` constructor - an env already set wins, so `=legacy` runs the same course on the controller).
  - Unarmed: the testbed's parts A and C, plus part M, behind the lane's start (z < 0):
    - strafes L/R, walk back, crouch walk / strafe / idle, stand, turn in place 180;
    - run, run turning 60 deg/s, run strafe;
    - jumps standing and running;
    - walk and run looking about (the view sweeping +-50 deg at 0.5 Hz);
    - looking 60 / 85 deg down standing, walking and running.
  - 45 checks:
    - MM poses every frame; ~10 searches/s, < 1 ms;
    - the crouch filter; airborne only in the segment;
    - the root on the capsule; the play rate in 0.8-1.25; the raw clip / capsule speed in 0.8-1.25 (walk, run, crouch,
      strafes);
    - strafing frames matched (travel > 50 deg off their facing); the run gait and its slowdown;
    - turn in place; jumps locked then released; per-foot heights fed;
    - looking about steady (pose step p99 and jumps/s within 1.5x + 1 of the straight leg's);
    - look-down torso bend and arm hang.
  - Per-leg tables are logged (`[MmPlayer]`): labels, raw ratio, play rate, jumps/s, pose step, searches forced by input
    or by end, torso bend, arms. Testbed A's loop-travel check accepts MM (shown travel = clip speed x play rate).
  - Debug lines (the wanted trajectory yellow, the matched one magenta, the root green) are drawn through `NpcTest::DebugLines`.
- **`BodyDebug::Snapshot`**:
  - `ClipBendDeg` / `TorsoBendDeg` / `ViewPitchDeg`;
  - `ArmDownAimDeg` / `ArmDownDeg`;
  - `kStateMotionMatching` as the state name under MM.
- **Unit test** "Motion matching: player hooks": play rate, lock and hold, the forced jump, root speed, `PlayerGaitSpeed`.

**Results so far:**
- **Last full pass** (`mm_smoke.ps1 testbed npcloco locomotion stopsteps feet labloco mmview mmdb mmplayer -PerfBench
  tests\perf-scenes\perf_stress.json`, `work\mm_smoke\20261117-000415`, *before* the look-about, dead-end, glitch,
  look-down and arm fixes): **all PASS**.
  - Unit 52256 checks, smoke 21/21, testbed 29/29, npcloco 33/33, locomotion 20/20, stopsteps 10/10, feet 6/6,
    labloco 1/1, mmview 12/12, mmdb 18/18, mmplayer 38/38.
  - Perf edit 3.09 / play 6.02 ms (Phase 3: 3.15 / 6.02).
  - Baseline comparison: `D:\mm_phase4\summary\SUMMARY.md`.
- **Since then** `mmplayer` is 45/45 and the MM unit tests pass (887 checks). The full pass hasn't been re-run.
- **Player foot slide** (worst per leg, cm; legacy ~0.1):
  - walk 7.1 (one plant), run 2.1, normal stairs 0.0, steep stairs 0.3, ramp 3.1;
  - strafes and walk back 0.0-0.1, crouch walk 11.6 (one plant), crouch strafe 0.0;
  - run 1.1, run turning 3.6, run strafe 0.0, run + jump 0.1;
  - walking looking about 0.1, running looking about 6.6, looking down 0.2-0.5.
- **Frame cost:** mmplayer ~5.5 ms / frame (~175 fps), against legacy testbed's 6.4 ms. Searches avg ~0.6 ms.
- **Looking about** (the owner's first play test: the body lunged whenever the view turned on the move):
  - before: 6.6 / 8.2 jumps/s walking / running, heading 19-23 deg behind the view;
  - after: 2.6 / 4.5 (straight legs: 1.8 / 3.0), 5.5 deg.
  - Cause: the clip's own root turn was added on top of the spring to the view, a feedback loop between turning clips.
- **Crouch dead ends:**
  - The jumps' deep landings, then the way down to prone in `Transitions_stationary`, won crouch searches and re-searched
    every frame (30-50 jumps/s).
  - Fixed by the runway, the prone pad and the forced pose weight: now 1.2-1.5 jumps/s, on the `Crouch_*` takes.
- **Run pose pops** (52 deg/frame on a forearm): capture glitches, now excluded; run pose step p99 44 -> 19 deg.

**The owner's play test 2 (2026-11-17 08:04, 23 s; fixed in session 2 - below):**
1. **Running posture** (0-2.4, 5.2-5.7, 8-9 s): the chest upright, the head thrown back (chin at the sky), the fists
   swinging up to the face, the knees pumping high - it reads as prancing.
   - Cause: Spine Stability (`FPBody::BlendLocomotionSpine`) sets spine_01-05 to the idle's *model-space* rotation. The
     neck, head, clavicles and arms keep their clip rotations relative to the chest.
   - The pack's run leans the chest ~28 deg, with the head and arm swing tipped forward to match. Stand the chest up and
     they tip up. Harmless on the legacy clips, which were upright.
2. **Looking up** (3.0-3.5, 10-10.5, 16.4-17.5 s): the body arches back like a limbo, the arms flung behind. Spine Aim 0.6
   of the view's pitch, with no limit (Spine Max Angle 1-5 = 0).
3. **Looking down** (4.4-5.0, 11-12.1, 19-20.5 s): at -75 to -89 deg the upper body still folds.
   - The head drops to about waist height, the forearms come up by the face, and running one arm wraps over the head.
   - Spine Aim Down 0.9, unlimited, mostly in spine_04/05 (Spine Weights 0 / .3 / .45 / .65 / 1): a hunch, not a hinge
     at the hips.
   - **The `TorsoBendDeg` metric (pelvis -> spine_05 position) understated this ("14-18 deg"): it has to measure the
     chest's orientation.**
4. **A 180 deg view flick while running** (5.0-6.3 s): a sideways lean with an arm thrown up, then both arms straight
   forward ("zombie arms"), knees knocking.
   - The heading follows the view on a 0.03 s spring, so a flick spins the body over the foot-locked feet in ~0.1 s.
   - The matcher then picks pivot / transition frames with balancing arms.
5. **Legs crossing or knock-kneed** in turns of the view while moving (4.0, 9.0, 11.4-11.9, 13.0 s). Probably 4's cause.
6. **Hands in the first-person view all the time, unarmed** (0.0, 1.5, 2.5, 6.0, 11-12, 21 s). Mostly 1, 3 and 4.
7. Minor: the head is tilted back walking too (part of 1). Idle looks fine.

**What landed in session 2 (2026-11-17; play test 2's problems):**
- **Metrics** (`BodyDebug::Snapshot`, filled by `FirstPersonBody::PostureMetrics` and in `LateUpdate`; all body frame, against
  the bind pose): the chest's pitch from spine_05's *orientation* as the clip has it, after Spine Stability and as drawn
  (`ChestPitchClipDeg` / `ChestPitchStableDeg` / `ChestPitchDeg`), its roll, the head's pitch against the chest and on its
  own (`HeadPitchDeg`, `HeadWorldPitchDeg`), each hand's height over its shoulder, its distance to the camera and whether it
  is in the view, the feet's sideways gap (< 0: crossed), the body's yaw rate. `mmplayer` logs them per leg (`[MmPosture]`).
- **New part M legs:** walk / run looking up 60 and 85 deg, run looking straight down (-85), a 180 deg view flick running
  and walking (`TbLeg::FlickAt`; 0.15 s smoothstep). 71 legs now (the per-leg arrays are 96).
- **A - the lean** (`ApplySpineStability`, MM only): Spine Stability blends the spine toward the reference by
  `1 - kMmLeanKept` (0.5, + 0.25 toward a sprint) and then gives **`neck_01` and both upper arms back the world orientation
  the clip had** - the trunk stands up under them, so the head and the arm swing no longer tip back with it. Sprinting, the
  upper arms' swing is damped 40 % toward the reference's (relative to the chest; `kMmSprintArmDamp`, from 2 to 3.6 m/s).
- **The references** (`SharedMotionDatabase::Get`): Spine Stability's standing / crouched references are now the median-tilt
  idle frame of `Stand_Idle_Neutral_Take001` / `Crouch_Idles_Take001` (crouched and searchable). The old "1 s in" crouched
  frame was the actor still standing (chest tilt 9.4 deg against the take's 33.4): every crouch stood its chest up on bent
  knees - the owner's "goofy" crouch.
- **B - the aim's caps:** `Spine Aim Max Up` / `Spine Aim Max Down` (degrees, `FirstPersonBodyComponent`, Inspector "Arms";
  0 = no cap; AnimTestbed 20 / 35). `FirstPersonBodyCappedSpineAim`: `max * tanh(aim / max)`, by the unarmed share (armed,
  the arms rig needs the full aim to reach the gun). No pelvis hinge: `SpineDistribution::PelvisAlpha` turns the pelvis
  *after* foot IK, which would swing the planted legs - the caps alone stopped the fold.
- **C - turning on the move:** the heading turns at most 450 deg/s while moving (`Settings::MaxMovingYawRate`); under MM
  the chest leads the view by at most 60 deg (`kMmChestTwistMax`, unarmed); over 240 deg/s of heading turn the foot lock
  holds nothing (`IK::FootLock::ResetFoot` each frame - a let-go foot's world offset easing out still crossed the legs).
- **Frames excluded at load for the player** (`MotionDatabase::ExcludeFrames` re-indexes the runway and boxes; the rig's FK
  per frame in `SharedMotionDatabase::HandOverShoulder`, ~0.1 s):
  - a hand over its shoulder by more than -0.02 m (the run takes' ends, catching breath with the hands on the head);
  - standing, under 1 m/s, the chest tilted over 30 deg (the same ends, bent over with the hands on the knees - it won
    the idle searches);
  - walk or faster, not strafing, the ankles crossed over 0.10 m (the snakes' and turnarounds' crossover steps - a flick
    matched them);
  - airborne frames stay (the jumps play locked); +-6 frames padded. 13975 raw frames, 20083 with the pads (7.5 %).
- **D - hands near the camera** (`KeepHandsFromEye`, unarmed): each hand at least 0.30 m from the eye (soft max over 0.06 m),
  the arm re-solved with `IK::SolveTwoBone`, the hand's orientation kept. Running looking down, the swing brought a hand to
  0.18 m.
- **Recording:** `NPC_TEST_RECORD` rendered the Game view only while its tab counted as visible - the old run caught 152 of
  ~4000 frames. It now renders and records every Play frame, at the tab's last size (`Application.cpp`). `mmplayer` records
  every other frame (30 fps; a full recorded run ~3.5 min).
- **Checks:** `mmplayer` 45 -> 56: the head not thrown back running, no hand over its shoulder on more than 5 % of a leg, no
  hand within 0.25 m of the eye, the chest's aim within its caps looking straight up / down (walk, run, standing), the
  flicks (turn rate, arms, feet). The old look-down torso-bend check now measures the bend the view adds over the gait's own.
- **Unit test** "Motion matching: player exclusions and the capped aim": `ExcludeFrames` (flags, runway, box = brute, never
  matched) and `FirstPersonBodyCappedSpineAim`.

**Results (session 2), the same course on both sources** (`[MmPosture]`; legacy = `TARTARUS_LOCOMOTION_SOURCE=legacy`;
chest and head in degrees off the bind pose, + forward / chin down):

| | legacy | MM before | MM now |
|---|---|---|---|
| Run: chest drawn / head (own pitch) | 21 / -6 vs chest | 7 / about -23 (head -30 vs chest) | 31 / +16 |
| Run: hand over shoulder, highest | -0.22 m | +0.11 m (7 % of frames) | -0.23 m (0 %) |
| Crouch walk: chest drawn | 74 | 25 (reference standing) | 50 |
| Look straight down standing / running: chest's aim | +77 / +77 | +77 / +77 | +34 / +34 |
| Look straight up walking / running: chest's aim | -48 / -48 | -51 / -51 | -20 / -20 |
| Nearest hand to the eye (any leg) | - | 0.18 m | 0.31 m |
| Hand over shoulder, worst leg | - | 100 % ("stand up", "stop after flick") | 0 % |
| Run flick: body turn rate, feet crossed, worst cross | 1762 deg/s, 4 %, 2 cm | 1437 deg/s, 0 %, 0 cm (arms up 9 %) | 450 deg/s, 2 %, 5 cm |

- **Full pass** (`mm_smoke.ps1 testbed npcloco locomotion stopsteps feet labloco mmview mmdb mmplayer -PerfBench
  tests\perf-scenes\perf_stress.json`, `D:\mm_phase4\fullpass`): **all PASS** - unit 53186 checks, smoke 21/21, testbed
  29/29, npcloco 33/33, locomotion 20/20, stopsteps 10/10, feet 6/6, labloco 1/1, mmview 12/12, mmdb 18/18, mmplayer 56/56
  (5.56 ms/frame, 180 fps), perf edit 3.11 / play 6.03 ms (Phase 3: 3.15 / 6.02).
- **Baseline comparison** (`baseline_summary.py` -> `D:\mm_phase4\summary2`): every legacy number is unchanged (foot slide,
  clip speeds, checks); the baseline's broken-asset log errors are gone; frame costs within noise.
- **Recording for the owner:** `D:\mm_phase4\mmplayer_phase4.mp4` (Scene view left, Game view right, 30 fps).
- Visual review (contact sheets of the recordings, 0.1 s): the run leans with a compact swing; the crouch walk is hunched,
  hands by the knees; looking down the body bends a little and the arms hang; looking up there is no arch; the flick turns
  without the arms going up.

**The owner's play test 3 (2026-11-17 09:21, 16 s; fixed):** the sprint "limps" and its arms look weird; turning while
sprinting "does a stop which disturbs the flow". Contact sheets: the turns at 1.9-2.9 and 7.9-8.3 s sank almost to a kneel
and drove off in a sprint start; the straight sprint was uneven stride to stride.
- **Cause: the pack has no sustained straight sprint** (a 9 m capture area): every straight run over 3.2 m/s is a 1-2.5 s
  fragment between a start and a stop (`Run_CardLong` 2.5 s, `Run_Loop` 1.9 s, the `AccelerationDeceleration` takes 1-2.5 s;
  only `Run_Circles` runs on, at 2.6-3.0 m/s, curving). Held at 3.91 m/s the matcher stitched fragments of three takes, ~3
  jumps a second (the limp); the run slowing in turns (to the jog strafe's 1.32 m/s) fell into their stops and starts.
- **The sprint loop** (`SharedMotionDatabase::FindSprintLoop`, `FirstPersonBodyMotion::Tick`): at load, the cleanest steady
  straight stride - frames searchable, standing, 3.2-4.8 m/s, turning under 25 deg/s, speed within 0.6 m/s, 0.5-0.95 s long,
  the seam (sum of the 41 bones' angles + feet / pelvis velocity + pelvis height) smallest. This bake: **`Run_Loop_Take001`
  4.63-5.23 s, 36 frames, 4.38 m/s, seam 3.99**. Sprinting ahead, grounded, the capsule over 0.85 of the run speed and the
  heading settled (under 90 deg/s for 0.15 s - entered out of a reversal's pivot, the dead blend carried crossing legs into
  it), the loop plays locked from the frame whose features match the pose now; its end wraps to its start
  (`MotionMatcher::LoopTo`: dead-blended, not a jump); speed warping as before. Out under 0.55 of the run speed, off the
  sprint, crouched or airborne (a jump keeps its own lock). The foot lock resets as it starts.
- **Turns:** the run slows at most 6 m/s^2 sideways (was 3) and never under 0.65 of the run speed (was the jog strafe's
  1.32 m/s), so a turn stays on the loop.
- **Arms:** the 40 % sprint arm damping is off (`kMmSprintArmDamp` 0) - toward the idle's hang relative to a leaning chest it
  swept the arms back stiff; the loop's own swing is used.
- **The head:** at most 15 deg chin down (`kMmHeadDownMax`, taken out at `neck_01`): the loop's runner stares at the ground.
- **Results** (`mmplayer` 57/57): `M run` on the loop 100 %, 0.3 jumps/s (was 3.0); `M run turning` 3.90 m/s against 3.91
  straight, 100 % on the loop, no jumps; the run flick's feet never cross (gap +1 cm; was 7 %, 19 cm on loop entry); run
  head 17 deg (was 38 with the loop, before the limit).
- New checks: the straight sprint on the loop >= 80 % at <= 1 jump/s; the sprint carries through a 60 deg/s turn (>= 85 % of
  the straight speed, on the loop); the head -10..20 deg running; a jump's landing may resume on the loop.

**The owner, after play test 3 (2026-11-17):** "the sprint loop doesn't seem seamless", "crouch still looks off", the arms
looking down standing still look awkward ("the hand motions while walking or running are fine"), then "stress test it".
- **The loop, crossfaded** (`SharedMotionDatabase::Loop::Close`, `MotionMatcher::AdjustTarget` / `LoopTo` / `Repose`): no
  stride in the pack is steady - the best cycle is 3.9 deg per bone (mean) off its start, a thigh 17 deg (every stride in a
  9 m capture is mid speed-up or slow-down). First a correction spread linearly over the cycle (the end = the start exactly):
  it shifted the cycle's average pose (the chest +8 deg, a foot out of reach - testbed A's reach check failed). Now a
  crossfade loop: over the stride's first 12 frames (0.2 s) the frames past its end blend into its start; the wrap is
  continuous in pose and velocity, the cycle otherwise untouched (no dead blend at the wrap; the hold runs 2 frames past
  the end so the wrap keeps its fraction). Run pose step p99 19.8 -> 17.8 deg, no spike at the wrap.
- **The loop's stride, re-picked** (the owner: "are you sure the sprint is correct?"): measured, the first loop was a sprint
  *start* (`Run_Loop_Take001` 4.63 s - the runner at 0.3 m/s 1.1 s before), cut 3 frames short of its real stride (0.60 s
  against ~0.64) with the right foot 21 cm off at the seam (a hitch every stride), and slowed to 0.88x. `FindSprintLoop` now
  cuts on the gait's phase (a left touchdown - the `ContactL` rising edge - to the next), scores the seam on the bones' mean
  angle (deg) + 2 x the feet / pelvis features (positions and velocities, in their std) + 3 x the speed change across it.
  The pack has 15 such strides (fast, straight, searchable); every one is 5-12 deg per bone off its start (an arm up to
  30-80) - no stride repeats. Picked: **`Run_CardLong_noStops_Take001` 6.25-6.87 s, 37 frames, 4.81 m/s** (the top of a long
  dash). Crossfade 16 frames (0.27 s). **The sprint's speed is now the loop's x 0.95 (4.57 m/s; was the gait table's 3.91)**
  so it plays near its natural rate (`FirstPersonBodyMotion::SprintSpeed`) - an owner call to keep or revert.
- The wall check now measures the head's clearance (`BodyDebug::WallHeadGap`): 7.4 cm at least with this stride.
- **The crouch is the walk, lowered** (`FirstPersonBodyMotion::Settings::CrouchFromWalk`, default; `TARTARUS_MM_CROUCH=pack`
  for the pack's crouch takes): every crouch take in the pack is one style - a deep, high-knee sneak, hunched over. Tried
  first: Spine Stability's crouched reference (it was the actor still standing - fixed to the median crouched idle), the hips
  raised to 0.65-0.72 of standing, the pelvis's forward tilt taken out (0.35-0.6), a chest hinge to 25 deg, the arms to the
  standing hang - the walk stayed a sneak (knees to the chest). Now crouched the matcher plays standing frames (the walk,
  strafes, idles, turns), `FirstPersonBody::LowerCrouch` drops the hips 0.27 of the standing pelvis height and re-solves the
  legs to the walk's feet (knees forward: `TwoBoneHint::BendAxis`), and the crouched reference is the standing one lowered
  as much with the chest hinged to 25 deg. Crouch pace 0.85 of the walk (1.08 m/s; the walk's data runs out under ~1 m/s),
  strafe 0.85 of the walk strafe. Crouch walk: pelvis 0.68 m, chest 25 deg, pose step p99 8 deg (the pack crouch ~19).
  The pack path keeps its fixes (`RaiseCrouch`: hips to 0.72, pelvis un-tilt 0.35; the median crouched reference).
- **Idle look-down arms** (`RelaxArms`): standing still, looking down past 15 deg (full at 60), the arms go to the standing
  reference's relaxed hang (upper arms in the model, elbows and wrists local), eased out as a walk starts; on the move the
  arms keep their motion (the owner). `Spine Aim Max Down` 35 -> 25 (AnimTestbed): standing, people look down with the neck,
  and at 35 the hanging arms dangled in front of the legs, in view.
- **The head off walls** (`KeepHeadOffWalls`, MM): the stress test sprinted into a wall - the capsule stops at it, but the
  gait's lean carried the chest and head ~0.3 m ahead of the capsule, into the wall (the camera stayed out). A sphere cast
  ahead at head height; the spine tips back (spine_01-05, eased in 30 ms, out 120 ms) to keep the head 8 cm off.
- **`--npc-test mmstress`** (part S of the testbed course, `TESTBED_PARTS=S`, ~71 s, 30 legs): a zig-zag walk (diagonals
  flipping every 0.4 s), stop-start walking and sprinting, crouch toggling on the move, jump spam at a sprint, 360 deg/s view
  spins walking and sprinting, sprint reversals, a strafe circle, a sprint into a wall, up stairs and down the ramp, an 8 s
  idle, a crouch mid-sprint, a crouched jump. 16 checks: posed every frame, every pose finite, no pop (no bone over 45 deg in
  a frame - worst 19.8), no hand over its shoulder (> 5 %), the crouch filter and airborne segment hold, the root on the
  capsule, every stop ends within 20 deg of the view, the spine tips back at the wall, search avg < 1 ms and max < 5 ms (0.39
  / 1.4). Per-leg table `[MmStress]`. `TbLeg` gained `Move2` / `AltPeriod` / `CrouchPeriod` / `JumpEvery`; the per-leg arrays
  are 128.
- **Full pass** (`mm_smoke.ps1 testbed npcloco locomotion stopsteps feet labloco mmview mmdb mmplayer mmstress -PerfBench
  tests\perf-scenes\perf_stress.json`, `D:\mm_phase4\fullpass2`): **all PASS** - unit 53187 checks, smoke 21/21, testbed
  29/29, npcloco 33/33, locomotion 20/20, stopsteps 10/10, feet 6/6, labloco 1/1, mmview 12/12, mmdb 18/18, mmplayer 57/57
  (179 fps), mmstress 16/16, perf edit 3.08 / play 6.03 ms. Baseline (`D:\mm_phase4\summary3`): 71 of 77 rows identical; the
  6 others differ only by the baseline's broken-asset log errors (gone) and frame cost within noise.
- Stress findings, judged fine: the zig-zag's crossover steps (13 % of frames, 11 cm), the strafe circle's (16 %), the wall
  push's search churn (3.7 jumps/s while the capsule is pinned), a teleport onto the ramp playing a turn on the spot.

**Session 3 (2026-11-17): audit - frame time, the sprint loop's tempo and stride.** Measured before changing anything; the
scratch scripts parse `Manny.mmanim` / `.mmdb` directly (formats in their headers).
- **Every MM test ran at a fixed 1/60 s; the owner plays at a variable frame time.** `Application.cpp` gives npc-test mode
  exactly 1/60 s and Play `Time::DeltaTime()`. The posture layers all ease with `exp(-dt / tau)` (frame-rate independent);
  one thing was not:
  - **The dead blend's carried velocity** (`MotionMatcher`): a jump took the last two output poses' difference over *this*
    update's dt, but they were one update apart - the previous dt. At any frame-time change (a hitch, the frame after it,
    uneven pacing) the carried velocity was scaled by the ratio: after a short frame a long one stalled the limbs (1/8 at
    240 -> 30 fps), the other way it ran 8x into the 4 pi rad/s clamp (a lurch at the jump). Now `BeginBlend` uses the time
    between those two poses (`m_OutInterval`). `ForceFrame` / `LoopTo` lost their `dt` (unused or wrong).
- **The sprint loop's tempo stepped every stride.** Speed warping divided by the *raw* frame's root speed; at the wrap the
  raw frame jumps from the stride's end to its start (4.49 -> 4.76 m/s on the old stride) while the pose shown - the
  crossfade - doesn't. So the play rate stepped ~6 % every 0.62 s: a hitch in the limbs' tempo with no pop in the pose. Now
  the warp divides by the speed of the pose shown (`Loop::ShownSpeed`: the crossfade's blend of the two speeds, by the same
  weight `Close` blends the poses with; continuous over the wrap).
  - *Considered and rejected:* warping by a wider (stride-long) speed window everywhere. A planted foot's speed against the
    root is the root's instantaneous speed, so the short window is what keeps the feet planted; a wide one trades the walk's
    tempo wobble (+-20 % root surge at sigma 0.10 s) for foot slide.
- **The loop's stride, re-picked on steadiness** (`FindSprintLoop`). Measured: the stride in use
  (`Run_CardLong_noStops_Take001` @ 6.25 s) had the pack's best seam (5.4 deg) but **slowed 4.8 % across the stride**
  (4.91 -> 4.49 m/s in its last 8 frames) with **its two steps 2.4 % apart in speed** - looped every 0.6 s, a slowing,
  uneven stride. The crossfade hides any of the pack's seams (5-9 deg); nothing hides drift or asymmetry. The score is now in
  comparable units: seam deg + 2 x the feet / pelvis features (as before) + the speed's linear drift across the stride, % +
  the steps' mean speeds apart, % + their durations apart, % (a stride needs its right touchdown between). Picked:
  **`Run_Loop_Take001` @ 19.80 s** - seam 6.6 deg, drift 1.0 %, steps 0.3 % / 2.7 % apart (old score 8.55 vs the old pick's
  7.48; new 12.16 vs 16.59; runner-up 14.52). The sprint is the loop's mean shown speed x 0.95.
- **`EnterAir` with no airborne match** (an edge: a database without jumps) left the state `Rising` with a stale
  `m_AirLast`: the landing then forced the matcher to an earlier jump's frame (or frame 0). Now it stays on the ground.
- **Unit tests** (+2): "dead blend across a frame-time change" (1/240 then 1/30 s and the reverse: the output's angular
  velocity carries across a forced jump within 0.65-1.25x) and "sprint loop wrap" (on a speeding-up synthetic take: the raw
  speed jumps at the wrap, the shown speed and the closed pose don't; smooth through the overlap). `Loop::SetSpan` builds a
  loop's shown-speed table (the picker's and the test's).
- **Results** (`mm_smoke.ps1 mmplayer -SkipSmoke -UnitFilter "Motion matching"`, `D:\mm_phase4\session3_run1`): unit 1812
  checks, 0 failures; **mmplayer 57/57**. The engine picked the predicted stride (`sprint loop: Run_Loop_Take001 @
  19.80-20.42 s, 4.63 m/s shown, score 12.16`); the sprint is now **4.40 m/s** (was 4.57). Against the last run on the old
  stride (`D:\mm_phase4\s10_loop2`), same course:
  - straight sprint: foot slide worst 0.8 -> 0.5 cm, run turning 1.6 -> 0.9; loop 100 %, 0.3 / 0.0 jumps/s (unchanged);
    pose step max 23.7 deg = its p99 22.7 (no spike at the wrap; the stride's limbs move more per frame than the old one's 17);
  - single worst plants up on transient legs: run flick 1.9 -> 9.8 cm, run looking about 11.0 -> 16.2, run + jump 0.2 ->
    2.2 (loop entries / exits on a different stride; the foot lock's job - watch them);
  - **the style changed** - read this before judging: chest lean in the clip 27 -> 38 deg (drawn 24 -> 32), pelvis 0.92 ->
    0.84 of standing, head down 8 -> 18 deg world, hand in view running looking down 9 -> 21 %. The new stride runs lower and
    leans in. It is the pack's *typical* sprint: its pelvis (0.879 of the source's standing) is exactly the median of the 15
    candidate strides; the old one was the outlier (+6.2 %), upright because it was slowing down (a runner rises as they
    brake).
  - (A before / after video was made for the owner, then deleted at the owner's request: the owner tests in the editor.)
- At a fixed 1/60 s the dead-blend fix is a no-op (the interval is the dt), so the course numbers above are the loop's alone.
- `mmstress` on the new loop (`D:\mm_phase4\session3_stress`): **16/16**, worst bone step 24.4 deg (sprint reversals; was
  19.8, the limit 45).

**Session 3, play test 4 (the owner's recording, 2026-11-17 11:28: idle hands jerking, sprint goofy).** Reviewed frame by
frame (Scene view, 0.1 s):
- **Idle: a rewind loop.** The hands' motion came in bursts every 0.41 s. While standing, the query is constant and the
  timer searched every 0.1 s, so the stillest moment won again as soon as `IgnoreNear` let it back in. Fixed:
  - **Idle mode** (`MotionMatcher::Idle`, `MotionSearchSchedule::Hold`): standing still on an Idle frame (capsule under
    0.1 m/s, not turning in place), no timer search. A search runs only on an input or filter change, on the frame running
    out, or when the continuation stops fitting (root travel over 0.25 m in 1 s, facing over 90 deg).
  - **Idle takes only** (`MotionLabel::IdleTake`, runtime, from the tag "idle") with a 3 s runway. Over the whole database
    the stillest second was `Stand_Jump_360`'s stance, picked again every 2.2 s.
  - **Facing bound 90 deg, not the Turn Threshold.** The pack's idles move the root under 0.17 m/s (p97), but
    `Stand_Idle_LookAround` turns its root over 30 deg within 1 s on 22 % of its frames (its facing is half the shoulder
    line). Standing, the heading follows that turn and the Turn Threshold brings the body back.
  - **A reselect history everywhere** (`MotionSearchParams::Avoid`): frames shown in the last second, padded by 10, are
    never candidates.
  - Result (`M idle`, 6 s): 1 jump, one take held 3.8 s, 0 rewinds anywhere on the course.
- **Sprint turning: the loop collapsed.** Mouse turns took the capsule 4.4 -> 1.5 m/s (the view-rate slowdown
  `m_MmRunScale`, plus the velocity cutting the corner), below the loop's exit, into the pack's starts and stops. Fixed:
  no slowdown while the sprint loop carries the run; the loop exits on intent (Shift, ahead, crouch, air, a reversal over
  100 deg) or when blocked (under 0.3 of the run speed for 0.2 s), not on a speed dip. `M run random look` (view wandering
  +-25 deg yaw, peaks ~150 deg/s, +-35 deg pitch): on the loop 100 %, 0 exits, speed 4.47-4.50 m/s, the warp never pinned.
- **Still open, measured** (step 3, after the owner's A/B):
  - **Mouse yaw swings the whole body** (heading follows the view: body yaw p99 129 deg/s), so the planted feet skate:
    `M run random look` slide worst 50.8 cm, mean 7.5 (straight 1.0).
  - **Mouse pitch nods the torso** (Spine Aim): chest pitch swing 44 deg against 20 straight.
  - `[MmLayers]` (new; per stream bone, drawn vs matched, mean / p95 deg): running, `neck_01` 16 / 23 (the head-down cap
    and the neck's world restore), upper arms 5 (10 / 19 with the random look).
- **A/B switches for the owner** (`FirstPersonBodyComponent`, Inspector, live in Play; temporary): "MM Post Layers" (off:
  the matcher's pose with foot IK only) and "MM Sprint Loop" (off: matching on the run fragments).
- New mmplayer legs: `M idle`, `M run random look`, `M run mouse noise`. New tables `[MmIdle]`, `[MmSprint]`, `[MmLayers]`.
  Checks 63 (idle holds, no rewinds; the search-rate check now excludes idle and loop time).
- Unit test: "reselect history and the idle hold". Unit 1821 checks, 0 failures; mmplayer 63/63 (`D:\mm_phase4\pt4_run4`).

**Session 3, "why does the sprint do a stop every few steps?" (the owner).** The loop's crossfade. For the first 16 frames
(0.27 s) after every wrap it played the frames *past* the stride's end, blending into the stride's start - and past the end
the pack's actor is braking (a dash in a 9 m room): 4.42 -> 3.48 m/s over those 16 frames on the current stride (4.49 -> 3.69
on the one before; every candidate the same), the crossfade's weight on them over half for the first 8. So every cycle
(~0.6 s, two steps) began with a brake and recovered. Before the stride the actor is still speeding up (3.90 -> 4.55); no
candidate has steady running on either side (weighted by what the crossfade shows, the best is 3-5 % off speed, the
current pick 12.5 % after / 7.8 % before) - so no re-pick or side swap fixes it, only lessens it.
- **Fixed: the wrap is closed by an offset, not by borrowed frames** (offset inertialization, `Loop::Close` / `SeamDecay`).
  After a wrap the stride plays its own start with the seam's difference added - each bone's end rotation off its start
  (scaled angle-axis) and their angular velocities' difference, the pelvis likewise - decaying as a critically damped spring
  (half-life 0.1 s; 0.2 % left at the next wrap). Continuous in pose and velocity; only the stride's own frames ever play.
  The shown speed (speed warping's divisor) carries the same offset. The offset applies only after a wrap (`m_LoopWrapped`;
  the crossfade also bent the loop's entry, which is already dead-blended). The picker no longer needs 16 frames past the
  stride (2, for the hold).
- Unit test rewritten: a 0.43 rad seam closes exactly (0.00000 rad at the wrap), the step across the wrap 0.0327 rad against
  0.0320 into it, 0.5 % of the seam left at the next wrap (a 30-frame span; 0.2 % on the real 37-frame stride), the shown
  speed continuous.
- `mmplayer` 61/61 (`D:\mm_phase4\seam1`, against `trunk_mm3` before): straight sprint pelvis pitch swing per cycle 21.2 ->
  15.3 deg and yaw 15.9 -> 14.4 (the brake's dip gone), pose step max 23.7 -> 23.2 (no spike at the wrap), loop 100 %, 0.3
  jumps/s; slide worst 0.5 -> 1.0 cm. **Watch:** run + jump's landing back into the sprint slid 8.5 cm worst / 3.3 mean (was
  2.2 / 0.7; 4 plants) - the transient legs have swung run to run all session (the flick 1.9 / 9.8 / 8.5 cm with unrelated
  changes), but this one has only gone up.

**Session 3, the trunk (the owner, relayed: "weird shit in the hips/lower spine - feels like the lower spine is grounded and
the torso isn't getting the same motions").** Measured first: `[MmTrunk]` (new, `mmplayer`) - per leg the per-step swing
(p95 - p5, deg) of the pelvis's, the chest's and the chest-against-pelvis yaw / roll / pitch, as the clip poses them and as
drawn (`FirstPersonBody::TrunkAngles`, `BodyDebug::TrunkClipDeg` / `TrunkDeg`).
- **Confirmed, two causes.** The pelvis was drawn as the clip had it; the chest above it lost half its per-step motion -
  running its yaw swing 22.5 -> 10.9 deg (against the pelvis 19.4 -> 10.2), walking its pitch bob 14.3 -> 7.2, the crouch walk
  its yaw 14.2 -> 1.7 and bob 16.0 -> 3.2, walking back its yaw 10.7 -> 2.7:
  - **Spine Stability** (MM path) pulled spine_01-05's model-space position and rotation halfway toward a fixed idle frame
    (`BlendLocomotionSpine`, the legacy controller's - its own comment: it "cancels inherited pelvis motion too"). That
    removed the lean it was meant to, and half of every per-step motion with it.
  - **Spine Twist** (1 on the testbed, unarmed) re-aimed the shoulder line at the view *every frame*, holding it square over
    hips that swing ~16 deg a stride - the extra yaw loss running.
- **Fixed (MM only; the legacy path is untouched):**
  - Spine Stability stands up only the chest's **slow lean**: a two-pole low-pass of the clip's chest pitch
    (`FirstPersonSlowAngle`, 0.25 s per pole: 11 % of a walk's step bob, 4 % of a run's reaches it), corrected by
    `1 - kMmLeanKept` of the way to the reference's (crouched as crouched), as one pitch spread evenly over spine_01-05. The
    neck and upper arms still get the clip's world orientation back, the crouch arm hang and the head-down cap are as before.
  - Spine Twist turns the chest's **slow heading** to the view (low-pass 0.5 s moving, 0.08 s standing; the view's target
    isn't filtered, so a flick still leads with the chest). At 0.35 s the walk's stride-rate leftover came back phase-lagged
    and swung the shoulders 34 % past the clip's.
- New `mmplayer` check (58 now): walk, run, crouch walk, walk back - the chest's swing against the pelvis 0.8-1.25 of the
  clip's, its pitch bob >= 0.6 (the slow lean's removal takes some of the pitch range).

### Phase 5: NPCs (unarmed)
- MM in `NpcBody::Tick` (~195–380), with one shared database.
- Trajectories from navmesh corners, plus LOD.
- Port `npcloco`, `locomotion`, `stopsteps`, `roam`, `labloco`, and testbed parts E and F.
- Perf compared against the baseline. The owner signs off on recordings.

### Phase 6: Legacy cleanup and final verification
- Delete only after a reference grep, and only after the owner approves the delete list (repo and Drive `Used\`).
- Remove the legacy body/NPC controllers, their generators, the legacy `BodyLocomotion` triggers and `LoopPlayRate`,
  `LocomotionSource`, and the legacy locomotion mocap - **except `Locomotion_V2/AM_Jog/`**: the motion matching sprint is baked
  from it (`tools/mm/gen_takes.py`, `LEGACY_*`; see "Asset gaps").
- Docs: `docs/MOTION_MATCHING.md`.
- Final full run of `mm_smoke.ps1`.

## Test protocol (every phase)

- Tests always run **visible**: never set `TARTARUS_HIDDEN`, and keep `--visible` on `tools/npc_test_runs.py`.
- **No footage** (the owner, 2026-11-17: "We don't need to waste time recording footage for something I can test"): don't
  set `NPC_TEST_RECORD` or render before/after videos. Verify with the checks and measurements; the owner plays it in the
  editor. Record only when the owner asks.
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
    labloco mmview mmdb mmplayer mmstress -PerfBench tests\perf-scenes\perf_stress.json`. It should be all PASS. Use `-AllowErrors` only for a local
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

## Fast iteration (keep current)

Run as little as answers the question. Three tiers (`tools\mm_smoke.ps1`):

| Tier | Command | What | When |
|---|---|---|---|
| Iterate | `mm_smoke.ps1 mmplayer -SkipUnit -SkipSmoke -Legs "..."` | the legs in question | every change while working (~30 s) |
| Quick | `mm_smoke.ps1 -Quick` | unit, mmplayer, mmstress | before committing motion matching work (~3.5 min engine runs + the build) |
| Quick + bake | `mm_smoke.ps1 -Quick mmview mmdb` | + the stream and database checks | when the bake or the database changed |
| Full | `mm_smoke.ps1 -Full` | unit, smoke, testbed, npcloco, locomotion, stopsteps, feet, labloco, mmview, mmdb, mmplayer, mmstress, perf bench (`tests\perf-scenes\perf_stress.json`) | once per PR, before merging |

- **The six old-locomotion scenarios** (testbed, npcloco, locomotion, stopsteps, feet, labloco) run only in `-Full`, or when a
  change touches code they share (foot IK, `Player`, `FirstPersonBody`'s legacy path). Phase 6 retires them.
- **In parallel:** the script runs up to `-Parallel` (default 3) engine runs at once; the timing checks (`mmdb`'s search time,
  the perf bench) run alone. Runs are deterministic under load: the same legs alone and two at once gave identical logs
  (2026-11-17), since every run steps a fixed 1/60 s. `-Parallel 1` runs one at a time, in order.
- **A subset of legs:** `TARTARUS_TESTBED_LEGS` (comma-separated, `*` wildcards) on testbed / mmplayer / mmstress, or
  `-Legs`. Each matching leg runs with the legs of its part from the last placement (settle) leg before it, so it starts as on
  the whole course. A check that names a leg that didn't run - its own, or a reference leg like `(A walk ...)` - is skipped and
  logged `[NpcTest] SKIP (leg filter: ...)`; a part with no leg running skips its setup checks, and the whole-course checks
  (`mmplayer: ...`, `mmstress: ...`, `testbed: ...` - averages over every leg) are skipped under a filter.
  Example: `powershell -ExecutionPolicy Bypass -File tools\mm_smoke.ps1 mmplayer -SkipUnit -SkipSmoke -Legs "M sprint*,M stop after sprint*,M jog,M jog turning"`
- **Tuning without a rebuild:** `TARTARUS_MM_TUNE="Key=value,..."` overrides at startup (`MmTune`, FirstPersonBodyMotion.cpp;
  logged `[MmTune]`). Keys: `MinWarp MaxWarp SimFacingHalflife CharacterFacingHalflife MaxFacingOffset TransitionCost MinRunway
  ForcedPoseWeight SprintCharacterFacingHalflife BlendTime DecayHalflife W.FootPos W.FootVel W.PelvisVel W.TrajPos W.TrajDir
  FootLock.Height FootLock.Lift FootLock.Speed FootLock.Reach FootLock.MaxDrift FootLock.Release FootLock.StepHeight
  FootLock.StepMax`. A new knob is one more `MmTune("Key", default)` call in a .cpp; an adopted value goes into the header
  default once, at the end. Several tunings at once: one `mm_smoke.ps1` per value, each with its own `-LogDir` and
  `TARTARUS_MM_TUNE` (`Start-Job`).
- **Code experiments side by side:** `tools\mm_exp.ps1 -Name <exp>` copies the runnable build (build\Release without
  .pdb/.lib/.exp, ~64 MB: the game code is in TartarusGame.dll beside the exe) to `work\exp\<exp>\`; run it with
  `-Exe work\exp\<exp>\TartarusEngine.exe` while the main build compiles the next one. (`-Exe` was silently ignored until
  2026-11-17 - PowerShell's names ignore case, so the script's own `$exe` was the parameter; fixed.)
- **Slow rebuilds:** the widely included headers - `IK.h`, `FirstPersonBody.h`, `FirstPersonBodyMotion.h`, `MotionSearch.h`,
  `MotionDatabase.h`, `MotionFeatures.h`, `Player.h`, `Components.h`, `Core/Env.h` - rebuild most of the engine. Put experiment
  knobs in .cpp files or `MmTune`, not in their defaults.
- **Measured (2026-11-17):**
  - a header edit's rebuild 2.5-5 min (148-281 s); a .cpp-only rebuild ~20 s;
  - a leg-filtered mmplayer (a few legs) ~30 s;
  - `-Quick`: 206 s of engine runs at `-Parallel 3` (mmplayer 205 s of it, slowed from ~120 s alone by the load) + the build;
  - the full pass, one run at a time: 14 min including a 3.6 min build.
- **Tried and dropped: skipping the drawing.** Drawing only every 8th frame of mmplayer + mmstress (`-Parallel 2`) took
  115 s against 131 s drawing every frame (~12 %; mmplayer 105 -> 120 fps), with identical check results: the time is the
  simulation, not the drawing. A CPU profile of an mmplayer slice (`--npc-test` with `--perf-sample`, which the option parser
  then had to be told to allow) put 66.5 % in `FirstPersonBody::InsideProbe` - the camera probe's every-view-ray against
  every-triangle test on the five look-down legs (~0.2 s a probe, every third frame). Binning the triangles by their
  screen footprint would give the same hits for a fraction of the cost; drafted but not verified or adopted
  (`work\probe_binning.patch`, local only).
- **Later:** the offline matcher replay Step 4 of the full-library plan called for (recorded inputs through the matcher and
  the capsule maths, scored in seconds, for parameter sweeps).

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
- Player (Phase 4): `src/Game/FirstPersonBodyMotion.{h,cpp}` (+ `MmPlayerDebug`), `FirstPersonBody::TickMotionMatching`,
  `Player::DirectionalSpeeds`; `--npc-test mmplayer` in `src/Game/AI/NpcTest_Testbed.cpp` (part M, `MmPlayerFrame`,
  `MmPlayerChecks`). Run: `tools\mm_smoke.ps1 mmplayer -SkipSmoke -UnitFilter "Motion matching"` (~2.5 min).
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
- *Phase 4 (2026-11-17):* the run speed is the table's 3.91 m/s, and the run slows in turns (3 m/s^2 sideways) rather than the
  view being capped. Kept below for the history.
- **Run:** the pack's runs barely cruise (17 sprint legs; 43 steady frames over 2.9 m/s) and never run fast on a curve
  (its run circles are 2-2.75 m/s). At the table's 3.91 m/s, `mmdb`'s run circles (60 °/s) match 0.9-1.1 m and 20-24°
  off at +1 s. Phase 4: cap the player's turn rate at run speed (or slow the run in turns), and choose run 3.9 m/s
  (the plateau) or something nearer the 4.5-5 peaks with speed warping.
- **Strafes are slower than forward** (walk 1.14 vs 1.27, jog 1.32 vs 1.85, crouch 0.76 vs 0.79), there are no run
  strafes, and jog strafes are 2:1 left (707 / 353 frames). Crouch strafes (8 legs) match 0.22-0.26 m off at +1 s, both
  sides. Phase 4 retunes strafe speeds from `GaitSpeedTable::Strafe` and doesn't strafe at run.
- *Phase 4:* the player adds TransitionCost 0.25, MinRunway 8, ForcedPoseWeight 0.25, trajectory position x2 while
  strafing, and gait x2 at run pace. The NPCs (Phase 5) should start from the same (`FirstPersonBodyMotion::Settings`).
- The search weights and the dead-blend times are Holden's defaults, not tuned on this pack. Tune them in Phase 4 on
  the player with recordings, not on numbers alone.
- The query's pose part is the playing frame's own features, not the blended output's. While a dead blend runs they
  differ for 0.2 s; standard practice, but a candidate if Phase 4 sees jumps chain.
- *Phase 4 confirmed it:* the jumps' deep landings won crouch searches and dead-ended. The runway (`MinRunway`) and the
  forced pose weight fixed it.
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
- **Phase 4 open:**
  - **Capture glitches** are excluded at load (`MotionDatabase::kGlitchDegrees` 35); repairing them at bake time
    (unwrapping twist flips) would get those frames back.
  - **A ~1 m ledge** is no longer in the player's predicted path (the two-height sweep).
  - **Crouch-strafe matching** passes at 50 % sideways frames - thin data (8 legs).
  - **Two first plants slide** (A walk 7 cm, crouch walk 12 cm).
  - *Resolved (session 2):* `--npc-test mmplayer` recorded only while the Game tab counted as visible (152 frames); it
    now records every frame of Play.
  - **The player's exclusions are the player's rules** (hands up, bent over, crossed legs: `SharedMotionDatabase::Get`
    with a rig). Phase 5's NPCs share the database: decide whether they want the same (they likely do).
  - **The run flick still crosses the feet briefly** (2 % of its frames, 5 cm; legacy 4 %, 2 cm): the matched turnaround's
    pivot. The heading cap (450 deg/s) is a feel choice - the owner may want it faster or slower.
  - **Hands in the view looking straight down** (100 % standing, 58 % running) is natural (they hang at your sides); D keeps
    them 0.30 m from the eye. Owner to judge.
  - **The chest's aim caps** (20 up / 35 down) are unarmed only; armed keeps the full aim for the arms rig's reach.
  - **The jog (1.85 m/s) has no key** (Shift is the run): an owner decision if a walk / jog / run split is wanted.
  - **No harness run exercises a variable frame time** (session 3): npc-test mode is pinned to 1/60 s. The dead-blend bug
    it hid is fixed and unit-tested; a jittered-dt mode for `mmplayer` would need its per-frame thresholds (pose step,
    jumps/s, the recording's every-other-frame) made per-second first.
  - **`SharedMotionDatabase::Get` is first-caller-wins** (session 3): the rig-based exclusions (hands up, bent over, crossed)
    run only on the load that passes a rig, and the database is cached by path alone. Today only the player loads it. Phase
    5: decide the NPCs' rules, then make the exclusion policy part of the cache key or always apply it - otherwise whichever
    of the player or an NPC loads first decides for both.
- Only `testbed` and `labloco` write `NPC_TEST_TRACE`. `npcloco`, `locomotion` and `stopsteps` log their metrics
  (`[NpcLoco]`, `[Locomotion]`, `[StopSteps]`, `[Slide]`).
- The baseline is one run per scenario. Re-runs were cut short on purpose. For noise bands, use
  `tools/npc_test_runs.py --runs 3`, but only if a later comparison is borderline.
- A forced focus steal (Notepad launched from a background job) did not take focus: Windows' foreground lock. Focus
  loss was checked through natural focus changes during the smoke run instead.

## Phase 4: next steps (continue on `dev/mm-phase4`)

**START HERE (2026-11-17, end of session 3): the strip-back.** The owner's verdict after play test 4: the sprint was a
patched-together fake (one dash stride looped; the pack has no sprint), and the motion matching path carries other
patches: a synthetic crouch, Spine Stability and the fixes it needed, spin caps. The approved plan removes them all and
plays the pack's mocap as captured:
- Shift = the jog at its natural pace;
- the pack's own crouch;
- the simulation / character facing model;
- Spine Aim fading while moving;
- additive Spine Twist.

**Read the plan first:** `C:\Users\Jacob\Desktop\MM_STRIPBACK_PLAN.md`. It holds the full inventory with a verdict per
item, three stages (the owner plays each), the tests to delete and add, and verification. Owner bar: AAA, no Frankenstein
work, no footage (the owner tests in the editor). The notes below predate it; where they conflict, the plan wins.

**Stage 1 (honest gaits) is in the working tree, not committed; the owner's play test is next** (2026-11-17, session 4):
- Removed: the sprint loop (and the matcher's loop hooks), the run gait for Shift, the run-in-turns slowdown, the crouch
  from the walk, `RaiseCrouch`, Spine Stability's MM branch (lean pull, neck/arm restore, crouch arm hang, head cap),
  `RelaxArms`, `KeepHeadOffWalls`, the reference poses, `MM Sprint Loop`, `TARTARUS_MM_CROUCH`. Spine Stability is
  legacy-only now. The matcher starts on the first searchable standing idle-take frame (`SharedMotionDatabase::StartFrame`).
- Shift = the jog (gait table 1.85 m/s ahead, 1.32 strafing; the gait is asked as Jog in every direction). Crouch = Require
  Crouch on the pack's takes at 0.79 / 0.76 m/s. Spine Aim fades with speed under MM, unarmed
  (`FirstPersonBodyMmAimFade`: 1 to 0.1 m/s, 0 at the walk/jog midpoint).
- Tests: the loop checks, the head-pitch running check, the wall head-clearance gate and `TestSprintLoopWrap` are gone;
  the M "run" legs are "jog" legs; new jog, crouch, aim-fade and wall-stop checks, the `[MmCrouch]` and `[MmJog]` tables,
  and an aim-fade unit test. `S settle at wall` starts 4.85 m off the wall (reached at the jog).
- Measured: unit 1819 checks pass; `mmplayer` 62/64; `mmstress` 15/16. The wall: stop frames 0.2 s after the capsule is
  blocked; the head clears the wall by 10 cm (no patch). The three failures are the Stage 2 items:
  - `M jog` trunk: the chest's yaw against the pelvis 1.36 of the clip's (0.8-1.25): Spine Twist's closed loop (chest
    yaw swing 9.1 -> 17.9 deg); the sprint loop's stride hid it. Stage 2's additive twist.
  - `M idle` 0.50 jumps/s (<= 0.3) and `S long idle` 24 deg off the view (< 20): the LookAround idle take turns the body
    with its root; past the 30 deg threshold the turn back forces searches. Stage 2's facing model.
- The crouched eye (`[MmCrouch]`, body eye on the head against the capsule's crouched eye): the crouch walk -1.4 cm, the
  crouch strafe -12.6 cm, the crouch idle -25.8 cm. The moving crouch fills the capsule, so Crouch Height was left as is
  (lowering it to the idle would put the crouch walk's head through the capsule top). The owner left it to us: kept.
- The owner's play test of Stage 1: "much better"; some idles "play half way through and snap back to the arms at rest".
  Cause, measured: the player's exclusions (a hand over its shoulder, bent over) removed only the frames over the line, so
  an idle take played the hand's rise, ran into the excluded frames, was forced to search and jumped to arms at rest - 5
  of the idle takes' 6 excluded stretches were entered with the hand over 5 cm above the take's rest. Fix: each run is
  excluded as its whole gesture (widened back and on while the hand / the chest's tilt is over the take's rest by 5 cm /
  5 deg, at most 2 s, then padded): 0 of 6 entered mid-gesture (an `mmplayer` check). Excluded frames 20083 -> 31477 of
  267059. New: `MotionMatcher::ForcedByExcluded`, the `[MmIdle]` "excluded" column, `S long idle` 20 s (one take 17.4 s).
  `mmplayer` 63/65 (the same two Stage 2 failures), `mmstress` 16/16.
- Then: "when I look all the way down I can see through my body". Measured with a new ray grid (`InsideProbe`: 336 view
  rays cast at the drawn body's triangles, the camera pass's hides applied; a ray whose first hit is a back face sees the
  inside): jogging and looking straight down, 110 rays (92 % of frames) saw into the torso, at the bottom of the screen -
  through the neck opening (the head is a Hidden Part). On the move the chest no longer bends with the view (Stage 1's
  Spine Aim fade), so the eye stayed over the opening. Legacy: 6 rays (the underpants' waistband). The near plane is 5 cm,
  so not a near-plane slice (checked first). Fix, a camera rule (`FirstPersonBody::EyeBodyPush`): looking down, the eye
  moves forward (flat, eased) only as far as it takes for the torso's neck rim (its open border round the neck, found once
  from the mesh) to leave the view while the eye is above it, and for the upper torso to stay 3 cm past the near plane.
  After: 3 rays jogging straight down (the waistband; up to 8 cm push), 2 walking, 0 standing (no push); legacy ~0.
  The camera probe now also runs unarmed, and counts its hides inside the view; `[MmLookDown]` and a look-down check
  (nearest body past the near plane, <= 10 inside rays) on the five look-down legs.
- Then: "still clips a little jogging" and the camera felt stiff. Measured (`[MmEye]`): the stabilized eye (the head's slow
  average within Eye Slack) kept 2.5 cm of the jog's 7.5 cm head bob. New **MM Head Ride** (component, 0-1, default 1;
  MM unarmed only): the eye rides the mocap head's own motion - the eye's bob is now the head's (7.5 cm jogging, 10 on
  A jog). The look-down push now snaps in (eased, it trailed the bobbing eye): jogging straight down, 0 torso rays; the
  residual <= 3 rays are the underpants' waistband (legacy 6) - a clothing gap, not the neck.
- Then: the owner asked to remove the procedural camera bob under the head ride and, still seeing the chest/neck, to limit
  looking down. Done:
  - `WeaponProceduralInput::CameraBobScale` = 1 - `FirstPersonBody::HeadRide()` (PlayFrame): riding the head, the camera
    bobs as the head does, nothing on top (an `A` check per mode: MM < 1 mm, legacy still > 5 mm - 7.5).
  - The look-down limit (`Player::MinPitch`, set in `BeforePlayerMove` under MM, applied after any view turn): each frame
    the steepest pitch some push (now at most 10 cm) clears, bisected, held over a stride (0.6 s), eased; never above -45.
    The testbed's straight-down legs now ask -89 (the camera's clamp), not -85. Jogging straight down: limit p50 -88,
    worst -84.5 deg, push up to 7 cm, 0 inside rays; walking and standing: no limit, 0 rays.
  - `mmplayer` 68/70 (the two Stage 2 failures), `mmstress` 16/16, legacy `testbed` 29/29.

**The full-library plan (2026-11-17, approved; `C:\Users\Jacob\.claude\plans\https-docs-google-com-spreadsheets-d-1sz-velvety-salamander.md`):**
use the whole pack, as designed. Owner decisions: compress then raise the caps; stroll; a run only if the data holds one;
animate fingers (unarmed); mirror only where the audit finds a gap.
- **Step 0, the audit (done):** `gen_takes.py --audit`, `tools/mm/audit.mmconfig` (every take, no budgets, into
  `work/mm_audit/`), `tools/mm/audit.py` -> `docs/mm_audit.md`. The bake now reports per-take finger motion. Findings:
  - all 139 takes bake (626k frames, 2.90 h, 161 MB poses / 66 MB features - the whole pack, uncompressed);
  - **no run**: the longest time anywhere at >= 3.5 m/s is 1.6 s / 5 steps, always a dash's peak (rises, brakes) - the run
    stays an asset gap;
  - steady speeds: stroll 0.69, walk 1.17, jog 1.87 forward / ~1.2-1.3 sideways and back, crouch 0.74 m/s;
  - mirror stroll (left/right 1.85 apart) and jog (1.40); walk, crouch, stand balanced;
  - jog takes are mostly speed-labelled walk (36.7k walk / 14.7k jog frames): the case for a style label;
  - 111 takes animate fingers; glitches: 520 jumps / 63 spikes (mostly toes, run starts); the Airborne label catches the
    jog's flight phases (671 frames in 19 non-jump takes).
- **Step 1, Stage 2's facing model (done, owner play test next):** simulation facing (spring to the facing wanted; standing
  and not turning it is the character's) / character facing (the clip's root turn, pulled to the simulation, clamped) -
  `FirstPersonBodyMotion::StepCharacterFacing`; Holden's demo values (0.27 s, 0.2 s, 90 deg - 0.12 / 0.15 / 45 measured
  6.4 jumps/s looking about). Spine Twist additive under MM. Deleted: the 450 deg/s cap, the foot-lock reset, the
  crossed-legs exclusion, `FirstPersonSlowAngle`. Results: `M jog` trunk 1.36 -> 0.96, `M idle` 0.50 -> 0.17 jumps/s
  (100 % idle mode), random-look slide 51 cm -> 0.8 / 1.7 cm, flicks <= 5.6 deg/frame. New: `M walk random look`, the
  facing / slide checks, a facing unit test. Left: looking about re-matches 5.2-5.6 a second (gate 3.7-5.0; pose steps
  and slide fine) - the curvy walk/jog data (Walk Snakes T2-5, Jog Freestyle T2/3) is budget-cut today: Step 2.
- **Steps 2 + 3, the bake rebuilt and the runtime asking for styles (done, owner play test next):**
  - **Curation** (`gen_takes.py`): every take whole (1 s trims) but the exaggerated `_S` takes and the dodges; the jog's
    and the stroll's strafes again mirrored (`<take>_Mirror`, `source` column, tag `mirrored`). 120 clips, **2.23 h**
    (was 1.27 h). The audit's redundancy found nothing worth cutting.
  - **Formats** (version 2): `.mmanim` adds per-clip finger tracks (38 bones, 32-bit smallest-three, worst 0.21 deg;
    `HasFingers` = the take moves a finger over 5 deg - 93 clips; others play the relaxed pose); `.mmdb` stores the features
    as 16-bit integers (worst error 3e-4 of a std; the search runs on them, `QuantQuery`, exactly as brute force) and a
    **style** byte per frame. Caps (`Manny.mmconfig`): poses 200 MB (174.9 used, 51 of it fingers), features 40 (26.2),
    load 2 s (files 0.28 s, database build 1.2 s, search 0.83 ms avg).
  - **Style label** (`MotionClipIntent::FromTags`): the take's intent - its group, or `style=` tags for starts (Stand_to_Jog:
    jog), the accelerations and transitions (several, per frame by stride-averaged speed; a start takes the cruise it
    reaches, a stop the one it leaves) and the run takes (walk + run: their walk-backs are walk). Jog frames 8.3k -> 103k.
  - **Airborne** only in jump takes (the jog's flights pass any physical test the hops pass): 0 outside them (was 671).
  - **Repair in the bake** (`MotionBake::RepairGlitches`): 191 twist flips unrolled (joints unmoved), 352 spikes slerped;
    216 frames left for the load-time exclusion. **Mirroring** (`MirrorPose`, rest-corrected): the mirrored takes
    rebuild on the Quantum rig within 0.02 cm.
  - **Runtime**: moving, the query's style is a **filter** (a soft cost of 1 lost to strafe costs of 3-15: the walk's strafes
    played the jog's); standing, any style. Shift = jog everywhere, **Walk action = stroll** (default key Caps Lock - Left
    Alt is Fire2's alternate; a gamepad stick under 0.6 strolls), crouch = crouch. Speeds per style and direction
    (`GaitSpeedTable` sideways / backwards apart; `Player::DirectionalSpeeds::Back`): stroll 0.79 / 0.59 / 0.77, walk
    1.26 / 1.17 / 1.10, jog 1.81 / 1.38 / 1.47, crouch 0.80 / 0.79 / 0.74 m/s. Fingers stream through the dead blend
    (`MotionPose::Fingers`); armed, the arms rig's copy takes the hands as before. `SharedMotionDatabase::CacheKey`: a
    rig's load and a rig-less one are cached apart.
  - **Gesture exclusion** (the full idle takes): the excluded stretch's padded ends walk out to the rest level on both the
    hand and the chest measure - mid-gesture cuts 0 (2 before the fix).
  - Checks: unit 2496/2496; `mmdb` 18/18, `mmview` 12/12, `mmstress` 16/16, legacy `testbed` 29/29; `mmplayer` 82/85
    with new legs (jog strafe right, jog back, stroll forward / left / right) - every style leg plays its own takes 100 %,
    left and right alike. Failing, reported: **the jog re-matches 2.6-2.7 times a second** (unchanged from before; the gate
    is relative to the walk, which improved to 1.2) - the pack's jog straights are short (9 m room: 1.4 s steady at
    most), so it hops between jog takes; **walk looking about 7.4 jumps/s** (pose steps p99 7.5 deg, better than the
    straight walk's) - Step 4's weight tuning.
- **Step 4, Stage 3's review and the tuning (done):** measured on `mmplayer` (deterministic run to run: two runs gave
  identical numbers), one change at a time against the committed defaults (`work/mm_smoke/s4_*`):
  - `KeepHandsFromEye` **stays**: it moves a hand on 1537 of 7387 steady frames (21 %), up to 8.1 cm (the crouch walk;
    jogs ~1-3 cm) - the data still brings hands to the eye (`[MmHands]` logs it; `BodyDebug::HandEyePush`).
  - `ForcedPoseWeight` **stays 0.25**: at 1.0 the stop legs re-match 46.3 a second (summed) against 40.7, the crouch 5.1
    against 4.9 (its pose steps fell 10.3 -> 6.5 deg, the stops' rose 4.7 -> 5.7).
  - The tuning, against the baseline (moving legs: 81.5 jumps/s summed, pose step p99 10.47 deg, trajectory error 0.168 m):
    | change | jumps/s | p99 | traj err | what broke |
    |---|---|---|---|---|
    | character facing pull 0.2 -> 0.35 s | 81.8 | 10.57 | 0.176 | jog flick slide 0 -> 45.6 cm (the 90 deg clamp drags), stairs reach |
    | trajDir weight 1.5 -> 1.0 | 72.7 | 10.53 | 0.164 | the idle (0.83 jumps/s), the stroll strafes lopsided, look-about slide 31 cm |
    | transition cost 0.25 -> 0.4 | 74.1 | 10.53 | 0.184 | walk flick slide 44.8 cm, the stroll strafes lopsided |
    | dead blend 0.2 -> 0.3 s | 81.5 | 9.60 | 0.168 | walk random look slide 0.2 -> 76.7 cm |
    None is better on balance: the defaults stay (Holden's). The worst foot slide swings 0.2 -> 77 cm on a small change -
    single events, where the foot lock lets a planted foot go (Reach 0.98 / MaxDrift 0.25 m) as the body turns over it
    faster than the clip turns, and drags it 0.2 s back to the animation. That is a foot-lock question, not a matching one
    (next steps).
  - Dead code: grep-clean (no sprint loop, run scale, crouch-from-walk, release-yaw reset or slow chest yaw left).
  - New logs: `[MmHands]`; the per-leg table lists every leg, with the median trajectory error.
- **The foot pivot (2026-11-17, the owner's pick):** the planted foot's lock (`IK::FootLock::Step`'s `ball`, motion matching
  only) holds the heel from the strike, then the ball once it is down (at the offset it has: nothing moves), and lets go when
  the toe leaves; a foot coming down on its ball is held there from the first frame. A foot let go steps clear over the way
  the turn carries it (`FootLockSettings::StepForTravel`: its lag plus the animation's speed x Release; a 5 cm lag lifted
  1.5 cm and skimmed 24 cm). Measured as the slide of the less moving of heel and ball (`FootSlideMeterTotals::MaxContactDrift`:
  a pivot on either is not a slide), worst plant, before -> after: walk flick 180 38.4 -> 1.7 cm, walk looking about
  18.8 -> 2.1, jog looking about 0.0 -> 1.6, walk random look 0.2 -> 4.3, jog random look 0.1 -> 1.8, jog flick 0.0 -> 0.0;
  the straight legs gain ~1-2 cm (the heel rolling as the hold passes to the ball), all within 5. Checked now on all six
  turning legs (<= 5 cm). (Rotating the body about the planted foot instead was ruled out: up to 28 cm off the capsule,
  and the camera stays on the capsule.)

Left (2026-11-17, after Step 4):

1. **The foot slide when the body turns faster than the clip** (the owner's call on the approach). Walking while sweeping
   the view slides a planted foot up to 19 cm (0.1 before the style label - the jog takes turned it), a 180 deg walk flick
   up to 38 cm (20 before). Cause, measured: the foot lock releases a planted foot past Reach / MaxDrift and drags it back over
   0.2 s while it is still down. Options: a planted foot that pivots on its ball with the body's turn (and steps when the
   leg runs out) instead of sliding; or the turn stepped by the data (the walk's turn-on-the-spot / pivot frames, searched
   for when the body is turning faster than the walk turns). Not tuning: every matching parameter moved it both ways.
2. **The owner's play test** of Step 4 (the defaults are unchanged; what changed is measurement) - then Phase 4's sign-off
   and Phase 5 (NPCs on the same database; `SharedMotionDatabase::CacheKey` keeps a rig-less load apart).
3. No sign-off videos: the owner plays it (`set TARTARUS_LOCOMOTION_SOURCE=mm`, `build\Release\TartarusEngine.exe`; the
   editor starts on the Forest - open `scenes/AnimTestbed.json` - and locks the exe: close it before building).

## Phase 5: first steps

1. Branch `dev/mm-phase5` from `dev/mm-phase4`, in the main checkout (licensed assets and `build\` only exist there).
2. `NpcBody::Tick` (~195-380): a `FirstPersonBodyMotion`-like driver per NPC on `SharedMotionDatabase::Get(..., rig)` (one
   database; decide on the player's exclusions), the trajectory from the navmesh path's corners (`TrajectoryPredictor::Predict`
   with `goalTurnRate`), LOD (search less often far away, or play on).
3. Start from the player's `FirstPersonBodyMotion::Settings` (TransitionCost 0.25, MinRunway 8, ForcedPoseWeight 0.25) and
   Spine Stability's MM path (lean kept, neck / arms keep their world orientation).
4. Port `npcloco`, `locomotion`, `stopsteps`, `roam`, `labloco`, testbed E and F to MM; add the `[MmPosture]` metrics for the
   dummy; measure against the legacy baseline first.

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
- **2026-11-17, Phase 4 (session 1, in progress):**
  - Added the MM pose source for the player (`FirstPersonBodyMotion`, `LocomotionSource`, `TARTARUS_LOCOMOTION_SOURCE`),
    the gait-table speeds by direction, the airborne segment, `--npc-test mmplayer` (45 checks) and a unit test.
  - First full pass all PASS (mmplayer 38/38, perf 3.09 / 6.02 ms).
  - The owner's play test 1 (the body lunged when the view turned on the move) led to:
    - the heading following the view without the clip's turn, and the schedule in the view's frame;
    - hysteresis, and the dead-end fixes: the runway, the forced pose weight, the prone pad;
    - glitch exclusion, which also fixed the run's 52 deg/frame pops.
  - A side session relayed the owner's "looking down folds the body":
    - measured against legacy;
    - Spine Stability now runs under MM with the database's idle reference;
    - then Arm Hang, for the stiff arms.
  - The owner's play test 2 found seven posture / turning problems, analysed under Phase 4.
  - The owner asked to continue in a new session. Committed as work in progress; the full pass wasn't re-run after the
    play-test fixes.
- **2026-11-17, Phase 4 (session 2):**
  - Added the posture metrics and the new legs; measured MM and legacy on the same course before changing anything.
  - Fixed A (the lean: neck and arms keep the clip's world orientation), B (aim caps), C (turn cap, chest twist limit, feet
    free in a fast turn), D (hands off the eye); excluded hands-up, bent-over and crossed-leg frames for the player; fixed the
    crouched reference (it was standing) and the short recording.
  - The owner, mid-session: "the sprint and crouch look goofy" - the crouch's reference, more lean kept, the sprint's arm
    swing damped; reviewed on contact sheets.
  - Visible runs through `mm_smoke.ps1`: ~10 `mmplayer` runs while working (three recorded), one legacy measurement run,
    then the full pass, all PASS.
- **2026-11-17, Phase 4 (session 3, audit):**
  - Read the runtime end to end; measured the stream directly before changing anything (one hypothesis - widen the warp's
    speed window - was measured and rejected: it would trade tempo wobble for foot slide).
  - Fixed: the dead blend's velocity under a variable frame time (every harness run is fixed 1/60 s, Play is not); the
    sprint loop's play-rate step at every wrap; the loop's stride (steady and symmetric, not the best seam); `EnterAir`'s
    stale state with no jump found. +2 unit tests.
  - Logged two latent issues for later (no variable-dt harness run; `SharedMotionDatabase::Get` first-caller-wins).
  - One recorded `mmplayer` run (57/57) plus the MM unit filter; no full pass (only MM code changed; legacy untouched).
  - Later in the session:
    - the trunk fix (a teammate session's relayed report);
    - the loop seam offset (the "stop every few steps");
    - play test 4's idle rewind loop (idle mode, idle takes only, the reselect history) and the sprint-turn collapse;
    - A/B switches and the `[MmTrunk]` / `[MmIdle]` / `[MmSprint]` / `[MmLayers]` metrics.
  - Last: `mmplayer` 63/63, unit 1821 checks.
  - The owner then judged the sprint a fake (still goofy with every layer off). Root cause: the pack has no sprint. The
    strip-back plan was approved; see "Phase 4: next steps". Recordings stopped at the owner's request.
- **2026-11-17, Phase 4 (the sprint):**
  - Phase A (the held run) and Phase B (the dash burst) on the pack's data, measured, then retired at the owner's word
    ("still extremely short").
  - The sprint is now the old locomotion's jog set, baked as the `sprint` style: project-path, root motion and looped takes in
    the bake; the side loops mirrored (the bake mirrors root travel); Left Shift pressed while moving, held. The burst code is
    gone. `mm_smoke.ps1 -Exe` fixed.
  - `mmplayer` sprint legs: all pass but `M sprint turning`'s foot slide (8.8 cm, <= 5). The owner's play test: "the sprint
    works great". No full pass yet; nothing committed.
