# The abandoned house in the middle of the Forest, built by build_forest.py.
#
# The house is the Suburb pack demo's two-storey House_01, furnished (forest_house_pack.json: its walls, floors,
# roof, porch, doors and every prop, in the pack's own space, without the basement - the Forest's floor is a
# heightfield and can't be cut). It stands south of the start clearing, its porch facing the player across the
# clearing, on a lot the terrain is eased flat for: a front yard with a cracked walk to a broken picket fence, a
# driveway with a dead car, and a fenced backyard the forest is taking back.
#
# Abandoned and trashed: a third of the glass is gone and the chairs and lamps are
# knocked over, furniture shoved about, pictures hang crooked, what was worth taking is gone; rubbish, papers,
# books and leaves blown in lie across the floors; the floors are filthy, the ceilings water-stained, the walls
# damp at the base and peeling. The power is mostly out: a dying porch light, a stuttering fluorescent tube in
# the kitchen, candles left burning in the living room, and the master bedroom upstairs, at the front, lit red.
#
# Interior work is authored in the pack's space and parented under the house's 'Pack Layout'; the yard in world
# metres. Every random choice comes from its own seeded stream, so a rebuild gives the same house.
import json
import math
import random
import re

import scene_kit as k

PACK = 'assets/Environments/SuburbNeighborhoodHousePack/'
PROPS_IN, PROPS_OUT = PACK + 'Prefabs/Props_interior/', PACK + 'Prefabs/Props_exterior/'
GROUND_P = PACK + 'Prefabs/Ground/'

# Where the house goes: pack point PACK_C lands at HOUSE_AT, turned HOUSE_YAW (pack +X, the porch side, faces
# world -Z: north, toward the clearing). Pack y 0.485 is the walls' pivot: their foundation runs 0.485 m below it,
# the floors' top is 0.194 above it.
LOT_Y = 0.0
PACK_C = (-33.0, 0.485, -1.0)
HOUSE_AT = (0.0, LOT_Y, 26.0)
HOUSE_YAW = 90.0
FLOOR_1, FLOOR_2 = 0.68, 3.59          # pack y of the two floors' top
CEIL_1, CEIL_2 = 3.395, 6.305          # pack y of the two ceilings

# The lot (world x0, x1, z0, z1), eased flat; trees, rocks and scans are kept out of it.
LOT = (-21.0, 21.0, 11.0, 46.0)
LOT_EASE = 7.0                          # metres outside the lot over which the ground rolls back in
HOUSE_BOX = (-14.9, 14.9, 19.9, 32.2)   # the house's footprint in the world, porch included
WALK_X = 2.9                            # the front walk's centre line, from the porch steps to the gate
DRIVE = (-19.6, -15.6, 11.0, 25.6)      # the driveway (x0, x1, z0, z1)

RED = (1.0, 0.035, 0.02)


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def lot_dist(x, z):
    """How far (x, z) is outside the lot (0 inside)."""
    dx = max(LOT[0] - x, 0.0, x - LOT[1])
    dz = max(LOT[2] - z, 0.0, z - LOT[3])
    return math.hypot(dx, dz)


def flatten(x, z, h):
    """The terrain height at (x, z) with the lot eased flat."""
    t = smoothstep(0.0, LOT_EASE, lot_dist(x, z))
    return LOT_Y + (h - LOT_Y) * t


def in_lot(x, z, pad=0.0):
    return LOT[0] - pad <= x <= LOT[1] + pad and LOT[2] - pad <= z <= LOT[3] + pad


def in_house(x, z, pad=0.0):
    return HOUSE_BOX[0] - pad <= x <= HOUSE_BOX[1] + pad and HOUSE_BOX[2] - pad <= z <= HOUSE_BOX[3] + pad


def to_world(p):
    """A pack-space point in the world."""
    a = math.radians(HOUSE_YAW)
    dx, dz = p[0] - PACK_C[0], p[2] - PACK_C[2]
    return (HOUSE_AT[0] + math.cos(a) * dx + math.sin(a) * dz, HOUSE_AT[1] + p[1] - PACK_C[1],
            HOUSE_AT[2] - math.sin(a) * dx + math.cos(a) * dz)


# --- quaternions ([x, y, z, w]) for the pack-space edits
def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]


def qrot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return [vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx)]


def axis_q(axis, deg):
    h = math.radians(deg) / 2
    s = math.sin(h)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h)]


def yaw_of(q):
    """The heading of a rotation: the yaw of its local +X."""
    v = qrot(q, (1.0, 0.0, 0.0))
    return math.degrees(math.atan2(-v[2], v[0]))


class Pack:
    """The house snapshot: entities by id, children, pack-space world transforms."""
    def __init__(self, path):
        with open(path, encoding='utf-8') as f:
            self.ents = json.load(f)['entities']
        self.by = {e['id']: e for e in self.ents}
        self.kids = {}
        for e in self.ents:
            self.kids.setdefault(e['parentId'], []).append(e['id'])
        self.gone = set()

    def world(self, i):
        e = self.by[i]
        p, r = e.get('position', [0, 0, 0]), e.get('rotation', [0, 0, 0, 1])
        if e['parentId'] in self.by:
            pp, pr = self.world(e['parentId'])
            v = qrot(pr, p)
            return [pp[0] + v[0], pp[1] + v[1], pp[2] + v[2]], qmul(pr, r)
        return list(p), list(r)

    def group(self, name):
        return next(e['id'] for e in self.ents if e['name'] == name)

    def props(self):
        """The furniture and dressing: House_01_Props' direct children."""
        return [self.by[i] for i in self.kids.get(self.group('House_01_Props'), [])]

    def named(self, pattern):
        rx = re.compile(pattern)
        return [e for e in self.ents if rx.match(e['name'])]

    def remove(self, i):
        self.gone.add(i)
        for c in self.kids.get(i, []):
            self.remove(c)


