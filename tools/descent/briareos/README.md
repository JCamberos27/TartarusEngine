# Briareos look tools (W6, Descent team, 2026-11-24)

Text-only copy of the sweep / look-sheet tooling from the girlfriend's PC (desktop-gqko56v), where the team folder
`D:\Jacob\tblocks\descent\` is NOT in git. No PNGs here. Paths inside the scripts are that PC's (`D:\Jacob\tblocks\descent\...`,
the GPU runner worktree `wt-gpu`): on another machine, edit the path constants at the top of each script.
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

## Run a tour (GPU, one engine job at a time)
`run_sweep.ps1 -Out <dir> -Tour <tour.json> [-TimeoutMin 30] [-SceneSwap]` launches the Release engine hidden and muted:
`TartarusEngine.exe --project <wt>\project --gi-tour <tour.json> --gi-tour-view full --out <dir>\png --perf-res 1920x1080`
(`--gi-tour-view full` writes both `NN_<id>_full.png` and `NN_<id>_gi.png`). It refuses to run unless the runner worktree is clean
(`-SceneSwap` allows only `project/scenes/Forest.json` dirty), the lock folder `locks\perf` names W6, no foreign engine is running
and D: has 100 GB free. Idle cost about 2.2 s per shot. Output: PNGs, `sweep_summary.txt`, stdout/stderr.
Example: `run_sweep.ps1 -Out house-polish\sweep_fix3\options\bathdoor_hall -Tour sweep_tour_opt_bathdoor_hall.json`.

## Generate the full sweep tours (CPU)
`make_sweep_tour_v2.py` (run from a worktree root at the sha to sweep: `python -I <path>\make_sweep_tour_v2.py`; options `--scene`,
`--out`, `--door-state`, `--eye`, `--bounds`) writes `sweep_tour_open.json` (doors all open: 35 room spots x (4 walls, ceiling, floor),
thresholds, doors both sides near/far, outside, stairs; about 439 shots) and `sweep_tour_shut.json` (doors as authored, about 97 shots)
plus `sweep_plan.json` and a dry check of the cameras (`SWEEP_DRYCHECK.md`, 0 camera problems expected). It reads the bounds audit
`bounds_audit_final.json` (not in git). The big tours are not committed: regenerate them.

## Contact sheets and the look sheet (CPU, Python with Pillow + numpy)
- `make_sweep_sheets.py --run baseline=<png dir> [--run after=<png dir>] --out <dir> [--thumb 480]`: one sheet per camera group, rows
  per run (full, then gi), thumbnails labelled with the PNG name, pairs runs by waypoint id; also writes `index.html`.
- `make_look_sheet.py` (`python -I`): Jacob's look sheet `look_sheet\look_sheet.html` + panels: a KNOWN section (seams thr24 / thr07 /
  thr02, door 20756 strip, master bedroom ceiling diagonal) and OPTIONS before/after (35613 south, hall line, kitchen half wall).
  Inputs are the `sweep_fix3` PNG folders; edit `B`, `FIX`, `BASE` at the top.
- `hard_edge_scan.py`: candidate list of hard lighting edges in the shots (`HARD_EDGES.md`; mostly geometry, a candidate list only).
- `EYES_ON_TEMPLATE.md`, `EYES_ON_SWEEP_677c753d.md`: how the shots are read and what was found at 677c753d.
- `JACOB_WALK_STEPS.md` / `WALK_POPS.md`: steps and pops for Jacob's own walk (29 uncounted steps + 2 ramps).
- `BOUNDS_COVERAGE.md`: which GI bounds the tour covers.

## Option stills (before/after, scene patch applied then restored)
`run_options.ps1` / `run_options_rest.ps1` run `sweep_tour_opt_*.json` on the runner: AS-BUILT, then each option's `Forest.json`
patch from `optpatches/` (tableb 6c4b9d89, south 525e3a75, hallline d26b5d3c) via `git apply`, one engine run, then
`git checkout -- project/scenes/Forest.json`. `make_option_tours.py` builds the `sweep_tour_opt_*.json` files. The tableB option is dropped.

## Gotchas
- Python strings with `\f \b \a` eat Windows paths: use raw strings.
- Never run the engine in a compile-only build dir; the GPU runner is the only place for engine, tour and bake runs.
- Every perf number from the girlfriend's PC is "DESKTOP-GQKO56V, RAM 2133 (XMP off)".
