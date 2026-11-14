# The abandoned house in the middle of the Forest, built by build_forest.py.
#
# The house is the Suburb pack demo's two-storey House_01, furnished (forest_house_pack.json: its walls, floors,
# roof, porch, doors and every prop, in the pack's own space, without the basement - the Forest's floor is a
# heightfield and can't be cut). It stands south of the start clearing, its porch facing the player across the
# clearing, on a lot the terrain is eased flat for: a front yard with a cracked walk to a broken picket fence, a
# driveway with a dead car, and a fenced backyard the forest is taking back.
#
# Abandoned and trashed: a third of the glass is gone and the chairs and lamps are
# knocked over, furniture shoved about, pictures hang crooked, what was worth taking is gone; rubbish, papers and
# books kicked against the walls, leaves blown in; the plaster cracked off the door and window corners, along the wall
# heads and across the ceilings, a leak's stain here and there. Every piece is fitted by its measured shape
# (house_fit.py): nothing in a wall, in another piece or in a doorway, and every door opens all the way. The power is mostly out: a dying porch light, a stuttering fluorescent tube in
# the kitchen, burned-down candles on the living room floor, and the master bedroom upstairs, at the front, lit red.
#
# Interior work is authored in the pack's space and parented under the house's 'Pack Layout'; the yard in world
# metres. Every random choice comes from its own seeded stream, so a rebuild gives the same house.
import json
import math
import random
import re

import sys

import house_fit as hf
import house_lights
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
GATE = 0.8                              # half the gateway in the picket fence, on the walk
WALK_X = 1.0                            # the front walk's centre line: the gap in the porch rail (pilars at x 0.03 / 1.97)
DRIVE = (-19.6, -15.6, 11.0, 25.6)      # the driveway (x0, x1, z0, z1)
OUTER_DOORS = ((1.0, 24.0), (14.6, 26.5))   # the front and side doors (world x, z)
PORCH_DECK = (-6.76, 8.76, 20.64, 24.52, 0.165)   # the front porch's deck (world x0, x1, z0, z1, top y)

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


# --- fitting (house_fit.py): every piece by its measured bounds, in the pack's space --------------------------------
# Pieces that hang on a wall or from a ceiling, or are part of the room: never shoved, never a support for clutter.
FIXED = ('Painting_wide', 'Painting_large', 'Painting_photo', 'Painting_small', 'Painting_A', 'Painting_C',
         'Painting_D', 'Painting_E', 'Painting_F', 'Painting_G', 'Mirror', 'Curtains_', 'Lamp_wall', 'Lamp_ceiling',
         'Lamp_chandelier', 'Lamp_bedroom', 'Lamp_kitchen', 'Ventilationgrid', 'Lightswitch', 'Shelf_',
         'To_towel_hanging', 'To_showercurtain', 'Kitcabinet_upper', 'Wooden_wallpilar', 'Clothcloset_with_wall',
         'Fireplace_withwall', 'Carpet_', 'To_toilet_shelf')
STRUCTURE_SKIP = ('Floor_', 'Ceiling_', 'Porch_floor', 'Carpet', 'Roof', 'Stair_carpet')


def stem(e):
    return re.sub(r' \(\d+\)$', '', e['name'])


