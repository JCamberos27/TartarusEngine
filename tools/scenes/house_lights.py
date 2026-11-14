# The Forest house's lighting (forest_house.py): a designed set, room by room. Each room has one circuit (an open-plan
# area too), one wall switch at its main way in, on the latch side of its door (the stairs have one at each end, both
# working the same circuit), and lamps switched with Interact on the lamp itself. Not every fitting works: the house has
# been dark for years, so some are dead (their shades stay unlit, no light, no switch).
#
# Every light is held to its room by the engine's light bounds (Light > Bounds: the room's box, out to the walls' faces),
# so nothing shows through a wall, floor or ceiling, shadowed or not; each bigger lit room also gets a faint, unshadowed
# bounce fill on the same circuit, so ceilings and corners aren't black. Everything starts off but what burned before:
# the red bulb in the master bedroom, the dying kitchen tube and the porch lamps (the candles have no switch).
import math

import house_fit as hf
import scene_kit as k

SWITCH_MODELS = 'assets/Props/Electrical/Models/'
SWITCH_H = 1.15                 # the switch's middle above the floor
SWITCH_OFF = 0.17               # ... and past the opening's edge (the jamb's face), along the wall: clear of the trim
CLUSTER = 2.7                   # ceiling fittings closer than this share a circuit
MAX_DISCS = 4                   # a cluster of flush discs bigger than this is split
CLICK = 'assets/Audio/Foley/door/latch_1.wav;assets/Audio/Foley/door/latch_2.wav'
# The toggle is a lit one (an old illuminated switch): a dim warm orange while its room is dark, out once the light is
# on, so a switch can be found in a black room.
ROCKER_MAT = 'assets/Props/Electrical/Materials/M_Light_Switch_White.mat'
ROCKER_GLOW = dict(emissiveColor=[1.0, 0.33, 0.07], emissiveStrength=0.08)

# kind: the light; at: where on the fitting it sits ('below' its underside, 'shade' inside the shade near the top,
# 'face' in front of a wall light); drop: metres below that point; I / R / K: intensity, range, colour temperature;
# spot: half-angle (None = point); shadow: may cast; glow: the fitting's emission kept while on (x its material's).
# Intensities sit at about a third of the first pass: the night grade's +1.8 EV lifts everything indoors 3.5x, and the
# shades read as warm diffusers, not white-hot.
FIT = {
    'Lamp_chandelier':       dict(at='below', drop=0.05, I=1.10, R=4.6, K=2700, spot=None, shadow=True, glow=0.30),
    'Lamp_ceiling_circle':   dict(at='below', drop=0.08, I=0.80, R=3.6, K=3000, spot=80.0, shadow=False, glow=0.22),
    'Lamp_bedroom':          dict(at='below', drop=0.08, I=0.85, R=4.2, K=2900, spot=None, shadow=True, glow=0.22),
    'Lamp_kitchen_A':        dict(at='below', drop=0.02, I=1.20, R=3.0, K=3000, spot=52.0, shadow=True, glow=0.12),
    'Lamp_kitchen_B':        dict(at='below', drop=0.02, I=1.20, R=3.0, K=3000, spot=52.0, shadow=True, glow=0.12),
    'Lamp_wall_chan':        dict(at='face', drop=0.0, I=0.25, R=2.0, K=2700, spot=None, shadow=False, glow=0.40),
    'Lamp_wall_toilet':      dict(at='face', drop=0.0, I=0.35, R=2.2, K=3400, spot=None, shadow=False, glow=0.15),
    'Lamp_wall_fluorescent': dict(at='below', drop=0.03, I=0.60, R=2.6, K=4300, spot=72.0, shadow=False, glow=0.60),
    'Lamp_table_A':          dict(at='shade', drop=0.14, I=0.35, R=2.2, K=2700, spot=None, shadow=False, glow=0.20),
    'Lamp_table_B':          dict(at='shade', drop=0.14, I=0.35, R=2.2, K=2700, spot=None, shadow=False, glow=0.20),
    'Lamp_stand':            dict(at='shade', drop=0.2, I=0.50, R=2.6, K=2700, spot=None, shadow=False, glow=0.20),
    'Lamp_floor':            dict(at='shade', drop=0.2, I=0.50, R=2.6, K=2700, spot=None, shadow=False, glow=0.20),
    'Lamp_wall_desk':        dict(at='below', drop=0.02, I=0.45, R=1.8, K=3200, spot=48.0, shadow=False, glow=0.50),
}
CEILING = ('Lamp_chandelier', 'Lamp_ceiling_circle', 'Lamp_bedroom', 'Lamp_kitchen_A', 'Lamp_kitchen_B')
WALL = ('Lamp_wall_chan', 'Lamp_wall_toilet', 'Lamp_wall_fluorescent')
PLUGGED = ('Lamp_table_A', 'Lamp_table_B', 'Lamp_stand', 'Lamp_floor', 'Lamp_wall_desk')


def guid(repo, path):
    import json
    import os
    with open(os.path.join(repo, 'project', path + '.meta'), encoding='utf-8') as f:
        return json.load(f)['guid']


