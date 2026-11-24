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