# --- the house itself -------------------------------------------------------------------------------------------
LAMPS_OFF = ('Lamp_bedroom', 'Lamp_ceiling_circle', 'Lamp_chandelier', 'Lamp_floor', 'Lamp_kitchen_A',
             'Lamp_kitchen_B', 'Lamp_stand', 'Lamp_table_A', 'Lamp_table_B', 'Lamp_wall_chan', 'Lamp_wall_fluorescent',
             'Lamp_wall_toilet')
OFF = {'guid': 'af7ac0342f5de963', 'path': PACK + 'Materials/M_Lamps_House_Off.mat'}
OFF_OUT = {'guid': 'd48f49996a84314b', 'path': PACK + 'Materials/M_Lamps_Off.mat'}
DEAD_SCREEN = {}    # filled by add_house: the devices' material with the emission off


def trash_house(pk, rng, notes):
    """Edits to the pack's own pieces: broken glass, doors left open, furniture knocked about, things taken."""
    def stem(e):
        return re.sub(r' \(\d+\)$', '', e['name'])
    broken = []
    for e in pk.named(r'Windowglass_singlewindow'):
        if rng.random() < 0.45:
            pos, rot = pk.world(e['id'])
            nrm = qrot(rot, (0, 0, 1))
            plain = abs(nrm[1]) < 0.05 and max(abs(nrm[0]), abs(nrm[2])) > 0.98 and \
                abs(e.get('scale', [1, 1, 1])[0] - 1.0) < 0.05 and 'bay' not in pk.by[e['parentId']]['name']
            broken.append((pos, rot, plain))
            pk.remove(e['id'])
    notes['broken windows'] = len(broken)

    # Doors: the front one stuck ajar, the side one wide open, the rest a mix.
    n_open = 0
    for e in pk.ents:
        o = e.get('Openable')
        if not o:
            continue
        frame = pk.by[e['parentId']]['name']
        if frame.startswith('Door_exterior_paneled'):
            o.update({'Start Open': True, 'Amount': 38.0})
        elif frame.startswith('Door_exterior_white'):
            o['Start Open'] = True
        else:
            u = rng.random()
            if u < 0.45:
                o['Start Open'] = True
            elif u < 0.7:
                o.update({'Start Open': True, 'Amount': round(rng.uniform(18.0, 40.0), 1)})
        n_open += o.get('Start Open', False)
    notes['doors left open'] = n_open

    # The power's off: every fitting dark (lights() relights the ones still burning), screens dead.
    for e in pk.ents:
        if e['kind'] == 'models' and stem(e).startswith(LAMPS_OFF):
            e['materials'] = [dict(OFF)]
        elif e['kind'] == 'models' and stem(e).startswith('D_'):
            e['materials'] = [{'embedded': dict(DEAD_SCREEN)}]

    knocked = moved = taken = crooked = 0
    for e in pk.props():
        if e['kind'] != 'models' or e['id'] in pk.gone:
            continue
        s = stem(e)
        pos, rot = e['position'], e['rotation']
        if s.startswith(('Kitchen_chair_wooden', 'Kitchen_chair_fabric')) and rng.random() < 0.6:
            # on its side: rolled about its own forward axis, lifted half its width
            e['rotation'] = [round(v, 6) for v in qmul(rot, axis_q((0, 0, 1), rng.choice((-90.0, 90.0))))]
            e['rotation'] = [round(v, 6) for v in qmul(axis_q((0, 1, 0), rng.uniform(-40, 40)), e['rotation'])]
            e['position'] = [pos[0], round(pos[1] + 0.215, 3), pos[2]]
            knocked += 1
        elif s == 'Lamp_stand' and rng.random() < 0.7:
            e['rotation'] = [round(v, 6) for v in qmul(rot, axis_q((1, 0, 0), 90.0))]
            e['position'] = [pos[0], round(pos[1] + 0.19, 3), pos[2]]
            knocked += 1
        elif s.startswith(('Sidetable_small', 'Sidetable_A', 'Sidetable_B', 'Bedstool')) and rng.random() < 0.45:
            e['rotation'] = [round(v, 6) for v in qmul(rot, axis_q((0, 0, 1), rng.choice((-90.0, 90.0))))]
            e['position'] = [pos[0], round(pos[1] + 0.25, 3), pos[2]]
            knocked += 1
        elif s.startswith('Plant_interior') and rng.random() < 0.5:
            pk.remove(e['id'])
            taken += 1
        elif s.startswith(('Couch_', 'Chair_leather', 'Sidetable', 'Kitchen_table', 'Nightstand', 'Bedstool')):
            turn, shove = rng.uniform(-22.0, 22.0), (rng.uniform(-0.35, 0.35), rng.uniform(-0.35, 0.35))
            e['rotation'] = [round(v, 6) for v in qmul(axis_q((0, 1, 0), turn), rot)]
            e['position'] = [round(pos[0] + shove[0], 3), pos[1], round(pos[2] + shove[1], 3)]
            moved += 1
        elif s.startswith('Painting_') and not s.startswith(('Painting_stand', 'Painting_standphoto')):
            if rng.random() < 0.45:
                e['rotation'] = [round(v, 6) for v in qmul(rot, axis_q((0, 0, 1), rng.choice((-1, 1)) *
                                                                       rng.uniform(5.0, 13.0)))]
                crooked += 1
        elif s.startswith(('D_TV', 'D_radio', 'D_alarmclock')) and rng.random() < 0.65:
            pk.remove(e['id'])          # the things worth carrying off
            taken += 1
        elif s.startswith(('KP_', 'LP_vase', 'LP_bowl', 'FP_vase', 'LP_candle')) and rng.random() < 0.4:
            pk.remove(e['id'])
            taken += 1
    notes.update({'knocked over': knocked, 'shoved': moved, 'taken': taken, 'crooked pictures': crooked})
    return broken


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


