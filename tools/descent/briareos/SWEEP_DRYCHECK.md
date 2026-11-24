# Sweep v2 dry check (CPU only): every surface

Scene `project/scenes/Forest.json`; eye 1.6 m. Two tours: **open** (`doorState allOpen`: rooms, thresholds, doors open, outside, stairs) and **shut** (`authored` = every door shut as Play starts them: doors, outside, each room first spot ceiling + floor).

* Run A: **439** shots (33 outside cameras); run B: **97** shots; total **536** (each gives a _full and a _gi PNG)
* GPU [E, per-shot rate extrapolated from a measured 12.4 ms/frame]: with `stillFrames 0` (needs the dev/tour-stillframes PR) run A ~14 min + run B ~4 min = **~18 min**; without it (272 frames/shot) 25 + 6 = **~31 min**
* Cameras with a problem: **0**

* Outside cameras (not inside a GI volume by design, flagged `outside:true`): 33; their floor is assumed = the door sill (0.194), not a terrain sample.

## Unresolved / skipped

* {"door": 20762, "side": "a", "dist": "far", "why": "no free spot at 0.7-3.6 m"}
* {"seam": "GI living room 1 | GI downstairs bathroom 2", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI living room 2 | GI living room 7", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI living room 2 | GI living room 8", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI living room 2 | GI downstairs bathroom 2", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI living room 3 | GI living room 7", "storey": "ground", "gap_m": -0.78, "why": "wall-only seam: no walkable point"}
* {"seam": "GI living room 4 | GI living room 10", "storey": "ground", "gap_m": -0.3, "why": "wall-only seam: no walkable point"}
* {"seam": "GI living room 6 | GI downstairs bathroom 1", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI living room 7 | GI stairwell", "storey": "ground", "gap_m": -0.13, "why": "wall-only seam: no walkable point"}
* {"seam": "GI living room 8 | GI downstairs bathroom 1", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI stair storeroom | GI kitchen-dining 2", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI stair storeroom | GI kitchen-dining 4", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI stair storeroom | GI kitchen-dining 5", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI stair storeroom | GI kitchen-dining 9", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI kitchen-dining 1 | GI kitchen-dining 2", "storey": "ground", "gap_m": -0.17, "why": "wall-only seam: no walkable point"}
* {"seam": "GI kitchen-dining 1 | GI entrance hall 2", "storey": "ground", "gap_m": -0.0, "why": "wall-only seam: no walkable point"}
* {"seam": "GI kitchen-dining 2 | GI entrance hall 2", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI kitchen-dining 6 | GI kitchen-dining 9", "storey": "ground", "gap_m": -0.58, "why": "wall-only seam: no walkable point"}
* {"seam": "GI kitchen-dining 6 | GI entrance hall 1", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI kitchen-dining 6 | GI entrance hall 2", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI downstairs bathroom 1 | GI downstairs bathroom 2", "storey": "ground", "gap_m": -0.14, "why": "wall-only seam: no walkable point"}
* {"seam": "GI downstairs bathroom 1 | GI entrance hall 1", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI entrance hall 1 | GI entrance hall 4", "storey": "ground", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI child's bedroom 1 | GI child's bedroom 2", "storey": "upstairs", "gap_m": -0.16, "why": "wall-only seam: no walkable point"}
* {"seam": "GI child's bedroom 2 | GI landing", "storey": "upstairs", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI upstairs den 1 | GI upstairs bathroom 2", "storey": "upstairs", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI upstairs den 2 | GI upstairs hall", "storey": "upstairs", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI upstairs den 2 | GI upstairs bathroom 1", "storey": "upstairs", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI upstairs den 2 | GI upstairs bathroom 2", "storey": "upstairs", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI master bedroom 1 | GI upstairs bathroom 1", "storey": "upstairs", "gap_m": -0.04, "why": "wall-only seam: no walkable point"}
* {"seam": "GI master bedroom 1 | GI upstairs bathroom 3", "storey": "upstairs", "gap_m": -0.04, "why": "wall-only seam: no walkable point"}
* {"seam": "GI master bedroom 1 | GI box room", "storey": "upstairs", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI master bedroom 3 | GI upstairs bathroom 1", "storey": "upstairs", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI landing | GI upstairs hall", "storey": "upstairs", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI landing | GI closet room", "storey": "upstairs", "gap_m": -0.14, "why": "wall-only seam: no walkable point"}
* {"seam": "GI upstairs hall | GI upstairs bathroom 4", "storey": "upstairs", "gap_m": -0.11, "why": "wall-only seam: no walkable point"}
* {"seam": "GI upstairs bathroom 2 | GI upstairs bathroom 4", "storey": "upstairs", "gap_m": -0.25, "why": "wall-only seam: no walkable point"}
* {"bounds_light": 35592, "face": "-x", "value": 4.437, "why": "no free in-volume spot 1-3.5 m back from the face"}
* {"jlook": "jlook-red-door20759_in-far", "why": "no free spot near (-10.3, 29.4)"}

## Closets / small rooms (<=16 m2 footprint or named closet/box/storeroom) and the doors reaching them

| room | area m2 | door ids | spots |
|---|---|---|---|
| box room | 19.9 | 20755 | box room |
| closet room | 9.7 | no door: open archway, seen via thresholds thr25-child-s-bedroom-closet-room | closet room |
| dining bay | 1.6 | no door: open archway, seen via thresholds thr09-kitchen-dining-dining-bay | dining bay |
| downstairs bathroom | 13.1 | 20762 | downstairs bathroom |
| stair storeroom | 14.5 | 20756 | stair storeroom |

## Doors (each: both sides x near 1.5 m / far 3 m, open + shut)

| id | name | at (x, z) | normal | sides |
|---|---|---|---|---|
| 20753 | Door_interior_mesh | -4.33, 27.92 | z | {"a": [{"dist": 1.5, "outside": false}, {"dist": 2.7, "outside": false}], "b": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}]} |
| 20754 | Door_exterior_white_mesh | 14.58, 25.98 | x | {"a": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}], "b": [{"dist": 1.5, "outside": true}, {"dist": 3.0, "outside": true}]} |
| 20755 | Door_interior_mesh | -11.12, 27.44 | x | {"a": [{"dist": 1.5, "outside": false}, {"dist": 2.7, "outside": false}], "b": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}]} |
| 20756 | Door_interior_mesh | -1.91, 30.34 | x | {"a": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}], "b": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}]} |
| 20757 | Door_interior_mesh | 4.39, 27.92 | z | {"a": [{"dist": 1.5, "outside": false}, {"dist": 2.7, "outside": false}], "b": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}]} |
| 20758 | Door_interior_mesh | -6.76, 26.47 | x | {"a": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}], "b": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}]} |
| 20759 | Door_interior_mesh | -6.76, 29.37 | x | {"a": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}], "b": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}]} |
| 20760 | Door_exterior_paneled_mesh | 1.00, 24.04 | z | {"a": [{"dist": 1.5, "outside": true}, {"dist": 3.0, "outside": true}], "b": [{"dist": 1.5, "outside": false}, {"dist": 2.4, "outside": false}]} |
| 20761 | Door_interior_mesh | 9.24, 28.40 | x | {"a": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}], "b": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}]} |
| 20762 | Door_interior_mesh | 6.34, 26.95 | z | {"a": [{"dist": 1.5, "outside": false}], "b": [{"dist": 1.5, "outside": false}, {"dist": 3.0, "outside": false}]} |

