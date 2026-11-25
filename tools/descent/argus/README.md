# Argus (W2 Renderer/GPU) WIP: specular / room-cube debug, 2026-11-24

Text only, copied from `D:\Jacob\tblocks\descent\gi-p2\argus\slot` (the team folder is not in git). The scripts hardcode the
desktop-gqko56v paths (`D:\Jacob\tblocks\descent\wt-gpu`, `locks\perf`, `...\slot\out`): edit those before running elsewhere.
Rule there: engine/GPU runs only under `locks\perf` with the SM's GO, in wt-gpu, one engine at a time.

## 1. Sink / mirror black (cause so far)
- Black mirrors and a black kitchen sink basin/faucet are the Metals material (M_Mirror forced `_Metallic 1.0` for the test).
- Not the cause: the `roomFace` gate (forcing roomFace = 1 is pixel-identical) and the spec-unscale (roomSpecW ~1, specIntensity ~1).
- The specular term is ~17x darker than the wall's although the probe dump (`probe1.ppm`) shows a lit room: the room-cube sample along the
  parallax-corrected direction (or its mip) is dark. [E] direction/box of probe 38101 (Probe Downstairs Bathroom) or the mip.
- Exposure is x3.5 (exposureEV +1.8), not x25. Mirrors stay matte until this is fixed.

## 2. Red-room (master bedroom) metal too shiny: hypothesis (Jacob look, build 657f87b8)
- Master bedroom (Red Bulb 35598) is at world ~(-10, 5.6, 23.1). Only House Probe Upstairs 35086 (captured cube, centre (0,4.6,26.4),
  box 29.4 x 2.8 x 11.4, importance 2) covers it: weight 1.0, ~10 m from the capture point.
- Commit 0b53893d ("a captured room cube's specular is not scaled by the sky's Ambient intensity", in 657f87b8) took the room-cube specular
  multiplier from 0.08 to 1.0 (x12.5). Master metals (M_Bed, M_Bedroom_Closets, M_NightTable, M_Lamps_House: Metallic 1 x mask, roughness map)
  now reflect the unclamped HDR upstairs cube (red bulb disc, red-lit walls; capture prefilter radiance clamp 0, IblProbe.cpp:395 [E]) with no
  specular occlusion = glow / wrong colour. Before the commit it was x0.08 (near black), so it was never seen.
- Alternative (b): the red lamps' direct specular (35611, 35598, 36370): unchanged by that commit, less likely.

## 3. Proposed fix: ONE fix for both symptoms, no global scale
A global specular scale cannot fix both (sink needs ~17x more, red room less). Act on the cube content and sampling:
1. clamp the captured room cube's radiance at prefilter/sample time (~4-8 HDR) so the bulb disc does not glare;
2. specular occlusion on room-cube spec (ao x NdotV x roughness, Lagarde);
3. optional per-probe `Specular Scale` field (default 1) as the escape hatch;
4. plus whatever batch #2 finds for the sink's dark sample (direction / box / mip).

## 4. GPU A/B for the red room (one batch, ~6 min, after the SM's GO; not yet run)
Add 4 master-room shots to `tour_dbg.json` (bed frame, closet handles, Lamp_stand 35611, nightstand), then per run `run_dbg.ps1 -Tag ...`:
1. baseline 657f87b8
2. roomSpecW forced 0 (pre-commit x0.08): confirms (a)
3. direct specular off on 35598 / 35611 / 36370: tests (b)
4. cube clamp 4
5. cube clamp 4 + specular occlusion
Reading: if 2 is calm and 3 unchanged -> (a); take the lightest of 4/5 that calms it without re-blackening the sink.

## 5. How to run
- `run_dbg.ps1 -Tag <t> [-Snippet <glsl after the ambient line>] [-From <exact text> -To <replacement>]`: one run of `tour_dbg.json`
  (mirror head-on, kitchen sink basin, front facade). Edits the BUILD copy of `ModelFragment.glsl` and `M_Mirror.mat` and restores both in `finally`.
- `run_dbg_batch.ps1`: mirror/sink debug batch #2, 5 runs (~5 min): prefiltered sample, specW, parallax off, mip 0, cube along N.
  Compare runs, not absolute values (exposure multiplies everything). Output: `slot\out\dbg_<tag>\png`.
