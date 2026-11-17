# Motion Matching Overhaul: Session Handoff

**Read this first in every motion matching session.** It is the only context a new session needs. Update it at the end of
every session: status, files touched, exact commands and results, open issues, next steps. Then commit.

Reference: [`MOCAP_EXPORT_FINDINGS.md`](MOCAP_EXPORT_FINDINGS.md), the verified facts about the exported mocap pack.

## Status

| Phase | Branch | State |
|---|---|---|
| 0. Analysis and roadmap | `dev/mm-phase1` | **Done** (2026-11-17) |
| 1. Test infrastructure and legacy baseline | `dev/mm-phase1` | **Done** (2026-11-16) |
| 2. Motion bake pipeline and root generation | `dev/mm-phase2` | **Next** |
| 3. Motion matching runtime | `dev/mm-phase3` | Pending |
| 4. First-person player integration (unarmed) | `dev/mm-phase4` | Pending |
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
    labloco -PerfBench tests\perf-scenes\perf_stress.json`. It should be all PASS. Use `-AllowErrors` only for a local
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

**Tests**
- `src/Tests/SmokeHarness.cpp`: the scenario-to-scene map at ~86–101.
- `src/Game/AI/NpcTest*.cpp`.
- `src/Tests/UnitTests_Animation.cpp`.
- Launch flags: `src/App/LaunchOptions.cpp`.
- Env vars: `src/Core/Env.h`. Every env var must be listed there.
- Tools: `tools/mm_smoke.ps1`, `tools/mm/baseline_summary.py`, `tools/npc_test_runs.py`; baseline in `docs/mm_baseline/`.

## Open issues

- `_FO` (idles) and `_M_L` (one jump) suffixes are undefined. They are tagged idle and jump; the owner should look at
  them in `mmview`.
- Pack-Manny vs Quantum rest pose and bone lengths: not yet compared (Phase 2).
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

## Phase 2: first steps

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