def blockers(pk):
    """Pack-space XZ boxes (x0, x1, z0, z1, y0, y1) that clutter keeps off: every collider and the stairs."""
    out = []
    for e in pk.ents:
        c = e.get('collider')
        if not c or e['id'] in pk.gone or c.get('isTrigger'):
            continue
        if any(e['name'].startswith(p) for p in ('Floor_', 'Ceiling_', 'Porch_floor', 'Carpet')):
            continue
        pos, rot = pk.world(e['id'])
        ctr, he = c.get('center', [0, 0, 0]), c.get('halfExtents', [0.3, 0.3, 0.3])
        sc = e.get('scale', [1, 1, 1])
        pts = []
        for sx in (-1, 1):
            for sz in (-1, 1):
                for sy in (-1, 1):
                    v = qrot(rot, ((ctr[0] + sx * he[0]) * sc[0], (ctr[1] + sy * he[1]) * sc[1],
                                   (ctr[2] + sz * he[2]) * sc[2]))
                    pts.append((pos[0] + v[0], pos[1] + v[1], pos[2] + v[2]))
        out.append((min(p[0] for p in pts), max(p[0] for p in pts), min(p[2] for p in pts), max(p[2] for p in pts),
                    min(p[1] for p in pts), max(p[1] for p in pts)))
    return out


def free(blocks, x, z, y, r):
    for x0, x1, z0, z1, y0, y1 in blocks:
        if y1 < y + 0.03 or y0 > y + 1.2:
            continue
        if x0 - r < x < x1 + r and z0 - r < z < z1 + r:
            return False
    return True


# Clutter: (prefab, weight, lie flat (book pitch), sink below the floor)
CLUTTER = [(PROPS_IN + 'Newspaper.prefab', 5, False), (PROPS_IN + 'LP_magazine.prefab', 4, False),
           (PROPS_IN + 'Book_A.prefab', 2, True), (PROPS_IN + 'Book_B.prefab', 2, True),
           (PROPS_IN + 'Book_C.prefab', 2, True), (PROPS_IN + 'Garage_cardboardbox.prefab', 3, False),
           (PROPS_IN + 'Garage_paperbag.prefab', 3, 'side'), (PROPS_IN + 'KP_plate.prefab', 2, False),
           (PROPS_IN + 'KP_glass_black.prefab', 2, 'side'), (PROPS_IN + 'Basement_box.prefab', 2, False),
           (PROPS_IN + 'Garage_plastic_box.prefab', 1, False), (PROPS_OUT + 'Trashbag.prefab', 2, False),
           (PROPS_IN + 'Garage_paintbucket.prefab', 1, 'side')]


