# The Forest house's pack space, shared by forest_house.py (which builds the house) and the tools that read it back
# (house_lights, house_gi, house_gi_check, house_shots, house_inventory): where the pack sits in the world, its floor
# and ceiling heights, the lights' red, and the floor tiles. No imports of forest_house, so house_lights needs no
# module handed in to reach these.
import math
import re

from scene_kit import qrot

PACK = 'assets/Environments/SuburbNeighborhoodHousePack/'

# Where the house goes: pack point PACK_C lands at HOUSE_AT, turned HOUSE_YAW (pack +X, the porch side, faces
# world -Z: north, toward the clearing). Pack y 0.485 is the walls' pivot: their foundation runs 0.485 m below it,
# the floors' top is 0.194 above it.
LOT_Y = 0.0
PACK_C = (-33.0, 0.485, -1.0)
HOUSE_AT = (0.0, LOT_Y, 26.0)
HOUSE_YAW = 90.0
FLOOR_1, FLOOR_2 = 0.68, 3.59          # pack y of the two floors' top
CEIL_1, CEIL_2 = 3.395, 6.305          # pack y of the two ceilings

RED = (1.0, 0.035, 0.02)
RED_I = 1.6
RED_GLOW, RED_GLOW_S = (1.0, 0.012, 0.006), 2.2   # a red fitting's glass: the bulb's, and the master bedroom's lamps
OFF = {'guid': 'af7ac0342f5de963', 'path': PACK + 'Materials/M_Lamps_House_Off.mat'}


def to_world(p):
    """A pack-space point in the world."""
    a = math.radians(HOUSE_YAW)
    dx, dz = p[0] - PACK_C[0], p[2] - PACK_C[2]
    return (HOUSE_AT[0] + math.cos(a) * dx + math.sin(a) * dz, HOUSE_AT[1] + p[1] - PACK_C[1],
            HOUSE_AT[2] - math.sin(a) * dx + math.cos(a) * dz)


def from_world(w):
    """A world point back in pack space (to_world inverted)."""
    a = math.radians(HOUSE_YAW)
    dx, dz = w[0] - HOUSE_AT[0], w[2] - HOUSE_AT[2]
    return [PACK_C[0] + math.cos(a) * dx - math.sin(a) * dz, w[1] - HOUSE_AT[1] + PACK_C[1],
            PACK_C[2] + math.sin(a) * dx + math.cos(a) * dz]


def world_y(pack_y):
    """A pack-space height in the world."""
    return HOUSE_AT[1] + pack_y - PACK_C[1]


def stem(e):
    """An entity's name without the ' (n)' the pack adds to repeats."""
    return re.sub(r' \(\d+\)$', '', e['name'])


def floor_rects(pk):
    """Every floor tile as (x0, x1, z0, z1, top y) in pack space."""
    out = []
    for e in pk.named(r'Floor_(\d+|½)'):
        m = re.match(r'Floor_([\d½]+)x_([\d½]+)x', e['name'])
        if not m or e['kind'] != 'models' or e['id'] in pk.gone:
            continue
        a, b = (float(v.replace('½', '.5')) * 0.97 for v in m.groups())
        pos, rot = pk.world(e['id'])
        corners = [qrot(rot, c) for c in ((0, 0, 0), (a, 0, 0), (0, 0, -b), (a, 0, -b))]
        xs = [pos[0] + c[0] for c in corners]
        zs = [pos[2] + c[2] for c in corners]
        out.append((min(xs), max(xs), min(zs), max(zs), round(pos[1] + 0.194, 3)))
    return out
