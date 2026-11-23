# GI bounce tune-down: plan (Phase 1 plan, applied in Phase 3)

Jacob's call (Nov 23): the fresh bake's brighter bounce comes **back down toward today's look**, and the by-design dark
rooms (box room, closet, red master bedroom 3) **stay dark**. This is a plan only: no code, no scene edit, no rebake in
Phase 1. It is applied in Phase 3 after the Phase 2 bake fixes and the rebake, and checked against the Phase 0 pictures.

## What "today's look" is

Argus's Phase 0 tour (`gi-p0/argus/before_gi.png`, `before_full.png`) was taken with the committed `Forest.gi`
(the Nov 14 bake, 378a1e74). So the target is **that bake's brightness, per volume**. Kronos's fresh bake of the same
scene is brighter in every lit room except the stairs: DC ratio fresh / Nov 14 is **median 1.64x (1.72x over whole
volumes), min 0.99x, max 2.20x** over the 35 lit layers, up to 3.0x on dim layers. The cause (Phase 2) is attributed
separately; this plan only decides how the brightness is brought back.

## Mechanism: how the bounce is scaled

The GI Volume already has a runtime field, `Intensity` (`GIVolume::Params.x`, `GIVolumes.cpp:291`). It is applied **after**
the atlas lookup: `GIVolumes.glsl:67`, `sum += e * Params.x * f`. It is not baked into the .gi. All 46 volumes in
`Forest.json` currently have `Intensity` 2.0 (Atlas, `gi_audit_volumes.csv`).

| | A. Per-volume Intensity (existing field) | B. One scene-level GI intensity (new) |
|---|---|---|
| Code | none | new scene field + uniform + shader multiply (renderer, scene serializer, editor UI, tests) |
| Rebake needed | no, runtime value | no, runtime value |
| Matches the measured spread | yes: ratios run 1.00x (stairs) to 2.7x (kitchen-dining 10, master bedroom 3) | no: one number cannot fix both ends |
| With the median (1/1.64) | n/a | the stairs, which did not brighten, drop ~39% too dark; kitchen-dining 4/5/10 and master bedroom 3 stay 1.5x too bright |
| By-design dark rooms | each returns to its Nov 14 level, so they stay as dark as today | scaled down together with the lit rooms, but they were 2.0-2.8x brighter, so they would stay brighter than today |
| Scene edit | 46 `Intensity` fields in Forest.json (use a minimal field edit and a diff proving only those lines changed; Forest.json has 91 `Intensity` lines, light and volume, so the script must target the GI Volume component) | 1 field |
| Re-tune after Phase 2 | edit values only | edit one value |
| Risk | per-volume values rest on bake data that has a 5-20% noise floor until cause 10 is fixed | none new, but it is a look knob, not a fix |

**Recommendation: A, per-volume Intensity, as the mechanism. No engine change.** It is the only option that returns each
room, including the dark ones, to its own old level. B's only advantage is one number; it is a code change and it cannot
express the room-by-room spread. If Jacob later wants a single overall dimmer for taste, B can be added then as a
multiplier on top of A (a small, separate renderer PR); it is not needed for the tune-down.

Side effect to accept: a volume's `Intensity` scales the whole mixed brick, which includes any Outside layer (the sun and
moon through windows) and any volume with `ReplacesSky`. The old bake had the same mix, so matching the old per-volume DC
matches that too.

## How the targets are set (Phase 3 procedure)

1. After the Phase 2 fixes, rebake once (Phase 3). Phase 3 changes the bake itself (doors open, the ceiling-row fill,
   coverage gaps, the 9-door bake), so **the ratios below must be re-measured on that bake**; they are a first estimate, not
   the values to commit.
2. With `gi-p0/kronos/gi_diff.py` (read-only, offline, no GPU): per volume, sum mean DC over the layers on at Play start
   for the Nov 14 `Forest.nov14.gi` and for the new bake. `ratio_v = new / old`.
3. `Intensity_v = 2.0 / ratio_v`, rounded to 2 decimals. Volumes where the old bake was defective (inner-probe fill over
   20%, cause 5; the storeroom, cause 9) have no meaningful old level: see "Do not match to Nov 14" below.