def dress_inside(s, lay, pk, rng, broken, forest_litter, notes):
    """Rubbish across the floors, thicker against the walls; leaves and needles where the windows are out."""
    rects, blocks = floor_rects(pk), blockers(pk)
    usable = [(p, w, f) for p, w, f in CLUTTER if s.prefab(p)]
    bag = [c for c in usable for _ in range(c[1])]
    g = s.empty('Rubbish', lay, (0, 0, 0), static=True)
    placed = 0
    for x0, x1, z0, z1, y in rects:
        area = (x1 - x0) * (z1 - z0)
        for _ in range(int(area * 0.95)):
            # thicker toward the tile's edges, where things get kicked
            u, v = rng.random(), rng.random()
            if rng.random() < 0.5:
                if rng.random() < 0.5:
                    u = rng.choice((0.05, 0.95)) + rng.uniform(-0.04, 0.04)
                else:
                    v = rng.choice((0.05, 0.95)) + rng.uniform(-0.04, 0.04)
            x, z = x0 + (x1 - x0) * u, z0 + (z1 - z0) * v
            path, _w, flat = rng.choice(bag)
            if not free(blocks, x, z, y, 0.12):
                continue
            yaw = rng.uniform(0, 360)
            tilt, lift = (0.0, 0.0), 0.0
            if flat is True:
                tilt, lift = (-90.0, 0.0), 0.016
            elif flat == 'side':
                tilt, lift = (0.0, 90.0), 0.06
            s.stamp(path, g, (round(x, 3), round(y + lift, 3), round(z, 3)), yaw=round(yaw, 1), tilt=tilt,
                    scale=round(rng.uniform(0.9, 1.1), 2))
            placed += 1
    notes['rubbish'] = placed

    # Leaves and dead needles blown in under the broken windows and round the open doors.
    leaves = s.usable(forest_litter)
    n = 0
    for pos, rot, _plain in broken:
        y = FLOOR_1 if pos[1] < 3.0 else FLOOR_2
        nrm = qrot(rot, (0, 0, 1))
        c = (pos[0], pos[2])
        for side in (-1, 1):            # whichever side is indoors is where the floor is
            for _ in range(rng.randint(2, 4)):
                d = rng.uniform(0.3, 1.4)
                x, z = c[0] + nrm[0] * side * d + rng.uniform(-0.5, 0.5), c[1] + nrm[2] * side * d + rng.uniform(-0.5, 0.5)
                if not any(r[0] < x < r[1] and r[2] < z < r[3] and abs(r[4] - y) < 0.3 for r in rects):
                    continue
                if not free(blocks, x, z, y, 0.05) or not leaves:
                    continue
                s.stamp(rng.choice(leaves), g, (round(x, 3), round(y + 0.005, 3), round(z, 3)),
                        yaw=round(rng.uniform(0, 360), 1), scale=round(rng.uniform(0.5, 0.8), 2))
                n += 1
    # Books swept off every bookshelf onto the floor in front of it.
    books = [p for p in (PROPS_IN + 'Book_A.prefab', PROPS_IN + 'Book_B.prefab', PROPS_IN + 'Book_C.prefab') if s.prefab(p)]
    nb = 0
    for e in pk.props():
        if e['id'] in pk.gone or not e['name'].startswith(('Bookshelf', 'Book_set_bookshelf')):
            continue
        pos, rot = pk.world(e['id'])
        y = FLOOR_1 if pos[1] < 3.0 else FLOOR_2
        fwd = qrot(rot, (0, 0, 1))
        for _ in range(rng.randint(8, 14)):
            d, side = rng.uniform(0.35, 1.3), rng.uniform(-0.8, 0.8)
            x = pos[0] + fwd[0] * d - fwd[2] * side
            z = pos[2] + fwd[2] * d + fwd[0] * side
            for sign in (1, -1):        # in front of it, whichever way its front faces
                xx, zz = pos[0] + (x - pos[0]) * sign, pos[2] + (z - pos[2]) * sign
                if free(blocks, xx, zz, y, 0.05) and books:
                    s.stamp(rng.choice(books), g, (round(xx, 3), round(y + 0.016, 3), round(zz, 3)),
                            yaw=round(rng.uniform(0, 360), 1), tilt=(-90.0, 0.0) if rng.random() < 0.8 else (0.0, 90.0))
                    nb += 1
                    break
    notes['books on the floor'] = nb
    # Dead leaves and needles all through the ground floor, drifted against the walls.
    for x0, x1, z0, z1, y in rects:
        if y > 3.0 or not leaves:
            continue
        for _ in range(int((x1 - x0) * (z1 - z0) * 0.3)):
            u, v = rng.random(), rng.random()
            if rng.random() < 0.6:
                if rng.random() < 0.5:
                    u = rng.choice((0.03, 0.97))
                else:
                    v = rng.choice((0.03, 0.97))
            x, z = x0 + (x1 - x0) * u, z0 + (z1 - z0) * v
            if not free(blocks, x, z, y, 0.02):
                continue
            s.stamp(rng.choice(leaves), g, (round(x, 3), round(y + 0.004, 3), round(z, 3)),
                    yaw=round(rng.uniform(0, 360), 1), scale=round(rng.uniform(0.45, 0.75), 2))
            n += 1
    notes['leaves blown in'] = n


# Decals (docs/DECALS.md): a fixed handful of maps so the house stays well inside the decal texture budget.
D = 'assets/Decals/'
FLOOR_DECALS = [D + f'Stains/Splatters/DA_Stain_{n}.decal' for n in (591, 592, 593, 594, 595)]
CEIL_DECALS = [D + f'Stains/LiquidSplatter/DA_LiquidSplatter_{n:03d}.decal' for n in (1, 10, 19)]
BASE_DECALS = [D + f'Stains/LiquidSplatter/DA_LiquidSplatter_{n:03d}.decal' for n in (2, 9)]
PEEL_DECALS = [D + f'Grime/PeelingPaint/DA_PeelingPaint_{n:03d}.decal' for n in (3, 19)]
MAX_DECALS, MAX_MAPS = 560, 12


def decal(s, parent, name, pos, rot, asset, size, seed, opacity, tint=None, sort=2):
    dc = {'Asset': asset, 'Size': [round(v, 3) for v in size], 'Seed': seed, 'Sort Order': sort,
          'Opacity': round(opacity, 3), 'Affect Static': True, 'Affect Dynamic': False,
          'Rotation Jitter': 0.0, 'Scale Jitter': 0.0, 'Random Flip': True}
    if tint:
        dc['Tint'] = list(tint)
    i = s.empty(name, parent, pos, **{'Decal': dc})
    s.empties[-1]['rotation'] = [round(v, 6) for v in rot]
    return i


