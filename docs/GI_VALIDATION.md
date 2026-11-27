# House GI validation (the Phase 3 AAA checks)

The pass/fail spec for the Forest house lighting. Phase 0 measures the current state with these checks (nothing changes
look); Phase 3 runs them again after the rebake and every check must pass. Same camera path, same settings, so before and
after are comparable.

**Status of the numbers.** Every threshold marked **PROPOSED** is a starting value from the plan, to be firmed up from the
Phase 0 baseline. The baseline tour has landed and the checks below now cite
it ("Baseline:" lines); a value stays PROPOSED until the follow-up GI-only / direct-only walks and the clipping
waypoint fixes are in. Nothing here is a pass/fail gate until it has no PROPOSED tag.

**By-design dark rooms.** A room whose lights are dead by design (the Nov 18 "dead fittings stay dark" change) is not a
dark-spot failure. The exclusion list is kept in the report, with the reason per room, and must be confirmed (by design,
not a defect) before a room goes on it. Confirmed by Phase 0: the **box room** (no fitting, door shut, no
window light), the **closet** (no lamp of its own, bounce only) and **master bedroom 3** (lit only by the red bulb, mean
baked 0.00033; runtime matches the bake). Whether a deliberately dark room is acceptable is a look decision. The stair storeroom is NOT on the list: it is a defect
(its door is baked shut but starts open in Play), and must pass the dark-spot rule after the fix.

**Bake noise caveat.** The Phase 0 run found the bake is not deterministic: three bakes of the same scene, binary and
settings differ by 2.8-6.6% mean per layer and up to 24% on some layers (worst: Downstairs bathroom, Lamp_table_A 15,
Outside, Living room). Until that is fixed, any before/after GI comparison has a 5-20% per-room noise floor. Rules that
compare two bakes (the before/after pairs and the switch check's on/off levels) use that tolerance per room, taken from
the room's measured spread across repeated bakes; rules that measure one bake against fixed numbers (walk stability, seams,
dark spots) are not affected.

## Setup (identical for baseline and after)

* Scene: `project/scenes/Forest.json`, Play lighting state, every switch at its Start On value, flashlight off, no NPCs.
* Tool: `--gi-tour <scene.json> --out <dir>` with the waypoint file `tools/gi/forest_house_tour.json`
  (stationary captures plus the continuous walk routes).
* Resolution `--perf-res 1920x1080`, fixed time of day, hidden window (the default for harness runs; `TARTARUS_SHOW=1` shows it).
* Luminance = mean luminance of the tone-mapped frame over the **static-surface mask** (depth-tested static geometry only;
  no player body, particles or sky), as the tour tool writes it. Units: relative luminance 0-1 of the final sRGB image, so
  thresholds are a fraction of full scale.
* Three pictures per waypoint: full lighting, GI only (debugView 8), and the shadow-slot log line.
* A check passes only if **every** room/doorway/circuit it covers passes. One failure fails the check; the report lists
  the failing rooms.

## 1. Walk stability

**What it tests.** Cause 1 and 2 of the plan (shadow slots hopping between lamps, camera-distance light fade) and any
view-dependent GI. With lights and switches unchanged, moving the camera must not change how a static surface is lit.

**Measure.**
* *Frame-to-frame:* along every continuous route segment, `|L[f] - L[f-1]|` of the static-surface luminance, per frame.
* *Arrival vs stationary:* at each waypoint, luminance when arriving along the route versus the same camera held still for
  2 s. Relative difference `|L_walk - L_still| / max(L_still, 0.02)`.
* *Slot churn:* number of frames in a route segment where the shadow-slot set hash changes.