4. Write the 46 values with a minimal JSON field edit, diff to prove only those lines changed.
5. Check: `--gi-tour`, same waypoints as P0, full and GI-only. Per room, the mean GI-only luminance must land within the
   room's bake-noise tolerance (GI_VALIDATION.md, bake noise caveat) of `before_gi`, and the by-design dark rooms must read
   no brighter than before. Adjust and repeat from step 4 (a value edit, no rebake).

## First estimate from Phase 0 data (indicative only)

Per volume, all layers summed (`gi_diff_nov14_vs_run1.csv`, bake run 1 vs the Nov 14 bake). `Intensity = 2.0 / ratio`. Noise
floor 5-20% per room, so treat ±0.15 as the resolution.

| Volume | Old DC | New DC | Ratio | Intensity |
|---|---|---|---|---|
| GI box room (dark by design) | 0.00001 | 0.00002 | 2.00 | 1.00 |
| GI child's bedroom 1 | 0.00630 | 0.00969 | 1.54 | 1.30 |
| GI child's bedroom 2 | 0.00210 | 0.00360 | 1.71 | 1.17 |
| GI closet room (dark by design) | 0.00017 | 0.00035 | 2.06 | 0.97 |
| GI downstairs bathroom 1 | 0.00341 | 0.00548 | 1.61 | 1.24 |
| GI downstairs bathroom 2 | 0.00106 | 0.00213 | 2.01 | 1.00 |
| GI entrance hall 1 | 0.00417 | 0.00731 | 1.75 | 1.14 |
| GI entrance hall 2 | 0.00350 | 0.00602 | 1.72 | 1.16 |
| GI entrance hall 3 | 0.00217 | 0.00366 | 1.69 | 1.19 |
| GI entrance hall 4 | 0.00076 | 0.00144 | 1.89 | 1.06 |
| GI entrance hall 5 | 0.00181 | 0.00326 | 1.80 | 1.11 |
| GI entrance hall 6 | 0.00161 | 0.00284 | 1.76 | 1.13 |
| GI kitchen-dining 1 | 0.01252 | 0.02002 | 1.60 | 1.25 |
| GI kitchen-dining 2 | 0.00333 | 0.00693 | 2.08 | 0.96 |
| GI kitchen-dining 3 | 0.00874 | 0.01591 | 1.82 | 1.10 |
| GI kitchen-dining 4 | 0.00098 | 0.00248 | 2.53 | 0.79 |
| GI kitchen-dining 5 | 0.00024 | 0.00056 | 2.33 | 0.86 |
| GI kitchen-dining 6 | 0.00008 | 0.00014 | 1.75 | 1.14 |
| GI kitchen-dining 7 | 0.00814 | 0.01465 | 1.80 | 1.11 |
| GI kitchen-dining 8 | 0.00003 | 0.00007 | 2.33 | 0.86 |
| GI kitchen-dining 9 | 0.00010 | 0.00017 | 1.70 | 1.18 |
| GI kitchen-dining 10 | 0.00023 | 0.00060 | 2.61 | 0.77 |
| GI landing | 0.00502 | 0.00586 | 1.17 | 1.71 |
| GI living room 1 | 0.00897 | 0.01401 | 1.56 | 1.28 |
| GI living room 2 | 0.00323 | 0.00627 | 1.94 | 1.03 |
| GI living room 3 | 0.00120 | 0.00262 | 2.18 | 0.92 |
| GI living room 4 | 0.01538 | 0.02310 | 1.50 | 1.33 |
| GI living room 5 | 0.00104 | 0.00220 | 2.12 | 0.95 |
| GI living room 6 | 0.00041 | 0.00102 | 2.49 | 0.80 |
| GI living room 7 | 0.00027 | 0.00064 | 2.37 | 0.84 |
| GI living room 8 | 0.00072 | 0.00168 | 2.33 | 0.86 |
| GI living room 9 | 0.00034 | 0.00079 | 2.32 | 0.86 |
| GI living room 10 | 0.00813 | 0.01194 | 1.47 | 1.36 |
| GI master bedroom 1 | 0.00469 | 0.00767 | 1.64 | 1.22 |
| GI master bedroom 2 | 0.00096 | 0.00211 | 2.20 | 0.91 |
| GI master bedroom 3 (red bulb, dark by design) | 0.00023 | 0.00063 | 2.74 | 0.73 |
| GI stair storeroom (defect, see below) | 0.00001 | 0.00002 | 2.00 | n/a |
| GI stairwell | 0.00371 | 0.00372 | 1.00 | 1.99 |
| GI upstairs bathroom 1 | 0.00395 | 0.00735 | 1.86 | 1.07 |
| GI upstairs bathroom 2 | 0.00303 | 0.00559 | 1.84 | 1.08 |
| GI upstairs bathroom 3 | 0.00855 | 0.01309 | 1.53 | 1.31 |
| GI upstairs bathroom 4 | 0.00765 | 0.01217 | 1.59 | 1.26 |
| GI upstairs bathroom 5 | 0.00345 | 0.00629 | 1.82 | 1.10 |
| GI upstairs den 1 | 0.00382 | 0.00682 | 1.79 | 1.12 |
| GI upstairs den 2 | 0.00543 | 0.00840 | 1.55 | 1.29 |
| GI upstairs hall | 0.00524 | 0.00524 | 1.00 | 2.00 |