def grime(s, lay, pk, rng, notes):
    """Filthy floors, water-stained ceilings, damp wall bases and peeling paint."""
    g = s.empty('Grime', lay, (0, 0, 0), static=True)
    n, maps = 0, set()
    down = axis_q((1, 0, 0), -90.0)     # -Z to -Y: projects onto the floor below
    up = axis_q((1, 0, 0), 90.0)
    for x0, x1, z0, z1, y in floor_rects(pk):
        area = (x1 - x0) * (z1 - z0)
        for _ in range(max(1, int(area / 2.2))):
            x, z = rng.uniform(x0 + 0.3, x1 - 0.3), rng.uniform(z0 + 0.3, z1 - 0.3)
            w = rng.uniform(1.2, 2.6)
            a = rng.choice(FLOOR_DECALS)
            decal(s, g, f'Floor Grime {n:03d}', (round(x, 3), y, round(z, 3)),
                  qmul(axis_q((0, 1, 0), rng.uniform(0, 360)), down), a, (w, w * rng.uniform(0.7, 1.0), 0.3),
                  rng.randint(1, 999), rng.uniform(0.8, 1.0), tint=[0.3, 0.26, 0.2])
            maps.add(a)
            n += 1
        ceil = CEIL_1 if y < 3.0 else CEIL_2
        if rng.random() < 0.7:
            x, z = rng.uniform(x0 + 0.4, x1 - 0.4), rng.uniform(z0 + 0.4, z1 - 0.4)
            w = rng.uniform(1.2, 2.6)
            a = rng.choice(CEIL_DECALS)
            decal(s, g, f'Ceiling Stain {n:03d}', (round(x, 3), ceil, round(z, 3)),
                  qmul(axis_q((0, 1, 0), rng.uniform(0, 360)), up), a, (w, w * rng.uniform(0.7, 1.0), 0.28),
                  rng.randint(1, 999), rng.uniform(0.6, 0.95), tint=[0.62, 0.55, 0.42])
            maps.add(a)
            n += 1
    # The walls: a decal box straddling the wall's thickness marks both faces.
    for e in pk.named(r'Wall_(Interior|1st|2nd)_(\d|½)'):
        if e['kind'] != 'models' or e['id'] in pk.gone or rng.random() < 0.12:
            continue
        m = re.match(r'Wall_\w+?_([\d½]+)x', e['name'])
        length = float(m.group(1).replace('½', '.5')) * 0.97 if m else 1.0
        pos, rot = pk.world(e['id'])
        floor = FLOOR_1 if pos[1] < 3.0 else FLOOR_2
        u = rng.uniform(0.3, max(0.31, length - 0.3))
        if rng.random() < 0.6:
            a, h, y = rng.choice(BASE_DECALS), rng.uniform(0.5, 0.9), floor + 0.25
            w = rng.uniform(0.9, 1.8)
        else:
            a, h, y = rng.choice(PEEL_DECALS), rng.uniform(0.6, 1.1), floor + rng.uniform(1.0, 2.0)
            w = rng.uniform(0.5, 1.0)
        v = qrot(rot, (u, 0.0, -0.097))
        decal(s, g, f'Wall Grime {n:03d}', (round(pos[0] + v[0], 3), round(y + h * 0.3, 3), round(pos[2] + v[2], 3)),
              rot, a, (w, h, 0.42), rng.randint(1, 999), rng.uniform(0.8, 1.0), tint=[0.42, 0.37, 0.28])
        maps.add(a)
        n += 1
        if n >= MAX_DECALS:
            break
    # Outside: rain streaks down the siding and green-black damp along the foundation, on every outer wall piece.
    for e in pk.named(r'Wall_(1st|2nd)_'):
        if e['kind'] != 'models' or e['id'] in pk.gone or n >= MAX_DECALS - 2:
            continue
        m = re.match(r'Wall_\w+?_([\d½]+)x', e['name'])
        length = float(m.group(1).replace('½', '.5')) * 0.97 if m else 1.0
        pos, rot = pk.world(e['id'])
        v = qrot(rot, (length / 2, 0.0, -0.097))
        cx, cz = pos[0] + v[0], pos[2] + v[2]
        if pos[1] < 3.0:
            decal(s, g, f'Foundation Damp {n:03d}', (round(cx, 3), pos[1] + 0.1, round(cz, 3)), rot,
                  rng.choice(BASE_DECALS), (length + 0.3, 1.0, 0.5), rng.randint(1, 999), rng.uniform(0.7, 0.95),
                  tint=[0.22, 0.27, 0.16])
            n += 1
        if rng.random() < 0.7:
            decal(s, g, f'Rain Streak {n:03d}', (round(cx, 3), pos[1] + rng.uniform(1.6, 2.2), round(cz, 3)), rot,
                  rng.choice(CEIL_DECALS), (length * rng.uniform(0.6, 1.0), rng.uniform(1.8, 2.6), 0.5),
                  rng.randint(1, 999), rng.uniform(0.6, 0.9), tint=[0.4, 0.38, 0.3])
            n += 1
    maps |= set(BASE_DECALS + CEIL_DECALS)
    assert n <= MAX_DECALS and len(maps) <= MAX_MAPS, (n, len(maps))
    notes['decals'] = n


RED_I, BOUNCE_I, PORCH_I = 4.5, 0.16, 4.0
POINT_SHADOWS = 2     # the red bulb's and the porch bulb's cube shadows


