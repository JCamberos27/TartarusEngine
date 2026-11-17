# Legacy locomotion baseline (motion matching overhaul, Phase 1)

The legacy body locomotion (`AnimatorController` graphs + `BodyLocomotion`), measured once before motion matching
replaces it. Phases 4 and 5 must match or beat these numbers. See [`../MOTION_MATCHING_HANDOFF.md`](../MOTION_MATCHING_HANDOFF.md).

**[SUMMARY.md](SUMMARY.md) has the tables:** results and frame cost, foot slide, clip vs capsule speed, eye jitter, perf.

## How it was captured

- 2026-11-16, branch `dev/mm-phase1` on top of `5093609f` (Phase 0) plus the Phase 1 engine changes, which don't touch
  locomotion: `TARTARUS_UNLOCKED_FPS`, harness input ignoring focus, and the summary line's timing.
- Release build on the owner's machine: GeForce RTX 4060 (driver 617.42); window 1920x1080; one visible run each, at unlocked FPS, muted.
- Command:

  ```powershell
  powershell -ExecutionPolicy Bypass -File tools\mm_smoke.ps1 testbed npcloco locomotion stopsteps feet labloco `
      -SkipUnit -SkipSmoke -PerfBench tests\perf-scenes\perf_stress.json -TraceDir work\mm_smoke\baseline\traces `
      -LogDir work\mm_smoke\baseline
  python tools/mm/baseline_summary.py work/mm_smoke/baseline docs/mm_baseline --traces work/mm_smoke/baseline/traces
  ```

## What's here

| File | Contents |
|---|---|
| `SUMMARY.md` | The tables. Generated, so don't edit it by hand. |
| `metrics.json` | The same numbers per run, for a script to compare against. |
| `logs/<run>.txt` | Each run's tagged lines: `[NpcTest]` checks, `[Slide]`, `[Testbed]`, `[NpcLoco]`, `[Locomotion]`, `[StopSteps]`, `[PerfBench]`, `[SmokeTest]`, and stderr `[error]` lines. |
| `traces/testbed_trace.csv` | `NPC_TEST_TRACE` of `testbed`, per frame: gait phase, camera, feet, reach, hands, dummy (columns in `NpcTest_Testbed.cpp` ~712). |
| `traces/labloco_trace.csv` | `NPC_TEST_TRACE` of `labloco`, per frame: gait phase, camera bob and roll, the gun (`NpcTest_LabLoco.cpp`). |

Only `testbed` and `labloco` write a trace. The other scenarios log their metrics instead.

## Where the metrics come from

- **Foot slide:** the `[Slide]` meter (`testbed`, `labloco`, `npcloco`). For each leg and actor: planted feet, mean
  and worst drift while planted.
- **Clip vs capsule speed:** `testbed` A walk and jog. The ratio of the gait loop's clip travel to the capsule's speed
  over the steady frames (the check allows 5%). Dummy top speeds come from `npcloco`.
- **Frame cost:** `perf_stress` p50/p95/p99 per phase (edit, play, play-max). For the npc scenarios, wall time over
  rendered frames, which includes loading, so it is only a rough figure.
- **Eye jitter:** `testbed` A, logged rather than checked.

## Known failures at capture time (not locomotion; both fixed since)

- **Every scenario logged `[error]` lines** for two zero-filled local asset files: `worklight_hum_1.wav` and
  `T_Spider_AO.png`. That is why the logs show "N new log error(s)". Both were restored from the Drive the same day,
  hash-checked against `project/external_assets.csv`.
- **`feet` failed 2 of 6** ("the dummy stands on the ramp (across / uphill)"). This was a test bug: the teleport onto the
  ramp didn't reset the director's velocity, so the dummy slid off the ramp across the Arena. It is fixed in
  `NpcTest::Feet` (6/6). The `feet` numbers here are from before the fix.
