# House GI validation (the Phase 3 AAA checks)

The pass/fail spec for the Forest house lighting. Phase 0 measures the current state with these checks (nothing changes
look); Phase 3 runs them again after the rebake and every check must pass. Same camera path, same settings, so before and
after are comparable.

**Status of the numbers.** Every threshold marked **PROPOSED** is a starting value from the plan, to be firmed up from the
Phase 0 baseline (W2 Argus's `gi-p0/argus/REPORT.md`). Once the baseline lands, each PROPOSED value is replaced by a
measured one and the line says where it came from. Nothing here is a pass/fail gate until it has no PROPOSED tag.

## Setup (identical for baseline and after)

* Scene: `project/scenes/Forest.json`, Play lighting state, every switch at its Start On value, flashlight off, no NPCs.
* Tool: `--gi-tour <scene.json> --out <dir>` with the waypoint file `tools/gi/forest_house_tour.json`
  (stationary captures plus the continuous "Jacob routes").
* Resolution `--perf-res 1920x1080`, fixed time of day, hidden window (`TARTARUS_HIDDEN=1`).
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
| Shadow-slot set changes inside one room (lights and switches unchanged) | 0. Changes are allowed only while crossing a doorway, and then only as a fade, never a one-frame step |
| A single-frame step larger than the frame-to-frame max while no slot hash changed | 0 (that would be a different cause, such as SSAO or GI sampling) |

The baseline run also reports how many segments exceed these numbers and whether a slot change coincides with each
exceedance (Argus's "shimmer" table). Phase 3 must show zero exceedances.

## 2. Dark-spot floor per lit room

**What it tests.** Cause 3, 5 and 6: stale bake, probe leaks and dark patches, coverage gaps.

**Measure.** For each room that has a working lamp on, with that room's circuit on: luminance of the GI-only view
(debugView 8) and of the full-lighting view at every walkable sample point (0.25 m grid, eye height and floor height, from
Atlas's coverage script). Compare each point with that room's median.

**Pass.**
| Metric | Threshold |
|---|---|
| Walkable points in a lit room below the floor `0.35 x room median` (full lighting) | 0% (**PROPOSED**) |
| Same, GI-only view, below `0.25 x room median` | <= 1% of the room's points, none larger than a 0.5 m cluster (**PROPOSED**) |
| Walkable points inside no GI volume (fall back to reflection probe or sky ambient) | 0 |
| Invalid-probe share (neighbour-averaged probes) per volume | <= 20% (Atlas's flag threshold) |
| Absolute floor: no lit room's median below | to be set from the baseline; the plan only says "a luminance floor" (**OPEN**) |

Rooms with their lamps off are exempt from the first two rows; they are covered by the switch check.

## 3. Doorway seam continuity

**What it tests.** Cause 6: a 0.05 m volume fade makes a doorway close to a hard switch between two lighting models.

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

**Method.** One GPU job at a time, under `C:\Users\Jacob\tblocks\descent\locks\perf`, on a quiet machine (Jacob not in
the editor, no other builds). Timings taken at below-normal priority or while Jacob is using the PC are **not** perf
numbers. Three timed `--perf-bench` runs per scene, same settings; report all three plus the median.

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
* One walk-through clip along a "Jacob route", before and after.
* These go to Jacob for Phase 4 look sign-off; they are not pass/fail by themselves.

## Result report format

One table per check: room or doorway, measured value, threshold, PASS/FAIL, picture link. A top line says PASS only if
every row passes. Phase 0 produces the same tables with the current values (all expected to fail where the plan's causes
are real), which is the baseline for each after-pair.

## Open items

* Replace each PROPOSED value with a baseline-derived one once Argus's `REPORT.md` is in; note the source run beside it.
* Pick the absolute dark floor per room (check 2, last row).
* Confirm with Atlas that "walkable floor" for the coverage grid matches the tour's rooms.
* Decide whether the frame-to-frame limit should be per route or global once the shimmer data shows its spread.