def lights(s, lay, pk, repo, notes):
    """What still lights the house. Indoor lights scatter nothing into the fog (no fog indoors)."""
    g = s.empty('House Lights', lay, (0, 0, 0), static=True)

    def lamp(name, pos, **kw):
        i = s.light(name, g, pos, **kw)
        e = s.empties[-1]
        e['Light']['Volumetric Intensity'] = 0.0
        return i, e

    # The master bedroom, upstairs at the front: one bare red bulb in the ceiling fitting lighting the whole room,
    # ceiling included, with cube shadows (the bed's, through the window onto the yard), a softer red fill, and the
    # bulb stuttering now and then.
    fit = next(e for e in pk.named(r'Lamp_bedroom$'))
    fit['materials'] = [{'embedded': k.from_mat(repo, PACK + 'Materials/M_Lamps_House.mat',
                                                emissiveColor=[1.0, 0.06, 0.03], emissiveStrength=6.0)}]
    fx, fy, fz = pk.world(fit['id'])[0]
    _, e = lamp('Red Bulb', (fx, fy - 0.25, fz), intensity=RED_I, rng=7.0, color=RED, shadows=True, softness=1.4)
    e.update(k.light_flicker(Mode=0, MinInterval=7.0, MaxInterval=20.0, Depth=0.85, Seed=31))
    _, e = lamp('Red Fill', (fx - 0.6, fy - 0.9, fz), intensity=RED_I * 0.12, rng=4.2, color=RED)
    e.update(k.light_flicker(Mode=0, MinInterval=7.0, MaxInterval=20.0, Depth=0.85, Seed=31))

    # The kitchen's fluorescent tube, dying.
    tube = next(e for e in pk.named(r'Lamp_wall_fluorescent$') if pk.world(e['id'])[0][1] > 1.0)
    tube.pop('materials', None)         # its own, lit material
    tx, ty, tz = pk.world(tube['id'])[0]
    _, e = lamp('Kitchen Tube', (tx + 0.6, ty - 0.1, tz + 0.5), intensity=3.2, rng=6.0, color=(0.82, 0.93, 1.0))
    e.update(k.light_flicker(Mode=0, MinInterval=1.5, MaxInterval=6.0, Depth=1.0, Seed=7))

    # Candles burned down on the living room floor, by the couch: someone's been here.
    couch = next(e for e in pk.props() if e['name'].startswith('Couch_3seat'))
    cx, _, cz = couch['position']
    s.stamp(PROPS_IN + 'LP_candle_triple.prefab', g, (round(cx + 1.0, 3), FLOOR_1, round(cz - 1.1, 3)), yaw=30.0)
    s.stamp(PROPS_IN + 'LP_candle_single.prefab', g, (round(cx + 1.35, 3), FLOOR_1, round(cz - 0.8, 3)))
    s.stamp(PROPS_IN + 'LP_candle_small.prefab', g, (round(cx + 0.7, 3), FLOOR_1, round(cz - 1.35, 3)))
    _, e = lamp('Candle Glow', (round(cx + 1.0, 3), FLOOR_1 + 0.5, round(cz - 1.1, 3)), intensity=1.5, rng=5.0,
                color=(1.0, 0.55, 0.22))
    e.update(k.light_flicker(Mode=2, MinInterval=0.1, MaxInterval=0.4, Depth=0.25, Seed=11))
    # The moon's bounce: a faint cold fill on each storey so the dark rooms read as shapes, not black.
    n = 0
    for y in (FLOOR_1 + 1.6, FLOOR_2 + 1.6):
        for x in (-37.4, -32.6):
            for z in (-12.6, -6.4, -0.2, 6.0, 11.6):
                lamp(f'Moon Bounce {n:02d}', (x, y, z), intensity=BOUNCE_I, rng=5.5, color=(0.55, 0.66, 0.9))
                n += 1
    notes['house lights'] = 4 + n


def porch_light(s, lay, notes):
    """The lamp by the front door, its bulb on its way out (the other one is dead)."""
    g = s.empty('Porch Light', lay, (0, 0, 0), static=True)
    for z in (-1.018, 2.891):       # where the pack's demo street had them, either side of the door
        r = s.stamp(PROPS_OUT + 'Lamp_wall_original.prefab', g, (-31.04, 2.316, z), yaw=-90.0)
        if z > 0:
            for m in s.models:
                if m['parentId'] == r:
                    m['materials'] = [dict(OFF_OUT)]
    s.light('Porch Bulb', g, (-30.75, 2.45, -1.018), intensity=PORCH_I, rng=8.0, kelvin=2300.0, shadows=True)
    e = s.empties[-1]
    e['Light']['Volumetric Intensity'] = 1.0        # outdoors: its glow in the night air
    e.update(k.light_flicker(Mode=0, MinInterval=2.5, MaxInterval=9.0, Depth=1.0, Seed=5))
    notes['porch light'] = 1


def add_house(s, root, repo, pack_path, notes):
    """The house: the pack snapshot under 'Abandoned House' / 'Pack Layout', trashed and dressed."""
    pk = Pack(pack_path)
    rng = random.Random(301)
    DEAD_SCREEN.update(k.from_mat(repo, PACK + 'Materials/M_Devices_On.mat', emissiveColor=[0.0, 0.0, 0.0],
                                  emissiveStrength=0.0))
    broken = trash_house(pk, rng, notes)
    house = s.empty('Abandoned House', root, HOUSE_AT, yaw=HOUSE_YAW, static=True)
    lay = s.empty('Pack Layout', house, (-PACK_C[0], -PACK_C[1], -PACK_C[2]), static=True)
    remap = {}
    for e in pk.ents:
        if e['id'] not in pk.gone:
            remap[e['id']] = s._next()
    for e in pk.ents:
        if e['id'] in pk.gone:
            continue
        c = json.loads(json.dumps(e))
        kind = c.pop('kind')
        c['id'] = c['order'] = remap[e['id']]
        c['parentId'] = remap.get(e['parentId'], lay)
        getattr(s, kind).append(c)
    return pk, lay, broken, rng


# --- the yard ---------------------------------------------------------------------------------------------------
def fence_line(s, g, path, a, b, seg, rng, H, missing=0.12, lean=0.15, name='Fence', gaps=()):
    """Segments of `seg` metres from a to b (world x, z); some gone, some leaning, `gaps` (t0, t1 along) left open."""
    dx, dz = b[0] - a[0], b[1] - a[1]
    length = math.hypot(dx, dz)
    yaw = math.degrees(math.atan2(-dz, dx))
    n = 0
    t = 0.0
    while t + seg <= length + 0.05:
        if any(g0 <= t + seg / 2 <= g1 for g0, g1 in gaps) or rng.random() < missing:
            t += seg
            continue
        x, z = a[0] + dx / length * t, a[1] + dz / length * t
        tilt = (0.0, 0.0)
        if rng.random() < lean:
            tilt = (round(rng.uniform(-18.0, 18.0), 1), 0.0)
        s.stamp(path, g, (round(x, 3), round(H(x, z) - 0.03, 3), round(z, 3)), yaw=round(yaw, 2), tilt=tilt,
                name=name)
        n += 1
        t += seg
    return n