- `run_moon_ab2.ps1`: moon in-scatter cap A/B #2, caps 0 / 0.15 / 0.3 / 0.5 (~2.5 min). The first A/B (caps 0-3) was invalid: the cap starts at
  cap/2 in HDR units and the wedge is only ~0.3 HDR before the +1.8 EV exposure.
- `run_w2_slot.ps1`: the earlier verify slot (guard bakes, main tour with mirror metallic, moon A/B v1). `make_tours.py` builds `tour_*.json`.

## 6. Status 2026-11-24 22:50 UTC: the runtime fix (branch dev/argus-spec-fix, off main db92576f)
Shader-only change in `src/Renderer/shaders/ModelFragment.glsl` (NOT compiled/run yet: GLSL compiles at the first engine run), all on the room-cube
share only (roomSpecW), no global scale, no recapture, independent of the GI bake:
1. `kRoomSpecClamp = 4.0`: the sampled room-cube radiance is capped per pixel (max channel), colour kept (red-room glow);
2. `kRoomSpecFloor = 0.4`: the room-cube specular is never darker than 0.4 x the local diffuse radiance (`irradiance`, GI-aware) (sink / basin black);
3. Lagarde specular occlusion from the material AO on the room-cube specular.
Per-probe `Specular Scale` (C++ + registry + uniform) NOT done: not needed unless the A/B says the constants cannot be one value.
A/B: `run_fix_ab.ps1` (variants base / fix / c2 / c8 / f2 / f6 / nospec) over `tour_redroom_sink.json` (4 red-room poses + the kitchen sink basin).
Poses are world positions (entity ids were renumbered on main; positions are not). The sink basin shot depends on kitchen GI only through `irradiance`.

## 7. A/B result 2026-11-24 23:00 UTC (wt-gpu at main db92576f, 7 variants x 5 shots; stills in gi-p2\argus\fix_ab, not in git)
- Sink (mean luma): basin 8.6 -> 10.1 (floor 0.6: 13.3), dishwasher front 34.7 -> 43.0 (0.6: 49.5), faucet 53.1 -> 54.2. Modest lift; floor 0.6 is better than 0.4.
- Red room: base = fix = clamp 2 = clamp 8 (identical means, 0% pixels > 240): the clamp does nothing in 4 poses. nospec (x0.08) differs by ~1% red / ~20% G,B, no visible change.
  => the spec-unscale / room cube is NOT the cause of Jacob's 'red-room metal too shiny' in these poses; hypothesis (a) NOT confirmed. Suspect (b): the red lamps'
  DIRECT specular (Red Bulb, Lamp_stand, nightstand lamps) on gloss surfaces (nightstand top, desk edge). Not yet A/B'd (needs: lamps' specular off, same tour).
- Next: ask Jacob which object; direct-specular A/B in a later GPU slot. PR #1217 stays draft: do not merge for the red room.

## 8. Final 2026-11-24 ~23:45 UTC (end of Argus session)
- Red-room metals: pole + lamp base = near-black metal (ambient spec ~0) with hard streaks = the lamps' DIRECT specular (not fixed; material/direct-spec design call).
  The armchair rail was grey because the room reflects the ONE big House Probe Upstairs cube. FIX SHIPPED in #1218: 'Probe Master Bedroom' (id 36200, order 268794, importance 0.5,
  Capture Scene, centre -9.95,4.6,23.1, size 6.4x2.8x5.9) via tools/descent/fix4/master_probe.py (branch dev/argus-master-probe 9f0b1544; applied on dev/fix4-house d9403a31).
  Rail (38,31,26) -> (48,13,10); no visible tint elsewhere. Importance 3 leaks red into the ground-floor kitchen: probe choice = importance/(1+d^2) of the TOP 2, NOT box containment
  (ReflectionProbeArray::Select). A real containment weight would be a C++ change (not done): worth doing before adding more small probes.
- This branch (dev/argus-spec-fix, PR #1217 DRAFT/HELD): room-cube clamp 4 + floor 0.4 + spec AO. Clamp changes nothing visible; sink lift small (basin 8.6 -> 10.1, floor 0.6: 13.3). Do not merge for the red room.
  If a 'sink lift' is wanted: floor 0.6, after Jacob's OK.
- Scripts: run_fix_ab.ps1, run_red_ab.ps1, run_probe_ab.ps1 (probe variants), make_sheet.py, add_master_probe.py, tours tour_redroom_*.json / tour_probe_leak.json (world positions).