Read it as: lit rooms land around 1.0-1.35; the stairs and hall stay at 2.0 (unchanged); the dark rooms land at 0.7-1.0.
Volumes under 0.001 old DC (the dim tail, most of kitchen-dining 4-10, living room 5-9, the box room, closet, master
bedroom 3) have noisy ratios (up to 20% run to run, and absolute values of 1e-4 or less): for those, **round to the room's
nearby lit volumes' value** rather than trust the single ratio, unless the tour shows the room off.

## Do not match to Nov 14

* **Defective old bake** (cause 5, 9): living room 7, kitchen-dining 8 (100% of inner probes invalid), the stair
  storeroom, the box room (94%), kitchen-dining 6, the closet room. The Nov 14 level there is an artefact (empty baked GI;
  the storeroom's bounce is ~1e-5 because its door was baked shut), not a look to preserve. After Phase 2 these get a
  value set by eye against their neighbours, not by ratio. The stair storeroom is a defect and must pass the dark-spot
  rule; it is not on the dark-room list.
* **Kitchen doorway** (Atlas's Phase 1 range fix) and anything else Phase 1 changes the direct light on: the tour
  comparison is full + GI-only, but Intensity only touches the GI-only part.

## Dark rooms: kept dark

By-design dark rooms (box room, closet, master bedroom 3, per Jacob and GI_VALIDATION.md) are tuned by the same rule, so
each returns to its Nov 14 level rather than to a "fixed" brightness. The check is a **ceiling**, not a floor: after the
tune-down each must read **no brighter than its before_gi value (within the room's bake-noise tolerance)**, and none is
brightened to meet the dark-spot floor. Whether a deliberately dark room is acceptable at all stays Jacob's call; it is
unchanged by this plan.

## Order of work and dependencies

1. Phase 1 (now): this plan; the 20 lamp shadows (Atlas), shadow slots (Argus), bake repeatability (Kronos), the SSAO-off
   renderer bug (Talos). None of these changes the look of the bounce. **The bake-repeatability fix (cause 10) lowers the
   5-20% noise floor on the ratios, which is the biggest improvement to step 2 above.**
2. Phase 2: bake fixes (doors open, ceiling-row fill, coverage gaps with 0.3-0.5 m fades, the tree emissive decision).
   Attribution of the 1.64x brightening. If a Phase 2 attribution **removes** part of the brightening at the source (for
   example the lamp-shade emissives, 05fd1801), the ratios in step 2 shrink and Intensity stays closer to 2.0. This is why
   Phase 3 measures on the final bake instead of committing the table above.
3. Phase 3: rebake, measure, write the 46 values, run the tour, check against the P0 pictures, then GI_VALIDATION checks.

## Open questions for Jacob / the SM

* Is the A recommendation (per-volume, no engine change) OK, or does Jacob want the single scene-level dimmer as well?
* Rooms where the new bake is *better* than Nov 14 (dim places the old bake left black: the kitchen-dining corner,
  the chandelier ring) are not a goal to dim back. The ratio method dims a whole volume. Per-room decision after Phase 2
  if a volume contains both a fixed dark spot and an over-bright area.
* Is "toward today's look" a match within the bake noise tolerance (proposed) or a fixed percentage?