class Fit:
    """The house's solid shapes in pack space - walls and stairs by their colliders, furniture by its measured mesh
    bounds - and the keep-outs: every doorway with the swing of its door, and the stairs' runs."""
    def __init__(self, pk):
        self.pk = pk
        self.B = hf.bounds()
        self.props = {e['id']: e for e in pk.props()}
        self.structure = []
        for e in pk.ents:
            c = e.get('collider')
            if not c or e['id'] in pk.gone or c.get('isTrigger') or e.get('Openable') or 'halfExtents' not in c:
                continue
            if self._prop_of(e['id']) is not None or e['name'].startswith(STRUCTURE_SKIP):
                continue
            pos, rot = pk.world(e['id'])
            he = c.get('halfExtents', [0.3, 0.3, 0.3])
            ctr = c.get('center', [0, 0, 0])
            lo = [ctr[i] - he[i] for i in range(3)]
            hi = [ctr[i] + he[i] for i in range(3)]
            self.structure.append(hf.Box.of(pos, rot, self.wscale(e['id']), lo, hi, tag=e['name']))
        self.boxes = {i: self.prop_boxes(i) for i in self.props if i not in pk.gone}
        self.floors = floor_rects(pk)
        self.zones = self.door_zones()
        self.clutter_zones = self.door_zones(wide=True)
        # pairs that already touch in the pack's own layout (a chair tucked under its table): left as they are
        self.touching = {}
        ids = [i for i in self.boxes if not stem(self.props[i]).startswith(FIXED)]
        for n, i in enumerate(ids):
            for j in ids[n + 1:]:
                if self.hits(self.boxes[i], self.boxes[j], 0.0):
                    self.touching[(min(i, j), max(i, j))] = self.depth(self.boxes[i], self.boxes[j])
        self.riders = self.find_riders()

    def _prop_of(self, i):
        while i in self.pk.by:
            if i in self.props:
                return i
            i = self.pk.by[i]['parentId']
        return None

    def wscale(self, i):
        sc = [1.0, 1.0, 1.0]
        while i in self.pk.by:
            s = self.pk.by[i].get('scale', [1, 1, 1])
            sc = [sc[k] * s[k] for k in range(3)]
            i = self.pk.by[i]['parentId']
        return sc

    def prop_boxes(self, i):
        """The prop's models as boxes (its own and its children's)."""
        out, todo = [], [i]
        while todo:
            j = todo.pop()
            if j in self.pk.gone:
                continue
            e = self.pk.by[j]
            todo.extend(self.pk.kids.get(j, []))
            b = self.B.get(e.get('path', ''))
            if e['kind'] == 'models' and b and e.get('active', True):
                pos, rot = self.pk.world(j)
                out.append(hf.Box.of(pos, rot, self.wscale(j), b[0], b[1], tag=e['name']))
        return out

    @staticmethod
    def depth(xs, ys):
        return max((hf.penetration(a, b) for a in xs for b in ys), default=0.0)

    @staticmethod
    def hits(xs, ys, margin):
        return any(hf.overlap(a, b, margin) for a in xs for b in ys)

    def door_zones(self, wide=False):
        """A keep-out per doorway: the opening's width (plus a hand) by the leaf's width either side of the wall - the
        swing and the way through."""
        zones = []
        for e in self.pk.ents:
            o, c = e.get('Openable'), e.get('collider')
            if not o or not c or e['id'] in self.pk.gone:
                continue
            he = c['halfExtents']
            if he[1] * 2 < 1.8 or he[0] * 2 < 0.6:
                continue
            pos, rot = self.pk.world(e['id'])
            ctr = c.get('center', [0, 0, 0])
            w = he[0] * 2
            mid = hf.qrot(rot, ctr)
            cx, cz = pos[0] + mid[0], pos[2] + mid[2]
            along, through = hf.qrot(rot, (1, 0, 0)), hf.qrot(rot, (0, 0, 1))
            # the swing and the way through; rubbish keeps further off (a bag by the frame crowds the door)
            hw, hd = (w / 2 + 0.55, w + 0.45) if wide else (w / 2 + 0.12, w + 0.15)
            pts = [(cx + along[0] * sa * hw + through[0] * sd * hd, cz + along[2] * sa * hw + through[2] * sd * hd)
                   for sa in (-1, 1) for sd in (-1, 1)]
            y0 = pos[1] + mid[1] - he[1]
            zones.append(hf.Zone(hf.hull(pts), y0, y0 + 2.2, self.pk.by[e['parentId']]['name']))
        # the stairs: their run and a landing at each end
        for e in self.pk.ents:
            if e['kind'] == 'models' and e['name'].startswith('Stair') and e['id'] not in self.pk.gone:
                b = self.B.get(e.get('path', ''))
                if not b:
                    continue
                pos, rot = self.pk.world(e['id'])
                box = hf.Box.of(pos, rot, self.wscale(e['id']), b[0], b[1])
                pts = box.xz()
                cx = sum(p[0] for p in pts) / len(pts)
                cz = sum(p[1] for p in pts) / len(pts)
                grown = [(cx + (p[0] - cx) * 1.0 + math.copysign(0.9, p[0] - cx),
                          cz + (p[1] - cz) * 1.0 + math.copysign(0.9, p[1] - cz)) for p in pts]
                zones.append(hf.Zone(hf.hull(grown), box.ylo() - 0.5, box.yhi() + 2.0, e['name']))
        return zones

    def find_riders(self):
        """What stands on what: a prop whose underside is on another's top (a lamp on a side table) rides it."""
        riders = {}
        for i, bs in self.boxes.items():
            e = self.props[i]
            if not bs or stem(e).startswith(FIXED):
                continue
            bottom = min(b.ylo() for b in bs)
            floor = FLOOR_1 if bottom < 3.0 else FLOOR_2
            if bottom < floor + 0.06:
                continue
            best, top = None, -1e9
            for j, cs in self.boxes.items():
                if j == i or not cs or stem(self.props[j]).startswith(('Carpet_', 'Curtains_')):
                    continue
                for c in cs:
                    t = c.yhi()
                    if bottom - 0.08 <= t <= bottom + 0.05 and t > top and \
                            any(hf.poly_overlap(c.xz(), b.xz()) for b in bs):
                        best, top = j, t
            if best is not None:
                riders.setdefault(best, []).append(i)
        return riders

    def clear(self, i, boxes, ignore=(), zones=True, margin=0.008, nested=True):
        """Can prop `i` stand as `boxes`? Not into the walls, other furniture, or a doorway it wasn't already in, and
        all of it over the house's floors (a lamp knocked over by a wall can't fall through it). With `nested`, a piece
        the pack had it tucked into (a chair under its table) may stay as deep, no deeper."""
        if not self.over_floor(boxes):
            return False
        for b in boxes:
            for s in self.structure:
                if hf.overlap(b, s, margin):
                    return False
        skip = set(ignore) | {i}
        for j, cs in self.boxes.items():
            if j in skip or stem(self.props[j]).startswith(('Carpet_', 'Curtains_')):
                continue
            was = self.touching.get((min(i, j), max(i, j)))
            if was is not None and nested:
                if self.depth(boxes, cs) > was + 0.005:
                    return False
            elif self.hits(boxes, cs, margin):
                return False
        if zones:
            before = self.boxes.get(i, [])
            for z in self.zones:
                if any(z.hits(b) for b in boxes) and not any(z.hits(b) for b in before):
                    return False
        return True

    def free_spot(self, boxes, others, margin=0.006):
        """Can a new piece (clutter) go here? Clear of the walls, the furniture, the doorways, the stairs and `others`."""
        for b in boxes:
            for s in self.structure:
                if hf.overlap(b, s, margin):
                    return False
            for z in self.clutter_zones:
                if z.hits(b):
                    return False
            for j, cs in self.boxes.items():
                if stem(self.props[j]).startswith('Carpet_'):
                    continue
                if any(hf.overlap(b, c, margin) for c in cs):
                    return False
            if any(hf.overlap(b, o, margin) for o in others):
                return False
        return True

    def over_floor(self, boxes):
        """Every corner of `boxes` over a floor tile of the storey they stand on."""
        for b in boxes:
            y = b.ylo()
            for x, z in b.xz():
                if not any(r[0] - 0.05 <= x <= r[1] + 0.05 and r[2] - 0.05 <= z <= r[3] + 0.05 and abs(r[4] - y) < 1.2
                           for r in self.floors):
                    return False
        return True

    def near_corner(self, x, z, y, d):
        """Walls within `d` along both X and Z: a corner (where bags and boxes get pushed, and nobody walks)."""
        def hit(px, pz):
            probe = hf.Box([px, y + 0.3, pz], [[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0.04, 0.2, 0.04])
            return any(hf.overlap(probe, s) for s in self.structure)
        return (hit(x - d, z) or hit(x + d, z)) and (hit(x, z - d) or hit(x, z + d))

    def near_wall(self, x, z, y, d):
        probe = hf.Box([x, y + 0.3, z], [[1, 0, 0], [0, 1, 0], [0, 0, 1]], [d, 0.2, d])
        return any(hf.overlap(probe, s) for s in self.structure)

    # --- moving a prop rigidly, its riders with it
    def set_world(self, i, pos, rot):
        e = self.pk.by[i]
        ppos, prot = self.pk.world(e['parentId']) if e['parentId'] in self.pk.by else ([0, 0, 0], [0, 0, 0, 1])
        inv = hf.qinv(prot)
        lp = hf.qrot(inv, [pos[k] - ppos[k] for k in range(3)])
        e['position'] = [round(v, 4) for v in lp]
        e['rotation'] = [round(v, 6) for v in hf.qmul(inv, rot)]

    def group_of(self, i):
        out, todo = [], [i]
        while todo:
            j = todo.pop()
            out.append(j)
            todo.extend(r for r in self.riders.get(j, []) if r not in self.pk.gone)
        return out

    def transform_group(self, ids, pivot, turn_q, shift, lift=0.0):
        """Turn the group about `pivot` (world) by `turn_q`, then shift; returns the old placements to undo with."""
        old = {}
        for j in ids:
            pos, rot = self.pk.world(j)
            old[j] = (list(self.pk.by[j]['position']), list(self.pk.by[j]['rotation']))
            rel = [pos[k] - pivot[k] for k in range(3)]
            v = hf.qrot(turn_q, rel)
            npos = [pivot[0] + v[0] + shift[0], pivot[1] + v[1] + lift, pivot[2] + v[2] + shift[1]]
            self.set_world(j, npos, hf.qmul(turn_q, rot))
        return old

    def undo(self, old):
        for j, (p, r) in old.items():
            self.pk.by[j]['position'], self.pk.by[j]['rotation'] = p, r

    def centre(self, boxes):
        xs = [b.c[0] for b in boxes]
        zs = [b.c[2] for b in boxes]
        return [sum(xs) / len(xs), 0.0, sum(zs) / len(zs)]


def open_sealed_doors(pk, notes):
    """The pack hangs one upstairs door on a solid wall piece, sealing the room behind it: any door whose wall has no
    opening gets the door wall of the same length in its place (same transform, its own colliders round the opening)."""
    walls = [e for e in pk.ents if e['kind'] == 'models' and re.match(r'Wall_Interior_\d+x$', stem(e))]
    fixed = 0
    next_id = max(pk.by) + 1
    for leaf in pk.ents:
        if not leaf.get('Openable') or not pk.by[leaf['parentId']]['name'].startswith('Door_interior'):
            continue
        lp, _ = pk.world(leaf['id'])
        for w in walls:
            if w['id'] in pk.gone:
                continue
            wp, wr = pk.world(w['id'])
            loc = qrot(hf.qinv(wr), [lp[k] - wp[k] for k in range(3)])
            length = float(re.match(r'Wall_Interior_(\d+)x', w['name']).group(1)) * 0.97
            if not (-0.3 < loc[2] < 0.3 and 0.2 < loc[0] < length - 0.2 and -0.6 < loc[1] < 0.6):
                continue
            template = next((e for e in pk.ents if e['name'] == stem(w) + '_door' and e['kind'] == 'models'), None)
            if not template:
                continue
            copy = json.loads(json.dumps(template))
            copy.update({'id': next_id, 'order': next_id, 'parentId': w['parentId'], 'name': stem(w) + '_door (sealed fix)',
                         'position': list(w['position']), 'rotation': list(w['rotation'])})
            if len(w.get('materials', [])) == len(template.get('materials', [])):
                copy['materials'] = json.loads(json.dumps(w['materials']))
            pk.ents.append(copy)
            pk.by[next_id] = copy
            pk.kids.setdefault(copy['parentId'], []).append(next_id)
            parent = next_id
            next_id += 1
            for cid in pk.kids.get(template['id'], []):
                c = json.loads(json.dumps(pk.by[cid]))
                c.update({'id': next_id, 'order': next_id, 'parentId': parent})
                pk.ents.append(c)
                pk.by[next_id] = c
                pk.kids.setdefault(parent, []).append(next_id)
                next_id += 1
            pk.remove(w['id'])
            fixed += 1
    notes['sealed doors opened'] = fixed


def clear_walls(pk, notes):
    """A real house has a ceiling light per room and a wall light only where it earns its place: every sconce goes,
    the bathroom lights stay only over a mirror, and of the tubes only the kitchen's one under its wall cabinets stays.
    The pack's own dead switch goes too: each room has one working switch by its door (house_lights.py).
    They hang on their own (no back box to leave behind), so the wall is clean where they were."""
    props = [e for e in pk.props() if e['id'] not in pk.gone]
    mirrors = [pk.world(e['id'])[0] for e in props if stem(e).startswith('Mirror')]
    cabinets = [pk.world(e['id'])[0] for e in props if stem(e).startswith('Kitcabinet_upper')]
    tubes = [e for e in props if stem(e) == 'Lamp_wall_fluorescent']
    keep_tube = min((e for e in tubes if pk.world(e['id'])[0][1] > 1.0),
                    key=lambda e: min((math.dist(pk.world(e['id'])[0], c) for c in cabinets), default=9.0), default=None)
    n = 0
    for e in props:
        s, p = stem(e), pk.world(e['id'])[0]
        if s in ('Lamp_wall_chan', 'Lightswitch') or (s == 'Lamp_wall_fluorescent' and e is not keep_tube) or \
                (s == 'Lamp_wall_toilet' and not any(math.hypot(p[0] - m[0], p[2] - m[2]) < 0.7 for m in mirrors)):
            pk.remove(e['id'])
            n += 1
    notes['wall fittings taken down'] = n


def ease_side_door(pk, notes):
    """The side door's latch jamb (its wall's 'Collider 2') stood 2.5 cm into the opening, so brushing past the latch
    side slowed the player to half speed (the doors test's latch graze): it is trimmed back to the opening's edge."""
    leaf = next((e for e in pk.ents if e.get('Openable') and pk.by[e['parentId']]['name'] == 'Door_exterior_white'), None)
    if leaf is None:
        return
    lp, _ = pk.world(leaf['id'])
    for e in pk.ents:
        par = pk.by.get(e['parentId'])
        if not par or par['name'] != 'Wall_1st_3x_door_B' or not e['name'].endswith('Collider 2') or not e.get('collider'):
            continue
        p, _ = pk.world(e['id'])
        c = e['collider']
        c.setdefault('center', [0.0, 0.0, 0.0])
        x0 = p[0] + c['center'][0] - c['halfExtents'][0]
        if abs(p[2] - lp[2]) > 0.3 or not (0.0 < x0 - lp[0] < 1.2):
            continue
        c['center'][0] = round(c['center'][0] + 0.0125, 5)
        c['halfExtents'][0] = round(c['halfExtents'][0] - 0.0125, 5)
        notes['side door jamb eased'] = 1


def floor_the_stairwell(pk, notes):
    """Without its basement the pack's cellar stairwell, behind the ground-floor door under the stairs, is a hole down
    to the forest floor: it gets a floor of the hall's boards (a 3 x 5 tile, the same as the ones round it) and is a
    store room now."""
    hole = (-38.8, -35.89, -7.76, -2.91)
    if any(r[0] < hole[1] - 0.1 and r[1] > hole[0] + 0.1 and r[2] < hole[3] - 0.1 and r[3] > hole[2] + 0.1
           for r in floor_rects(pk) if r[4] < 3.0):
        return
    template = next(e for e in pk.ents if e['kind'] == 'models' and stem(e) == 'Floor_3x_5x'
                    and pk.by[e['parentId']]['name'] == 'Floors_1st')
    c = json.loads(json.dumps(template))
    i = max(pk.by) + 1
    c.update({'id': i, 'order': i, 'name': 'Floor_3x_5x (stairwell fill)', 'position': [hole[0], 0.485, hole[3]],
              'rotation': [0.0, 0.0, 0.0, 1.0]})
    pk.ents.append(c)
    pk.by[i] = c
    pk.kids.setdefault(c['parentId'], []).append(i)
    notes['stairwell floored'] = 1


def trash_house(pk, rng, notes):
    """Edits to the pack's own pieces: broken glass, doors left open, furniture knocked about, things taken. Every move
    is fitted: nothing goes into a wall, another piece or a doorway, what stood on a piece moves with it (or falls to the
    floor when it goes over), and whatever is knocked over lies on the floor by its own shape."""
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

    # Doors: the front one stuck ajar, the side one wide open, the rest a mix of shut, ajar and open. Ajar is a start
    # pose (Start Ajar), never a short swing: every door still opens all the way.
    n_open = 0
    for e in pk.ents:
        o = e.get('Openable')
        if not o:
            continue
        o.pop('Amount', None) if o.get('Amount') == 90.0 else None
        frame = pk.by[e['parentId']]['name']
        if frame.startswith('Door_exterior_paneled'):
            o.update({'Start Open': True, 'Start Ajar': 0.35})
        elif frame.startswith('Door_exterior_white'):
            o['Start Open'] = True
        elif frame.startswith('Door_'):
            u = rng.random()
            if u < 0.45:
                o['Start Open'] = True
            elif u < 0.75:
                o.update({'Start Open': True, 'Start Ajar': round(rng.uniform(0.2, 0.45), 2)})
        n_open += o.get('Start Open', False)
    notes['doors left open'] = n_open

    # The screens are dead; the fittings keep their lit materials - their circuits (house_lights.py) take the glow
    # out while they're switched off.
    for e in pk.ents:
        if e['kind'] == 'models' and stem(e).startswith('D_'):
            e['materials'] = [{'embedded': dict(DEAD_SCREEN)}]

    # No basement under this house: its pipes go.
    for e in pk.props():
        if stem(e).startswith('Basement_pipe'):
            pk.remove(e['id'])

    # What was worth carrying off goes first, so nothing is fitted round it.
    taken = 0
    for e in pk.props():
        s = stem(e)
        if e['id'] in pk.gone:
            continue
        if (s.startswith('Plant_interior') and rng.random() < 0.5) or \
                (s.startswith(('D_TV', 'D_radio', 'D_alarmclock')) and rng.random() < 0.65) or \
                (s.startswith(('KP_', 'LP_vase', 'LP_bowl', 'FP_vase', 'LP_candle')) and rng.random() < 0.4):
            pk.remove(e['id'])
            taken += 1

    fit = Fit(pk)
    knocked = moved = crooked = fell = 0

    # Nothing stands in a doorway or a door's swing, the pack's own layout included: slid along the wall it stands
    # against (its local X) to the nearest clear spot, or taken away. Only the doors count here - the stairs' run is
    # a keep-out for things moved about, not for the furniture the pack stood beside them.
    cleared = 0
    doors = [z for z in fit.zones if not z.name.startswith('Stair')]
    for i in sorted(fit.boxes):
        e = fit.props[i]
        if i in pk.gone or not fit.boxes[i] or stem(e).startswith(FIXED):
            continue
        if any(i in rs for rs in fit.riders.values()):
            continue
        if not any(z.hits(b.shrunk(0.01)) for z in doors for b in fit.boxes[i]):
            continue
        group = [j for j in fit.group_of(i) if j not in pk.gone]
        c = fit.centre(fit.boxes[i])
        along = qrot(pk.world(i)[1], (1, 0, 0))
        done = False
        for d in sorted((k_ * 0.1 for k_ in range(-15, 16) if k_), key=abs):
            old = fit.transform_group(group, c, [0.0, 0.0, 0.0, 1.0], (along[0] * d, along[2] * d))
            news = {j: fit.prop_boxes(j) for j in group}
            if all(fit.clear(j, news[j], ignore=group) for j in group) and                     not any(z.hits(b.shrunk(0.01)) for z in doors for bs in news.values() for b in bs):
                fit.boxes.update(news)
                done = True
                break
            fit.undo(old)
        if not done:
            for j in group:
                pk.remove(j)
                fit.boxes.pop(j, None)
        cleared += 1
    notes['out of doorways'] = cleared

    def floor_of(boxes):
        return FLOOR_1 if min(b.ylo() for b in boxes) < 3.0 else FLOOR_2

    def refresh(ids):
        for j in ids:
            fit.boxes[j] = fit.prop_boxes(j)

    def drop_riders(i):
        """What stood on a piece that went over: on the floor beside it, lying as it fell, or gone."""
        nonlocal fell
        for r in list(fit.riders.get(i, [])):
            if r in pk.gone:
                continue
            for sub in fit.group_of(r)[1:]:
                pk.remove(sub)
                fit.boxes.pop(sub, None)
            placed = False
            for _ in range(10):
                bs = fit.boxes[r]
                c = fit.centre(bs)
                fl = floor_of(fit.boxes[i])
                lo_y = min(b.ylo() for b in bs)
                hi_y = max(b.yhi() for b in bs)
                wide = max(max(b.h[0], b.h[2]) for b in bs) * 2
                tall = (hi_y - lo_y) > wide * 1.3
                a = rng.uniform(0, 2 * math.pi)
                d = rng.uniform(0.25, 0.7)
                q = axis_q((0, 1, 0), rng.uniform(0, 360))
                if tall:
                    q = qmul(q, axis_q((0, 0, 1), rng.choice((-90.0, 90.0))))
                # land it: turned about its centre, moved out beside the piece, then set down on the floor
                pos, rot = pk.world(r)
                old = {r: (list(pk.by[r]['position']), list(pk.by[r]['rotation']))}
                v = hf.qrot(q, [pos[k] - c[k] for k in range(3)])
                fit.set_world(r, [c[0] + v[0] + math.cos(a) * d, pos[1], c[2] + v[2] + math.sin(a) * d], qmul(q, rot))
                bs2 = fit.prop_boxes(r)
                dy = fl + 0.002 - min(b.ylo() for b in bs2)
                p2, r2 = pk.world(r)
                fit.set_world(r, [p2[0], p2[1] + dy, p2[2]], r2)
                bs2 = fit.prop_boxes(r)
                if fit.clear(r, bs2, ignore=(i,), zones=True, nested=False):
                    fit.boxes[r] = bs2
                    placed = True
                    fell += 1
                    break
                fit.undo(old)
            if not placed:
                pk.remove(r)
                fit.boxes.pop(r, None)
            fit.riders[i] = [x for x in fit.riders.get(i, []) if x != r]

    def knock(i, axis, tries=8):
        """Over on its side about its own `axis`, its centre about where it stood, lying on the floor."""
        bs0 = fit.boxes[i]
        fl = floor_of(bs0)
        c = fit.centre(bs0)
        pos, rot = pk.world(i)
        for _ in range(tries):
            q = qmul(axis_q((0, 1, 0), rng.uniform(-50, 50)), qmul(rot, qmul(axis_q(axis, rng.choice((-90.0, 90.0))),
                                                                               hf.qinv(rot))))
            rel = [pos[k] - c[k] for k in range(3)]
            v = hf.qrot(q, rel)
            sh = (rng.uniform(-0.2, 0.2), rng.uniform(-0.2, 0.2))
            old = {i: (list(pk.by[i]['position']), list(pk.by[i]['rotation']))}
            fit.set_world(i, [c[0] + v[0] + sh[0], pos[1], c[2] + v[2] + sh[1]], qmul(q, rot))
            bs = fit.prop_boxes(i)
            p2, r2 = pk.world(i)
            fit.set_world(i, [p2[0], p2[1] + fl + 0.002 - min(b.ylo() for b in bs), p2[2]], r2)
            bs = fit.prop_boxes(i)
            if fit.clear(i, bs, ignore=fit.riders.get(i, []), nested=False):
                fit.boxes[i] = bs
                drop_riders(i)
                return True
            fit.undo(old)
        return False

    def shove(i, tries=8):
        """Pushed and turned a little, about its own middle, with what stands on it."""
        group = [j for j in fit.group_of(i) if j not in pk.gone]
        c = fit.centre(fit.boxes[i])
        for k in range(tries):
            f = 1.0 - k / tries
            turn = rng.uniform(6.0, 18.0) * rng.choice((-1, 1)) * f
            sh = (rng.uniform(-0.3, 0.3) * f, rng.uniform(-0.3, 0.3) * f)
            old = fit.transform_group(group, c, axis_q((0, 1, 0), turn), sh)
            news = {j: fit.prop_boxes(j) for j in group}
            if all(fit.clear(j, news[j], ignore=group) for j in group):
                fit.boxes.update(news)
                return True
            fit.undo(old)
        return False

    for e in pk.props():
        i = e['id']
        if i in pk.gone or e['kind'] != 'models' or not fit.boxes.get(i):
            continue
        s = stem(e)
        if any(i in rs for rs in fit.riders.values()):
            continue                    # rides something: goes where that goes
        if s.startswith(('Kitchen_chair_wooden', 'Kitchen_chair_fabric')) and rng.random() < 0.6:
            knocked += knock(i, (0, 0, 1))
        elif s == 'Lamp_stand' and rng.random() < 0.7:
            knocked += knock(i, (1, 0, 0))
        elif s.startswith(('Sidetable_small', 'Sidetable_A', 'Sidetable_B', 'Bedstool')) and rng.random() < 0.45:
            knocked += knock(i, (0, 0, 1))
        elif s.startswith(('Couch_', 'Chair_leather', 'Sidetable', 'Kitchen_table', 'Nightstand', 'Bedstool')):
            moved += shove(i)
        elif s.startswith(('Painting_', 'Mirror')) and not s.startswith(('Painting_stand', 'Painting_standphoto')):
            if rng.random() < 0.4:
                e['rotation'] = [round(v, 6) for v in qmul(e['rotation'], axis_q((0, 0, 1), rng.choice((-1, 1)) *
                                                                                  rng.uniform(2.5, 7.0)))]
                crooked += 1
    notes.update({'knocked over': knocked, 'shoved': moved, 'taken': taken, 'crooked pictures': crooked,
                  'fell off': fell})
    pk.fit = fit
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


# Clutter: (prefab, weight, how it lies, against a wall only)
CLUTTER = [(PROPS_IN + 'Newspaper.prefab', 5, 'flat', False), (PROPS_IN + 'LP_magazine.prefab', 4, 'flat', False),
           (PROPS_IN + 'Book_A.prefab', 2, 'book', False), (PROPS_IN + 'Book_B.prefab', 2, 'book', False),
           (PROPS_IN + 'Book_C.prefab', 2, 'book', False), (PROPS_IN + 'Garage_cardboardbox.prefab', 2, 'up', True),
           (PROPS_IN + 'Garage_paperbag.prefab', 2, 'side', True), (PROPS_IN + 'KP_plate.prefab', 2, 'up', False),
           (PROPS_IN + 'KP_glass_black.prefab', 1, 'side', False), (PROPS_IN + 'Basement_box.prefab', 1, 'up', True),
           (PROPS_IN + 'Garage_plastic_box.prefab', 1, 'up', True), (PROPS_OUT + 'Trashbag.prefab', 2, 'up', True),
           (PROPS_IN + 'Garage_paintbucket.prefab', 1, 'side', True)]
LIE = {'flat': (0.0, 0.0), 'up': (0.0, 0.0), 'book': (-90.0, 0.0), 'side': (0.0, 90.0)}


def lay_down(s, fit, g, path, x, z, y, yaw, tilt, scale, others):
    """Stamp a clutter prefab resting on the floor at (x, z) by its own shape, if it fits there."""
    b = fit.B.get(path)
    if not b:
        return False
    rot = k.euler_quat(yaw, tilt[0], tilt[1])
    sc = (scale, scale, scale)
    lift = hf.rest_lift(b[0], b[1], rot, sc)
    box = hf.Box.of((x, y + lift + 0.001, z), rot, sc, b[0], b[1])
    if not fit.free_spot([box], others):
        return False
    s.stamp(path, g, (round(x, 3), round(y + lift + 0.001, 4), round(z, 3)), yaw=round(yaw, 1), tilt=tilt,
            scale=scale)
    others.append(box)
    return True


def dress_inside(s, lay, pk, rng, broken, forest_litter, notes):
    """Rubbish where it would end up - kicked against the walls and into the corners, piled by the furniture - with
    the doorways, door swings and stairs left clear; leaves and needles where the windows are out."""
    fit = pk.fit
    rects = floor_rects(pk)
    usable = [c for c in CLUTTER if s.prefab(c[0]) and c[0] in fit.B]
    loose = [c for c in usable if not c[3] for _ in range(c[1])]
    heavy = [c for c in usable if c[3] for _ in range(c[1])]
    g = s.empty('Rubbish', lay, (0, 0, 0), static=True)
    placed = []
    for x0, x1, z0, z1, y in rects:
        area = (x1 - x0) * (z1 - z0)
        for _ in range(int(area * 0.9)):
            x, z = rng.uniform(x0 + 0.1, x1 - 0.1), rng.uniform(z0 + 0.1, z1 - 0.1)
            walled = fit.near_wall(x, z, y, 0.45)
            if heavy and fit.near_corner(x, z, y, 0.5) and rng.random() < 0.6:
                path, _w, how, _h = rng.choice(heavy)       # bags and boxes only in the corners
            elif walled or rng.random() < 0.25:
                path, _w, how, _h = rng.choice(loose)
            else:
                continue                # the middle of a room stays mostly bare: things get kicked to the sides
            lay_down(s, fit, g, path, x, z, y, rng.uniform(0, 360), LIE[how], round(rng.uniform(0.92, 1.08), 2),
                     placed)
    notes['rubbish'] = len(placed)

    # Books swept off every bookshelf onto the floor in front of it.
    books = [p for p in (PROPS_IN + 'Book_A.prefab', PROPS_IN + 'Book_B.prefab', PROPS_IN + 'Book_C.prefab')
             if s.prefab(p) and p in fit.B]
    nb = 0
    for e in pk.props():
        if e['id'] in pk.gone or not e['name'].startswith(('Bookshelf', 'Book_set_bookshelf')):
            continue
        pos, rot = pk.world(e['id'])
        y = FLOOR_1 if pos[1] < 3.0 else FLOOR_2
        fwd = qrot(rot, (0, 0, 1))
        for _ in range(rng.randint(10, 16)):
            d, side = rng.uniform(0.3, 1.0), rng.uniform(-0.7, 0.7)
            for sign in (1, -1):        # in front of it, whichever way its front faces
                x = pos[0] + (fwd[0] * d - fwd[2] * side) * sign
                z = pos[2] + (fwd[2] * d + fwd[0] * side) * sign
                if books and lay_down(s, fit, g, rng.choice(books), x, z, y, rng.uniform(0, 360),
                                      LIE['book'] if rng.random() < 0.8 else LIE['side'], 1.0, placed):
                    nb += 1
                    break
    notes['books on the floor'] = nb

    # Leaves and dead needles: blown in under the broken windows, drifted along the ground floor's walls.
    leaves = s.usable(forest_litter)
    n = 0

    def leaf(x, z, y, scale):
        nonlocal n
        probe = hf.Box([x, y + 0.03, z], [[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0.12 * scale, 0.03, 0.12 * scale])
        if not leaves or any(hf.overlap(probe, b) for b in fit.structure):
            return
        if any(z_.hits(probe) for z_ in fit.zones if z_.name.startswith('Stair')):
            return
        s.stamp(rng.choice(leaves), g, (round(x, 3), round(y + 0.004, 3), round(z, 3)),
                yaw=round(rng.uniform(0, 360), 1), scale=scale)
        n += 1
    for pos, rot, _plain in broken:
        y = FLOOR_1 if pos[1] < 3.0 else FLOOR_2
        nrm = qrot(rot, (0, 0, 1))
        for side in (-1, 1):            # whichever side is indoors is where the floor is
            for _ in range(rng.randint(2, 4)):
                d = rng.uniform(0.3, 1.4)
                x, z = pos[0] + nrm[0] * side * d + rng.uniform(-0.5, 0.5), pos[2] + nrm[2] * side * d + rng.uniform(-0.5, 0.5)
                if any(r[0] < x < r[1] and r[2] < z < r[3] and abs(r[4] - y) < 0.3 for r in rects):
                    leaf(x, z, y, round(rng.uniform(0.5, 0.8), 2))
    for x0, x1, z0, z1, y in rects:
        if y > 3.0:
            continue
        for _ in range(int((x1 - x0) * (z1 - z0) * 0.25)):
            x, z = rng.uniform(x0 + 0.05, x1 - 0.05), rng.uniform(z0 + 0.05, z1 - 0.05)
            if fit.near_wall(x, z, y, 0.35) or rng.random() < 0.2:
                leaf(x, z, y, round(rng.uniform(0.45, 0.75), 2))
    notes['leaves blown in'] = n
    store_room(s, lay, pk, notes)
    stories(s, lay, pk, rng, g, placed, notes)
    over_the_fireplace(s, lay, pk, notes)


FRAME_Y = 2.15      # the middle of the moulded panel on the master bedroom's chimney breast, above its floor


def over_the_fireplace(s, lay, pk, notes):
    """The master bedroom's chimney breast has a moulded panel above the mantel made for a picture, and the pack left
    it empty (a frame round nothing): a painting hangs in it, flat on the breast's face (the model's front, local
    +Z at 1.166)."""
    for e in pk.props():
        if stem(e) != 'Fireplace_withwall' or e['id'] in pk.gone:
            continue
        pos, rot = pk.world(e['id'])
        if pos[1] < 3.0:
            continue                    # the living room's already has its long painting
        off = qrot(rot, (0.0, FRAME_Y, 0.86))
        g = s.empty('Over The Fireplace', lay, (0, 0, 0), static=True)
        s.stamp(PROPS_IN + 'Painting_wide_A.prefab', g, (round(pos[0] + off[0], 4), round(pos[1] + off[1], 4),
                                                         round(pos[2] + off[2], 4)), yaw=round(yaw_of(rot), 2), scale=1.18)
        notes['painting over the fireplace'] = 1


ALLEY = 'assets/Environments/ModularCityAlleyPack/Prefabs/Prefabs_Props/'
BEDROOM = 'assets/Furniture/Bedroom/Models/'
GARAGE = 'assets/Furniture/Garage/Models/'


def wall_face(fit, x, z, y, dx, dz, reach=3.0):
    """The first wall face from (x, z) along (dx, dz), at height y: the point just short of it, or None."""
    for k_ in range(1, int(reach / 0.01)):
        px, pz = x + dx * k_ * 0.01, z + dz * k_ * 0.01
        probe = hf.Box([px, y, pz], [[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0.004, 0.05, 0.004])
        if any(hf.overlap(probe, s_) for s_ in fit.structure):
            return px - dx * 0.01, pz - dz * 0.01
    return None


def store_room(s, lay, pk, notes):
    """The old cellar stairwell under the stairs, floored over, is the house's store room: shelving along its back
    wall, boxes stacked at its end, paint cans by the door - what a family puts away and nobody came back for."""
    fit = pk.fit
    y = FLOOR_1
    cx, cz = -37.35, -5.3                       # the room's middle (floor_the_stairwell's hole)
    west = wall_face(fit, cx, cz, y + 1.0, -1, 0)
    north = wall_face(fit, cx, cz, y + 1.0, 0, -1)
    east = wall_face(fit, cx, cz, y + 1.0, 1, 0)
    if not (west and north and east):
        return
    g = s.empty('Store Room', lay, (0, 0, 0), static=True)
    placed = []
    n = 0
    # two shelving units, backs to the west wall (their back is local -Z: yaw 90 turns it to -X)
    for z, path in ((-6.55, 'Garage_shelf_with_props_A'), (-5.0, 'Garage_shelf_with_props_B')):
        n += lay_down(s, fit, g, PROPS_IN + path + '.prefab', west[0] + 0.26, z, y, 90.0, (0.0, 0.0), 1.0, placed)
    # boxes against the north wall, one on another
    for x, path, yaw in ((-37.6, 'Garage_plastic_box', 0.0), (-36.95, 'Garage_cardboardbox', 8.0),
                         (-36.5, 'Garage_cardboardbox', -6.0)):
        if lay_down(s, fit, g, PROPS_IN + path + '.prefab', x, north[1] + 0.24, y, yaw, (0.0, 0.0), 1.0, placed):
            n += 1
            if path == 'Garage_plastic_box':      # a second crate on the first
                b = placed[-1]
                s.stamp(PROPS_IN + 'Garage_cardboardbox.prefab', g, (round(x + 0.03, 3), round(b.yhi() + 0.001, 4),
                                                                      round(north[1] + 0.24, 3)), yaw=-4.0)
                n += 1
    # paint cans and a bag by the east wall
    for z, path, yaw in ((-6.9, 'Garage_paintbucket', 0.0), (-6.68, 'Garage_paintbucket', 40.0),
                         (-6.8, 'Garage_paintbucket', 75.0), (-6.2, 'Garage_paperbag', 15.0)):
        x = east[0] - (0.12 if 'bucket' in path else 0.16)
        n += lay_down(s, fit, g, PROPS_IN + path + '.prefab', x, z, y, yaw, (0.0, 0.0), 1.0, placed)
    notes['store room pieces'] = n


def stories(s, lay, pk, rng, g, placed, notes):
    """Where the rubbish tells something: someone slept on the living room floor by the candles - a mattress dragged
    against the wall, a pillow, newspapers; and the kitchen was picked over, its plates and glasses on the floor
    in front of the sink."""
    fit = pk.fit
    y = FLOOR_1
    n = 0
    couch = next((e for e in pk.props() if e['name'].startswith('Couch_3seat') and e['id'] not in pk.gone), None)
    if couch:
        cp, _ = pk.world(couch['id'])
        # the nearest free stretch of floor by a wall in the living room, long enough for a mattress
        rooms_ = house_lights.named_rooms(fit, floor_rects(pk), sys.modules[__name__])
        cells = [(x, z) for r in rooms_ if r['name'] == 'living room' for x, z in r['cells']]
        cells = [c for c in cells[::2] if fit.near_wall(c[0], c[1], y, 0.8) and not fit.near_wall(c[0], c[1], y, 0.42)]
        cells.sort(key=lambda c: math.hypot(c[0] - cp[0], c[1] - cp[2]))
        for mx, mz in cells[:400]:
            yaw = 0.0 if fit.near_wall(mx, mz + 0.75, y, 0.1) or fit.near_wall(mx, mz - 0.75, y, 0.1) else 90.0
            if not lay_down(s, fit, g, ALLEY + 'Bed_Mattress.prefab', mx, mz, y, yaw + rng.uniform(-5, 5), (0.0, 0.0),
                            1.0, placed):
                continue
            m = placed[-1]
            top = m.yhi()
            mx, mz = m.c[0], m.c[2]
            ext = [sum(abs(m.h[i] * m.ax[i][k]) for i in range(3)) for k in (0, 2)]
            ax = (1, 0) if ext[0] >= ext[1] else (0, 1)          # its long side, in the world
            px, pz = mx + ax[0] * 0.7, mz + ax[1] * 0.7
            s.stamp(BEDROOM + 'Bed_Pillow_A.prefab', g, (round(px, 3), round(top + 0.001, 4), round(pz, 3)),
                    yaw=round(rng.uniform(-20, 20) + (90.0 if ax == (1, 0) else 0.0), 1))
            side = (ax[1], ax[0])
            for k_ in range(3):
                lay_down(s, fit, g, PROPS_IN + 'Newspaper.prefab', mx + side[0] * (0.75 + 0.25 * k_) - ax[0] * 0.3 * k_,
                         mz + side[1] * (0.75 + 0.25 * k_) - ax[1] * 0.3 * k_, y, rng.uniform(0, 360), (0.0, 0.0), 1.0,
                         placed)
            lay_down(s, fit, g, PROPS_IN + 'KP_glass_black.prefab', mx - ax[0] * 1.2, mz - ax[1] * 1.2, y,
                     rng.uniform(0, 360), LIE['side'], 1.0, placed)
            n += 1
            break
    sink = next((e for e in pk.props() if fh_stem(e).startswith('Kitcabinet_low_1') and 'sink' in fh_stem(e)
                 and pk.world(e['id'])[0][1] < 3.0 and e['id'] not in pk.gone), None)
    if sink:
        sp, sr = pk.world(sink['id'])
        fwd = qrot(sr, (0, 0, 1))
        for k_ in range(9):
            d, side = rng.uniform(0.55, 1.1), rng.uniform(-0.6, 0.6)
            x, z = sp[0] + fwd[0] * d - fwd[2] * side, sp[2] + fwd[2] * d + fwd[0] * side
            path, how = rng.choice(((PROPS_IN + 'KP_plate.prefab', 'up'), (PROPS_IN + 'KP_glass_black.prefab', 'side'),
                                    (PROPS_IN + 'KP_coffee_mug.prefab', 'side'), (PROPS_IN + 'KP_cookiebox.prefab', 'side')))
            n += lay_down(s, fit, g, path, x, z, y, rng.uniform(0, 360), LIE[how], 1.0, placed)
    notes['story pieces'] = n


def fh_stem(e):
    return stem(e)


# Decals (docs/DECALS.md): a fixed handful of maps so the house stays well inside the decal texture budget. The plaster
# is cracked where a settling house cracks - off the corners of the door and window openings, along the wall heads and
# across the ceilings - with a leak's stain where a ceiling crack has let the rain through.
D = 'assets/Decals/'
CRACK = D + 'Grime/PeelingPaint/DA_PeelingPaint_{:03d}.decal'
WALL_CRACKS = [CRACK.format(n) for n in (72, 79, 91)]   # crack lines only (the others carry pale paint flakes)
CEIL_CRACKS = [CRACK.format(n) for n in (72, 79, 91)]   # long hairlines, the same as the walls'
LEAK_DECALS = [D + f'Stains/LiquidSplatter/DA_LiquidSplatter_{n:03d}.decal' for n in (1, 10, 19)]
BASE_DECALS = [D + f'Stains/LiquidSplatter/DA_LiquidSplatter_{n:03d}.decal' for n in (2, 9)]
CAR_DECAL = D + 'Stains/Splatters/DA_Stain_591.decal'
MAX_DECALS, MAX_MAPS = 320, 12
CRACK_TINT = [0.6, 0.56, 0.5]


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
    """Cracked plaster and the odd leak inside; rain streaks and damp on the outside walls only."""
    fit = pk.fit
    g = s.empty('Damage', lay, (0, 0, 0), static=True)
    n, maps = 0, set()
    house_c = (-33.0, -1.0)
    furniture = [b for i, bs in fit.boxes.items() if not stem(fit.props[i]).startswith('Carpet_') for b in bs]

    def lands_clean(pos, rot, size):
        """The decal's box must not reach any furniture (a crack smeared across a couch back) or a doorway."""
        box = hf.Box(list(pos), [qrot(rot, a) for a in ((1, 0, 0), (0, 1, 0), (0, 0, 1))], [v / 2 for v in size])
        return not any(hf.overlap(box, b, 0.0) for b in furniture)

    def put(name, pos, rot, asset, size, opacity, tint):
        nonlocal n
        if n >= MAX_DECALS or not lands_clean(pos, rot, size):
            return False
        decal(s, g, f'{name} {n:03d}', tuple(round(v, 3) for v in pos), rot, asset, size, rng.randint(1, 999),
              opacity, tint=tint)
        maps.add(asset)
        n += 1
        return True

    def wall_faces(e):
        """The wall's solid pieces (colliders: a door wall's are either side of and over the opening) as (centre, rot,
        half extents, outward sign or 0 for an inside wall)."""
        out = []
        pid = e['id']
        for cid in [pid] + pk.kids.get(pid, []):
            c = pk.by[cid].get('collider')
            if not c or c.get('isTrigger') or 'halfExtents' not in c:
                continue
            pos, rot = pk.world(cid)
            v = qrot(rot, c.get('center', [0, 0, 0]))
            ctr = [pos[0] + v[0], pos[1] + v[1], pos[2] + v[2]]
            he = c['halfExtents']
            outward = 0
            if e['name'].startswith(('Wall_1st', 'Wall_2nd')):
                nz = qrot(rot, (0, 0, 1))
                outward = 1 if nz[0] * (ctr[0] - house_c[0]) + nz[2] * (ctr[2] - house_c[1]) > 0 else -1
            out.append((ctr, rot, he, outward))
        return out

    def on_wall(face, u, y, size, twist, asset, opacity, tint, name):
        """A decal on the wall piece at `u` along it and height y: through both faces of an inside wall, only the inside
        face of an outer one."""
        ctr, rot, he, outward = face
        along = qrot(rot, (1, 0, 0))
        nz = qrot(rot, (0, 0, 1))
        r = qmul(rot, axis_q((0, 0, 1), twist))
        if outward == 0:
            off, depth = 0.0, he[2] * 2 + 0.06
        else:
            off, depth = -outward * he[2], 0.1
        p = [ctr[0] + along[0] * u + nz[0] * off, y, ctr[2] + along[2] * u + nz[2] * off]
        return put(name, p, r, asset, (size[0], size[1], depth), opacity, tint)

    # Off the corners of the door openings: the lintel's corners, running up and out.
    for e in pk.ents:
        o, c = e.get('Openable'), e.get('collider')
        if not o or not c or c['halfExtents'][1] * 2 < 1.8 or e['id'] in pk.gone or rng.random() < 0.2:
            continue
        pos, rot = pk.world(e['id'])
        he, cc = c['halfExtents'], c.get('center', [0, 0, 0])
        frame = pk.by[e['parentId']]['name']
        for side in rng.sample((-1, 1), rng.choice((1, 1, 2))):
            v = qrot(rot, (cc[0] + side * (he[0] + 0.14), cc[1] + he[1] + 0.16, cc[2]))
            p = [pos[0] + v[0], pos[1] + v[1], pos[2] + v[2]]
            w = rng.uniform(0.3, 0.5)
            r = qmul(rot, axis_q((0, 0, 1), side * rng.uniform(35.0, 60.0)))
            exterior = frame.startswith('Door_exterior')
            depth = 0.2 if exterior else 0.3
            if exterior:        # the inside face only: from just inside the leaf's plane to past the wall's inner face
                nz = qrot(rot, (0, 0, 1))
                inward = -1 if nz[0] * (p[0] - house_c[0]) + nz[2] * (p[2] - house_c[1]) > 0 else 1
                p = [p[0] + nz[0] * inward * 0.13, p[1], p[2] + nz[2] * inward * 0.13]
            put('Door Crack', p, r, rng.choice(WALL_CRACKS), (w, w * 0.5, depth), rng.uniform(0.85, 1.0), CRACK_TINT)

    # Off the corners of the windows (inside face).
    for e in pk.named(r'Windowglass_singlewindow'):
        if rng.random() < 0.5:
            continue
        b = fit.B.get(e.get('path', ''))
        if not b:
            continue
        pos, rot = pk.world(e['id'])
        box = hf.Box.of(pos, rot, fit.wscale(e['id']), b[0], b[1])
        if box.h[1] < 0.2:
            continue
        nz = box.ax[2] if box.h[2] < box.h[0] else box.ax[0]
        along = box.ax[0] if box.h[2] < box.h[0] else box.ax[2]
        ha = max(box.h[0], box.h[2])
        inward = -1 if nz[0] * (box.c[0] - house_c[0]) + nz[2] * (box.c[2] - house_c[1]) > 0 else 1
        sa, sy = rng.choice((-1, 1)), rng.choice((-1, 1))
        p = [box.c[0] + along[0] * sa * (ha + 0.14) + nz[0] * inward * 0.14, box.c[1] + sy * (box.h[1] + 0.14),
             box.c[2] + along[2] * sa * (ha + 0.14) + nz[2] * inward * 0.14]
        yaw = math.degrees(math.atan2(-nz[2], nz[0])) - 90.0
        r = qmul(axis_q((0, 1, 0), yaw), axis_q((0, 0, 1), sa * sy * rng.uniform(30.0, 55.0)))
        w = rng.uniform(0.25, 0.42)
        put('Window Crack', p, r, rng.choice(WALL_CRACKS), (w, w * 0.55, 0.12), rng.uniform(0.85, 1.0), CRACK_TINT)

    # Along the wall heads, and the odd long crack down a wall.
    for e in pk.named(r'Wall_(Interior|1st|2nd)_'):
        if e['kind'] != 'models' or e['id'] in pk.gone:
            continue
        for face in wall_faces(e):
            ctr, rot, he, outward = face
            if he[0] < 0.35 or he[1] < 0.9:
                continue
            ceil = CEIL_1 if ctr[1] < 3.4 else CEIL_2
            u = rng.uniform(-he[0] + 0.3, he[0] - 0.3)
            w = rng.uniform(0.35, min(0.6, he[0] * 2 - 0.2))
            if rng.random() < 0.25:
                on_wall(face, u, ceil - 0.1, (w, 0.16), rng.uniform(-8, 8) + rng.choice((0, 180)),
                        rng.choice(WALL_CRACKS), rng.uniform(0.8, 0.95), CRACK_TINT, 'Wall Head Crack')
            elif rng.random() < 0.16:
                floor = FLOOR_1 if ctr[1] < 3.4 else FLOOR_2
                on_wall(face, u, floor + rng.uniform(1.2, 2.1), (rng.uniform(0.35, 0.6), 0.22),
                        rng.uniform(55, 125), rng.choice(WALL_CRACKS), rng.uniform(0.8, 0.95), CRACK_TINT, 'Wall Crack')

    # Across the ceilings, a leak's stain at the end of some of them - most often upstairs, under the roof.
    up = axis_q((1, 0, 0), 90.0)
    lamps = [pk.world(e['id'])[0] for e in pk.named(r'Lamp_(ceiling|chandelier|bedroom|kitchen)')]
    for x0, x1, z0, z1, y in floor_rects(pk):
        upstairs = y > 3.0
        if rng.random() > (0.65 if upstairs else 0.4) or x1 - x0 < 1.6 or z1 - z0 < 1.6:
            continue
        ceil = CEIL_2 if upstairs else CEIL_1
        for _ in range(6):
            x, z = rng.uniform(x0 + 0.6, x1 - 0.6), rng.uniform(z0 + 0.6, z1 - 0.6)
            if all(math.hypot(x - l[0], z - l[2]) > 0.9 for l in lamps):
                break
        else:
            continue
        yaw = rng.uniform(0, 360)
        w = rng.uniform(0.5, 0.9)
        if put('Ceiling Crack', (x, ceil, z), qmul(axis_q((0, 1, 0), yaw), up), rng.choice(CEIL_CRACKS),
               (w, w * 0.35, 0.12), rng.uniform(0.85, 1.0), CRACK_TINT) and rng.random() < (0.6 if upstairs else 0.3):
            a = math.radians(yaw)
            ex, ez = x + math.cos(a) * w * 0.35, z - math.sin(a) * w * 0.35
            sw = rng.uniform(0.3, 0.55)
            put('Leak Stain', (ex, ceil, ez), qmul(axis_q((0, 1, 0), rng.uniform(0, 360)), up),
                rng.choice(LEAK_DECALS), (sw, sw * rng.uniform(0.75, 1.0), 0.1), rng.uniform(0.55, 0.8),
                [0.62, 0.55, 0.42])

    # Outside, on the outer face only: a thin band of damp along the foundation where the ground splashes it, and a
    # streak of grime under the sills of the windows on the ground floor, where the rain runs off them.
    for e in pk.named(r'Wall_(1st|2nd)_'):
        if e['kind'] != 'models' or e['id'] in pk.gone or n >= MAX_DECALS - 2:
            continue
        m = re.match(r'Wall_\w+?_([\d½]+)x', e['name'])
        length = float(m.group(1).replace('½', '.5')) * 0.97 if m else 1.0
        pos, rot = pk.world(e['id'])
        if pos[1] > 3.0:
            continue
        nz = qrot(rot, (0, 0, 1))
        v = qrot(rot, (length / 2, 0.0, 0.0))
        cx, cz = pos[0] + v[0], pos[2] + v[2]
        outward = 1 if nz[0] * (cx - house_c[0]) + nz[2] * (cz - house_c[1]) > 0 else -1
        ox, oz = cx + nz[0] * outward * 0.11, cz + nz[2] * outward * 0.11
        decal(s, g, f'Foundation Damp {n:03d}', (round(ox, 3), pos[1] + 0.16, round(oz, 3)), rot,
              rng.choice(BASE_DECALS), (length + 0.05, 0.38, 0.12), rng.randint(1, 999), rng.uniform(0.45, 0.6),
              tint=[0.24, 0.27, 0.18])
        maps.update(BASE_DECALS)
        n += 1
    for e in pk.named(r'Windowglass_singlewindow'):
        b = fit.B.get(e.get('path', ''))
        pos, rot = pk.world(e['id'])
        if not b or pos[1] > 3.0 or rng.random() < 0.4:
            continue
        box = hf.Box.of(pos, rot, fit.wscale(e['id']), b[0], b[1])
        nz = box.ax[2] if box.h[2] < box.h[0] else box.ax[0]
        ha = max(box.h[0], box.h[2])
        outward = 1 if nz[0] * (box.c[0] - house_c[0]) + nz[2] * (box.c[2] - house_c[1]) > 0 else -1
        yaw = math.degrees(math.atan2(-nz[2], nz[0])) - 90.0
        hgt = rng.uniform(0.45, 0.7)
        p = (box.c[0] + nz[0] * outward * 0.2, box.c[1] - box.h[1] - 0.08 - hgt / 2, box.c[2] + nz[2] * outward * 0.2)
        decal(s, g, f'Sill Streak {n:03d}', tuple(round(v_, 3) for v_ in p), axis_q((0, 1, 0), yaw),
              rng.choice(LEAK_DECALS), (ha * 2 * rng.uniform(0.5, 0.8), hgt, 0.3), rng.randint(1, 999),
              rng.uniform(0.35, 0.55), tint=[0.4, 0.38, 0.3])
        maps.update(LEAK_DECALS)
        n += 1
    maps.add(CAR_DECAL)
    assert n <= MAX_DECALS and len(maps) <= MAX_MAPS, (n, len(maps))
    notes['decals'] = n


RED_I, PORCH_I = 1.6, 1.3
# Shadow slots the engine hands to the shadow-capable lights nearest the player each frame. They are cached (a face
# redraws only when something moving is in it), so a still room costs next to nothing once it has drawn.
POINT_SHADOWS = 4
SPOT_SHADOWS = 6


def lights(s, lay, pk, repo, notes):
    """What lights the house: every fitting and lamp on its circuit, with switches (house_lights.py). The candles on
    the living room floor are burned out and give no light. Indoor lights scatter nothing into the fog (no fog indoors)."""
    # The master bedroom, upstairs at the front: its ceiling fitting holds a bare red bulb.
    red = next(e for e in pk.named(r'Lamp_bedroom$'))
    red['materials'] = [{'embedded': k.from_mat(repo, PACK + 'Materials/M_Lamps_House.mat',
                                                emissiveColor=[1.0, 0.012, 0.006], emissiveStrength=2.2)}]
    red['castShadows'] = 0      # the bulb hangs in it: its own shade would print a dark disc on the ceiling round it
    pk.wiring = house_lights.wire(s, lay, pk, repo, sys.modules[__name__], notes)
    g = pk.wiring.g

    def lamp(name, pos, **kw):
        i = s.light(name, g, pos, **kw)
        e = s.empties[-1]
        e['Light']['Volumetric Intensity'] = 0.0
        return i, e

    # Candles burned down on the living room floor, in front of the couch: someone's been here.
    couch = next(e for e in pk.props() if e['name'].startswith('Couch_3seat'))
    cb = pk.fit.boxes[couch['id']][0]
    front = qrot(pk.world(couch['id'])[1], (0, 0, 1))
    lit = []
    for d in (0.75, 0.95, 1.15, 1.35):
        if lit:
            break
        bx, bz = cb.c[0] + front[0] * (cb.h[2] + d), cb.c[2] + front[2] * (cb.h[2] + d)
        for name, ox, oz, yaw in (('LP_candle_triple', 0.0, 0.0, 30.0), ('LP_candle_single', 0.22, 0.12, 0.0),
                                  ('LP_candle_small', -0.18, 0.16, 0.0)):
            lay_down(s, pk.fit, g, PROPS_IN + name + '.prefab', bx + ox, bz + oz, FLOOR_1, yaw, (0.0, 0.0), 1.0, lit)
    # (burned out: the candles give no light - they have no flame)


def porch_light(s, lay, notes, pk=None):
    """The two lamps either side of the front door, on the porch circuit, with its switch inside by the door; the
    left bulb is on its way out (it stutters). On from the start, as the house has always been seen."""
    g = s.empty('Porch Light', lay, (0, 0, 0), static=True)
    w = getattr(pk, 'wiring', None) if pk else None
    # The lamp's back plate is its local XY plane at z = 0 and the lamp stands out along +Z: yaw +90 turns +Z out of
    # the front wall (pack +X), its plate flat on the siding's face.
    for z in (-1.018, 2.891):       # where the pack's demo street had them, either side of the door
        x = -31.04
        if w:
            f = w.face((-31.04, z), (1.0, 0.0), 2.316, reach=0.8)
            x = f[0] if f else x
        r = s.stamp(PROPS_OUT + 'Lamp_wall_original.prefab', g, (round(x, 4), 2.316, z), yaw=90.0)
        for m in s.models:
            if m['parentId'] == r:
                m['Light Circuit'] = {'Circuit': 'Porch', 'Glow': 0.8}
                m['static'] = False
        s.light(f'Porch Bulb {"L" if z < 0 else "R"}', g, (round(x + 0.14, 4), 2.40, z), intensity=PORCH_I, rng=7.0,
                kelvin=2300.0, shadows=z < 0)
        e = s.empties[-1]
        e['Light']['Volumetric Intensity'] = 1.0        # outdoors: its glow in the night air
        e['Light Circuit'] = {'Circuit': 'Porch', 'Glow': 0.0}
        # out on the porch and the yard, never in through the front wall
        e['Light'].update({'Bounds': True, 'Bounds Center': [round(x + 4.0, 3), 2.0, round(z, 3)],
                           'Bounds Size': [8.0, 6.0, 12.0], 'Bounds Fade': 0.05})
        if z < 0:
            e.update(k.light_flicker(Mode=0, MinInterval=2.5, MaxInterval=9.0, Depth=1.0, Seed=5))
    if w:
        hall = w.by_room.get('entrance hall')
        if hall:
            w.place_plate('Porch', hall, ('door', 'Door_exterior_paneled'), FLOOR_1, True, spacing=0.085)
    notes['porch lights'] = 2


# Small and flat things that cast no shadow worth its cost (the Forest's shadow pass): the rubbish, the books, the blown-in
# leaves and the dead wall fittings.
QUIET = re.compile(r'(Basement_box|Book_[ABC]|Garage_cardboardbox|Garage_paintbucket|Garage_paperbag|KP_glass_black|'
                   r'KP_plate|LP_magazine|Newspaper|Lamp_wall_chan|Lamp_wall_toilet|Ground_Leaves_var\d@.*|'
                   r'Generic_dry_leaves_var\d@?.*|Dry_cedrus_debris_Var\d@.*)\.(FBX|fbx)$')


# ... and in the yard, the ground litter, weeds and wild grass (as across the rest of the Forest).
YARD_QUIET = re.compile(r'(Dry_cedrus_debris_Var\d@.*|Ground_flower_weed_var\d|Ground_weed_var\d@.*|'
                        r'Wild_forest_grass_var\d@.*|Ground_Leaves_var\d@.*)\.(FBX|fbx)$')


def quiet_shadows(s, house, rule=QUIET):
    """castShadows off on the models under `house` that match `rule`."""
    kids = {}
    for e in s.empties + s.models + s.boxes:
        kids.setdefault(e['parentId'], []).append(e)
    todo, n = [house], 0
    while todo:
        for e in kids.get(todo.pop(), []):
            todo.append(e['id'])
            if rule.search(e.get('path', '')):
                e['castShadows'] = 0
                n += 1
    return n


def add_house(s, root, repo, pack_path, notes):
    """The house: the pack snapshot under 'Abandoned House' / 'Pack Layout', trashed and dressed."""
    pk = Pack(pack_path)
    rng = random.Random(301)
    DEAD_SCREEN.update(k.from_mat(repo, PACK + 'Materials/M_Devices_On.mat', emissiveColor=[0.0, 0.0, 0.0],
                                  emissiveStrength=0.0))
    open_sealed_doors(pk, notes)
    floor_the_stairwell(pk, notes)
    clear_walls(pk, notes)
    ease_side_door(pk, notes)
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
    pk.remap = remap
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
    # the slab's measured middle is 8.5 cm off its pivot in x; the first one butts up to the porch's front edge
    z = PORCH_DECK[2] - 0.01
    i = 0
    while z > LOT[2] + 0.2:
        if i != 3:
            s.stamp(GROUND_P + 'Road_walkway_piece.prefab', walk, (round(WALK_X - 0.085, 3), round(LOT_Y - 0.004, 3),
                                                                   round(z - 0.977, 3)),
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
        decal(s, drive, f'Car Grime {i}', pos, rot, CAR_DECAL, size, 40 + i, 0.95, tint=[0.36, 0.33, 0.25])

    # Fences: a broken white picket out front with the gate gone; the backyard's tall wooden fence, rotten through.
    fences = s.empty('Fences', yard, (0, 0, 0), static=True)
    nf = 0
    picket = PROPS_OUT + 'Fence_white_2x.prefab'
    if s.prefab(PROPS_OUT + 'Fence_small_2x_R.prefab'):
        picket = PROPS_OUT + 'Fence_small_2x_R.prefab'
    nf += fence_line(s, fences, picket, (LOT[0], LOT[2]), (DRIVE[0] - 0.4, LOT[2]), 2.07, rng, H, name='Picket')
    # either side of the gateway, run outward from it so the opening is exactly the walk's width plus a post's room
    nf += fence_line(s, fences, picket, (WALK_X - GATE, LOT[2]), (DRIVE[1] + 0.4, LOT[2]), 2.07, rng, H, name='Picket')
    nf += fence_line(s, fences, picket, (WALK_X + GATE, LOT[2]), (LOT[1], LOT[2]), 2.07, rng, H, name='Picket')
    wood = PROPS_OUT + 'Fence_wooden_2x.prefab'
    for a, b in (((LOT[0], 30.5), (LOT[0], LOT[3])), ((LOT[0], LOT[3]), (LOT[1], LOT[3])),
                 ((LOT[1], LOT[3]), (LOT[1], 30.5)), ((LOT[0], 30.5), (HOUSE_BOX[0], 30.5)),
                 ((HOUSE_BOX[1], 30.5), (LOT[1], 30.5))):
        nf += fence_line(s, fences, wood, a, b, 2.07, rng, H, missing=0.1, lean=0.2, name='Wood Fence')
    notes['fence panels'] = nf

    # Yard junk.
    junk = s.empty('Yard Junk', yard, (0, 0, 0), static=True)
    B = hf.bounds()

    def put(p, x, z, yaw=0.0, tilt=(0.0, 0.0), dy=0.0, name=None, rest=True):
        """On the porch deck or the ground under it, resting by its own measured shape (dy: sunk or raised more)."""
        d = PORCH_DECK
        ground = d[4] if d[0] <= x <= d[1] and d[2] <= z <= d[3] else H(x, z)
        b = B.get(p)
        lift = hf.rest_lift(b[0], b[1], k.euler_quat(yaw, tilt[0], tilt[1])) if b and rest else 0.0
        return s.stamp(p, junk, (x, round(ground + lift + dy, 3), z), yaw=yaw, tilt=tilt, name=name)
    put(PROPS_OUT + 'Mailbox.prefab', WALK_X + 1.6, LOT[2] + 0.5, yaw=-90.0, tilt=(0.0, 8.0), dy=-0.06)
    put(PROPS_OUT + 'Trashbin.prefab', -15.9, 27.5, yaw=70.0, tilt=(0.0, 90.0))      # knocked over
    for x, z in ((-16.4, 26.3), (-15.6, 28.9), (-17.0, 28.1), (16.2, 22.4)):
        put(PROPS_OUT + 'Trashbag.prefab', x, z, yaw=rng.uniform(0, 360))
    # The porch: a table and stools left out, one blown over.
    put(PROPS_OUT + 'Porch_table.prefab', -4.0, 21.6, yaw=12.0)
    put(PROPS_OUT + 'Porch_stool.prefab', -5.0, 21.5, yaw=80.0)
    put(PROPS_OUT + 'Porch_stool.prefab', -2.9, 21.7, yaw=200.0, tilt=(0.0, 90.0))
    # The backyard: loungers and a sun umbrella from a summer a long time ago.
    put(PROPS_OUT + 'Sun_table.prefab', 4.0, 38.5, yaw=15.0)
    put(PROPS_OUT + 'Sun_umbrella.prefab', 5.6, 39.6, yaw=-30.0, tilt=(59.0, 0.0), name='Fallen Umbrella', rest=False)   # pole's foot and canopy rim on the ground
    put(PROPS_OUT + 'Sun_chair.prefab', 2.6, 37.8, yaw=110.0)
    put(PROPS_OUT + 'Sun_chair.prefab', 6.1, 37.1, yaw=40.0, tilt=(0.0, 90.0))
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
        if any(math.hypot(x - dx, z - dz) < 1.4 for dx, dz in OUTER_DOORS):
            continue                     # the doorsteps stay clear
        if tall:
            s.stamp(rng.choice(tall), wild, (round(x, 2), round(H(x, z), 3), round(z, 2)),
                    yaw=round(rng.uniform(0, 360), 1), scale=round(rng.uniform(0.8, 1.4), 2))
            n_w += 1
    ivy = [p for p in (PROPS_OUT + 'Plant_climber_long.prefab', PROPS_OUT + 'Plant_climber_short.prefab') if s.prefab(p)]
    # on blank stretches of wall only: never across a door or a window (the side door's is at z 26.5)
    for x, z, yaw in ((-14.95, 23.5, -90.0), (-14.95, 29.0, -90.0), (14.95, 21.8, 90.0), (-6.0, 32.25, 180.0),
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
        keep = 0.05                              # a few weeds in the walk's cracks
    if DRIVE[0] < x < DRIVE[1] and DRIVE[2] - 1.0 < z < DRIVE[3]:
        keep = 0.07
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