def add_yard(s, root, repo, H, forest_paths, notes):
    """Front yard, driveway, side yards and backyard."""
    rng = random.Random(302)
    yard = s.empty('Yard', root, (0, 0, 0), static=True)

    # The front walk: cracked slabs from the porch steps to the gate, one gone.
    walk = s.empty('Front Walk', yard, (0, 0, 0), static=True)
    z = 20.45
    i = 0
    while z > LOT[2] + 0.2:
        if i != 3:
            s.stamp(GROUND_P + 'Road_walkway_piece.prefab', walk, (WALK_X, round(LOT_Y - 0.004, 3), round(z - 0.97, 3)),
                    yaw=round(rng.uniform(-1.5, 1.5), 2))
        z -= 1.95
        i += 1
    # The driveway and a dead car on it.
    drive = s.empty('Driveway', yard, (0, 0, 0), static=True)
    for zi in range(3):
        for xi in range(2):
            x = DRIVE[0] + 1.95 * xi
            zz = DRIVE[3] - 4.85 * zi
            s.stamp(GROUND_P + 'Road_driveway_piece.prefab', drive, (round(x, 3), LOT_Y - 0.004, round(zz, 3)),
                    yaw=90.0)
    s.stamp(PROPS_OUT + 'Car_gray.prefab', drive, (-17.4, round(LOT_Y - 0.05, 3), 18.6), yaw=-4.0, tilt=(0.0, 1.8),
            name='Dead Car')
    # years of dust, sap and needles on it: one decal down onto the roof, bonnet and boot, one onto each side
    down = axis_q((1, 0, 0), -90.0)
    for i, (pos, rot, size) in enumerate((
            ((-17.4, 1.6, 18.6), qmul(axis_q((0, 1, 0), -4.0), down), (2.3, 4.7, 1.8)),
            ((-16.3, 0.7, 18.6), axis_q((0, 1, 0), 86.0), (4.6, 1.3, 0.8)),
            ((-18.5, 0.7, 18.6), axis_q((0, 1, 0), -94.0), (4.6, 1.3, 0.8)))):
        decal(s, drive, f'Car Grime {i}', pos, rot, FLOOR_DECALS[i], size, 40 + i, 0.95, tint=[0.36, 0.33, 0.25])

    # Fences: a broken white picket out front with the gate gone; the backyard's tall wooden fence, rotten through.
    fences = s.empty('Fences', yard, (0, 0, 0), static=True)
    nf = 0
    picket = PROPS_OUT + 'Fence_white_2x.prefab'
    if s.prefab(PROPS_OUT + 'Fence_small_2x_R.prefab'):
        picket = PROPS_OUT + 'Fence_small_2x_R.prefab'
    nf += fence_line(s, fences, picket, (LOT[0], LOT[2]), (DRIVE[0] - 0.4, LOT[2]), 2.07, rng, H, name='Picket')
    nf += fence_line(s, fences, picket, (DRIVE[1] + 0.4, LOT[2]), (LOT[1], LOT[2]), 2.07, rng, H, name='Picket',
                     gaps=((WALK_X - DRIVE[1] - 1.6, WALK_X - DRIVE[1] + 1.2),))
    wood = PROPS_OUT + 'Fence_wooden_2x.prefab'
    for a, b in (((LOT[0], 30.5), (LOT[0], LOT[3])), ((LOT[0], LOT[3]), (LOT[1], LOT[3])),
                 ((LOT[1], LOT[3]), (LOT[1], 30.5)), ((LOT[0], 30.5), (HOUSE_BOX[0], 30.5)),
                 ((HOUSE_BOX[1], 30.5), (LOT[1], 30.5))):
        nf += fence_line(s, fences, wood, a, b, 2.07, rng, H, missing=0.1, lean=0.2, name='Wood Fence')
    notes['fence panels'] = nf

    # Yard junk.
    junk = s.empty('Yard Junk', yard, (0, 0, 0), static=True)
    put = lambda p, x, z, yaw=0.0, tilt=(0.0, 0.0), dy=0.0, name=None: s.stamp(  # noqa: E731
        p, junk, (x, round(H(x, z) + dy, 3), z), yaw=yaw, tilt=tilt, name=name)
    put(PROPS_OUT + 'Mailbox.prefab', WALK_X + 1.6, LOT[2] + 0.5, yaw=-90.0, tilt=(0.0, 14.0))
    put(PROPS_OUT + 'Trashbin.prefab', -15.9, 27.5, yaw=70.0, tilt=(0.0, 90.0), dy=0.3)      # knocked over
    for x, z in ((-16.4, 26.3), (-15.6, 28.9), (-17.0, 28.1), (16.2, 22.4)):
        put(PROPS_OUT + 'Trashbag.prefab', x, z, yaw=rng.uniform(0, 360))
    # The porch: a table and stools left out, one blown over.
    put(PROPS_OUT + 'Porch_table.prefab', -4.0, 21.6, dy=0.2, yaw=12.0)
    put(PROPS_OUT + 'Porch_stool.prefab', -5.0, 21.4, dy=0.2, yaw=80.0)
    put(PROPS_OUT + 'Porch_stool.prefab', -2.8, 21.2, dy=0.5, yaw=200.0, tilt=(0.0, 90.0))
    # The backyard: loungers and a sun umbrella from a summer a long time ago.
    put(PROPS_OUT + 'Sun_table.prefab', 4.0, 38.5, yaw=15.0)
    put(PROPS_OUT + 'Sun_umbrella.prefab', 5.6, 39.6, yaw=-30.0, tilt=(84.0, 0.0), dy=0.25, name='Fallen Umbrella')
    put(PROPS_OUT + 'Sun_chair.prefab', 2.6, 37.8, yaw=110.0)
    put(PROPS_OUT + 'Sun_chair.prefab', 6.1, 37.1, yaw=40.0, tilt=(0.0, 90.0), dy=0.26)
    put(PROPS_OUT + 'Roadblock_wooden.prefab', -9.0, 41.5, yaw=25.0)

    # The forest coming back: tall dry grass and weeds along the foundation, ivy up the walls, saplings and ferns in
    # the yard, leaf litter and fallen branches everywhere.
    wild = s.empty('Overgrowth', yard, (0, 0, 0), static=True)
    tall = [p for p in (PROPS_OUT + 'Plant_tall_grass.prefab', PROPS_OUT + 'Plant_drygrass_set.prefab') if s.prefab(p)]
    x0, x1, z0, z1 = HOUSE_BOX
    n_w = 0
    for _ in range(140):
        side = rng.randrange(4)
        if side < 2:
            x, z = rng.uniform(x0, x1), (z0 - rng.uniform(0.1, 0.9) if side == 0 else z1 + rng.uniform(0.1, 0.9))
        else:
            x, z = (x0 - rng.uniform(0.1, 0.9) if side == 2 else x1 + rng.uniform(0.1, 0.9)), rng.uniform(z0, z1)
        if abs(x - WALK_X) < 1.4 and z < z0 + 1:
            continue
        if tall:
            s.stamp(rng.choice(tall), wild, (round(x, 2), round(H(x, z), 3), round(z, 2)),
                    yaw=round(rng.uniform(0, 360), 1), scale=round(rng.uniform(0.8, 1.4), 2))
            n_w += 1
    ivy = [p for p in (PROPS_OUT + 'Plant_climber_long.prefab', PROPS_OUT + 'Plant_climber_short.prefab') if s.prefab(p)]
    for x, z, yaw in ((-14.95, 23.5, -90.0), (-14.95, 29.0, -90.0), (14.95, 27.0, 90.0), (-6.0, 32.25, 180.0),
                      (8.5, 32.25, 180.0)):
        if ivy:
            s.stamp(rng.choice(ivy), wild, (x, round(H(x, z), 3), z), yaw=yaw, scale=round(rng.uniform(0.9, 1.2), 2))
    yard_plants = [p for p in forest_paths['yard'] if s.prefab(p)]
    litter = [p for p in forest_paths['litter'] if s.prefab(p)]
    saplings = [p for p in forest_paths['saplings'] if s.prefab(p)]
    n_p = 0
    for _ in range(420):
        x, z = rng.uniform(LOT[0] + 0.5, LOT[1] - 0.5), rng.uniform(LOT[2] + 0.5, LOT[3] - 0.5)
        if in_house(x, z, 0.3) or abs(x - WALK_X) < 0.8 and z < HOUSE_BOX[2] or \
                (DRIVE[0] - 0.3 < x < DRIVE[1] + 0.3 and DRIVE[2] < z < DRIVE[3]):
            continue
        edge = min(x - LOT[0], LOT[1] - x, z - LOT[2], LOT[3] - z)
        if rng.random() < 0.55:      # litter anywhere
            pool = litter
        elif edge < 4.0 or z > 33.0:  # weeds and ferns creeping in from the fence lines, thickest at the back
            pool = yard_plants
        else:
            continue
        if not pool:
            continue
        s.stamp(rng.choice(pool), wild, (round(x, 2), round(H(x, z), 3), round(z, 2)),
                yaw=round(rng.uniform(0, 360), 1), scale=round(rng.uniform(0.7, 1.2), 2))
        n_p += 1
    for x, z in ((-12.0, 41.0), (13.5, 43.0), (17.5, 36.0), (-18.5, 34.5)):
        if saplings:
            s.stamp(rng.choice(saplings), wild, (x, round(H(x, z) - 0.05, 3), z), yaw=rng.uniform(0, 360),
                    scale=round(rng.uniform(0.3, 0.45), 2), name='Sapling', lod=forest_paths.get('tree_lod'))
    notes.update({'foundation weeds': n_w, 'yard plants': n_p})


