# The Forest house's lighting (forest_house.py): every fitting and lamp works, on circuits (the engine's Light Circuit /
# Light Switch, Game/LightSwitch.h). Ceiling fittings are wired per room - several in one open room split into
# clusters, each with a wall switch by the way in - wall lights join the nearest ceiling circuit in their room, and a
# lamp is its own circuit, switched with Interact on the lamp. Everything starts off but what burned before: the red
# bulb in the master bedroom, the dying kitchen tube and the porch lamps (the candles have no switch).
#
# Light choice per fitting, with the house's cost in mind: one light per fitting (a cluster of flush ceiling discs
# shares one), ranges kept to the room (an unshadowed light goes through walls), points where the fitting lights the
# ceiling too, spots where it only throws light down. Only the fittings that light a whole room can cast shadows; the
# engine hands its shadow slots (cached, so a still room costs nothing) to the ones nearest the player.
import math

import house_fit as hf
import scene_kit as k

SWITCH_MODELS = 'assets/Props/Electrical/Models/'
SWITCH_H = 1.15                 # the switch's middle above the floor
CLUSTER = 2.7                   # ceiling fittings closer than this share a circuit
MAX_DISCS = 4                   # a cluster of flush discs bigger than this is split
CLICK = 'assets/Audio/Foley/door/latch_1.wav;assets/Audio/Foley/door/latch_2.wav'

# kind: the light; at: where on the fitting it sits ('below' its underside, 'shade' inside the shade near the top,
# 'face' in front of a wall light); drop: metres below that point; I / R / K: intensity, range, colour temperature;
# spot: half-angle (None = point); shadow: may cast.
FIT = {
    'Lamp_chandelier':       dict(at='below', drop=0.05, I=3.25, R=5.2, K=2700, spot=None, shadow=True, glow=1.3),
    'Lamp_ceiling_circle':   dict(at='below', drop=0.08, I=2.10, R=4.0, K=3000, spot=None, shadow=False, glow=1.4),
    'Lamp_bedroom':          dict(at='below', drop=0.08, I=2.40, R=4.6, K=2900, spot=None, shadow=True, glow=1.4),
    'Lamp_kitchen_A':        dict(at='below', drop=0.02, I=3.50, R=3.4, K=3000, spot=52.0, shadow=True, glow=1.4),
    'Lamp_kitchen_B':        dict(at='below', drop=0.02, I=3.50, R=3.4, K=3000, spot=52.0, shadow=True, glow=1.4),
    'Lamp_wall_chan':        dict(at='face', drop=0.0, I=0.65, R=2.2, K=2700, spot=None, shadow=False, glow=1.3),
    'Lamp_wall_toilet':      dict(at='face', drop=0.0, I=0.90, R=2.3, K=3400, spot=None, shadow=False, glow=1.3),
    'Lamp_wall_fluorescent': dict(at='below', drop=0.03, I=1.50, R=2.8, K=4300, spot=72.0, shadow=False, glow=1.4),
    'Lamp_table_A':          dict(at='shade', drop=0.14, I=1.10, R=2.5, K=2700, spot=None, shadow=False, glow=1.3),
    'Lamp_table_B':          dict(at='shade', drop=0.14, I=1.10, R=2.5, K=2700, spot=None, shadow=False, glow=1.3),
    'Lamp_stand':            dict(at='shade', drop=0.2, I=1.50, R=3.0, K=2700, spot=None, shadow=False, glow=1.3),
    'Lamp_floor':            dict(at='shade', drop=0.2, I=1.50, R=3.0, K=2700, spot=None, shadow=False, glow=1.3),
    'Lamp_wall_desk':        dict(at='below', drop=0.02, I=1.20, R=2.0, K=3200, spot=48.0, shadow=False, glow=1.4),
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
    def place_switch(self, circuit, near, floor, start_on, room_cells):
        """A switch plate for `circuit` on a wall of its room, by a doorway if one is close, else the wall nearest
        `near`; flush to the wall at SWITCH_H, clear of furniture, windows, pictures and other switches."""
        cands = []
        doors = [z for z in self.fit.zones if not z.name.startswith('Stair')]
        jambs = [q for z in doors for q in z.poly]
        # only cells by the room's walls, near a doorway or near the fittings
        near_cells = [(cx, cz) for cx, cz in room_cells
                      if any(math.hypot(cx - q[0], cz - q[1]) < 1.0 for q in jambs) or
                      math.hypot(cx - near[0], cz - near[1]) < 3.5]
        for (cx, cz) in near_cells:
            for ang in (0, 90, 180, 270):
                dx, dz = round(math.cos(math.radians(ang))), round(math.sin(math.radians(ang)))
                y = floor + SWITCH_H
                hit = None
                for step in range(1, 8):
                    d = step * 0.04
                    if self.in_wall(cx + dx * d, y, cz + dz * d):
                        hit = d
                        break
                if hit is None:
                    continue
                # the face, then back off a hair; the plate's normal points back into the room
                px, pz = cx + dx * (hit - 0.045), cz + dz * (hit - 0.045)
                cands.append((px, y, pz, -dx, -dz))
        if not cands:
            return None
        def jamb_dist(px, pz):
            best = 9.0
            for z in doors:
                for (qx, qz) in z.poly:
                    best = min(best, math.hypot(px - qx, pz - qz))
            return best

        def clear(px, y, pz, nx, nz):
            # in front: a reach's depth of room free of furniture; on the wall: no window, picture or switch
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
                if z.hits(plate):
                    return False
            for (sx, sy, sz) in self.switches:
                if math.hypot(px - sx, pz - sz) < 0.12 and abs(y - sy) < 0.2:
                    return False
            return True
        scored = []
        for (px, y, pz, nx, nz) in cands:
            jd = jamb_dist(px, pz)
            by_door = 0.0 if 0.12 < jd < 0.6 else 2.5
            scored.append((by_door + 0.25 * math.hypot(px - near[0], pz - near[1]), px, y, pz, nx, nz))
        scored.sort()
        for _, px, y, pz, nx, nz in scored[:400]:
            if clear(px, y, pz, nx, nz):
                return self.build_switch(circuit, (px, y, pz), (nx, nz), start_on)
        return None

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
                  'pathGuid': guid(self.repo, rocker_path), 'static': False, 'castShadows': 0}
        s.models += [plate, rocker]
        self.switches.append(pos)
        return plate


