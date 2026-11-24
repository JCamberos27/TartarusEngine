# Briareos look tools (W6, Descent team, 2026-11-24)

Text-only copy of the sweep / look-sheet tooling from the girlfriend's PC (desktop-gqko56v), where the team folder
`D:\Jacob\tblocks\descent\` is NOT in git. No PNGs here. Scripts take their locations from arguments / env vars: `DESCENT_ROOT` = the team folder
(default `D:\Jacob\tblocks\descent`), `DESCENT_RUNNER` = the worktree whose `build\Release` runs the engine (default `<root>\wt-gpu`).
`../FOUND_ISSUES.md` is a copy of the issue log (`house-polish\FOUND_ISSUES.md`) as of this commit.

## OPEN: Jacob #2 (hall -> bathroom doorway wall blotch) is NOT verified
Jacob's screenshot (lamp-1 look, `jacob_look_lamp1\2.webp`, not in git) showed a blotchy, hard-edged dark shape on the wall
beside the bathroom doorway (door 20762, doorway x ~6.34, z ~26; hall side is z > 26) and a dark strip on the bathroom tile.
Argus raised Shadow Softness 1.3 -> 2.5 on the hall spots 35594/35596 for the wall blotch. The sweep poses so far never stood
at his pose (the shot framed another wall; the 18:19 retake stood INSIDE the bathroom).

THE RETAKE TO RUN: `sweep_tour_opt_bathdoor_hall.json`, pose **jlook-bathdoor-hall_match1**: feet (7.3, 0.194, 27.4), yaw -125,
pitch -25 = in the hall just east of the doorway, 0.6 m off the doorway wall, looking down and west-north-west into the bathroom
(bathroom tile ahead, jamb and blotch wall on the RIGHT, open door leaf on the left). Variants match2 (7.0, 27.0, -120, -28),
match3 (7.7, 27.9, -130, -20), wall_e, wall_e_up, wall_w (control). Convention: yaw 0 = +x, 90 = +z, -90 = -z; facing -z, +x is on the right.
Camera positions are NOT engine-checked: if one sits inside a collider, nudge it and re-shoot. `sweep_tour_opt_bathdoor_retake.json`
is obsolete (inside the bathroom).

## Entity ids are renumbered (#1216): everything here is keyed by NAME / POSITION
Waypoint ids never contain an entity id: doors are `door-x<m|p><x*10>-z<m|p><z*10>` (position of the door, e.g. `door-xm19-zp303`), thresholds
`thrNN-<rooms>` (the NN follows scene order: look them up by their `at` position in `sweep_plan.json`), red-room doors `jlook-red-door-z26/z29`,
jamb looks `jlook-jamb-x9 / -z26 / -z29`. Entity ids that remain inside waypoint NOTES are the pre-#1216 numbers and are history only.
The bounds-audit shots (`--bounds bounds_audit.json`) are refused row by row unless the scene's light with that id has the row's name:
regenerate the audit on the current scene (Atlas's `bounds_audit.py`) before expecting `bounds_*` shots.

## Run a tour (GPU, one engine job at a time)
`run_sweep.ps1 -Out <dir> -Tour <tour.json> [-Runner <worktree>] [-TimeoutMin 30] [-SceneSwap] [-NoLock] [-LockOwner W6]` launches the Release engine
hidden and muted: `TartarusEngine.exe --project <runner>\project --gi-tour <tour.json> --gi-tour-view full --out <dir>\png --perf-res 1920x1080`
(`full` writes both `NN_<id>_full.png` and `NN_<id>_gi.png`). It refuses to run unless the runner is clean (`-SceneSwap` allows only
`project/scenes/Forest.json` dirty), `<root>\locks\perf\owner.txt` names the owner (skip with `-NoLock` on a machine without the team folder), no foreign
engine is running and the drive has 100 GB free. About 2.2 s per shot idle. Output: PNGs, `sweep_summary.txt`, stdout/stderr.

## Generate tours (CPU)
Run from a worktree root at the sha to sweep (it needs `tools/gi/gi_audit.py` and `project/scenes/Forest.json`):
`python -I tools/descent/briareos/make_sweep_tour_v2.py --out <gen dir> [--bounds <audit.json>] [--descent-root <team folder>]` writes `sweep_tour_open.json`
(doors all open: room spots x (4 walls, ceiling, floor), thresholds, doors both sides near/far, outside, stairs, the look poses; ~370 shots),
`sweep_tour_shut.json` (doors as authored, ~97 shots), `sweep_plan.json` and a dry check (`SWEEP_DRYCHECK.md`). Not committed: regenerate.
`make_look_tour.py --gen <gen dir> --out <dir>/sweep_tour_look4.json [--extra more.json]` picks the compact LOOK tour (master ceiling line, south wall fade, hall
line, the three known seams by position, plus the `--extra` tours, e.g. `sweep_tour_opt_bathdoor_hall.json`) and writes `<out>_groups.json`.

## Sheets (CPU, Python with Pillow + numpy)
- `make_look_sheet4.py --tour <look tour> --before <png dir> --after <png dir> --out <dir> [--gi] [--note GROUP=text]`: per shot BEFORE | AFTER | difference x6
  (same pose both times), grouped as in the look tour; `look_sheet.html` + `panels\`.
- `make_sweep_sheets.py --run baseline=<png dir> [--run after=<png dir>] --out <dir> [--thumb 480]`: contact sheets per camera group for the full sweeps, pairs by waypoint id.
- `hard_edge_scan.py`: candidate list of hard lighting edges in the shots (mostly geometry; a candidate list only).
- `EYES_ON_TEMPLATE.md`, `EYES_ON_SWEEP_677c753d.md` (history, pre-#1216 ids): how the shots are read. `JACOB_WALK_STEPS.md` / `WALK_POPS.md`: steps and pops for Jacob's walk.
  `BOUNDS_COVERAGE.md`: which GI bounds the tour covered.
- Removed (pre-#1216 scene patches and the fix-3 look sheet): `run_options*.ps1`, `optpatches\`, `make_look_sheet.py`, `make_option_tours.py`: the options they previewed are now the fix4 scripts.

## Gotchas
- Python strings with `  ` eat Windows paths: use raw strings (and the Edit tool, not a python heredoc, for edits that contain them).
- Never run the engine in a compile-only build dir; the GPU runner is the only place for engine, tour and bake runs.
- Every perf number from the girlfriend's PC is "DESKTOP-GQKO56V, RAM 2133 (XMP off)".