class Wiring:
    def __init__(self, s, lay, pk, repo, fh):
        self.s, self.lay, self.pk, self.repo, self.fh = s, lay, pk, repo, fh
        self.fit = pk.fit
        self.by_id = {e['id']: e for e in s.empties + s.models + s.boxes}
        self.g = s.empty('House Lights', lay, (0, 0, 0), static=True)
        self.switches = []          # placed switch plates (centre)
        self.n_lights = self.n_shadow = 0
        # walls as world boxes (x0, x1, y0, y1, z0, z1): the switch and wall-light probes are point tests
        self.wall_aabb = []
        for b in self.fit.structure:
            ps = b.xz()
            self.wall_aabb.append((min(p[0] for p in ps), max(p[0] for p in ps), b.ylo(), b.yhi(),
                                   min(p[1] for p in ps), max(p[1] for p in ps)))
        self.furn_aabb = []
        for bs in self.fit.boxes.values():
            for b in bs:
                ps = b.xz()
                self.furn_aabb.append((min(p[0] for p in ps), max(p[0] for p in ps), b.ylo(), b.yhi(),
                                       min(p[1] for p in ps), max(p[1] for p in ps)))

    def in_wall(self, x, y, z, r=0.005):
        return any(a[0] - r < x < a[1] + r and a[2] < y < a[3] and a[4] - r < z < a[5] + r for a in self.wall_aabb)

    # --- the scene copies of pack entities
    def scene_of(self, pack_id):
        return self.by_id.get(self.pk.remap.get(pack_id))

    def subtree(self, pack_id):
        out, todo = [], [pack_id]
        while todo:
            i = todo.pop()
            if i in self.pk.gone:
                continue
            out.append(i)
            todo.extend(self.pk.kids.get(i, []))
        return out

    def on_circuit(self, pack_id, circuit, glow):
        """Every model in the fitting (pack entity `pack_id` and below) joins `circuit`, its glow `glow`; the fitting
        keeps its own lit materials (the circuit takes the emission out while it's off) and is not static, so the
        swap never rebuilds a static batch."""
        for i in self.subtree(pack_id):
            e = self.scene_of(i)
            if e is None or 'path' not in e:
                continue
            e['Light Circuit'] = {'Circuit': circuit, 'Glow': glow}
            e['static'] = False
            e['castShadows'] = 0    # its own light sits in it: no giant shadow of the fitting on the ceiling

    # --- lights
    def light(self, name, parent, pos, f, circuit, pitch=-90.0, yaw=0.0, color=None, kelvin=None, intensity=None,
              rng=None, shadows=None, extra=None):
        i = self.s.light(name, parent, pos, intensity=intensity if intensity is not None else f['I'],
                         rng=rng if rng is not None else f['R'], color=color or (1.0, 0.86, 0.7),
                         kelvin=0.0 if color else float(kelvin or f['K']),
                         shadows=f['shadow'] if shadows is None else shadows, spot=f['spot'], pitch=pitch, yaw=yaw,
                         softness=1.3)
        e = self.s.empties[-1]
        e['Light']['Volumetric Intensity'] = 0.0      # no fog indoors
        e['Light Circuit'] = {'Circuit': circuit, 'Glow': 0.0}
        e.update(extra or {})
        self.n_lights += 1
        self.n_shadow += bool(e['Light']['Cast Shadows'])
        return e

    def anchor(self, pack_id, f):
        """Where the fitting's light goes (pack space) and the fitting's box."""
        boxes = [b for i in self.subtree(pack_id) for b in self.fit.boxes.get(i, [])] or self.fit.prop_boxes(pack_id)
        if not boxes:
            p, _ = self.pk.world(pack_id)
            return p, None
        lo_y = min(b.ylo() for b in boxes)
        hi_y = max(b.yhi() for b in boxes)
        cx = sum(b.c[0] for b in boxes) / len(boxes)
        cz = sum(b.c[2] for b in boxes) / len(boxes)
        if f['at'] == 'below':
            return [cx, lo_y - f['drop'], cz], boxes
        if f['at'] == 'shade':
            # the shade's middle: just under the top of the whole lamp along its own up (a knocked-over lamp lies on
            # its side, its shade at one end)
            _, rot = self.pk.world(pack_id)
            up = hf.qrot(rot, (0, 1, 0))
            pts = [p for b in boxes for p in b.corners()]
            c = [sum(p[a] for p in pts) / len(pts) for a in range(3)]
            top = max(hf._dot(up, [p[a] - c[a] for a in range(3)]) for p in pts)
            t = max(top - f['drop'], 0.0)
            return [c[0] + up[0] * t, c[1] + up[1] * t, c[2] + up[2] * t], boxes
        # face: the middle of the wall light, a hand out from the wall
        p = [cx, (lo_y + hi_y) / 2, cz]
        n = self.wall_normal(p)
        return [p[0] + n[0] * 0.12, p[1], p[2] + n[1] * 0.12], boxes

    def wall_normal(self, p):
        """The unit (x, z) direction away from the nearest wall at p."""
        best, nrm = 1e9, (1.0, 0.0)
        for ang in range(0, 360, 15):
            dx, dz = math.cos(math.radians(ang)), math.sin(math.radians(ang))
            for step in range(1, 12):
                d = step * 0.05
                if self.in_wall(p[0] + dx * d, p[1], p[2] + dz * d):
                    if d < best:
                        best, nrm = d, (-dx, -dz)
                    break
        return nrm

    # --- the switch on the wall
    def face(self, q, n, y, reach=0.6):
        """The wall face behind point q seen from direction n (unit x, z): marching from q + n * reach back along -n,
        the first point in a wall, nudged back out. None if it starts inside a wall or meets none."""
        sx, sz = q[0] + n[0] * reach, q[1] + n[1] * reach
        if self.in_wall(sx, y, sz, r=0.0):
            return None
        for k_ in range(1, int((reach + 0.35) / 0.005)):
            x, z = sx - n[0] * k_ * 0.005, sz - n[1] * k_ * 0.005
            if self.in_wall(x, y, z, r=0.0):
                return (x + n[0] * 0.0025, z + n[1] * 0.0025)
        return None

    def door(self, name, floor):
        """The doorway of the door named `name` on that storey: centre, along (unit, towards the latch), through, half
        width."""
        for e in self.pk.ents:
            o, c = e.get('Openable'), e.get('collider')
            if not o or not c or self.pk.by[e['parentId']]['name'] != name:
                continue
            pos, rot = self.pk.world(e['id'])
            if abs(pos[1] - floor) > 0.6:
                continue
            ctr = hf.qrot(rot, c.get('center', [0, 0, 0]))
            along, through = hf.qrot(rot, (1, 0, 0)), hf.qrot(rot, (0, 0, 1))
            cx, cz = pos[0] + ctr[0], pos[2] + ctr[2]
            # the pivot is the hinge: the latch is the other side of the middle
            s_ = 1.0 if (cx - pos[0]) * along[0] + (cz - pos[2]) * along[2] > 0 else -1.0
            return (cx, cz), (along[0] * s_, along[2] * s_), (through[0], through[2]), c['halfExtents'][0]
        return None

    def opening(self, room, other):
        """Where `room` opens into `other` (an archway): its middle, its line (unit), the direction into `room`, its
        half width."""
        C = ROOM_CELL
        ka = {cell_key(x, z) for x, z in room['cells']}
        kb = {cell_key(x, z) for x, z in other['cells']}
        nb = ((1, 0), (-1, 0), (0, 1), (0, -1))
        a_side = [q for q in ka if any((q[0] + d[0], q[1] + d[1]) in kb for d in nb)]
        b_side = [q for q in kb if any((q[0] + d[0], q[1] + d[1]) in ka for d in nb)]
        if not a_side:
            return None
        ax = sum(q[0] for q in a_side) / len(a_side) * C
        az = sum(q[1] for q in a_side) / len(a_side) * C
        bx = sum(q[0] for q in b_side) / len(b_side) * C
        bz = sum(q[1] for q in b_side) / len(b_side) * C
        nx, nz = ax - bx, az - bz
        ln = math.hypot(nx, nz) or 1.0
        n = (nx / ln, nz / ln)
        a_ = (-n[1], n[0])
        ts = [(q[0] * C - ax) * a_[0] + (q[1] * C - az) * a_[1] for q in a_side]
        return ((ax + bx) / 2, (az + bz) / 2), a_, n, (max(ts) - min(ts)) / 2 + C / 2

    def place_plate(self, circuit, room, how, floor, start_on, spacing=0.12):
        """One wall switch for `circuit` in `room`: ('door', name) beside that door on its latch side (inside the
        room), ('open', other room) beside the archway into it, ('at', ((x, z), n)) on the wall behind that point seen
        from n (beside an archway's jamb), ('near', (x, z)) on the nearest wall. Flush on the
        wall at SWITCH_H, clear of trim, furniture, pictures and other switches."""
        kind, arg = how
        y = floor + SWITCH_H

        def inside(x, z):
            return room_of([room], (x, floor + 0.2, z), 0.25) is not None
        cands = []
        if kind in ('door', 'open'):
            if kind == 'door':
                d = self.door(arg, floor)
                if d is None:
                    return None
                c, along, thr, hw = d
                n = thr if inside(c[0] + thr[0] * 0.55, c[1] + thr[1] * 0.55) else (-thr[0], -thr[1])
                sides = ((1.0, 0.0), (-1.0, 3.0))      # the latch side first; the hinge side only if it must
            else:
                o = self.opening(room, self.by_room[arg])
                if o is None:
                    return None
                c, along, n, hw = o
                cx_ = sum(x for x, _ in room['cells']) / len(room['cells'])
                cz_ = sum(z for _, z in room['cells']) / len(room['cells'])
                toward = 1.0 if (cx_ - c[0]) * along[0] + (cz_ - c[1]) * along[1] > 0 else -1.0
                sides = ((toward, 0.0), (-toward, 0.5))
            for sgn, pen in sides:
                d_ = (along[0] * sgn, along[1] * sgn)
                edge = None             # the jamb's face: the first wall from the doorway's middle that way
                for k_ in range(10, 160):        # across the wall's thickness: a leaf may hang at either face
                    t = k_ * 0.005
                    if any(self.in_wall(c[0] + d_[0] * t + thr[0] * u, y, c[1] + d_[1] * t + thr[1] * u, r=0.0)
                           for u in (0.0, -0.06, 0.06, -0.12, 0.12)):
                        edge = t
                        break
                if edge is None:
                    continue
                for extra in (0.0, 0.05, 0.1):
                    off = edge + SWITCH_OFF + extra
                    q = (c[0] + d_[0] * off, c[1] + d_[1] * off)
                    f = self.face(q, n, y)
                    if f and inside(f[0] + n[0] * 0.15, f[1] + n[1] * 0.15):
                        cands.append((pen + extra, f[0], f[1], n[0], n[1]))
        elif kind == 'edge':
            (ex, ez), d_, n = arg
            for extra in (0.0, 0.05, 0.1):
                off = SWITCH_OFF + extra
                f = self.face((ex + d_[0] * off, ez + d_[1] * off), n, y, reach=0.3)
                if f:
                    cands.append((extra, f[0], f[1], n[0], n[1]))
        elif kind == 'at':
            (px, pz), n = arg
            for slide in (0.0, 0.06, -0.06, 0.12, -0.12):
                q = (px - n[1] * slide, pz + n[0] * slide)
                f = self.face(q, n, y, reach=0.3)
                if f:
                    cands.append((abs(slide), f[0], f[1], n[0], n[1]))
        else:
            px, pz = arg
            for ang in range(0, 360, 90):
                dx, dz = round(math.cos(math.radians(ang))), round(math.sin(math.radians(ang)))
                for step in range(0, 25):
                    d_ = step * 0.05
                    q = (px + dx * d_, pz + dz * d_)
                    if self.in_wall(q[0] + dx * 0.05, y, q[1] + dz * 0.05, r=0.0):
                        for slide in (0.0, 0.15, -0.15, 0.3, -0.3):
                            g = self.face((q[0] - dz * slide, q[1] + dx * slide), (-dx, -dz), y, reach=0.3)
                            if g and inside(g[0] - dx * 0.15, g[1] - dz * 0.15):
                                cands.append((d_ + abs(slide), g[0], g[1], -dx, -dz))
                        break
        cands.sort()
        own = arg if kind == 'door' else None     # beside its own door's latch jamb: never in that door's swing
        for _, px, pz, nx, nz in cands:
            if self.clear(px, y, pz, nx, nz, spacing, own):
                return self.build_switch(circuit, (px, y, pz), (nx, nz), start_on)
        self.unplaced.append((circuit, how))
        return None

    def clear(self, px, y, pz, nx, nz, spacing=0.12, own=None):
        """In front: a reach's depth free of furniture; on the wall: no window, picture, trim or other switch."""
        plate = hf.Box([px, y, pz], [[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0.07, 0.1, 0.07])
        fx, fz = px + nx * 0.35, pz + nz * 0.35
        for a in self.furn_aabb:
            if a[2] > y + 0.3 or a[3] < y - 0.3:
                continue
            if a[0] - 0.25 < fx < a[1] + 0.25 and a[4] - 0.25 < fz < a[5] + 0.25:
                return False
            if a[0] - 0.07 < px < a[1] + 0.07 and a[4] - 0.07 < pz < a[5] + 0.07:
                return False
        for z in self.fit.zones:
            if z.hits(plate) and not z.name.startswith('Stair') and z.name != own:
                return False
        for (sx, sy, sz) in self.switches:
            if math.hypot(px - sx, pz - sz) < spacing and abs(y - sy) < 0.2:
                return False
        return True

    def build_switch(self, circuit, pos, nrm, start_on):
        s = self.s
        yaw = math.degrees(math.atan2(-nrm[1], nrm[0]))         # the plate's +X out of the wall
        plate_path, rocker_path = SWITCH_MODELS + 'Light_Switch_Plate.fbx', SWITCH_MODELS + 'Light_Switch_Rocker.fbx'
        pid = s._next()
        plate = {'id': pid, 'order': pid, 'parentId': self.g, 'name': f'Light Switch {circuit}',
                 'position': [round(v, 4) for v in pos], 'rotation': k.euler_quat(yaw), 'scale': [1.0, 1.0, 1.0],
                 'path': plate_path, 'pathGuid': guid(self.repo, plate_path), 'static': False, 'castShadows': 0,
                 'collider': {'center': [0.005, 0.0, 0.0], 'friction': 0.6, 'halfExtents': [0.005, 0.057, 0.035],
                              'isTrigger': False, 'shape': 0},
                 'Light Switch': {'Circuit': circuit, 'Start On': start_on, 'Rocker': 'Rocker', 'Rocker Angle': 12.0,
                                  'On Sound': CLICK, 'Off Sound': CLICK}}
        rid = s._next()
        rocker = {'id': rid, 'order': rid, 'parentId': pid, 'name': 'Rocker', 'position': [0.0, 0.0, 0.0],
                  'rotation': [0.0, 0.0, 0.0, 1.0], 'scale': [1.0, 1.0, 1.0], 'path': rocker_path,
                  'pathGuid': guid(self.repo, rocker_path), 'static': False, 'castShadows': 0,
                  'materials': [{'embedded': k.from_mat(self.repo, ROCKER_MAT, **ROCKER_GLOW)}],
                  'Light Circuit': {'Circuit': circuit, 'Glow': 1.0, 'Glow When Off': True}}
        s.models += [plate, rocker]
        self.switches.append(pos)
        self.plates.append((circuit, pos, nrm))
        return plate


ROOM_CELL = 0.1      # the room map's cell


def cell_key(x, z):
    """The room-map cell holding (x, z)."""
    return (math.floor(x / ROOM_CELL), math.floor(z / ROOM_CELL))


DOORWAY = 0.55      # free space narrower than twice this (a doorway, a gap between wall pieces) parts two rooms


def rooms(fit, floor_rects, floor):
    """The storey's rooms as lists of (x, z) cells (ROOM_CELL apart). The free floor is eroded by DOORWAY, so every
    doorway and wall gap closes and what's left are the rooms' cores; each core then takes back the free cells nearest
    it, never through a wall. An opening wider than a doorway (an open-plan archway) keeps two areas one room."""
    import heapq
    CELL = ROOM_CELL
    rects = [r for r in floor_rects if abs(r[4] - floor) < 0.3]
    if not rects:
        return []
    walls = [b for b in fit.structure if b.ylo() < floor + 1.4 and b.yhi() > floor + 1.0]
    x0, x1 = min(r[0] for r in rects), max(r[1] for r in rects)
    z0, z1 = min(r[2] for r in rects), max(r[3] for r in rects)

    def aabb(b):
        ps = b.xz()
        return (min(p[0] for p in ps), max(p[0] for p in ps), min(p[1] for p in ps), max(p[1] for p in ps))
    wr = [aabb(b) for b in walls]
    nx, nz = int((x1 - x0) / CELL) + 1, int((z1 - z0) / CELL) + 1
    free = {}
    for i in range(nx):
        x = x0 + (i + 0.5) * CELL
        col = [a for a in wr if a[0] - 0.05 < x < a[1] + 0.05]
        rx = [r for r in rects if r[0] <= x <= r[1]]
        for j in range(nz):
            z = z0 + (j + 0.5) * CELL
            if not any(r[2] <= z <= r[3] for r in rx):
                continue
            if any(a[2] - 0.05 < z < a[3] + 0.05 for a in col):
                continue
            free[(i, j)] = (x, z)
    # distance to the nearest blocked cell (chamfer 1 / 1.41)
    dist = {}
    heap = []
    for c in free:
        for d in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if (c[0] + d[0], c[1] + d[1]) not in free:
                dist[c] = 0.5 * CELL
                heap.append((dist[c], c))
                break
    heapq.heapify(heap)
    N8 = [(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0), (1, 1, 1.414), (1, -1, 1.414), (-1, 1, 1.414),
          (-1, -1, 1.414)]
    while heap:
        dc, c = heapq.heappop(heap)
        if dc > dist.get(c, 1e9):
            continue
        for dx, dz, w in N8:
            n = (c[0] + dx, c[1] + dz)
            if n in free and dc + w * CELL < dist.get(n, 1e9):
                dist[n] = dc + w * CELL
                heapq.heappush(heap, (dist[n], n))
    # cores, then each grows back over the free cells (the nearest core wins)
    core = {c for c in free if dist.get(c, 0.0) >= DOORWAY}
    label, seeds = {}, []
    for start in core:
        if start in label:
            continue
        comp, todo = [start], [start]
        label[start] = len(seeds)
        while todo:
            c = todo.pop()
            for dx, dz, _ in N8[:4]:
                n = (c[0] + dx, c[1] + dz)
                if n in core and n not in label:
                    label[n] = len(seeds)
                    comp.append(n)
                    todo.append(n)
        if len(comp) * CELL * CELL < 0.6:
            for c in comp:
                del label[c]
            continue
        seeds.append(comp)
    todo = [c for c in label]
    while todo:
        nxt = []
        for c in todo:
            for dx, dz, _ in N8[:4]:
                n = (c[0] + dx, c[1] + dz)
                if n in free and n not in label:
                    label[n] = label[c]
                    nxt.append(n)
        todo = nxt
    out = [[] for _ in seeds]
    for c, l in label.items():
        out[l].append(free[c])
    return [r for r in out if len(r) * CELL * CELL >= 1.0]


# The rooms by name: a point (pack x, z) inside each, per storey.
ROOM_NAMES = [
    ('ground', -33.0, 9.0, 'living room'), ('ground', -37.3, -5.0, 'stair storeroom'),
    ('ground', -33.0, -12.0, 'kitchen-dining'), ('ground', -32.7, 5.5, 'downstairs bathroom'),
    ('ground', -33.0, -3.0, 'entrance hall'),
    ('upstairs', -34.0, 10.0, "child's bedroom"), ('upstairs', -37.0, -4.0, 'upstairs den'),
    ('upstairs', -32.0, -11.0, 'master bedroom'), ('upstairs', -36.5, 6.3, 'landing'),
    ('upstairs', -33.5, 2.3, 'upstairs hall'), ('upstairs', -33.0, -4.7, 'upstairs bathroom'),
    ('upstairs', -36.0, -13.8, 'box room'), ('upstairs', -37.5, 10.0, 'closet room'),
]


def named_rooms(fit, floor_rects, fh):
    """Every room of both storeys: {'name', 'storey', 'floor', 'ceil', 'cells'} (unnamed ones 'room N')."""
    out = []
    for floor, ceil, storey in ((fh.FLOOR_1, fh.CEIL_1, 'ground'), (fh.FLOOR_2, fh.CEIL_2, 'upstairs')):
        for n, cells in enumerate(rooms(fit, floor_rects, floor)):
            have = {cell_key(x, z) for x, z in cells}
            name = next((nm for st, x, z, nm in ROOM_NAMES if st == storey and
                         cell_key(x, z) in have), f'{storey} room {n + 1}')
            out.append({'name': name, 'storey': storey, 'floor': floor, 'ceil': ceil, 'cells': cells})
    return out


def room_of(rooms_, p, slack=0.35):
    """The named room holding pack point p (x, y, z), or None."""
    ci, cj = cell_key(p[0], p[2])
    reach = int(slack / ROOM_CELL) + 1
    best = None
    for r in rooms_:
        if not (r['floor'] - 0.15 <= p[1] <= r['ceil'] + 0.06):     # a ceiling fitting belongs to the room below
            continue
        if 'keys' not in r:
            r['keys'] = {cell_key(x, z) for x, z in r['cells']}
        for di in range(-reach, reach + 1):
            for dj in range(-reach, reach + 1):
                d = di * di + dj * dj
                if (ci + di, cj + dj) in r['keys'] and d * ROOM_CELL ** 2 < slack * slack and (best is None or d < best[0]):
                    best = (d, r)
    return best[1] if best else None


def room_box(cells, at, fit, floor, ceil, pad=0.05):
    """The biggest axis-aligned box of the room's cells round `at` (x, z), grown a row at a time while the whole row is
    the room's; its sides pushed out to the walls' faces plus `pad`, floor to ceiling. (x0, x1, y0, y1, z0, z1)."""
    CELL = ROOM_CELL
    have = {cell_key(x, z) for x, z in cells}
    ci, cj = cell_key(at[0], at[1])
    if (ci, cj) not in have:
        ci, cj = min(have, key=lambda c: (c[0] - ci) ** 2 + (c[1] - cj) ** 2)
    i0 = i1 = ci
    j0 = j1 = cj
    grew = True
    while grew:
        grew = False
        if all((i1 + 1, j) in have for j in range(j0, j1 + 1)):
            i1 += 1; grew = True
        if all((i0 - 1, j) in have for j in range(j0, j1 + 1)):
            i0 -= 1; grew = True
        if all((i, j1 + 1) in have for i in range(i0, i1 + 1)):
            j1 += 1; grew = True
        if all((i, j0 - 1) in have for i in range(i0, i1 + 1)):
            j0 -= 1; grew = True
    bx0, bx1, bz0, bz1 = i0 * CELL, (i1 + 1) * CELL, j0 * CELL, (j1 + 1) * CELL
    # out to the wall faces (the cells stop short of them), but no further than one cell's slack
    y = floor + 1.2
    walls = [b for b in fit.structure if b.ylo() < y < b.yhi()]

    def hits(x, z):
        for b in walls:
            ps = b.xz()
            if min(p[0] for p in ps) <= x <= max(p[0] for p in ps) and min(p[1] for p in ps) <= z <= max(p[1] for p in ps):
                return True
        return False
    mx, mz = (bx0 + bx1) / 2, (bz0 + bz1) / 2

    def push(edge, axis, sign):
        for k in range(0, 13):
            v = edge + sign * k * 0.01
            if (hits(v, mz) if axis == 0 else hits(mx, v)):
                return v + sign * pad
        return edge + sign * 0.12
    return (push(bx0, 0, -1), push(bx1, 0, 1), floor - pad, ceil + pad, push(bz0, 2, -1), push(bz1, 2, 1))
def clusters(points, gap):
    """Single-link clusters of (x, z, payload) closer than `gap`."""
    groups = []
    for p in points:
        joined = [g for g in groups if any(math.hypot(p[0] - q[0], p[1] - q[1]) < gap for q in g)]
        merged = [p]
        for g in joined:
            merged += g
            groups.remove(g)
        groups.append(merged)
    return groups


# The circuits, room by room: (circuit, on at the start, its switches). A switch is ('door', door name) by that door
# on its latch side, ('open', room) by the archway into that room, or ('near', (x, z)) on the wall nearest that point.
ROOM_CIRCUITS = {
    # archways ('edge': the opening's edge, the way along the wall from it, the way into the room), the same rule as
    # the doors: inside the room, on the wall beside the opening, SWITCH_OFF past its edge
    'kitchen-dining': ('Kitchen', False, [('edge', ((-35.79, -7.86), (-1.0, 0.0), (0.0, -1.0)))]),
    'living room': ('Living room', False, [('edge', ((-34.05, 3.98), (0.0, 1.0), (-1.0, 0.0)))]),
    'entrance hall': ('Hall', False, [('door', 'Door_exterior_paneled')]),
    'downstairs bathroom': ('Downstairs bathroom', False, [('door', 'Door_interior')]),
    'master bedroom': ('Master bedroom', True, [('door', 'Door_interior (2)')]),
    'upstairs den': ('Den', False, [('door', 'Door_interior (2)')]),
    'upstairs bathroom': ('Upstairs bathroom', False, [('door', 'Door_interior (3)')]),
    "child's bedroom": ("Child's bedroom", False, [('door', 'Door_interior (7)')]),
    'upstairs hall': ('Stairs', False, []),
    'landing': ('Stairs', False, []),
}
# The stairs' two-way switch: at the foot (the ground floor's corridor) and at the top (the landing).
# The stairs' two ends, the same rule: at the foot, inside the stair hall through the double archway, beside its
# edge; at the top, on the landing's wall beside the last step.
STAIR_SWITCHES = [('kitchen-dining', ('edge', ((-34.05, 0.36), (0.0, -1.0), (-1.0, 0.0)))),
                  ('landing', ('edge', ((-38.61, 5.85), (0.0, 1.0), (1.0, 0.0))))]
# The stairwell, two storeys tall with no floor of its own: its lights' bounds (pack x0, x1, z0, z1).
STAIRWELL = (-38.75, -34.95, -0.95, 4.9)
# Fittings that no longer work (pack ids): blown bulbs, a cut wire. They show their unlit shade and give no light.
DEAD = {1047, 1174, 996,    # the master bedroom's flush discs: only the red bulb is left
        1218,               # the closet room
        1226}               # under the stairs
FILL_I = 0.16               # a room's bounce fill: x the sum of its own lights' intensities
FILL_MIN_AREA = 12.0        # rooms smaller than this get none (their fitting reaches every corner)
# A room's switch must be lit once its light is on. The room's light boxes are found from the walls' collision shapes,
# which stand a few cm proud of the plaster, so first every box of the circuit that holds the room in front of the
# switch is pushed SWITCH_WALL into the switch's wall. Then the light the circuit's lights put on the wall there
# (sum of I x range window x the wall's cosine / d^2, a spot only inside its cone) must be at least SWITCH_LUX - the hall's, where the
# plate reads plainly. Short of it, a faint 'Spill' light under the ceiling a pace into the room makes up the rest:
# the room's light reaching its entrance (a passage, a corner the fitting's range stops short of).
SWITCH_WALL = 0.05
SWITCH_LUX = 0.04
SPILL_IN = 1.0              # the spill light: this far into the room from the switch ...
SPILL_DROP = 0.3            # ... this far under the ceiling
SPILL_R = 3.0
DISC_GAP = 2.0              # flush discs closer than this share one light...
DISC_SKIP = 2.6             # ... and a group with a chandelier or pendant this near gives none (it only glows)
DISC_SKIP_IN = {'kitchen-dining': 1.5}   # the kitchen's counter run needs its own light beside the pendants
# The entrance hall had only sconces: two flush discs down its middle (pack x, z), like the rest of the house's.
HALL_DISCS = [(-33.0, -4.0), (-33.0, 1.2)]
DISC_PREFAB = 'assets/Environments/SuburbNeighborhoodHousePack/Prefabs/Props_interior/Lamp_ceiling_circle.prefab'


def wire(s, lay, pk, repo, fh, notes):
    """The house's lights, fittings, switches and lamps. Returns the Wiring (for the porch)."""
    w = Wiring(s, lay, pk, repo, fh)
    fit = pk.fit
    rooms_ = named_rooms(fit, fh.floor_rects(pk), fh)
    w.rooms = rooms_
    w.by_room = by_name = {r['name']: r for r in rooms_}
    w.unplaced = []
    w.plates = []
    w.n_dead = 0
    w.n_spill = 0
    by_circuit = {}
    w.circuit_I = {}
    fittings = {}
    for e in pk.props():
        st = fh.stem(e)
        if e['id'] in pk.gone or st not in FIT:
            continue
        fittings[e['id']] = st

    def upright(i):
        return abs(hf.qrot(pk.world(i)[1], (0, 1, 0))[1]) > 0.7

    def dead(i):
        for j in w.subtree(i):
            e = w.scene_of(j)
            if e is not None and 'path' in e:
                e['materials'] = [dict(fh.OFF)]
        w.n_dead += 1

    def bounds_of(room, p):
        """The light's room box (pack space) round point p, as Light > Bounds keys."""
        if room is None:          # the stairwell
            x0, x1, z0, z1 = STAIRWELL
            y0, y1 = fh.FLOOR_1 - 0.05, fh.CEIL_2 + 0.05
        else:
            x0, x1, y0, y1, z0, z1 = room_box(room['cells'], (p[0], p[2]), fit, room['floor'], room['ceil'])
        return {'Bounds': True,
                'Bounds Center': [round((x0 + x1) / 2, 3), round((y0 + y1) / 2, 3), round((z0 + z1) / 2, 3)],
                'Bounds Size': [round(x1 - x0, 3), round(y1 - y0, 3), round(z1 - z0, 3)], 'Bounds Fade': 0.05}

    def lit(name, pos, f, circuit, room, **kw):
        e = w.light(name, w.g, pos, f, circuit, **kw)
        e['Light'].update(bounds_of(room, pos))
        w.circuit_I[circuit] = w.circuit_I.get(circuit, 0.0) + e['Light']['Intensity']
        by_circuit.setdefault(circuit, []).append((e, pos))
        return e

    def inside(L, q):
        c, sz = L['Bounds Center'], L['Bounds Size']
        return all(abs(q[a] - c[a]) <= sz[a] / 2 for a in range(3))

    def lux_at(circuit, q, n):
        """Roughly how much light the circuit's lights put on a wall at point q (pack space) facing n (x, z)."""
        tot = 0.0
        for e, p in by_circuit.get(circuit, []):
            L = e['Light']
            if not inside(L, q):
                continue
            v = [q[a] - p[a] for a in range(3)]
            d = max(math.sqrt(sum(x * x for x in v)), 0.3)
            if d >= L['Range']:
                continue
            if L['Type'] == 'Spot' and -v[1] / d < math.cos(math.radians(L['Spot Angle'] / 2)):
                continue        # outside a down-spot's cone
            facing = max(0.0, -(v[0] * n[0] + v[2] * n[1]) / d)     # the wall turned to the light (cosine)
            tot += L['Intensity'] * (1.0 - (d / L['Range']) ** 4) ** 2 / (d * d) * facing
        return tot

    def light_switch(circuit, room, n_before):
        """The plates just placed for `circuit`: their wall inside the room's light boxes, and a spill light where
        the room's light still leaves them dark."""
        for _, (px, py, pz), (nx, nz) in w.plates[n_before:]:
            a = 0 if abs(nx) > abs(nz) else 2          # the wall's normal axis (walls are axis-aligned)
            sgn = (nx if a == 0 else nz) > 0
            front = (px + nx * 0.3, py, pz + nz * 0.3)
            face = (px if a == 0 else pz) - (1 if sgn else -1) * SWITCH_WALL
            for e, _ in by_circuit.get(circuit, []):
                L = e['Light']
                if not inside(L, front):
                    continue
                lo = L['Bounds Center'][a] - L['Bounds Size'][a] / 2
                hi = L['Bounds Center'][a] + L['Bounds Size'][a] / 2
                if sgn and face < lo:
                    lo = face
                elif not sgn and face > hi:
                    hi = face
                L['Bounds Center'][a] = round((lo + hi) / 2, 3)
                L['Bounds Size'][a] = round(hi - lo, 3)
            have = lux_at(circuit, (px + nx * 0.02, py, pz + nz * 0.02), (nx, nz))
            if have >= SWITCH_LUX * 0.75 or not by_circuit.get(circuit):     # near enough: no light for a sliver
                continue
            sp = (px + nx * SPILL_IN, room['ceil'] - SPILL_DROP, pz + nz * SPILL_IN)
            d = math.dist(sp, (px, py, pz))
            need = (SWITCH_LUX - have) * d * d / (1.0 - (d / SPILL_R) ** 4) ** 2 / (SPILL_IN / d)
            src = max(by_circuit[circuit], key=lambda ep: ep[0]['Light']['Intensity'])[0]['Light']
            red_ = src.get('ColorTempK', 0.0) == 0.0 and src['Color'][1] < 0.5
            e = lit(f'Spill {circuit}', sp, dict(FIT['Lamp_ceiling_circle'], shadow=False, spot=None), circuit, room,
                    intensity=round(need, 3), rng=SPILL_R, color=fh.RED if red_ else None,
                    kelvin=None if red_ else 3000.0)
            L = e['Light']                               # its own box reaches the switch's wall too
            lo = L['Bounds Center'][a] - L['Bounds Size'][a] / 2
            hi = L['Bounds Center'][a] + L['Bounds Size'][a] / 2
            lo, hi = (min(lo, face), hi) if sgn else (lo, max(hi, face))
            L['Bounds Center'][a] = round((lo + hi) / 2, 3)
            L['Bounds Size'][a] = round(hi - lo, 3)
            w.n_spill += 1

    in_room = {}
    for i, st in fittings.items():
        r = room_of(rooms_, pk.world(i)[0], 0.6)
        in_room.setdefault(r['name'] if r else None, []).append((i, st))

    # The red bulb: the master bedroom's ceiling fitting, a bare red bulb that never went out.
    red = next((i for i, st in fittings.items() if st == 'Lamp_bedroom' and pk.by[i].get('materials')), None)
    stairs_lights = [i for i, st in in_room.get(None, []) if st == 'Lamp_bedroom' and i not in DEAD]
    for rname, (circuit, start_on, plates) in ROOM_CIRCUITS.items():
        room = by_name.get(rname)
        if room is None:
            continue
        here = [(i, st) for i, st in in_room.get(rname, []) if i not in DEAD]
        ceil = [(i, st) for i, st in here if st in CEILING]
        mains = []
        if rname == 'entrance hall' and not ceil:
            for x, z in HALL_DISCS:
                r = s.stamp(DISC_PREFAB, w.g, (x, room['ceil'], z), name='Lamp_ceiling_circle (hall)')
                for m in s.models:
                    if m['parentId'] == r:
                        m.update({'Light Circuit': {'Circuit': circuit, 'Glow': FIT['Lamp_ceiling_circle']['glow']},
                                  'static': False, 'castShadows': 0})
                lit(f'Ceiling {circuit}', (x, room['ceil'] - 0.22, z), FIT['Lamp_ceiling_circle'], circuit, room)
        for i, st in ceil:
            if st == 'Lamp_ceiling_circle':
                continue
            w.on_circuit(i, circuit, FIT[st]['glow'] if i != red else 1.0)
            pos, _ = w.anchor(i, FIT[st])
            if i == red:
                lit('Red Bulb', pos, dict(FIT[st], shadow=True), circuit, room, color=fh.RED, intensity=fh.RED_I,
                    rng=6.0, extra=k.light_flicker(Mode=0, MinInterval=7.0, MaxInterval=20.0, Depth=0.85, Seed=31))
            elif st in ('Lamp_kitchen_A', 'Lamp_kitchen_B'):
                mains.append(('pendant', pos))
                continue
            else:
                lit(f'{st} {circuit}', pos, FIT[st], circuit, room)
            mains.append((st, pos))
        pend = [p for k_, p in mains if k_ == 'pendant']
        if pend:
            c = [sum(p[a] for p in pend) / len(pend) for a in range(3)]
            spread = max(math.hypot(p[0] - c[0], p[2] - c[2]) for p in pend)
            f = dict(FIT['Lamp_kitchen_A'], spot=min(70.0, FIT['Lamp_kitchen_A']['spot'] + 12.0 * spread))
            lit(f'Pendants {circuit}', c, f, circuit, room, intensity=f['I'] * (1.0 + 0.2 * (len(pend) - 1)),
                rng=f['R'] + spread * 0.5)
        # flush discs: they all glow; a group shares one light, unless a bigger fitting is right there
        discs = [(pk.world(i)[0][0], pk.world(i)[0][2], i) for i, st in ceil if st == 'Lamp_ceiling_circle']
        for d in discs:
            w.on_circuit(d[2], circuit, FIT['Lamp_ceiling_circle']['glow'])
        groups = []
        for g in clusters(discs, DISC_GAP):
            # a long row (a kitchen's run of discs) shares a light every three
            ax = 0 if max(p[0] for p in g) - min(p[0] for p in g) > max(p[1] for p in g) - min(p[1] for p in g) else 1
            g = sorted(g, key=lambda p: p[ax])
            n = max(1, round(len(g) / 3))
            groups += [g[j * len(g) // n:(j + 1) * len(g) // n] for j in range(n)]
        for g in groups:
            cx = sum(p[0] for p in g) / len(g)
            cz = sum(p[1] for p in g) / len(g)
            if any(math.hypot(p[0] - cx, p[2] - cz) < DISC_SKIP_IN.get(rname, DISC_SKIP) for _, p in mains):
                continue
            f = FIT['Lamp_ceiling_circle']
            spread = max(math.hypot(p[0] - cx, p[1] - cz) for p in g)
            lit(f'Ceiling {circuit}', (cx, room['ceil'] - 0.22, cz), f, circuit, room,
                intensity=f['I'] * (1.0 + 0.3 * (len(g) - 1)), rng=min(f['R'] + spread * 0.6, 5.2))
        # the wall lights left (one over each bathroom mirror) join the room's circuit; a pair a pace apart shares one
        walls = [(i, st) for i, st in here if st in ('Lamp_wall_toilet', 'Lamp_wall_chan')]
        for g in clusters([(pk.world(i)[0][0], pk.world(i)[0][2], (i, st)) for i, st in walls], 1.7):
            pts = []
            for _, _, (i, st) in g:
                w.on_circuit(i, circuit, FIT[st]['glow'])
                pts.append(w.anchor(i, FIT[st])[0])
            c = [sum(p[a] for p in pts) / len(pts) for a in range(3)]
            st = g[0][2][1]
            lit(f'{st} {circuit}', c, FIT[st], circuit, room, intensity=FIT[st]['I'] * (1.0 + 0.45 * (len(g) - 1)))
        if rname == 'landing':
            for i in stairs_lights:
                w.on_circuit(i, circuit, FIT['Lamp_bedroom']['glow'])
                pos, _ = w.anchor(i, FIT['Lamp_bedroom'])
                lit(f'Stairwell {circuit}', pos, FIT['Lamp_bedroom'], circuit, None)
        # the room's bounce: faint, unshadowed, at the room's middle under the ceiling
        area = len(room['cells']) * ROOM_CELL ** 2
        if w.circuit_I.get(circuit) and area >= FILL_MIN_AREA and rname not in ('landing', 'upstairs hall'):
            cx = sum(x for x, _ in room['cells']) / len(room['cells'])
            cz = sum(z for _, z in room['cells']) / len(room['cells'])
            box = room_box(room['cells'], (cx, cz), fit, room['floor'], room['ceil'])
            reach = math.hypot(box[1] - box[0], box[5] - box[4]) / 2 + 0.8
            red_fill = circuit == 'Master bedroom'
            lit(f'Bounce {circuit}', ((box[0] + box[1]) / 2, room['ceil'] - 0.7, (box[4] + box[5]) / 2),
                dict(FIT['Lamp_ceiling_circle'], shadow=False, spot=None), circuit, room,
                intensity=round(FILL_I * w.circuit_I[circuit], 3), rng=round(min(reach, 7.0), 2),
                color=fh.RED if red_fill else None, kelvin=None if red_fill else 3000.0)
        n0 = len(w.plates)
        for how in plates:
            w.place_plate(circuit, room, how, room['floor'], start_on)
        light_switch(circuit, room, n0)
    for rname, how in STAIR_SWITCHES:
        room = by_name.get(rname)
        if room:
            n0 = len(w.plates)
            w.place_plate('Stairs', room, how, room['floor'], False)
            light_switch('Stairs', room, n0)
    # The fittings no room circuit took: dead.
    for i, st in fittings.items():
        e = w.scene_of(i)
        if e is None or e.get('Light Circuit'):
            continue
        if st in CEILING or st in ('Lamp_wall_chan', 'Lamp_wall_toilet') or i in DEAD:
            dead(i)
    # Lamps and the tubes left: each its own circuit, switched on itself (Interact on the lamp). One knocked over
    # stays dark - its bulb broke when it fell - but its switch still clicks.
    n_circuits = len({c for c, _, _ in ROOM_CIRCUITS.values()})
    for i, st in fittings.items():
        e = w.scene_of(i)
        if e is None or e.get('Light Circuit') or (st not in PLUGGED and st != 'Lamp_wall_fluorescent') or i in DEAD:
            continue
        n_circuits += 1
        circuit = f'{st} {n_circuits:02d}'
        f = FIT[st]
        e['Light Switch'] = {'Circuit': circuit, 'Start On': False, 'Rocker': '', 'Rocker Angle': 0.0,
                             'On Sound': CLICK, 'Off Sound': CLICK}
        if not upright(i):
            dead(i)
            continue
        room = room_of(rooms_, pk.world(i)[0], 0.6)
        w.on_circuit(i, circuit, f['glow'])
        pos, _ = w.anchor(i, f)
        extra = None
        if st == 'Lamp_wall_fluorescent':
            # the kitchen's tube under the wall cabinets, dying: on from the start, stuttering
            e['Light Switch']['Start On'] = True
            extra = k.light_flicker(Mode=0, MinInterval=1.5, MaxInterval=6.0, Depth=1.0, Seed=7)
        lit(f'{st} {circuit}', pos, f, circuit, room, extra=extra)
    notes.update({'circuits': n_circuits, 'house lights': w.n_lights, 'shadow-capable': w.n_shadow,
                  'switches': len(w.switches), 'switch spills': w.n_spill,
                  'dead fittings': w.n_dead})
    if w.unplaced:
        notes['unplaced switches'] = w.unplaced
    return w