def rooms(fit, floor_rects, floor):
    """The storey's rooms: 0.2 m cells on its floor, walls and door leaves between them (open archways join rooms)."""
    CELL = 0.2
    rects = [r for r in floor_rects if abs(r[4] - floor) < 0.3]
    if not rects:
        return []
    walls = [b for b in fit.structure if b.ylo() < floor + 1.4 and b.yhi() > floor + 1.0]
    doors = [z for z in fit.zones if not z.name.startswith('Stair') and abs(z.y0 - floor) < 0.8]
    x0, x1 = min(r[0] for r in rects), max(r[1] for r in rects)
    z0, z1 = min(r[2] for r in rects), max(r[3] for r in rects)

    def aabb(b):
        ps = b.xz()
        return (min(p[0] for p in ps), max(p[0] for p in ps), min(p[1] for p in ps), max(p[1] for p in ps))
    wr = [aabb(b) for b in walls]
    # a door's leaf line closes the doorway between the rooms: the middle third of its keep-out
    dl = []
    for z in doors:
        xs = [p[0] for p in z.poly]
        zs = [p[1] for p in z.poly]
        cx, cz = sum(xs) / len(xs), sum(zs) / len(zs)
        dl.append((cx, cz, (max(xs) - min(xs)), (max(zs) - min(zs))))
    free = {}
    nx, nz = int((x1 - x0) / CELL) + 1, int((z1 - z0) / CELL) + 1
    for i in range(nx):
        for j in range(nz):
            x, z = x0 + (i + 0.5) * CELL, z0 + (j + 0.5) * CELL
            if not any(r[0] <= x <= r[1] and r[2] <= z <= r[3] for r in rects):
                continue
            if any(a[0] - 0.06 < x < a[1] + 0.06 and a[2] - 0.06 < z < a[3] + 0.06 for a in wr):
                continue
            if any(abs(x - cx) < max(w * 0.5, 0.15) and abs(z - cz) < max(d_ * 0.5, 0.15) and min(w, d_) < 2.0 and
                   ((w > d_ and abs(z - cz) < 0.15) or (d_ >= w and abs(x - cx) < 0.15)) for cx, cz, w, d_ in dl):
                continue
            free[(i, j)] = (x, z)
    out, seen = [], set()
    for start in free:
        if start in seen:
            continue
        comp, todo = [], [start]
        seen.add(start)
        while todo:
            c = todo.pop()
            comp.append(free[c])
            for d in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                n = (c[0] + d[0], c[1] + d[1])
                if n in free and n not in seen:
                    seen.add(n)
                    todo.append(n)
        if len(comp) * CELL * CELL >= 1.0:
            out.append(comp)
    return out


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