**Pass.**
| Metric | Threshold |
|---|---|
| Frame-to-frame luminance change, 99.9th percentile over a route | <= 0.5% of the route's median luminance (**PROPOSED**) |
| Frame-to-frame luminance change, max | <= 2% of the route's median luminance (**PROPOSED**) |
| Arrival vs stationary, every waypoint | <= 2% relative (**PROPOSED**) |
| Lamps with Shadow on, in range of the camera, left without a shadow slot, at any waypoint or frame | 0 (**baseline: 0 at all 66 waypoints**, so this is a no-regression gate) |
| Shadow-slot set changes (any lamp) | allowed; they are not a failure by themselves (**baseline: 33 changes, none on a pop**). They fail only when they coincide with a pop |
| Pops: a frame step above max(0.004, 3%) AND above 4x the segment's median step (the baseline tour's definition) | Phase 3: 0 pops within 1 m of a door_in point; at most 5 in all route segments, none above 10% (**PROPOSED**). **Baseline: 31 pops in 14 of 97 segments, biggest +94%, 7 within 1 m of a door** |
| Stationary holds: luminance range within a 2 s hold | <= 0.0005 absolute (**baseline: worst hold 0.00023**) |

The follow-up (GI-only and direct-only walks) says which pops at doors are GI seams and which are view change; the
door-pop row is tightened or relaxed from that. Near-black frames are excluded from the pop count only where the depth
check shows the camera inside geometry; those waypoints are moved before the tour is used as the Phase 3 acceptance path.
(Phase 0 found most of the 12 black waypoints are genuinely unlit spaces, door gaps or the shut-door bake, not tour faults.)

## 2. Dark-spot floor per lit room

**What it tests.** Cause 3, 5 and 6: stale bake, probe leaks and dark patches, coverage gaps.

**Measure.** For each room that has a working lamp on, with that room's circuit on: luminance of the GI-only view
(debugView 8) and of the full-lighting view at every walkable sample point (0.25 m grid, eye height and floor height, from
the coverage script). Compare each point with that room's median.

