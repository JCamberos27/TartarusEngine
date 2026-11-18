# Motion Matching

The first-person player's body and the NPC dummies are posed by motion matching. Every frame, a matcher searches a database of
motion capture for the pose that best continues the current one along the trajectory the character is about to take, and blends
over to it. There is no locomotion state machine, and no start, stop or turn clips to trigger. The overhaul that replaced the old
locomotion graphs, and its decisions, measurements and history, is in [`MOTION_MATCHING_HANDOFF.md`](MOTION_MATCHING_HANDOFF.md).

## The data

- **The source:** the Quantum Stage "Make Movement Feel Real" mocap pack (139 takes, 2.98 h, 60 fps, UE5 Manny skeleton). The
  exaggerated `_S` takes and the dodges are left out (`tools/mm/takes.csv`).
- **The sprint:** the old locomotion's jog set (`assets/Animations/Mocap/RootMotion/Locomotion_V2/AM_Jog/`), baked in as the
  `sprint` style, because the pack has no sustained run. Keep that folder.
- **The bake:** `TartarusEngine.exe --bake-motion assets/Animations/MotionMatching/Manny.mmconfig`, with
  `TARTARUS_MM_SOURCE` set to the exported pack. It reads the raw FBX files, generates each take's root
  (`RootSynth`, or the clip's own root for the jog set), repairs capture glitches, mirrors the lopsided strafes, labels every
  frame (gait, style, crouch, airborne, strafe), and writes `Manny.mmanim` (the poses) and `Manny.mmdb` (the search features).
  Both are git-ignored and live on the asset Drive.
- **The budgets** (in `Manny.mmconfig`, and the bake fails over any of them): pose memory 200 MB, features 40 MB, load time 2 s.
  The current database is 2.29 h of motion, 178.6 MB of poses and 26.9 MB of features, and loads in about 0.5 s.

## The runtime

- **`SharedMotionDatabase`:** loaded once and shared by every body (cached per rig). At load it also drops frames that look wrong
  in first person (hands over the shoulders, the actor bent over), measures the gait speed table and the turn-in-place rate.
- **`FirstPersonBodyMotion`:** the per-body driver.
  - The capsule leads, and the pose follows it.
  - A `TrajectoryPredictor` predicts the next second from the capsule's state and the wish. For an NPC, it predicts along the
    navmesh route's corners (`PredictAlong`).
  - The `MotionMatcher` searches at a set schedule, with a transition cost, a minimum runway and a forced-pose weight, and blends
    to the new pose by inertialization.
  - Speed warping keeps the clip's pace on the capsule (play rate 0.8-1.25).
  - The character facing follows the matched clip's own root turn, pulled toward a simulation facing (Holden's facing model).
  - Jumps and falls play a baked jump locked to its label.
- **The player** (`FirstPersonBody::TickMotionMatching`): the capsule moves at the gait table's speeds.
  - **Controls:**
    - Walk by default.
    - Stroll with Caps Lock or a partial stick.
    - Jog with Left Shift held.
    - Sprint by pressing Left Shift while moving and holding it.
    - Crouch.
  - **Turning in place:** past the Turn Threshold, at the database's turn-in-place rate.
  - **Still layered on top:** foot IK and the foot lock, the spine aim and twist, the arm hang, the eye's head ride, the camera
    probe.
- **The dummies** (`NpcBody::StepMotionMatching`): the same driver and database.
  - **Speeds:** the director moves the capsule at the gait table's walk, jog, and sprint (for a run).
  - **Trajectory:** follows the crowd's route corners.
  - **LOD:** the animation LOD holds frames, and a held frame doesn't step the matcher. The next posed frame steps it over the
    whole held time, so a far dummy searches a quarter as often.
  - **After the matcher:** the foot pass, the look and the piece sync run as before.
  - **Without the database** (CI has no licensed assets), a dummy stands in its bind pose, and the engine warns once.
- **Body pieces** need no Animator Controller. The driver is the piece with the whole skeleton (pelvis, head, hands). If a piece
  still has a controller (an old prefab), it's held.

## Tuning

- **The defaults** are in `FirstPersonBodyMotion::Settings`.
- **`TARTARUS_MM_TUNE="Key=value,..."`** overrides a setting at startup with no rebuild (`MmTune`). The keys are listed in
  `MOTION_MATCHING_HANDOFF.md`, "Fast iteration".
- **The Inspector's First Person Body** keeps two A/B switches:
  - MM Post Layers: off draws the matcher's pose with foot IK only.
  - MM Head Ride: how much of the mocap head's motion the eye rides.

## Testing

`tools\mm_smoke.ps1` is the entry point. Run one script at a time: it already runs up to three engines in parallel. Every
scenario runs on a light test scene; none runs in the apartment scene.

| Tier | Command | Covers |
|---|---|---|
| Iterate | `mm_smoke.ps1 mmplayer -SkipUnit -SkipSmoke -Legs "..."` | the player's legs in question |
| Quick | `mm_smoke.ps1 -Quick` | unit, mmplayer, mmstress |
| Full | `mm_smoke.ps1 -Full` | unit, smoke, testbed, npcloco, stopsteps, feet, labloco, mmview, mmdb, mmplayer, mmstress, npcperf, the perf bench |

- **`mmplayer` / `mmstress`** (AnimTestbed): the player's course, and a stress course.
- **`npcloco`** (LocomotionLab): one dummy through every gait.
- **`npcperf`** (LocomotionLab): a dozen roaming dummies at every LOD tier, with their cost and the frame time.
- **`testbed`** (AnimTestbed): the mixed course; part E is a dummy setting off behind itself.
- **`mmview` / `mmdb`** (LocomotionLab): the baked stream and the database's search.
- **Unit tests:** `--unit-tests "Motion matching"`.
- **CI** has no licensed assets. Anything that needs the pack skips rather than fails.

## Known gaps (assets the pack doesn't have)

- **Sprint:** there's no sustained run or sprint, so the old jog set stands in for it.
- **Long straight jogs:** the steady jog stretches are short, so a long straight jog keeps re-matching.
- **Tight turns at a walk:** there are none.
- **Running jumps:** every jump starts from standing.
- **Prone:** only the transitions to and from it.

The open measurements that still fail are in the handoff's open items. They're reported there; their limits haven't been
loosened.

## Where things are

- `src/Game/MotionMatching/`: the bake (`MotionBake`, `RootSynth`, `MotionClipStream`, `MotionFeatures`) and the runtime
  (`MotionDatabase`, `MotionSearch`, `MotionMatcher`, `Inertialization`, `TrajectoryPredictor`, `MotionGait`).
- `src/Game/FirstPersonBodyMotion.{h,cpp}`: the per-body driver and the shared database.
- `src/Game/FirstPersonBody.cpp`: the player body. `src/Game/Npc/NpcBody.cpp` and `src/Game/AI/NpcDirector.cpp`: the dummies.
- `project/assets/Animations/MotionMatching/Manny.mmconfig`, `tools/mm/gen_takes.py`, and `tools/mm/takes.csv`.
- The tests: `src/Game/AI/NpcTest_*.cpp`, `src/Tests/UnitTests_MotionMatching*.cpp`, and `tools/mm_smoke.ps1`.