## Room spots

| room | storey | spot (x, z) | volume | clear m | long m |
|---|---|---|---|---|---|
| dining bay | ground | -9.30, 20.39 | GI dining bay | 0.61 | 2.3 |
| downstairs bathroom | ground | 6.19, 25.83 | GI downstairs bathroom 1 | 1.01 | 2.7 |
| entrance hall | ground | 1.44, 25.53 | GI entrance hall 1 | 1.30 | 10.9 |
| entrance hall-spot2 | ground | -4.86, 25.53 | GI entrance hall 2 | 1.30 | 10.9 |
| entrance hall-spot3 | ground | 4.04, 27.93 | GI entrance hall 4 | 0.84 | 10.9 |
| kitchen-dining | ground | -8.93, 27.03 | GI kitchen-dining 3 | 1.51 | 15.3 |
| kitchen-dining-spot2 | ground | -0.23, 28.63 | GI kitchen-dining 6 | 1.58 | 15.3 |
| kitchen-dining-spot3 | ground | -11.33, 21.63 | GI kitchen-dining 1 | 1.28 | 15.3 |
| kitchen-dining-spot4 | ground | -12.53, 26.93 | GI kitchen-dining 3 | 1.12 | 15.3 |
| kitchen-dining-spot5 | ground | -8.03, 23.63 | GI kitchen-dining 1 | 1.07 | 15.3 |
| kitchen-dining-spot6 | ground | -4.63, 27.83 | GI kitchen-dining 5 | 0.83 | 15.3 |
| living room | ground | 10.33, 27.52 | GI living room 1 | 1.54 | 9.8 |
| living room-spot2 | ground | 6.03, 29.42 | GI living room 3 | 1.50 | 9.8 |
| living room-spot3 | ground | 12.03, 22.32 | GI living room 4 | 0.98 | 9.8 |
| living room-spot4 | ground | 13.33, 25.62 | GI living room 1 | 1.06 | 9.8 |
| living room-spot5 | ground | 11.53, 30.82 | GI living room 1 | 0.78 | 9.8 |
| stair storeroom | ground | -3.77, 30.29 | GI stair storeroom | 1.30 | 3.9 |
| stairwell-low | ground | 2.80, 29.62 | GI stairwell | 1.57 | 5.6 |
| box room | upstairs | -12.73, 28.77 | GI box room | 1.51 | 4.9 |
| child's bedroom | upstairs | 10.97, 27.63 | GI child's bedroom 1 | 1.52 | 5.4 |
| child's bedroom-spot2 | upstairs | 13.67, 25.33 | GI child's bedroom 1 | 0.71 | 5.4 |
| closet room | upstairs | 11.64, 30.17 | GI closet room | 0.82 | 3.1 |
| landing | upstairs | 7.34, 29.45 | GI landing | 1.54 | 4.0 |
| master bedroom | upstairs | -9.27, 25.75 | GI master bedroom 1 | 1.52 | 10.7 |
| master bedroom-spot2 | upstairs | -11.47, 21.85 | GI master bedroom 1 | 1.50 | 10.7 |
| master bedroom-spot3 | upstairs | -8.37, 30.05 | GI master bedroom 3 | 1.51 | 10.7 |
| master bedroom-spot4 | upstairs | -7.97, 21.25 | GI master bedroom 1 | 0.90 | 10.7 |
| stairwell-top | upstairs | 3.00, 29.82 | GI stairwell | 1.67 | 5.6 |
| upstairs bathroom | upstairs | -4.67, 26.33 | GI upstairs bathroom 1 | 1.50 | 5.0 |
| upstairs den | upstairs | -3.07, 30.12 | GI upstairs den 2 | 1.49 | 7.7 |
| upstairs den-spot2 | upstairs | 1.03, 29.62 | GI upstairs den 2 | 1.59 | 7.7 |
| upstairs hall | upstairs | 4.44, 26.23 | GI upstairs hall | 1.31 | 5.5 |
| upstairs hall-spot2 | upstairs | 1.04, 25.03 | GI upstairs hall | 0.76 | 5.5 |

