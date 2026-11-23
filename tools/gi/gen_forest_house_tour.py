"""Writes tools/gi/forest_house_tour.json: the GI Phase 0 camera tour of the Forest house.

Read by `--gi-tour` (see GI_PHASE0_ORDERS). Everything is hand-placed from project/scenes/Forest.json:
GI Volume boxes (rooms), the Wall_*_door pieces (doorway centres), floor collider tops and the
Stairs_main_L flight. Re-run after moving waypoints:  python -I tools/gi/gen_forest_house_tour.py

Conventions (the engine's own, src/Renderer/Camera.cpp):
  feet    world metres, the player's feet on the floor; eye = feet + player.eyeHeight
  yawDeg  front = (cos yaw, 0, sin yaw): 0 = +X (east), 90 = +Z (north), 180 = -X (west), -90 = -Z (south)
  pitchDeg + is up
Waypoints are walked in file order. "cut": true on a waypoint means it can't be reached from the one
before it in a straight line, so the tour teleports there instead of walking (no walk rows for that segment).
"""
import json
import os

# Floor tops (collider top surfaces in Forest.json): Floors_1st, Floors_2nd, Porch_floor_*.
GROUND = 0.194
UPSTAIRS = 3.104
PORCH = 0.165

# Stairs_main_L: one straight flight on the north side of the stairwell, climbing east.
STAIR_Z = 30.75
STAIR_X0, STAIR_X1 = 1.73, 5.60  # first and last tread


def stair_y(x):
    t = min(max((x - STAIR_X0) / (STAIR_X1 - STAIR_X0), 0.0), 1.0)
    return round(GROUND + t * (UPSTAIRS - GROUND), 3)


# First-person setup: the 'Player Spawn' First Person Controller in Forest.json (Eye Height 1.7,
# Field of View 90 = horizontal on 16:9, Move Speed 4.0). verticalFovDeg is FirstPersonControllerComponent::VerticalFov().
PLAYER = {
    "eyeHeight": 1.7,
    "horizontalFovDeg": 90.0,
    "verticalFovDeg": 58.72,
    "walkSpeed": 4.0,
    "source": "Forest.json 'Player Spawn' First Person Controller (Eye Height, Field of View, Move Speed)",
}

YAW = {"E": 0.0, "N": 90.0, "W": 180.0, "S": -90.0}
STEP = {"E": (1, 0), "N": (0, 1), "W": (-1, 0), "S": (0, -1)}

waypoints = []


def wp(id, room, vol, kind, x, y, z, yaw, pitch=0.0, cut=False, note=None):
    w = {"id": id, "room": room, "giVolume": vol, "kind": kind,
         "feet": [round(x, 3), round(y, 3), round(z, 3)], "yawDeg": float(yaw), "pitchDeg": float(pitch)}
    if cut:
        w["cut"] = True
    if note:
        w["note"] = note
    waypoints.append(w)


def door(id, x, z, heading, y, before, after, d=0.6, y_before=None, y_after=None, cut=False):
    """A doorway walked through heading N/E/S/W: just before, in the opening, just after.
    before/after are (room, giVolume) on each side."""
    dx, dz = STEP[heading]
    yaw = YAW[heading]
    wp(f"{id}_before", before[0], before[1], "door_before", x - dx * d, y if y_before is None else y_before,
       z - dz * d, yaw, cut=cut)
    wp(f"{id}_in", f"{before[0]} | {after[0]}", f"{before[1]} | {after[1]}", "door_in", x, y, z, yaw)
    wp(f"{id}_after", after[0], after[1], "door_after", x + dx * d, y if y_after is None else y_after,
       z + dz * d, yaw)


PORCH_R = ("Porch (outside)", "")
HALL = ("Entrance hall", "GI entrance hall 1")
HALL_W = ("Entrance hall", "GI entrance hall 2")
KITCHEN = ("Kitchen-dining", "GI kitchen-dining 4")
STORE = ("Stair storeroom", "GI stair storeroom")
KITCHEN6 = ("Kitchen-dining", "GI kitchen-dining 6")
HALL3 = ("Entrance hall", "GI entrance hall 3")
CORR = ("Living room corridor", "GI living room 5")
BATH_D = ("Downstairs bathroom", "GI downstairs bathroom 1")
LIV3 = ("Living room", "GI living room 3")
LIV1 = ("Living room", "GI living room 1")
GALLERY = ("Stairwell gallery", "GI stairwell")
UHALL = ("Upstairs hall", "GI upstairs hall")
LANDING = ("Landing", "GI landing")
CHILD = ("Child's bedroom", "GI child's bedroom 1")
CLOSET = ("Closet room", "GI closet room")
DEN1 = ("Upstairs den", "GI upstairs den 1")
BATH_U = ("Upstairs bathroom", "GI upstairs bathroom 1")
MASTER2 = ("Master bedroom", "GI master bedroom 2")
MASTER3 = ("Master bedroom", "GI master bedroom 3")
BOX = ("Box room", "GI box room")