def wire(s, lay, pk, repo, fh, notes):
    """The house's lights, fittings, switches and lamps. Returns the Wiring (for the porch)."""
    w = Wiring(s, lay, pk, repo, fh)
    fit = pk.fit
    fittings = {}
    for e in pk.props():
        st = fh.stem(e)
        if e['id'] in pk.gone or st not in FIT:
            continue
        fittings[e['id']] = st
    rects = fh.floor_rects(pk)
    red_room = None
    n_circuits = 0
    for floor in (fh.FLOOR_1, fh.FLOOR_2):
        for ri, cells in enumerate(rooms(fit, rects, floor)):
            cell_set = {(round(x, 2), round(z, 2)) for x, z in cells}

            def inside(p):
                return any(abs(p[0] - x) <= 0.11 and abs(p[2] - z) <= 0.11 for x, z in cells) and \
                    abs(p[1] - floor) < 3.2

            here = [(i, st) for i, st in fittings.items() if inside(pk.world(i)[0])]
            ceil = [(pk.world(i)[0][0], pk.world(i)[0][2], (i, st)) for i, st in here if st in CEILING]
            for gi, group in enumerate(clusters(ceil, CLUSTER)):
                n_circuits += 1
                circuit = f'Room {n_circuits:02d}'
                ids = [p[2] for p in group]
                # the discs of one cluster share a light at their middle; chandeliers, pendants and bedroom fittings
                # each have their own
                discs = [(i, st) for i, st in ids if st == 'Lamp_ceiling_circle']
                pendants = []
                for i, st in ids:
                    if st == 'Lamp_ceiling_circle':
                        w.on_circuit(i, circuit, FIT[st]['glow'])
                        continue
                    pos, _ = w.anchor(i, FIT[st])
                    if st == 'Lamp_bedroom' and pk.by[i].get('materials') and 'embedded' in pk.by[i]['materials'][0]:
                        # the red bulb: a circuit of its own, on from the start, with its own switch
                        n_circuits += 1
                        red_room = f'Red Bulb {n_circuits:02d}'
                        w.on_circuit(i, red_room, 1.0)
                        w.light('Red Bulb', w.g, pos, dict(FIT[st], shadow=True), red_room, color=fh.RED,
                                intensity=fh.RED_I, rng=7.0,
                                extra=k.light_flicker(Mode=0, MinInterval=7.0, MaxInterval=20.0, Depth=0.85, Seed=31))
                        p0 = pk.world(i)[0]
                        w.place_switch(red_room, (p0[0], p0[2]), floor, True, cells)
                        continue
                    w.on_circuit(i, circuit, FIT[st]['glow'])
                    if st in ('Lamp_kitchen_A', 'Lamp_kitchen_B'):
                        pendants.append(pos)
                        continue
                    w.light(f'{st} {circuit}', w.g, pos, FIT[st], circuit)
                if pendants:
                    c = [sum(p[a] for p in pendants) / len(pendants) for a in range(3)]
                    spread = max(math.hypot(p[0] - c[0], p[2] - c[2]) for p in pendants)
                    f = dict(FIT['Lamp_kitchen_A'], spot=min(70.0, FIT['Lamp_kitchen_A']['spot'] + 12.0 * spread))
                    w.light(f'Pendants {circuit}', w.g, c, f, circuit,
                            intensity=f['I'] * (1.0 + 0.5 * (len(pendants) - 1)), rng=f['R'] + spread * 0.5)
                ceiling_y = (fh.CEIL_1 if floor < 3.0 else fh.CEIL_2) - 0.22
                ps = [pk.world(i)[0] for i, _ in discs]
                parts = [ps]
                while any(len(q) > MAX_DISCS for q in parts):     # split the biggest along its longer side
                    q = max(parts, key=len)
                    parts.remove(q)
                    ax = 0 if (max(p[0] for p in q) - min(p[0] for p in q)) > (max(p[2] for p in q) - min(p[2] for p in q)) else 2
                    q = sorted(q, key=lambda p: p[ax])
                    parts += [q[:len(q) // 2], q[len(q) // 2:]]
                for q in parts:
                    if not q:
                        continue
                    c = [sum(p[0] for p in q) / len(q), ceiling_y, sum(p[2] for p in q) / len(q)]
                    spread = max(math.hypot(p[0] - c[0], p[2] - c[2]) for p in q)
                    f = FIT['Lamp_ceiling_circle']
                    w.light(f'Ceiling {circuit}', w.g, c, f, circuit, intensity=f['I'] * (1.0 + 0.3 * (len(q) - 1)),
                            rng=min(f['R'] + spread * 0.6, 5.2))
                # wall lights in this room join the nearest cluster
                cx = sum(p[0] for p in group) / len(group)
                cz = sum(p[1] for p in group) / len(group)
                group_xz = (cx, cz)
                for i, st in list(here):
                    if st not in WALL or st == 'Lamp_wall_fluorescent':
                        continue
                    p = pk.world(i)[0]
                    nearest = min(clusters(ceil, CLUSTER), key=lambda g: min(math.hypot(p[0] - q[0], p[2] - q[1]) for q in g))
                    if nearest is not group:
                        continue
                    w.on_circuit(i, circuit, FIT[st]['glow'])
                    pos, _ = w.anchor(i, FIT[st])
                    w.light(f'{st} {circuit}', w.g, pos, FIT[st], circuit)
                    here.remove((i, st))
                if any(st != 'Lamp_bedroom' or not (pk.by[i].get('materials') and 'embedded' in pk.by[i]['materials'][0])
                       for i, st in ids):
                    w.place_switch(circuit, group_xz, floor, False, cells)
            # wall lights in a room with no ceiling fitting: their own circuit and switch
            loose = [(i, st) for i, st in here if st in WALL and st != 'Lamp_wall_fluorescent' and
                     not any(e.get('Light Circuit') for e in [w.scene_of(i)] if e)]
            if loose:
                n_circuits += 1
                circuit = f'Room {n_circuits:02d}'
                for i, st in loose:
                    w.on_circuit(i, circuit, FIT[st]['glow'])
                    pos, _ = w.anchor(i, FIT[st])
                    w.light(f'{st} {circuit}', w.g, pos, FIT[st], circuit)
                p = pk.world(loose[0][0])[0]
                w.place_switch(circuit, (p[0], p[2]), floor, False, cells)
    # Tubes and lamps: each its own circuit, switched on itself (Interact on the tube or the lamp).
    for i, st in fittings.items():
        e = w.scene_of(i)
        if e is None or e.get('Light Circuit'):
            continue
        if st not in PLUGGED and st != 'Lamp_wall_fluorescent':
            continue
        n_circuits += 1
        circuit = f'{st} {n_circuits:02d}'
        f = FIT[st]
        w.on_circuit(i, circuit, f['glow'])
        pos, _ = w.anchor(i, f)
        extra = None
        start_on = False
        p_world = pk.world(i)[0]
        if st == 'Lamp_wall_fluorescent' and p_world[1] > 1.0 and not notes.get('_tube'):
            # the kitchen's tube, dying: on from the start, stuttering
            notes['_tube'] = True
            start_on = True
            extra = k.light_flicker(Mode=0, MinInterval=1.5, MaxInterval=6.0, Depth=1.0, Seed=7)
        w.light(f'{st} {circuit}', w.g, pos, f, circuit, extra=extra)
        e['Light Switch'] = {'Circuit': circuit, 'Start On': start_on, 'Rocker': '', 'Rocker Angle': 0.0,
                             'On Sound': CLICK, 'Off Sound': CLICK}
    notes.pop('_tube', None)
    # Wall lights a pace apart on one circuit (the pairs either side of a mirror, a hall's run of sconces) share one
    # light between them: half the lights over a pixel, the same look. Their fittings all still glow.
    walls = [e for e in s.empties if e.get('Light Circuit') and e['name'].startswith(('Lamp_wall_chan', 'Lamp_wall_toilet'))]
    gone = set()
    for a in walls:
        if a['id'] in gone:
            continue
        mates = [b for b in walls if b is not a and b['id'] not in gone and
                 b['Light Circuit']['Circuit'] == a['Light Circuit']['Circuit'] and
                 math.dist(a['position'], b['position']) < 1.7]
        if not mates:
            continue
        group = [a] + mates
        a['position'] = [round(sum(e['position'][i] for e in group) / len(group), 4) for i in range(3)]
        a['Light']['Intensity'] = round(a['Light']['Intensity'] * (1.0 + 0.45 * len(mates)), 3)
        a['Light']['Range'] = round(a['Light']['Range'] + 0.4, 2)
        gone.update(b['id'] for b in mates)
    s.empties[:] = [e for e in s.empties if e['id'] not in gone]
    w.n_lights -= len(gone)
    notes.update({'circuits': n_circuits, 'house lights': w.n_lights, 'shadow-capable': w.n_shadow,
                  'switches': len(w.switches)})
    return w
