# Sweep eyes-on report: <build sha>, <date UTC>, <who>
Run: <wt-gpu sha>, open <n> + shut <n> shots, PASS/FAIL, GL/log errors <n>. PNGs: <dirs>. Sheets: <dirs>. Hard-edge scan: <out dir>\HARD_EDGES.md.

## PASS / FAIL LINES (every line must be filled; FAIL needs shot names)
| Check | Result | Evidence (shot names) |
|---|---|---|
| **HARD EDGES (hard shading lines, house-wide, inside AND out): PASS / FAIL** | | Scan: `python -I hard_edge_scan.py --run <open png> --run <shut png> --baseline <99579b4e png> --out <dir>`; every NEW / STRONGER / NO BASELINE row in the top 60 looked at on its marks\ PNG and classed: real geometry corner / lighting edge / texture. FAIL = any lighting edge (square patch, light cut across floor/ceiling/siding/rug) that a player would notice. List each: shot, location, step %, verdict. |
| Hard edges: exterior siding, all 4 facades + corners at night (jlook-facade-*, jlook-corner-*, jlook-porch-front) | | |
| Hard edges: red room floor spill into doors / hall (jlook-red-*) | | |
| Hard edges: upper-hall ceiling dome by the stairs (jlook-hall-dome_*) | | |
| Hard edges: entrance-hall runner rug + stairs (jlook-entrance-rug_*) | | |
| Kitchen sink basin reads lit, probe recapture (jlook-kitchen-sink_*) | | |
| Master bedroom ceiling edge (144/22 master-bedroom_ceil, bounds-35598_pz, bounds-35611_pz) | | |
| Den lamp column patch not hot (den-lamp-column20599_view) + den spot2 floor ~x3 brighter | | |
| Row 111 jamb strip, 35614 doorway (jlook-jamb-35614_west, _east; door20761_a/b): PASS or "lit strip" | | |
| Row 111 jamb sliver, 35601/36406 (jlook-jamb-35601_threshold; thr29-upstairs-bathroom if it frames z 25.99..26.94): PASS or "lit strip" | | |
| Row 111 jamb sliver, 36400 (jlook-jamb-36400_threshold; thr27/thr28-master-bedroom if they frame z 28.90..29.85): PASS or "lit strip" | | |
| Door 20755 threshold (GI 0.59 vs the 0.60 bar, known): door20755_a-near/a-far/b-near/b-far, open + shut. Dark strip inside the jamb? | | |
| Child's bedroom 2 (cb2) dark outliers 45/125 probes (Argus bar <= 22 not met, known): are dark spots VISIBLE? child-s-bedroom-spot2_* (all 6 views), child-s-bedroom_*, plus Kronos stills gi-p2\kronos\doors\fix-3\childbed2 | | |
| Kronos counted walk pops (full route 23, GI route 3) + ramps R1 (hall_center->hall_west_corner -74%) and R2 (kitchen_door_before->hall_center +65%): walkpop-*_mid, walkwp-*_pose; see WALK_POPS.md. Per segment: visible brightness jump / smooth | | |
| Lit strip on the siding? (family E): main facade east of the living-room bay, x 13.6..14.6, z 21.1..24.0, at night (jlook-facade-bay-east_night): PASS or "lit strip" | | |
| Jacob #2 hall -> bathroom doorway WALL blotch gone / soft (jlook-bathdoor-wall_face): PASS or FAIL | | |
| Chandelier graze: no cube-face seam on the south wall (jlook-chandelier-graze_face, _oblique): PASS or FAIL | | |
| Thin caster shadow reads clean, no stair-step / banding (jlook-thin-caster_chandelier, living chandelier 35592 arms): PASS or FAIL | | |
| Dark rooms read OK (list) | | |
| Doors both sides: no dark strip / no leak line | | |
| Bounds shots: no hard light edge across the face | | |
| Dark holes near furniture | | |

## Hard-edge scan summary
Thresholds: see HARD_EDGES.md header (step >= 12 %, STRONG >= 25 %, sharp >= 0.6, len >= 90 px @960, <= 6 per shot/view). Edges flagged: <n> in <n> shots; NEW/STRONGER vs baseline: <n>; no-baseline shots: <n>.
| Shot | Location | Step % | vs baseline | Class (geometry / lighting / texture) | Owner |
|---|---|---|---|---|---|

## Coverage (sheets read / not read)
## Reads OK / too dark for Jacob
## Found issues (also in FOUND_ISSUES.md)