def grass_value(x, z, v):
    """The yard's grass: an overgrown lawn, full across the lot but for the house, the walk and the driveway."""
    if not in_lot(x, z, 1.0):
        return v
    if in_house(x, z, 0.15):
        return 0.0
    keep = 1.0
    if HOUSE_BOX[2] - 0.6 > z >= LOT[2] - 1.0 and abs(x - WALK_X) < 0.75:
        keep = 0.12                              # weeds in the walk's cracks
    if DRIVE[0] < x < DRIVE[1] and DRIVE[2] - 1.0 < z < DRIVE[3]:
        keep = 0.18
    if -18.6 < x < -16.2 and 16.3 < z < 20.9:
        keep = 0.0                               # under the car
    t = smoothstep(0.0, 1.0, lot_dist(x, z))
    return v * t + keep * (1.0 - t)


def reflection_probes(s, root):
    """One per storey, inside the walls (world-aligned boxes over the house's footprint)."""
    cx, cz = (HOUSE_BOX[0] + HOUSE_BOX[1]) / 2, (HOUSE_BOX[2] + 0.7 + HOUSE_BOX[3]) / 2
    for name, y, h in (('House Probe Ground', LOT_Y + 1.7, 3.0), ('House Probe Upstairs', LOT_Y + 4.6, 2.8)):
        s.empty(name, root, (cx, y, round(cz, 2)), static=True,
                **{'Reflection Probe': {'Importance': 2.0, 'Size': [29.4, h, 11.4], 'Capture Scene': True}})
