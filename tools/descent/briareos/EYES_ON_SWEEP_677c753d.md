# Sweep eyes-on report: 677c753d (fix-3 Forest.gi c8b97bd7 + tent PCF 471d1673), 2026-11-24 ~18:30 UTC, W6 Briareos
Run: wt-gpu 677c753d, open 439 + shut 97 shots, SmokeTest PASS both, GL errors 0, log errors 0. PNGs: house-polish\sweep_fix3\open\png, \shut\png. Sheets: \sheets_open (172), \sheets_shut (37). Look sheet: \look_sheet\look_sheet.html. Hard-edge scan: \edges_open\HARD_EDGES.md, \edges_shut\HARD_EDGES.md.
HONEST SCOPE: I read the named eyes-on shots below (contact composites of every jlook-* shot, den lamp, master ceiling, bounds, door 20755, cb2 stills). I did NOT read the other ~700 sweep shots one by one, and the scan list (4718 + 805 edges) is a candidate list, not a verdict.

## PASS / FAIL LINES
| Check | Result | Evidence (shot names) |
|---|---|---|
| **HARD EDGES house-wide: FAIL (1 confirmed in the shots I read, master bedroom ceiling)** | FAIL | master-bedroom_ceil: a straight diagonal line across the red ceiling, bright above / darker below (the Talos row). bounds-35598_pz and bounds-35611_pz: a straight red / dark step at the ceiling edge on the right of the frame, probably the same Bounds line (thumbnail read, not certain). Atlas's 5802c9f0 (35598 + 35611 +x face) and 5564577b ARE in this build (ancestors of 677c753d), so the diagonal survives those fixes: still present, cause under investigation (Atlas + Talos). |
| Hard edges: exterior siding, 4 facades at night (jlook-facade-*) | PASS (read) | S x-9/0/9, N x-9/0/9, E z23/z28 and bay-east: siding evenly lit, no square patches; the N and E hard shapes are tree shadows. W z23 / z28 are almost black (west wall unlit, no patches). Note: jlook-facade-s_x-9 has a hard diagonal light-shaft wedge in the fog at the upper left (moon shaft cut by the roof); not on the siding. jlook-corner-* not read in detail. |
| Hard edges: red room floor spill into doors / hall (jlook-red-*) | PASS with note | jlook-red-zface_along: the spill is a soft wedge from the doorway across the hall carpet; no hard step at z 25.938 in zface_across / along. jlook-red-door20758 / 20759 in/out: no step. The wedge has two straight edges (door-jamb shadow); soft, but Jacob should judge it. |
| Hard edges: upper-hall ceiling dome by the stairs (jlook-hall-dome_view1..3) | PASS | smooth falloff, no hard rim round the fixtures. |
| Hard edges: entrance-hall runner rug + stairs (jlook-entrance-rug_along-e, along-w, stairs) | PASS | rug and floor evenly lit, no shadow line across the runner. |
| Kitchen sink (jlook-kitchen-sink_basin, _wide) | AS EXPECTED, still dark, known | the black basin bowls still read dark (known); counter, splash wall and cabinets are lit and even, no dark blotch. |
| Master bedroom ceiling edge (144/22 master-bedroom_ceil, bounds-35598_pz, bounds-35611_pz) | FAIL | hard line still present (see above); 5802c9f0 and 5564577b are already in this build; the diagonal survives them, cause under investigation (Atlas + Talos). |
| Den lamp column patch not hot (den-lamp-column20599_view) | PASS | lamp, wall and column lit softly, not blown out. (den spot2 floor x3 brighter: not measured.) |
| Row 111 jamb strip 35614 doorway (jlook-jamb-35614_west, _east) | PASS (thumbnail read) | threshold floor evenly dark on both sides, no ~15 cm lit strip seen. |
| Row 111 jamb sliver 35601/36406 (jlook-jamb-35601_threshold) | PASS (thumbnail read) | threshold tile uniform up to the jamb, no wedge. |
| Row 111 jamb sliver 36400 (jlook-jamb-36400_threshold) | PASS (thumbnail read) | threshold carpet uniform up to the jamb. |
| Lit strip on the siding, family E (jlook-facade-bay-east_night) | PASS (read) | pose frames the pocket east of the bay; siding even, window light only, no lit strip. Row stays open for the AFTER (follow-up set). |
| Door 20755 threshold, GI 0.59 vs 0.60 bar (door20755_a-near, b-near, open) | AS EXPECTED (known bar) | no dark strip inside the jamb on either side. |
| Child's bedroom 2 dark outliers 45/125 (child-s-bedroom-spot2_floor, wall-xn) | AS EXPECTED (known; no visible dark spots) | floor and wall read evenly lit; the 45 probe outliers are not visible in these frames. |
| Chandelier graze, no cube seam (jlook-chandelier-graze_face, _oblique) | PASS | south wall and ceiling smooth round chandelier 35586, no vertical or horizontal seam. |
| Thin caster (jlook-thin-caster_chandelier, living chandelier 35592) | PASS (inconclusive) | ceiling smooth, no stair-step; the arm shadow is too faint to judge. |
| Jacob #2 hall -> bathroom doorway WALL blotch | NOT VERIFIED at the named pose | jlook-bathdoor-wall_face framed another wall; the 4-pose retake (optionsathdoor_retake, run 18:19 before the GPU hold) stood inside the bathroom. Side evidence only: door20762_b-near / b-far (hall side of the doorway) show the wall on both sides smooth, no blotch. The sheet keeps "not verified in the sweep" per the SM. |
| Walk pops by name (walkpop-*_mid, walkwp-*_pose; WALK_POPS.md) | NOT READ | 33 shots in sheets_open; not judged. |
| Dark rooms read OK | not read in full | only the named shots above. |
| Doors both sides: no dark strip / no leak line | partial | 20756 a-near and 20755 a/b near read OK; the other door shots not read. |
| Dark holes near furniture | not seen in the shots read | |

## Scan summary (candidate list, not a verdict)
Open: 4718 edges in 819 shots (NEW vs the 99579b4e baseline 2063, STRONGER 118, same 1725, no baseline 812). Shut: 805 in 148 shots. Almost all STRONG hits (step up to 73 %) are geometry: door frames, cabinet and wall corners, window frames, GI-view edges at room boundaries (e.g. 220_door20758_b-near_gi, 03_dining-bay_wall-xn_gi). "NEW" is inflated because the tours were regenerated (poses and framing shift a little). Use the edge list only to point at shots, then judge by eye.

## Option stills (look sheet)
- tableB 6c4b9d89: no visible change at 10 poses; DROPPED from the options per the PM rule, KNOWN line: table lamp line (Lamp_table_B, z 26.07) not visible with all lights on; no action (upstairs master bedroom, GI master bedroom 1). Atlas's ceiling-corner poses (sweep_tour_opt_tableb_corner.json) were not shot: no GPU.
- 35613 south 525e3a75: clear change, left wall darker and the corner step 125 -> 117 becomes 114 -> 118.
- hall line d26b5d3c: small change (0.35 % of frame, max 65/255), runtime preview, no re-bake.
- kitchen half wall: as-built only.