**Pass.**
| Metric | Threshold |
|---|---|
| Walkable points in a lit room below the floor `0.35 x room median` (full lighting) | 0% (**PROPOSED**) |
| Same, GI-only view, below `0.25 x room median` | <= 1% of the room's points, none larger than a 0.5 m cluster (**PROPOSED**) |
| Walkable points inside no GI volume (fall back to reflection probe or sky ambient) | 0 |
| Invalid-probe share (neighbour-averaged probes) per volume | <= 20% (the audit's flag threshold) |
| Absolute floor: no valid walkable view below 0.005 mean linear luminance, circuit on (**PROPOSED**; baseline darkest valid views 0.0007-0.0028, a room mean range 0.006-0.064) |
| In-room spread, brightest valid view / darkest valid view | <= 6x (**PROPOSED**; baseline 38.4x kitchen-dining, 12.5x upstairs den, 9.3x upstairs bathroom) |

Rooms with their lamps off are exempt from the dark-spot rows; they are covered by the switch check. So are **by-design
dark rooms** (see the top of this document): they are listed with the reason, and a room that reads black but is not on
that list counts as a failure. A black frame is a dark-spot failure unless the tour's depth check shows the camera is inside
geometry, in which case it is a tour fault and the waypoint is moved.

## 3. Doorway seam continuity

**What it tests.** Cause 6: a 0.05 m volume fade makes a doorway close to a hard switch between two lighting models.

**Measure it on the GI-only view (debugView 8) as well as the full image.** Phase 0 found the seams are real but hidden:
at the child's bedroom door the GI-only luminance swung +244% / -70% / +180% over about 40 frames while the full image
moved under 7%, because direct light dominates today. The check must read the GI layer on its own, and the same walk with
GI off (direct only) separates a seam from a view change.

| GI-only pops, Phase 3 | 0 within 1 m of a door_in point (**baseline: 17 in the GI-only walk**, at the child's bedroom door, the box-room door, the stairs top and the landing) |
|---|---|

**Measure.** For every doorway, sample the static-surface luminance at 0.1 m steps along a straight line through the
opening, from 1.0 m before to 1.0 m after, at eye height and at floor height. This is also captured as the tour's
"just before, in, just after" waypoints. A true brightness difference between the two rooms is fine; a step is not.

**Pass.**
| Metric | Threshold |
|---|---|
| Largest luminance change between two neighbouring samples (0.1 m apart) in the doorway zone, relative to the two room medians' difference | <= 25% of that difference, and never > 3% of full scale (**PROPOSED**) |
| Fade width actually in effect where volumes meet or overlap | >= 0.3 m (the plan's Phase 2 target is 0.3-0.5 m) |
| Lighting-model switches (GI to fallback or back) inside the doorway zone | 0 |

## 4. Switch on/off match

**What it tests.** Each circuit's baked layer still matches its room: the bake is not stale or mislabelled.

**Measure.** For each circuit, capture the room's waypoints with the circuit on, then off (other circuits unchanged), in
both full lighting and GI-only.

**Pass.**
| Metric | Threshold |
|---|---|
| Circuit off: lit-room luminance falls to within 15% of the all-off ambient level of that room | **PROPOSED** |
| Circuit on: luminance rises by >= 3x the all-off level, or the lamp visibly lights the room (reviewed by picture) | **PROPOSED** |
| Circuit on or off changes luminance in a room that circuit does not serve by more than 2% (leak) | 0 rooms (**PROPOSED**) |
| Circuits whose bake layer is missing, renamed or sized differently from the scene | 0 |
| Circuits with more shadowed lamps than the bake budget (4 point / 8 spot) | 0, or the baker proves it hops nothing (cause 4) |

## 5. Performance

**What it tests.** The fixes must not cost frame rate. Forest is CPU-bound at about 66 fps today against its goal.

**Method.** One GPU job at a time, on a quiet machine (no editor open, no other builds). Timings taken at below-normal
priority or while the machine is in use are **not** perf numbers. Three timed `--perf-bench` runs per scene, same settings; report all three plus the median.

**Pass.**
| Scene | Target | Rule |
|---|---|---|
| Forest | 90 fps (11.1 ms) | median of 3 not worse than the Phase 0 baseline median by more than 3%; goal 90 fps |
| Apartment Hallway | 120 fps (8.3 ms) | median of 3 not worse than its baseline median by more than 3% |
| Spread | | max-min of the 3 runs <= 5% of the median, else the machine was not quiet: rerun |
| GI-specific | | GI pass GPU time and shadow-slot cost reported separately, before and after |

The Hallway has no GI volumes, so its run only proves the renderer changes do not regress it.

## 6. Pictures and clip

* Before and after PNG pairs per room (full lighting and GI-only) from the same waypoint camera.
* One walk-through clip along a walk route, before and after.
* These go to the Phase 4 look sign-off; they are not pass/fail by themselves.

## Result report format

One table per check: room or doorway, measured value, threshold, PASS/FAIL, picture link. A top line says PASS only if
every row passes. Phase 0 produces the same tables with the current values (all expected to fail where the plan's causes
are real), which is the baseline for each after-pair.

## Phase 1 changes that affect these checks

Phase 1 (Nov 23) changes the direct light and the tooling, not the bake. Rollup rows are added as each PR lands.

* **Bounce: keep the new bake, trim rooms on request (Nov 23).** The blanket tune-down to the Nov 14 levels is cancelled:
  H1 showed the committed Nov 14 `.gi` was stale for the room circuits, so the fresh bake is the correct brightness.
  In Phase 3, rooms to change are marked on the sign-off sheet (per-room "darker / OK / lighter"); only those get a
  per-volume `Intensity`; see `docs/GI_BOUNCE_TUNE_PLAN.md`. Effect on checks: **no brightness ceiling** for the by-design dark
  rooms unless they are marked; the before/after pairs for unmarked rooms must show no GI change from Phase 3's trim.
* **20 lamps get Cast Shadows** (scene PR), **room-aware shadow slots**, **repeatable bake**,
  **SSAO-off GL bug**.

### Phase 1 rollup (status at 2026-11-25, main d627b2c6)

| PR | Change | Evidence | Effect on the PROPOSED thresholds |
|---|---|---|---|
| #1187, #1191, #1195 (merged) | Bounce plan; then the decision "Keep it, trim rooms" after H1: no blanket tune-down, per-room trims on request | H1 (Nov 14 `.gi` stale; Hall/Master bedroom main/Nov14-code 1.0-1.05x, vs committed 1.6x; lamp circuits +1.68x from a later code change, likely 15930c7d, to confirm) | No dark-room ceiling; trims only where a room is marked. |
| #1192 (merged with #1189) | Cast Shadows ON for the 20 shadowless house lamps (Forest.json, exactly 20 flips; lamp list matches the P0 audit; second review PASS) | Full-frame tour pairs change most in kitchen north, kitchen main, dining, living east bay, hall, landing. The 22 fps seen first did not reproduce (busy machine): see #1189 | Check 1/2 expect fewer through-wall leaks. |
| #1189 (merged with #1192) | Room-aware, stable shadow slots that fade instead of popping; point-shadow ceiling 8 -> 12 | Tour pace 58 fps before and after (smoke fps); route sweep at point budgets 4 / 8 / 12: walk 24.17 / 24.14 / 24.35 ms, point shadow GPU 0.16 / 0.23 / 0.28 ms | Check 1 (walk stability): slot hand-overs fade, no pops. Check 5 (performance): met, the casters cost < 0.3 ms. |
| #1190 (merged, PARTIAL) | `--bake-gi` pins every clock a capture sees | Repeatability NOT achieved: the clocks were not the main noise; state carried over between runs is (open issue #1204) | The bake-noise caveat stays at the measured 2.8-6.6% until #1204 is fixed. |
| #1188 (merged) | SSAO-off sampler clash fix (impostor draws) | Smoke 22/22, 0 sampler clashes; SSAO A/B walk on 3 routes / 34 waypoints | Cause #8 only. |
| #1202 (merged; replaced #1193) | Kitchen vestibule lit by Ceiling Kitchen 35588 (Bounds only, 2 lines) | "After" evidence = the fresh-bake shots; Range 9.5 variant dropped unless the vestibule reads too dim at sign-off | Kitchen doorway rows (doors before/in/after). |
| #1200 (merged) | GPU-driven batches split on GI volumes and the full probe key | 5 launches + 1 no-MDI launch: 0 of 66 waypoints flip, hold means identical to 4 decimals | GI-only pairs are a valid bake-unchanged check again. |

**Finding (found in the #1192 review, Nov 23): runtime GI values were BISTABLE run to run.** The GI-only view of the SAME
inputs (same exe, same `.gi`, same lamp set; `slot_hash` equal on 66/66 waypoints) differed between runs: P0 vs the
before run differ by more than 2% at 29 of 66 waypoints (18 over 10%), the same size as before vs after #1192 (29 / 20), with
mirrored sign (correlation -0.76); the after run matches P0 at 61 of 66. Largest swings: landing corner +139%, kitchen
north -53%, master den door in -40%. P0 verdicts that rest on runtime GI (#6 GI-only pops, the dark-spot spreads) and the
two residual #1192 waypoints (stairs_top -66%, child door before +27%) are re-checked with a GI-only run on a build with
#1200; verdicts from `.gi` data and the offline audit stand. The kitchen-B tours are full-view only.
Detail: the #1192 review.

**Root cause of the bistability: FIXED by #1200.** The MDI batch key lacked the GIMask and truncated the
ProbeKey, so draw groups from different GI volumes merged, and `drawBatch` took the mask and probe from the lead group only.
The result was darker GI that depended on pointer order, so it differed per launch. The fix adds both fields to the key
(`OpaqueBatchKey.h` plus a unit test). Proof: 0 of 66 waypoints flip across 5 MDI launches and 1 no-MDI launch.

**Phase 1 exit criteria (additions):**
* **GI identical across 5 launches** of the same scene and bake: MET by #1200's proof run (re-run it after any change to
  draw batching or GI sampling).
* #1192 and #1189 merged together, with the fps sweep near the old pace: MET (58 fps before and after).
* Room trims are marked on the sign-off sheet before Phase 3 applies any `Intensity` change (default: no change). OPEN.
* **Repeatable bake:** OPEN. #1190 pinned the clocks, but two bakes still differ (state carried between runs, #1204). Target:
  byte-identical or under 0.5% per layer; until then the bake-noise caveat above stays at the measured spread.

## Open items

* Replace each PROPOSED value with a baseline-derived one once the baseline report is in; note the source run beside it.
* Pick the absolute dark floor per room (check 2, last row).
* Confirm that "walkable floor" for the coverage grid matches the tour's rooms.
* Decide whether the frame-to-frame limit should be per route or global once the shimmer data shows its spread.