# ---- Ground floor -------------------------------------------------------------------------------
door("front_door", 1.00, 24.04, "N", GROUND, PORCH_R, HALL, y_before=PORCH)
wp("hall_east_corner", *HALL, "corner", 3.00, GROUND, 25.60, YAW["W"])
wp("hall_center", *HALL_W, "room", -1.50, GROUND, 25.40, YAW["W"])
wp("hall_west_corner", *HALL_W, "corner", -5.90, GROUND, 25.60, YAW["E"],
   note="Atlas gap: hall/kitchen seam at x -6.59 behind this point")
door("kitchen_door", -4.82, 26.95, "N", GROUND, HALL_W, KITCHEN)
wp("kitchen_main", "Kitchen-dining", "GI kitchen-dining 3", "room", -11.00, GROUND, 27.25, YAW["W"])
wp("dining", "Kitchen-dining", "GI kitchen-dining 1", "room", -10.60, GROUND, 22.70, YAW["N"])
# The island counter and the half-height wall split the kitchen: its north strip and east end are cuts.
wp("kitchen_north", "Kitchen-dining", "GI kitchen-dining 7", "room", -12.75, GROUND, 30.25, YAW["E"], cut=True)
wp("kitchen_east", "Kitchen-dining", "GI kitchen-dining 5", "room", -3.00, GROUND, 27.75, YAW["E"], cut=True,
   note="Atlas gap: half-wall seam at z 28.73 to the left")
wp("kitchen_stair_side", *KITCHEN6, "corner", -0.20, GROUND, 28.40, YAW["N"])
door("storeroom_door", -1.91, 30.35, "W", GROUND, KITCHEN6, STORE)
door("hall_double_door", 2.46, 26.95, "N", GROUND, HALL, HALL3, cut=True)
wp("corridor_east", *CORR, "corner", 4.75, GROUND, 27.55, YAW["E"],
   note="Atlas gap: hall 4 / living room 9 seam at x 4.45")
# The bathroom door, held open, stands across the corridor between corridor_east and its doorway: a cut.
door("bathroom_down_door", 6.34, 26.95, "S", GROUND, CORR, BATH_D, cut=True)
door("living_door", 7.79, 29.37, "E", GROUND, LIV3, LIV1, cut=True)
wp("living_room", *LIV1, "room", 12.00, GROUND, 26.50, YAW["N"])
wp("living_east_bay", "Living room", "GI living room 4", "room", 12.00, GROUND, 22.50, YAW["N"])
# The TV table blocks the bay -> east door line, so the east door is a cut.
door("east_door", 14.48, 25.98, "E", GROUND, LIV1, ("Outside (east)", ""), y_after=0.0, cut=True)  # terrain is 0.0 here  # terrain is 0.0 here

# ---- Stairs -------------------------------------------------------------------------------------
wp("stairs_foot", "Stairwell", "GI stairwell", "stairs", 1.40, GROUND, STAIR_Z, YAW["E"], pitch=10.0, cut=True)
wp("stairs_mid", "Stairwell", "GI stairwell", "stairs", 3.65, stair_y(3.65), STAIR_Z, YAW["E"], pitch=10.0)
wp("stairs_top", *LANDING, "stairs", 6.05, UPSTAIRS, STAIR_Z, YAW["E"])

# ---- Upstairs -----------------------------------------------------------------------------------
# Kept 1 m west of the landing floor lamp (Lamp_floor at x 8.87, z 30.39: no collider), which the walk to the
# child's door grazed at x 8.40.
wp("landing_corner", *LANDING, "corner", 7.60, UPSTAIRS, 31.00, YAW["W"])
door("child_door", 9.25, 28.40, "E", UPSTAIRS, LANDING, CHILD)
wp("child_bedroom", *CHILD, "room", 11.90, UPSTAIRS, 26.75, YAW["N"])
door("closet_door", 11.67, 29.37, "N", UPSTAIRS, CHILD, CLOSET)
door("upstairs_hall_door", 4.40, 27.92, "S", UPSTAIRS, GALLERY, UHALL, cut=True)
wp("upstairs_hall", *UHALL, "room", 3.35, UPSTAIRS, 26.20, YAW["W"])
wp("upstairs_hall_south", *UHALL, "gap", 2.00, UPSTAIRS, 24.90, YAW["N"],
   note="Atlas gap: 5.0 m2 strip x -0.34..5.72, z 24.31..25.11 in no volume")
