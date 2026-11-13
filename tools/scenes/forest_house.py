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
# the kitchen, candles left burning in the living room, and the master bedroom upstairs, at the front, lit red.
#
# Interior work is authored in the pack's space and parented under the house's 'Pack Layout'; the yard in world
# metres. Every random choice comes from its own seeded stream, so a rebuild gives the same house.
import json
import math
import random
import re

import house_fit as hf
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
        self.zones = self.door_zones()
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

    def door_zones(self):
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
            hw, hd = w / 2 + 0.12, w + 0.15
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
        """Can prop `i` stand as `boxes`? Not into the walls, other furniture, or a doorway it wasn't already in. With
        `nested`, a piece the pack had it tucked into (a chair under its table) may stay as deep, no deeper."""
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
            for z in self.zones:
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

    # The power's off: every fitting dark (lights() relights the ones still burning), screens dead.
    for e in pk.ents:
        if e['kind'] == 'models' and stem(e).startswith(LAMPS_OFF):
            e['materials'] = [dict(OFF)]
        elif e['kind'] == 'models' and stem(e).startswith('D_'):
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

    # Nothing stands in a doorway or a door's swing, the pack's own layout included: slid out to the nearest clear spot
    # (along the wall it stood against, as a rule), or taken away.
    cleared = 0
    for i in sorted(fit.boxes):
        e = fit.props[i]
        if i in pk.gone or not fit.boxes[i] or stem(e).startswith(FIXED):
            continue
        if any(i in rs for rs in fit.riders.values()):
            continue
        if not any(z.hits(b.shrunk(0.01)) for z in fit.zones for b in fit.boxes[i]):
            continue
        group = [j for j in fit.group_of(i) if j not in pk.gone]
        c = fit.centre(fit.boxes[i])
        steps = sorted(((dx * 0.1, dz * 0.1) for dx in range(-14, 15) for dz in range(-14, 15)),
                       key=lambda d: math.hypot(*d))
        done = False
        for sh in steps[1:]:
            old = fit.transform_group(group, c, [0.0, 0.0, 0.0, 1.0], sh)
            news = {j: fit.prop_boxes(j) for j in group}
            if all(fit.clear(j, news[j], ignore=group) for j in group) and                     not any(z.hits(b.shrunk(0.01)) for z in fit.zones for bs in news.values() for b in bs):
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
            if walled and heavy and rng.random() < 0.35:
                path, _w, how, _h = rng.choice(heavy)
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


