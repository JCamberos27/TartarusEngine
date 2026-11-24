# Kronos counted walk pops + fast ramps, shot by name (W6 Briareos, 2026-11-24)
Source: gi-p2\kronos\doors\fix-3\REPORT.md, seam_pops_tour_full_route.md (23 counted) and seam_pops_tour_gi_route.md (3 counted); poses = his walk waypoints (gi-p1\argus\routes_only.json).
outes_only.json).
His frame index inside a segment is not mapped to a position (interpolated walk), so each segment has its end poses (walkwp-<waypoint>_pose, shared when segments meet) and a midpoint pose (walkpop-<from>-to-<to>_mid). Open run, NO VALID BASELINE (new ids). Separate from JACOB_WALK_STEPS.md (the 29 uncounted steps).

| Segment | Counted pops | Ramp | Mid pose shot | Mid feet (x, y, z) yaw |
|---|---|---|---|---|
| front_door_before->front_door_in | full:2 | - | walkpop-front-door-before-to-front-door-in_mid | (1.00, 0.179, 23.74) 90 |
| hall_double_door_after->corridor_east | full:2 | - | walkpop-hall-double-door-after-to-corridor-east_mid | (3.60, 0.194, 27.55) 45 |
| hall_east_corner->hall_center | full:1 | - | walkpop-hall-east-corner-to-hall-center_mid | (0.75, 0.194, 25.50) -180 |
| hall_center->hall_west_corner | full:1 | ramp R1 -74% | walkpop-hall-center-to-hall-west-corner_mid | (-3.70, 0.194, 25.50) 90 |
| kitchen_main->kitchen_door_after | full:2 | - | walkpop-kitchen-main-to-kitchen-door-after_mid | (-7.91, 0.194, 27.40) 135 |
| kitchen_door_after->kitchen_door_in | full:2 | - | walkpop-kitchen-door-after-to-kitchen-door-in_mid | (-4.82, 0.194, 27.25) 90 |
| kitchen_door_before->hall_center | full:3 | ramp R2 +65% | walkpop-kitchen-door-before-to-hall-center_mid | (-3.16, 0.194, 25.88) 135 |
| landing_corner->child_door_before | full:1 | - | walkpop-landing-corner-to-child-door-before_mid | (8.12, 3.104, 29.70) 90 |
| child_door_before->upstairs_hall_door_before | full:2 | - | walkpop-child-door-before-to-upstairs-hall-door-before_mid | (6.53, 3.104, 28.46) -45 |
| den_east->den_west | full:2 | - | walkpop-den-east-to-den-west_mid | (-2.77, 3.104, 29.85) -180 |
| den_west->master_den_door_before | full:3 | - | walkpop-den-west-to-master-den-door-before_mid | (-5.28, 3.104, 29.59) -180 |
| master_den_door_before->master_den_door_in | full:2 | - | walkpop-master-den-door-before-to-master-den-door-in_mid | (-6.46, 3.104, 29.38) -180 |
| child_door_before->child_door_in | gi:1 | - | walkpop-child-door-before-to-child-door-in_mid | (8.95, 3.104, 28.40) 0 |
| upstairs_hall_door_before->den_east | gi:2 | - | walkpop-upstairs-hall-door-before-to-den-east_mid | (1.62, 3.104, 29.21) -135 |

pops: full = counted in the full-view route (sum 23), gi = counted in the GI-only route (sum 3); the same segment can appear in both rows above only when listed once with both tags.