wp("den_east", "Upstairs den", "GI upstairs den 2", "room", -1.15, UPSTAIRS, 29.90, YAW["W"], cut=True)
wp("den_west", *DEN1, "room", -4.40, UPSTAIRS, 29.80, YAW["W"])
door("master_den_door", -6.76, 29.38, "W", UPSTAIRS, DEN1, MASTER3)
wp("master_bedroom", "Master bedroom", "GI master bedroom 1", "room", -9.75, UPSTAIRS, 23.20, YAW["W"],
   note="Atlas gap: looks at the 5.5 m2 west strip x -14.14..-13.39 in no volume")
door("box_room_door", -11.13, 27.44, "W", UPSTAIRS, MASTER2, BOX)
wp("box_room", *BOX, "room", -12.80, UPSTAIRS, 28.80, YAW["N"])
door("master_hall_door", -6.76, 26.47, "E", UPSTAIRS, MASTER2, BATH_U, cut=True)
wp("upstairs_bathroom", "Upstairs bathroom", "GI upstairs bathroom 2", "room", -2.50, UPSTAIRS, 25.80, YAW["E"],
   note="Atlas gap: looks at the 3.1 m2 strip x -0.34..0.67 in no volume")
wp("upstairs_bath_north", *BATH_U, "gap", -2.50, UPSTAIRS, 27.25, YAW["E"],
   note="Atlas gap: 1.75 m2 x -2.70..-0.66, z 26.73..28.05 in no volume")
door("den_door", -4.34, 27.92, "N", UPSTAIRS, BATH_U, DEN1)

# ---- Jacob routes: continuous walks, looking along travel ---------------------------------------
routes = [
    {"id": "route_front_door_to_living_room",
     "name": "Come in the front door, through the hall's double doors and the corridor, into the living room",
     "points": ["front_door_before", "front_door_in", "front_door_after", "hall_double_door_before",
                "hall_double_door_in", "hall_double_door_after", "corridor_east", "living_door_before",
                "living_door_in", "living_door_after", "living_room"]},
    {"id": "route_hall_to_kitchen_and_back",
     "name": "Hall to the kitchen and dining room, round the kitchen, back out to the hall",
     "points": ["hall_east_corner", "hall_center", "hall_west_corner", "kitchen_door_before", "kitchen_door_in",
                "kitchen_door_after", "kitchen_main", "dining", "kitchen_main", "kitchen_door_after",
                "kitchen_door_in", "kitchen_door_before", "hall_center", "hall_east_corner"]},
    {"id": "route_upstairs_to_bedrooms",
     "name": "Up the stairs, across the landing into the child's bedroom, back along the gallery to the den and master bedroom",
     "points": ["hall_double_door_after", "stairs_foot", "stairs_mid", "stairs_top", "landing_corner",
                "child_door_before", "child_door_in", "child_door_after", "child_bedroom", "child_door_after",
                "child_door_in", "child_door_before", "upstairs_hall_door_before", "den_east", "den_west",
                "master_den_door_before", "master_den_door_in", "master_den_door_after", "master_bedroom"]},
]


def main():
    ids = [w["id"] for w in waypoints]
    assert len(ids) == len(set(ids)), "duplicate waypoint id"
    for r in routes:
        for p in r["points"]:
            assert p in ids, f"{r['id']}: unknown waypoint {p}"
    doc = {
        "version": 1,
        "scene": "scenes/Forest.json",
        "player": PLAYER,
        # Every Openable door held fully open for the whole tour (the --gi-tour loader's Openable::SetAll),
        # so doorway waypoints see through the opening the way Jacob walks the house.
        "doorState": "allOpen",
        "waypoints": waypoints,
        "routes": routes,
    }
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "forest_house_tour.json")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    print(f"{out}: {len(waypoints)} waypoints, {len(routes)} routes")


if __name__ == "__main__":
    main()