# Decals (docs/DECALS.md): a fixed handful of maps so the house stays well inside the decal texture budget. The plaster
# is cracked where a settling house cracks - off the corners of the door and window openings, along the wall heads and
# across the ceilings - with a leak's stain where a ceiling crack has let the rain through.
D = 'assets/Decals/'
CRACK = D + 'Grime/PeelingPaint/DA_PeelingPaint_{:03d}.decal'
WALL_CRACKS = [CRACK.format(n) for n in (72, 79, 91)]   # crack lines only (the others carry pale paint flakes)
CEIL_CRACKS = [CRACK.format(73)]
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
            v = qrot(rot, (cc[0] + side * (he[0] + 0.25), cc[1] + he[1] + 0.3, cc[2]))
            p = [pos[0] + v[0], pos[1] + v[1], pos[2] + v[2]]
            w = rng.uniform(0.55, 0.85)
            r = qmul(rot, axis_q((0, 0, 1), side * rng.uniform(35.0, 60.0)))
            exterior = frame.startswith('Door_exterior')
            depth = 0.2 if exterior else 0.3
            if exterior:        # the inside face only: from just inside the leaf's plane to past the wall's inner face
                nz = qrot(rot, (0, 0, 1))
                inward = -1 if nz[0] * (p[0] - house_c[0]) + nz[2] * (p[2] - house_c[1]) > 0 else 1
                p = [p[0] + nz[0] * inward * 0.13, p[1], p[2] + nz[2] * inward * 0.13]
            put('Door Crack', p, r, rng.choice(WALL_CRACKS), (w, w * 0.55, depth), rng.uniform(0.85, 1.0), CRACK_TINT)

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
        p = [box.c[0] + along[0] * sa * (ha + 0.22) + nz[0] * inward * 0.14, box.c[1] + sy * (box.h[1] + 0.22),
             box.c[2] + along[2] * sa * (ha + 0.22) + nz[2] * inward * 0.14]
        yaw = math.degrees(math.atan2(-nz[2], nz[0])) - 90.0
        r = qmul(axis_q((0, 1, 0), yaw), axis_q((0, 0, 1), sa * sy * rng.uniform(30.0, 55.0)))
        w = rng.uniform(0.5, 0.8)
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
            w = rng.uniform(0.6, min(1.2, he[0] * 2 - 0.2))
            if rng.random() < 0.25:
                on_wall(face, u, ceil - 0.16, (w, 0.3), rng.uniform(-8, 8) + rng.choice((0, 180)),
                        rng.choice(WALL_CRACKS), rng.uniform(0.8, 0.95), CRACK_TINT, 'Wall Head Crack')
            elif rng.random() < 0.16:
                floor = FLOOR_1 if ctr[1] < 3.4 else FLOOR_2
                on_wall(face, u, floor + rng.uniform(1.2, 2.1), (rng.uniform(0.7, 1.1), 0.45),
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
        w = rng.uniform(1.0, 1.7)
        if put('Ceiling Crack', (x, ceil, z), qmul(axis_q((0, 1, 0), yaw), up), rng.choice(CEIL_CRACKS),
               (w, w * 0.6, 0.12), rng.uniform(0.85, 1.0), CRACK_TINT) and rng.random() < (0.6 if upstairs else 0.3):
            a = math.radians(yaw)
            ex, ez = x + math.cos(a) * w * 0.35, z - math.sin(a) * w * 0.35
            sw = rng.uniform(0.55, 0.95)
            put('Leak Stain', (ex, ceil, ez), qmul(axis_q((0, 1, 0), rng.uniform(0, 360)), up),
                rng.choice(LEAK_DECALS), (sw, sw * rng.uniform(0.75, 1.0), 0.1), rng.uniform(0.55, 0.8),
                [0.62, 0.55, 0.42])

    # Outside: rain streaks down the siding and green-black damp along the foundation, on the outer face only.
    for e in pk.named(r'Wall_(1st|2nd)_'):
        if e['kind'] != 'models' or e['id'] in pk.gone or n >= MAX_DECALS - 2:
            continue
        m = re.match(r'Wall_\w+?_([\d½]+)x', e['name'])
        length = float(m.group(1).replace('½', '.5')) * 0.97 if m else 1.0
        pos, rot = pk.world(e['id'])
        nz = qrot(rot, (0, 0, 1))
        v = qrot(rot, (length / 2, 0.0, 0.0))
        cx, cz = pos[0] + v[0], pos[2] + v[2]
        outward = 1 if nz[0] * (cx - house_c[0]) + nz[2] * (cz - house_c[1]) > 0 else -1
        ox, oz = cx + nz[0] * outward * 0.11, cz + nz[2] * outward * 0.11
        if pos[1] < 3.0:
            decal(s, g, f'Foundation Damp {n:03d}', (round(ox, 3), pos[1] + 0.1, round(oz, 3)), rot,
                  rng.choice(BASE_DECALS), (length + 0.3, 1.0, 0.14), rng.randint(1, 999), rng.uniform(0.7, 0.95),
                  tint=[0.22, 0.27, 0.16])
            maps.update(BASE_DECALS)
            n += 1
        if rng.random() < 0.6:
            decal(s, g, f'Rain Streak {n:03d}', (round(ox, 3), pos[1] + rng.uniform(1.6, 2.2), round(oz, 3)), rot,
                  rng.choice(LEAK_DECALS), (length * rng.uniform(0.6, 1.0), rng.uniform(1.8, 2.6), 0.14),
                  rng.randint(1, 999), rng.uniform(0.6, 0.9), tint=[0.4, 0.38, 0.3])
            maps.update(LEAK_DECALS)
            n += 1
    maps.add(CAR_DECAL)
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
    lit = []
    for name, dx, dz, yaw in (('LP_candle_triple', 1.0, -1.1, 30.0), ('LP_candle_single', 1.35, -0.8, 0.0),
                              ('LP_candle_small', 0.7, -1.35, 0.0)):
        lay_down(s, pk.fit, g, PROPS_IN + name + '.prefab', cx + dx, cz + dz, FLOOR_1, yaw, (0.0, 0.0), 1.0, lit)
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


# Small and flat things that cast no shadow worth its cost (the Forest's shadow pass): the rubbish, the books, the blown-in
# leaves and the dead wall fittings.
QUIET = re.compile(r'(Basement_box|Book_[ABC]|Garage_cardboardbox|Garage_paintbucket|Garage_paperbag|KP_glass_black|'
                   r'KP_plate|LP_magazine|Newspaper|Lamp_wall_chan|Lamp_wall_toilet|Ground_Leaves_var\d@.*)\.(FBX|fbx)$')


def quiet_shadows(s, house):
    """castShadows off on QUIET models under the house."""
    kids = {}
    for e in s.empties + s.models + s.boxes:
        kids.setdefault(e['parentId'], []).append(e)
    todo, n = [house], 0
    while todo:
        for e in kids.get(todo.pop(), []):
            todo.append(e['id'])
            if QUIET.search(e.get('path', '')):
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
        decal(s, drive, f'Car Grime {i}', pos, rot, CAR_DECAL, size, 40 + i, 0.95, tint=[0.36, 0.33, 0.25])

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