## Thresholds (away from doors)

| id | volumes | axis | gap m | storey | at | clear m |
|---|---|---|---|---|---|---|
| thr01-entrance-hall | GI entrance hall 2 / GI entrance hall 5 | z | -0.33 | ground | -4.86, 26.84 | 0.42 |
| thr02-entrance-hall | GI entrance hall 2 / GI entrance hall 1 | x | -0.14 | ground | -0.95, 25.53 | 0.80 |
| thr03-entrance-hall | GI entrance hall 1 / GI entrance hall 3 | z | -0.22 | ground | 2.40, 26.89 | 0.89 |
| thr04-entrance-hall-stairwell | GI entrance hall 3 / GI stairwell | z | -0.16 | ground | 2.40, 27.94 | 1.26 |
| thr05-entrance-hall | GI entrance hall 3 / GI entrance hall 4 | x | -0.14 | ground | 3.45, 27.45 | 0.40 |
| thr06-entrance-hall-stairwell | GI entrance hall 4 / GI stairwell | z | -0.15 | ground | 3.93, 27.94 | 0.90 |
| thr07-kitchen-dining | GI kitchen-dining 3 / GI kitchen-dining 7 | z | -0.11 | ground | -12.43, 28.90 | 0.27 |
| thr08-kitchen-dining | GI kitchen-dining 1 / GI kitchen-dining 3 | z | -0.11 | ground | -11.23, 25.00 | 0.30 |
| thr09-kitchen-dining-dining-bay | GI dining bay / GI kitchen-dining 1 | z | -0.27 | ground | -9.20, 20.32 | 0.53 |
| thr10-kitchen-dining | GI kitchen-dining 3 / GI kitchen-dining 2 | x | -0.14 | ground | -8.55, 27.00 | 1.13 |
| thr11-kitchen-dining | GI kitchen-dining 7 / GI kitchen-dining 2 | x | -0.14 | ground | -8.55, 30.29 | 0.61 |
| thr12-kitchen-dining | GI kitchen-dining 2 / GI kitchen-dining 4 | x | -0.21 | ground | -6.81, 27.94 | 0.82 |
| thr13-kitchen-dining-entrance-hall | GI entrance hall 2 / GI kitchen-dining 4 | z | -0.11 | ground | -4.96, 26.95 | 0.32 |
| thr14-kitchen-dining | GI kitchen-dining 10 / GI kitchen-dining 4 | z | -0.23 | ground | -4.96, 27.01 | 0.32 |
| thr15-kitchen-dining | GI kitchen-dining 4 / GI kitchen-dining 5 | x | -0.14 | ground | -4.85, 27.95 | 0.72 |
| thr16-kitchen-dining | GI kitchen-dining 5 / GI kitchen-dining 6 | x | -0.58 | ground | -1.67, 27.95 | 0.81 |
| thr17-kitchen-dining | GI kitchen-dining 5 / GI kitchen-dining 9 | z | -0.37 | ground | -1.52, 28.76 | 0.30 |
| thr18-kitchen-dining-stairwell | GI kitchen-dining 6 / GI stairwell | x | -1.30 | ground | 0.70, 29.82 | 1.74 |
| thr19-kitchen-dining-entrance-hall | GI kitchen-dining 6 / GI entrance hall 3 | x | -0.11 | ground | 1.30, 27.45 | 0.39 |
| thr20-living-room-entrance-hall | GI entrance hall 4 / GI living room 6 | x | -0.11 | ground | 4.54, 27.45 | 0.27 |
| thr21-living-room-stairwell | GI stairwell / GI living room 3 | x | -0.11 | ground | 5.85, 29.82 | 1.12 |
| thr22-living-room | GI living room 3 / GI living room 2 | x | -0.11 | ground | 7.79, 29.35 | 1.60 |
| thr23-living-room | GI living room 2 / GI living room 1 | x | -0.21 | ground | 8.81, 29.35 | 1.92 |
| thr24-living-room | GI living room 4 / GI living room 1 | z | -0.23 | ground | 11.16, 24.20 | 0.76 |
| thr25-child-s-bedroom-closet-room | GI child's bedroom 1 / GI closet room | z | -0.11 | upstairs | 11.48, 29.38 | 0.27 |
| thr26-landing-stairwell | GI stairwell / GI landing | x | -0.11 | upstairs | 5.85, 29.82 | 1.77 |
| thr27-master-bedroom | GI master bedroom 1 / GI master bedroom 2 | z | -0.25 | upstairs | -8.93, 25.91 | 1.89 |
| thr28-master-bedroom | GI master bedroom 2 / GI master bedroom 3 | z | -0.14 | upstairs | -8.93, 27.45 | 1.88 |
| thr29-upstairs-bathroom | GI upstairs bathroom 3 / GI upstairs bathroom 1 | z | -0.14 | upstairs | -5.27, 25.45 | 0.62 |
| thr30-upstairs-bathroom | GI upstairs bathroom 3 / GI upstairs bathroom 2 | x | -0.11 | upstairs | -3.85, 25.34 | 0.33 |
| thr31-upstairs-bathroom | GI upstairs bathroom 5 / GI upstairs bathroom 2 | x | -1.03 | upstairs | -3.39, 25.33 | 0.26 |
| thr32-upstairs-bathroom | GI upstairs bathroom 5 / GI upstairs bathroom 1 | z | -0.14 | upstairs | -3.35, 25.45 | 0.38 |
| thr33-upstairs-den | GI upstairs den 1 / GI upstairs den 2 | x | -0.14 | upstairs | -3.35, 29.82 | 1.42 |
| thr34-upstairs-den-stairwell | GI upstairs den 2 / GI stairwell | x | -1.07 | upstairs | 0.58, 29.82 | 1.36 |
