# Builds project/scenes/Apartment_3Bed.json: a furnished three-bedroom apartment (master with ensuite, two more
# bedrooms, living room, kitchen + dining, bathroom, laundry, hall) from the imported model library.
#
# Plan coordinates are metres: X runs east, Z runs south, the floor is y = 0. Walls are listed by centre line and
# built as two half-thickness boxes so each side carries its own room's paint. Every model is placed by its measured
# bounds (scene_kit.model_index), so a placement names where the piece's footprint goes, not where its pivot is.
#
#   python tools/scenes/build_apartment.py <repo> [--shots <dir>]   (--shots also writes per-room review cameras)
import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scene_kit as k  # noqa: E402

H = 2.6          # ceiling height (the imported wall modules' height)
T = 0.12         # wall thickness
DOOR_W, DOOR_H = 0.92, 2.06   # opening for Door_Op_1's frame (0.90 x 2.05; the trim covers the gap)
DOOR_WALL_Z = 0.032           # the frame's wall centre, door-local +Z of its origin

ROOMS = {  # centre-line rectangles (x0, z0, x1, z1)
    'Master Bedroom': (0.0, 0.0, 4.4, 4.2),
    'Ensuite': (4.4, 0.0, 6.6, 4.2),
    'Bedroom 2': (6.6, 0.0, 10.0, 4.2),
    'Kids Bedroom': (10.0, 0.0, 13.0, 4.2),
    'Hall': (6.0, 4.2, 13.0, 5.4),
    'Bathroom': (10.4, 5.4, 13.0, 8.3),
    'Laundry': (10.4, 8.3, 13.0, 10.0),
    'Kitchen': (6.0, 5.4, 10.4, 10.0),
    'Living Room': (0.0, 4.2, 6.0, 10.0),
}

# (axis, fixed coordinate, from, to, openings [(centre, width, height)]); axis 'x' = runs along X at z = fixed.
DOOR = (DOOR_W, DOOR_H)
WALLS = [
    # exterior
    ('x', 0.0, 0.0, 4.4, []), ('x', 0.0, 4.4, 6.6, []), ('x', 0.0, 6.6, 10.0, []), ('x', 0.0, 10.0, 13.0, []),
    ('x', 10.0, 0.0, 6.0, []), ('x', 10.0, 6.0, 10.4, []), ('x', 10.0, 10.4, 13.0, []),
    ('z', 0.0, 0.0, 4.2, []), ('z', 0.0, 4.2, 10.0, []),
    ('z', 13.0, 0.0, 4.2, []), ('z', 13.0, 4.2, 5.4, [(4.8,) + DOOR]), ('z', 13.0, 5.4, 8.3, []),
    ('z', 13.0, 8.3, 10.0, []),
    # bedrooms / hall line
    ('x', 4.2, 0.0, 4.4, [(2.9,) + DOOR]), ('x', 4.2, 4.4, 6.0, []), ('x', 4.2, 6.0, 6.6, []),
    ('x', 4.2, 6.6, 10.0, [(7.3,) + DOOR]), ('x', 4.2, 10.0, 13.0, [(10.7,) + DOOR]),
    ('z', 4.4, 0.0, 4.2, [(3.5,) + DOOR]), ('z', 6.6, 0.0, 4.2, []), ('z', 10.0, 0.0, 4.2, []),
    # hall / kitchen / bathroom line
    ('x', 5.4, 6.0, 10.4, []), ('x', 5.4, 10.4, 13.0, [(11.1,) + DOOR]),
    ('z', 6.0, 5.4, 10.0, [(7.5, 2.4, 2.2)]),
    ('z', 10.4, 5.4, 8.3, []), ('z', 10.4, 8.3, 10.0, [(9.15,) + DOOR]),
    ('x', 8.3, 10.4, 13.0, []),
]

# Doors: (name, wall axis, fixed, centre, which side it swings into: +1 / -1 along the wall's normal, exterior?)
DOORS = [
    ('Front Door', 'z', 13.0, 4.8, -1, True),
    ('Master Door', 'x', 4.2, 2.9, -1, False),
    ('Ensuite Door', 'z', 4.4, 3.5, +1, False),
    ('Bedroom 2 Door', 'x', 4.2, 7.3, -1, False),
    ('Kids Bedroom Door', 'x', 4.2, 10.7, -1, False),
    ('Bathroom Door', 'x', 5.4, 11.1, +1, False),
    ('Laundry Door', 'z', 10.4, 9.15, +1, False),
]


def room_at(x, z):
    for name, (x0, z0, x1, z1) in ROOMS.items():
        if x0 < x < x1 and z0 < z < z1:
            return name
    return None


def inner(room):
    """The room's interior rectangle (wall faces)."""
    x0, z0, x1, z1 = ROOMS[room]
    return x0 + T / 2, z0 + T / 2, x1 - T / 2, z1 - T / 2


class Builder:
    def __init__(self, repo):
        self.repo = repo
        self.idx = k.model_index(repo)
        base = json.load(open(os.path.join(repo, 'project', 'scenes', 'Apartment.json'), encoding='utf-8'))
        self.s = k.Scene(base)
        self.d = self.s.data
        self.mats = self._materials()
        self.root = self.s.empty('Apartment')
        self.groups = {}
        self.placed = []
        self.cams = []

    # --- materials -------------------------------------------------------------------------------------------
    def _materials(self):
        def tex(folder, stem, ext='png', maps=('Base_Color', 'Normal', 'Roughness', 'AO')):
            base = f'assets/Architecture/Building/Textures/{folder}/{stem}_'
            have = lambda m: os.path.exists(os.path.join(self.repo, 'project', base + m + '.' + ext))  # noqa: E731
            return {key: base + m + '.' + ext if have(m) else None
                    for key, m in zip(('albedo', 'normal', 'rough', 'ao'), maps)}

        def ph(folder, stem):
            base = f'assets/Architecture/Building/Textures/{folder}/{stem}_'
            return {'albedo': base + 'Diffuse_2k.jpg', 'normal': base + 'nor_gl_2k.jpg', 'rough': base + 'Rough_2k.jpg',
                    'ao': base + 'AO_2k.jpg'}
        # Paint: the plaster's normal / roughness under a flat colour. (The Wall_* sets are UV atlases with a
        # baseboard strip in them - tiled triplanar they smear trim bands across the wall.)
        plaster = ph('PH_Beige_Wall', 'beige_wall_001')

        def paint(rgb, rough=0.9):
            return k.textured(self.repo, normal=plaster['normal'], rough=plaster['rough'], base=rgb, roughness=rough,
                              triplanar_scale=0.5)
        M = {'wall_white': paint((0.86, 0.85, 0.82)), 'wall_sage': paint((0.66, 0.72, 0.62)),
             'wall_blue': paint((0.32, 0.42, 0.56)), 'tile_wall': paint((0.9, 0.91, 0.92), 0.45),
             'wall_beige': k.textured(self.repo, **plaster, triplanar_scale=0.5)}
        # Floor scales from the boards in each texture (triplanar scale = tiles per metre): the plank set has ~9
        # boards a tile -> 0.9 gives 12 cm boards; the laminate ~8 -> 0.8 gives 16 cm; the strip floor ~13 -> 0.45.
        M['hardwood'] = k.textured(self.repo, **ph('PH_Dark_Wooden_Planks', 'dark_wooden_planks'), base=(1.25, 1.2, 1.15),
                                   triplanar_scale=0.9)
        M['laminate'] = k.textured(self.repo, **ph('PH_Laminate_Floor', 'laminate_floor_02'), triplanar_scale=0.8)
        M['strip'] = k.textured(self.repo, **tex('Floor_Hardwood', 'Floor_Hardwood'), base=(1.3, 1.25, 1.2),
                                triplanar_scale=0.45)
        # wet rooms: Poly Haven's Interior Tiles (CC0), a 1.9 m sample -> one repeat every 1.9 m keeps the tiles their
        # real size
        tiles = 'assets/Architecture/Building/Textures/PH_Interior_Tiles/interior_tiles_'
        M['tile'] = k.textured(self.repo, albedo=tiles + 'diff_2k.jpg', normal=tiles + 'nor_gl_2k.jpg',
                               rough=tiles + 'rough_2k.jpg', ao=tiles + 'ao_2k.jpg', roughness=1.0, triplanar_scale=1 / 1.9)
        M['ceiling'] = k.textured(self.repo, **tex('Ceiling_Default', 'Ceiling_Default'), triplanar_scale=0.33)
        M['trim'] = k.textured(self.repo, base=(0.9, 0.9, 0.88), roughness=0.45)
        M['exterior'] = k.textured(self.repo, base=(0.5, 0.5, 0.5), roughness=1.0)
        return M

    WALL_PAINT = {'Master Bedroom': 'wall_beige', 'Bedroom 2': 'wall_sage', 'Kids Bedroom': 'wall_blue',
                  'Ensuite': 'tile_wall', 'Bathroom': 'tile_wall', 'Laundry': 'wall_white'}
    FLOORS = {'Master Bedroom': 'laminate', 'Bedroom 2': 'laminate', 'Kids Bedroom': 'laminate', 'Ensuite': 'tile',
              'Bathroom': 'tile', 'Laundry': 'tile', 'Kitchen': 'strip'}

    def group(self, name, parent=None):
        if name not in self.groups:
            self.groups[name] = self.s.empty(name, self.root if parent is None else parent)
        return self.groups[name]

    # --- shell -----------------------------------------------------------------------------------------------
    def shell(self):
        shell = self.group('Shell')
        floors = self.s.empty('Floors', shell)
        for room, (x0, z0, x1, z1) in ROOMS.items():
            self.s.box(f'Floor - {room}', floors, ((x0 + x1) / 2, -0.05, (z0 + z1) / 2), (x1 - x0, 0.1, z1 - z0),
                       self.mats[self.FLOORS.get(room, 'hardwood')])
        self.s.box('Ceiling', shell, (6.5, H + 0.05, 5.0), (13.0 + T, 0.1, 10.0 + T), self.mats['ceiling'],
                   castShadows=0)
        walls = self.s.empty('Walls', shell)
        trim = self.s.empty('Baseboards', shell)
        for axis, c, a, b, openings in WALLS:
            pieces = []  # solid stretches along the wall, full height or (header) above an opening
            pos = a - T / 2
            for oc, ow, oh in sorted(openings):
                pieces.append((pos, oc - ow / 2, 0.0, H))
                pieces.append((oc - ow / 2, oc + ow / 2, oh, H))
                pos = oc + ow / 2
            pieces.append((pos, b + T / 2, 0.0, H))
            mid = (a + b) / 2
            for side in (-1, 1):
                px, pz = (mid, c + side * T) if axis == 'x' else (c + side * T, mid)
                room = room_at(px, pz)
                mat = self.mats[self.WALL_PAINT.get(room, 'wall_white')] if room else self.mats['exterior']
                off = c + side * T / 4
                for p0, p1, y0, y1 in pieces:
                    if p1 - p0 < 1e-3:
                        continue
                    L = p1 - p0
                    if axis == 'x':
                        center, size = ((p0 + p1) / 2, (y0 + y1) / 2, off), (L, y1 - y0, T / 2)
                    else:
                        center, size = (off, (y0 + y1) / 2, (p0 + p1) / 2), (T / 2, y1 - y0, L)
                    self.s.box(f'Wall {room or "Exterior"}', walls, center, size, mat)
                    if room and y0 == 0.0:
                        # baseboard: 9 cm tall, 1.2 cm proud of the wall face; trimmed back from the ends so it
                        # doesn't poke through the perpendicular wall
                        bo = c + side * (T / 2 + 0.006)
                        b0, b1 = p0 + (T if p0 <= a - T / 2 + 1e-6 else 0), p1 - (T if p1 >= b + T / 2 - 1e-6 else 0)
                        if b1 - b0 > 0.05:
                            if axis == 'x':
                                bc, bs = ((b0 + b1) / 2, 0.045, bo), (b1 - b0, 0.09, 0.012)
                            else:
                                bc, bs = (bo, 0.045, (b0 + b1) / 2), (0.012, 0.09, b1 - b0)
                            self.s.box('Baseboard', trim, bc, bs, self.mats['trim'], collider=False, static=True)

    # --- doors -----------------------------------------------------------------------------------------------
    def doors(self):
        grp = self.group('Doors')
        for name, axis, c, along, swing, exterior in DOORS:
            # local -Z of the door faces the room it opens into (the hinge swings both ways: the player can only
            # push); wall along X: normal is +-Z; along Z: +-X
            if axis == 'x':
                yaw = 0.0 if swing < 0 else 180.0
                wall_pt = (along, c)
            else:
                yaw = 90.0 if swing < 0 else -90.0
                wall_pt = (c, along)
            a = math.radians(yaw)
            fz = (math.sin(a), math.cos(a))  # door-local +Z in the world
            ox, oz = wall_pt[0] - fz[0] * DOOR_WALL_Z, wall_pt[1] - fz[1] * DOOR_WALL_Z
            frame = 'Door_Op_1_Frame' if exterior else 'Door_Op_1_Frame_Interior'
            leaf = 'Door_Op_1_Leaf' if exterior else 'Door_Op_1_Leaf_Interior'
            parent = self.s.empty(name, grp)
            self.s.model(self.idx, frame, parent, (ox, 0.0, oz), yaw, name='Frame', static=True)
            if exterior:  # the way out: nothing beyond it, so it stays shut (a plain collider, no hinge)
                self.s.model(self.idx, leaf, parent, (ox, 0.0, oz), yaw, name='Leaf', static=True, collider={
                    'isTrigger': False, 'halfExtents': [0.4, 0.995, 0.024], 'center': [-0.005, 1.004, 0.0]})
                continue
            self.s.model(self.idx, leaf, parent, (ox, 0.0, oz), yaw, name='Leaf', **{
                'collider': {'isTrigger': False, 'halfExtents': [0.4, 0.995, 0.024], 'center': [-0.005, 1.004, 0.0],
                             'friction': 0.5},
                'Rigidbody': {'Angular Damping': 2.5, 'Continuous Collision': False, 'Freeze Position X': False,
                              'Freeze Position Y': False, 'Freeze Position Z': False, 'Freeze Rotation X': False,
                              'Freeze Rotation Y': False, 'Freeze Rotation Z': False, 'Initial Velocity': [0.0, 0.0, 0.0],
                              'Interpolate': 'Interpolate', 'Is Kinematic': False, 'Linear Damping': 0.5, 'Mass': 18.0,
                              'Use Gravity': True},
                'joint': {'anchor': [-0.4, 1.0, -0.013], 'axis': [0.0, 1.0, 0.0], 'breakForce': 0.0, 'breakTorque': 0.0,
                          'connectedOrder': -1, 'limitLower': -90.0, 'limitUpper': 90.0,
                          'type': 1, 'useLimit': True}})

    # --- furniture placement ---------------------------------------------------------------------------------
    def wbox(self, stem, yaw):
        """The model's bounds rotated by yaw, relative to its pivot: (x0, x1, y0, y1, z0, z1)."""
        lo, hi = self.idx[stem]['min'], self.idx[stem]['max']
        a = math.radians(yaw)
        xs, zs = [], []
        for x in (lo[0], hi[0]):
            for z in (lo[2], hi[2]):
                xs.append(x * math.cos(a) + z * math.sin(a))
                zs.append(-x * math.sin(a) + z * math.cos(a))
        return min(xs), max(xs), lo[1], hi[1], min(zs), max(zs)

    def put(self, room, stem, x, z, yaw=0.0, y=0.0, collide=False, name=None):
        """Footprint centre at (x, z), bottom at y. Returns the placed piece's world box.
        (The library's loose doors / drawers are laid out beside their bodies, not in place: use the
        *_Assembled bodies from tools/assets/assemble_kits.py instead.)"""
        x0, x1, y0, y1, z0, z1 = self.wbox(stem, yaw)
        px, py, pz = x - (x0 + x1) / 2, y - y0, z - (z0 + z1) / 2
        parent = self.group(room, self.group('Furniture'))
        extra = {'static': True}
        if collide:
            extra['collider'] = {'friction': 0.8, 'isTrigger': False, 'shape': 4}
        self.s.model(self.idx, stem, parent, (px, py, pz), yaw, name=name, **extra)
        box = {'x0': px + x0, 'x1': px + x1, 'y0': y, 'y1': py + y1, 'z0': pz + z0, 'z1': pz + z1,
               'x': x, 'z': z, 'yaw': yaw}
        self.placed.append({'room': room, 'stem': stem, 'collide': collide, **box})
        return box

    def validate(self):
        """Layout checks on the placed boxes: anything poking out of its room, solid furniture overlapping solid
        furniture, and solid furniture in a door's swing (0.9 m either side of the opening). Returns the problems."""
        out = []
        for p in self.placed:
            x0, z0, x1, z1 = inner(p['room'])
            if p['x0'] < x0 - 0.02 or p['x1'] > x1 + 0.02 or p['z0'] < z0 - 0.02 or p['z1'] > z1 + 0.02:
                out.append(f"{p['room']}: {p['stem']} pokes out of the room ({p['x0']:.2f}..{p['x1']:.2f}, "
                           f"{p['z0']:.2f}..{p['z1']:.2f})")
            if p['y1'] > H - 0.02 and 'Lamp' not in p['stem']:
                out.append(f"{p['room']}: {p['stem']} goes through the ceiling (top {p['y1']:.2f})")
        solid = [p for p in self.placed if p['collide']]

        def overlap(a, b, m=0.02):
            return (min(a['x1'], b['x1']) - max(a['x0'], b['x0']) > m and min(a['z1'], b['z1']) - max(a['z0'], b['z0']) > m
                    and min(a['y1'], b['y1']) - max(a['y0'], b['y0']) > m)
        tucks = ({'Table_Dining', 'Chair_Dining'}, {'Workdesk_Assembled', 'Chair_Office'})
        for i, a in enumerate(solid):
            for b in solid[i + 1:]:
                if overlap(a, b) and {a['stem'], b['stem']} not in tucks:
                    out.append(f"{a['room']}: {a['stem']} overlaps {b['stem']}")
        for name, axis, c, along, _, _ in DOORS:
            if axis == 'x':
                zone = {'x0': along - DOOR_W / 2, 'x1': along + DOOR_W / 2, 'z0': c - 0.95, 'z1': c + 0.95}
            else:
                zone = {'x0': c - 0.95, 'x1': c + 0.95, 'z0': along - DOOR_W / 2, 'z1': along + DOOR_W / 2}
            zone.update({'y0': 0.0, 'y1': 2.0})
            for p in solid:
                if overlap(zone, p):
                    out.append(f"{p['room']}: {p['stem']} is in the swing of the {name}")
            if axis == 'x':
                gap = {'x0': along - DOOR_W / 2 - 0.08, 'x1': along + DOOR_W / 2 + 0.08, 'z0': c - 0.2, 'z1': c + 0.2}
            else:
                gap = {'x0': c - 0.2, 'x1': c + 0.2, 'z0': along - DOOR_W / 2 - 0.08, 'z1': along + DOOR_W / 2 + 0.08}
            gap.update({'y0': 0.0, 'y1': DOOR_H + 0.05})
            for p in self.placed:
                if overlap(gap, p, 0.005):
                    out.append(f"{p['room']}: {p['stem']} is in the {name}'s doorway / trim")
        return out

    SIDE_YAW = {'N': 0.0, 'S': 180.0, 'W': 90.0, 'E': -90.0}  # the front (local +Z) faces into the room

    def wall(self, room, side, t, stem, y=0.0, gap=0.005, collide=False, extra_yaw=0.0, name=None):
        """Back against a wall of the room: side N/S/W/E, t = position along the wall (x for N/S, z for W/E)."""
        rx0, rz0, rx1, rz1 = inner(room)
        yaw = self.SIDE_YAW[side] + extra_yaw
        x0, x1, _, _, z0, z1 = self.wbox(stem, yaw)
        w, d = x1 - x0, z1 - z0
        if side == 'N':
            x, z = t, rz0 + gap + d / 2
        elif side == 'S':
            x, z = t, rz1 - gap - d / 2
        elif side == 'W':
            x, z = rx0 + gap + w / 2, t
        else:
            x, z = rx1 - gap - w / 2, t
        return self.put(room, stem, x, z, yaw, y, collide, name)

    def on(self, room, base, stem, dx=0.0, dz=0.0, yaw=None, name=None):
        """On top of a placed piece, offset in its own frame (dx right, dz toward its front)."""
        yaw = base['yaw'] if yaw is None else yaw
        a = math.radians(base['yaw'])
        x = (base['x0'] + base['x1']) / 2 + dx * math.cos(a) + dz * math.sin(a)
        z = (base['z0'] + base['z1']) / 2 - dx * math.sin(a) + dz * math.cos(a)
        return self.put(room, stem, x, z, yaw, base['y1'], name=name)

    def ceiling_light(self, room, stem, x, z, intensity=2.4, rng=5.5, shadows=False, color=(1.0, 0.88, 0.74)):
        x0, x1, y0, y1, z0, z1 = self.wbox(stem, 0.0)
        drop = y1 - y0
        self.put(room, stem, x, z, 0.0, H - drop)
        self.s.light(f'{room} Light', self.group('Lighting'), (x, H - drop - 0.12, z), intensity, rng, color, shadows)

    def lamp_light(self, room, piece, intensity=0.6, rng=2.5):
        self.s.light(f'{room} Lamp', self.group('Lighting'),
                     ((piece['x0'] + piece['x1']) / 2, piece['y1'] - 0.18, (piece['z0'] + piece['z1']) / 2),
                     intensity, rng, (1.0, 0.8, 0.6))

    def switch(self, room, side, t, y=1.2):
        # the switch plate is modelled facing -X: turn it a further +90 so it faces into the room
        self.wall(room, side, t, 'Light_Switches_Single', y=y, gap=0.0, extra_yaw=90.0, name='Light Switch')

    def outlet(self, room, side, t, y=0.3, gfi=False):
        self.wall(room, side, t, 'Outlet_Gfi' if gfi else 'Outlet_Electrical', y=y, gap=0.0, extra_yaw=90.0,
                  name='Outlet')

    def cam(self, name, x, z, tx, tz, pitch=-8.0, y=1.65):
        yaw = math.degrees(math.atan2(tz - z, tx - x))  # the shot camera's yaw: 0 looks down +X, -90 down -Z
        self.cams.append(f'{name}:{x:.2f},{y:.2f},{z:.2f},{yaw:.0f},{pitch:.0f}')

    # --- rooms -----------------------------------------------------------------------------------------------
    def living(self):
        r = 'Living Room'
        x0, z0, x1, z1 = inner(r)
        tvs = self.wall(r, 'W', 7.1, 'Tv_Stand_Assembled', collide=True)
        self.on(r, tvs, 'Tv', dz=-0.05)
        self.on(r, tvs, 'Tv_Remote', dx=0.65, dz=0.1, yaw=110.0)
        self.put(r, 'Carpet_C', 2.3, 7.1, 90.0)
        couch = self.put(r, 'Couch', 3.75, 7.1, -90.0, collide=True)
        sw = self.wbox('Sidetable_A', -90.0)
        half = (sw[5] - sw[4]) / 2 + 0.05
        st1 = self.put(r, 'Sidetable_A', couch['x'] + 0.1, couch['z1'] + half, -90.0, collide=True)
        st2 = self.put(r, 'Sidetable_A', couch['x'] + 0.1, couch['z0'] - half, -90.0, collide=True)
        lamp = self.on(r, st1, 'Lamp_Table_A_Assembled')
        self.lamp_light(r, lamp)
        self.on(r, st2, 'Vase_A', dx=0.1)
        self.on(r, st2, 'Candle_Holder_A', dx=-0.2)
        self.put(r, 'Chair_Arm', 2.1, 9.15, 170.0, collide=True)
        self.put(r, 'Arm_Chair_01', 1.85, 5.65, 10.0, collide=True)
        self.put(r, 'Pillow', 3.6, 6.95, -90.0, y=0.48)
        bs = self.wall(r, 'S', 3.55, 'Bookshelf', collide=True)
        for i, (stem, dy) in enumerate((('Books_A', 0.42), ('Books_C', 0.88), ('Books_E', 1.33), ('Books_B', 1.78))):
            self.put(r, stem, bs['x0'] + 0.4 + 0.2 * (i % 2), bs['z1'] - 0.3, 180.0, dy)
        self.put(r, 'Decorative_Bowl', bs['x1'] - 0.35, bs['z1'] - 0.3, 0.0, 1.33)
        fl = self.wall(r, 'S', 5.45, 'Lamp_Floor_A_Assembled', gap=0.15)
        self.lamp_light(r, fl, 0.9, 3.0)
        self.wall(r, 'S', 1.1, 'Plant_A', gap=0.2, collide=True)
        self.put(r, 'Plant_B', x0 + 0.35, z0 + 0.35)
        self.wall(r, 'W', 5.3, 'Cabinet_A_Assembled', collide=True)
        self.wall(r, 'W', 9.0, 'Painting_Medium_C', y=1.25)
        self.wall(r, 'N', 1.6, 'Painting_Large_A', y=1.1)
        self.wall(r, 'S', 1.9, 'Clock_Wall_Analog', y=1.9)
        self.switch(r, 'N', 2.15)
        self.outlet(r, 'W', 6.4)
        self.outlet(r, 'S', 4.6)
        self.ceiling_light(r, 'Lamp_Ceiling_B', 2.5, 7.1, 2.6, 6.0, shadows=True)
        self.ceiling_light(r, 'Lamp_Ceiling_B', 5.0, 4.9, 1.6, 4.0)
        self.cam('living_1', 5.6, 9.6, 1.0, 6.0, -12)
        self.cam('living_2', 4.9, 4.6, 0.5, 9.0, -12)

    def kitchen(self):
        r = 'Kitchen'
        x0, z0, x1, z1 = inner(r)

        def run_n(stem, x, **kw):  # along the north wall, left edge at x
            w = self.idx[stem]['max'][0] - self.idx[stem]['min'][0]
            return self.wall(r, 'N', x + w / 2, stem, **kw)
        fr = run_n('Fridge_Assembled', x0 + 0.02, collide=True)
        c1 = run_n('Kitchen_Counter_A_Assembled', fr['x1'] + 0.02, collide=True)
        st = run_n('Stove', c1['x1'], collide=True)
        c2 = run_n('Kitchen_Counter_A_Assembled', st['x1'], collide=True)
        self.wall(r, 'N', (st['x0'] + st['x1']) / 2, 'Stove_Cooker_Hood', y=1.72)
        for base in (c1, c2):
            self.wall(r, 'N', (base['x0'] + base['x1']) / 2, 'Kitchen_Cabinet_B_Assembled', y=1.52, collide=True)
        self.on(r, c1, 'Coffeemaker', dx=-0.25, dz=-0.1)
        self.on(r, c1, 'Coffee_Mug', dx=0.05, dz=0.05)
        self.on(r, c1, 'Utensil_Holder_Full', dx=0.3, dz=-0.12)
        self.on(r, st, 'Cooking_Pot', dx=-0.22, dz=0.05)
        self.on(r, st, 'Frying_Pan', dx=0.22, dz=0.1, yaw=30.0)
        self.on(r, c2, 'Jar_A', dx=-0.35, dz=-0.15)
        self.on(r, c2, 'Jar_B', dx=-0.2, dz=-0.16)
        self.on(r, c2, 'Jar_C', dx=-0.05, dz=-0.16)
        self.on(r, c2, 'Pepper_Mill', dx=0.2, dz=-0.1)
        self.on(r, c2, 'Teapot', dx=0.3, dz=0.1)
        # east run: sink then counter
        sk = self.wall(r, 'E', z0 + 0.62 + 0.5, 'Kitchen_Counter_Sink_Assembled', collide=True)
        c3 = self.wall(r, 'E', sk['z1'] + 0.5, 'Kitchen_Counter_A_Assembled', collide=True)
        self.on(r, sk, 'Handsoap', dx=-0.38, dz=-0.15)
        self.on(r, c3, 'Plate_Pile', dx=-0.2, dz=-0.05)
        self.on(r, c3, 'Bowl_Pile', dx=0.15, dz=-0.1)
        self.on(r, c3, 'Glass_Drinking_Pile', dx=0.35, dz=0.1)
        self.on(r, c3, 'Macaroni_Package', dx=-0.38, dz=-0.15, yaw=-70.0)
        for base in (sk, c3):
            self.wall(r, 'E', (base['z0'] + base['z1']) / 2, 'Kitchen_Cabinet_B_Assembled', y=1.52, collide=True)
        # pantry and dining
        self.wall(r, 'S', x0 + 0.85, 'Kitchen_Cabinet_Large_Assembled', collide=True)
        tx, tz = 8.15, 8.05
        tb = self.put(r, 'Table_Dining', tx, tz, 0.0, collide=True)
        for yaw, dx, dz in ((180.0, 0.0, -0.75), (0.0, 0.0, 0.75), (90.0, -0.75, 0.0), (-90.0, 0.75, 0.0)):
            self.put(r, 'Chair_Dining', tx + dx, tz + dz, yaw, collide=True)
        self.on(r, tb, 'Decorative_Bowl')
        for dx, dz in ((0.0, -0.5), (0.0, 0.5), (-0.5, 0.0), (0.5, 0.0)):
            self.on(r, tb, 'Plate_Single', dx, dz)
        self.on(r, tb, 'Glass_Wine', 0.25, -0.45)
        self.on(r, tb, 'Glass_Wine', -0.25, 0.45)
        self.ceiling_light(r, 'Lamp_Chandelier_A', tx, tz, 2.2, 5.0, shadows=True)
        self.ceiling_light(r, 'Lamp_Ceiling_A', 8.0, 6.3, 1.6, 4.0)
        self.wall(r, 'S', 9.4, 'Painting_Medium_E', y=1.3)
        self.switch(r, 'W', 6.0)
        self.outlet(r, 'N', c1['x0'] + 0.2, y=1.12, gfi=True)
        self.outlet(r, 'E', c3['z1'] - 0.2, y=1.12, gfi=True)
        self.cam('kitchen_1', 5.0, 8.6, 9.5, 6.0, -12)
        self.cam('kitchen_2', 10.0, 9.6, 6.5, 6.0, -12)

    def master(self):
        r = 'Master Bedroom'
        x0, z0, x1, z1 = inner(r)
        bed = self.wall(r, 'W', 2.0, 'Bed', collide=True)
        self.put(r, 'Carpet_A', bed['x1'] - 0.3, 2.0, 90.0, y=0.0)
        for zz in (bed['z0'] - 0.34, bed['z1'] + 0.34):
            ns = self.wall(r, 'W', zz, 'Nightstand_B_Assembled', collide=True)
            lamp = self.on(r, ns, 'Lamp_Table_B_Assembled', dx=0.0, dz=-0.05)
            self.lamp_light(r, lamp, 0.45, 2.0)
        self.on(r, ns, 'Clock_Table_Digital', dx=0.18, dz=0.1)
        self.on(r, ns, 'Book_Single_A', dx=-0.15, dz=0.12, yaw=110.0)
        dr = self.wall(r, 'N', 3.2, 'Dresser_C_Assembled', collide=True)
        self.on(r, dr, 'Candle_Holder_B', dx=-0.5)
        self.on(r, dr, 'Vase_B', dx=0.45)
        self.wall(r, 'E', 1.4, 'Mirror_Body', gap=0.05, collide=True)
        self.put(r, 'Laundrybasket', 2.0, z0 + 0.3)
        self.wall(r, 'W', 2.0, 'Painting_Large_C', y=1.45)
        self.wall(r, 'S', 1.3, 'Painting_Medium_B', y=1.35)
        self.switch(r, 'S', 3.62)
        self.outlet(r, 'W', bed['z0'] - 0.7)
        self.ceiling_light(r, 'Lamp_Ceilingfan', 2.4, 2.1, 2.2, 5.0, shadows=True)
        self.cam('master_1', 4.0, 3.8, 0.5, 1.0, -14)
        self.cam('master_2', 0.4, 0.4, 3.5, 3.5, -12)

    def ensuite(self):
        r = 'Ensuite'
        x0, z0, x1, z1 = inner(r)
        sh = self.wall(r, 'N', (x0 + x1) / 2, 'Shower_Stall_190cm_L_Assembled', collide=True)
        self.wall(r, 'N', (x0 + x1) / 2 + 0.55, 'Shower', y=0.35)
        self.wall(r, 'N', (x0 + x1) / 2 - 0.4, 'Shower_Soap_Holder', y=1.1)
        self.put(r, 'Shampoo_Bottle_A', (x0 + x1) / 2 - 0.5, z0 + 0.15, 0.0, 0.05)
        vn = self.wall(r, 'W', 2.02, 'Bathroom_Sink_Body', collide=True)
        self.wall(r, 'W', 2.02, 'Mirror_Bathroom', y=1.15)
        self.on(r, vn, 'Toothbrush_Mug', dx=-0.15, dz=-0.15)
        self.on(r, vn, 'Toothpaste', dx=0.0, dz=-0.18, yaw=0.0)
        self.on(r, vn, 'Handsoap', dx=0.2, dz=-0.18)
        self.on(r, vn, 'Lotiontube', dx=0.4, dz=-0.15)
        self.wall(r, 'E', 3.4, 'Toilet_Assembled', collide=True)
        self.wall(r, 'E', 2.75, 'Toiletpaper_Single', y=0.7)
        self.wall(r, 'E', 1.9, 'Towel_Holder', y=1.25)
        self.wall(r, 'E', 1.9, 'Towel_Hanging_Large', y=0.75, gap=0.04)
        self.switch(r, 'W', 2.6)
        self.ceiling_light(r, 'Lamp_Ceiling_A', 5.5, 2.4, 1.8, 4.0, color=(1.0, 0.95, 0.88))
        self.cam('ensuite_1', 6.3, 4.0, 4.8, 0.5, -18)

    def bedroom2(self):
        r = 'Bedroom 2'
        x0, z0, x1, z1 = inner(r)
        bed = self.wall(r, 'N', x1 - 0.75, 'Bed_Single_Person', collide=True)
        ns = self.wall(r, 'N', bed['x0'] - 0.3, 'Nightstand_B_Assembled', collide=True)
        lamp = self.on(r, ns, 'Lamp_Table_A_Assembled')
        self.lamp_light(r, lamp, 0.45, 2.0)
        desk = self.wall(r, 'W', 1.5, 'Workdesk_Assembled', collide=True)
        self.put(r, 'Chair_Office', desk['x1'] + 0.35, 1.5, -100.0, collide=True)
        self.on(r, desk, 'Papers_Pile_A', dx=-0.4, dz=0.0)
        self.on(r, desk, 'Pen_Holder_With_Pens', dx=0.55, dz=-0.15)
        self.on(r, desk, 'Folders_Set_A', dx=0.3, dz=-0.2)
        self.on(r, desk, 'Book_Pile_A', dx=-0.6, dz=-0.15)
        dr = self.wall(r, 'E', 3.05, 'Dresser_B_Assembled', collide=True)
        self.on(r, dr, 'Plant_B', dx=0.5)
        self.on(r, dr, 'Books_D', dx=-0.4)
        self.put(r, 'Carpet_B', 8.1, 2.6, 0.0)
        self.wall(r, 'W', 1.5, 'Painting_Medium_D', y=1.5)
        self.wall(r, 'S', 8.9, 'Painting_Medium_F', y=1.35)
        self.switch(r, 'S', 7.95)
        self.outlet(r, 'W', 2.6)
        self.ceiling_light(r, 'Lamp_Ceiling_A', 8.3, 2.1, 2.0, 5.0)
        self.cam('bedroom2_1', 7.0, 3.9, 9.5, 0.5, -14)

    def kids(self):
        r = 'Kids Bedroom'
        x0, z0, x1, z1 = inner(r)
        self.wall(r, 'N', x1 - 0.76, 'Bed_Bunk_Assembled', collide=True)
        dr = self.wall(r, 'W', 1.1, 'Dresser_D_Assembled', collide=True)
        self.on(r, dr, 'Lamp_Table_Rocket_Assembled', dx=0.2)
        self.on(r, dr, 'Toy_Dinosaur', dx=-0.2, yaw=60.0)
        self.put(r, 'Carpet_Kid', 11.5, 2.4, 0.0)
        tb = self.wall(r, 'S', 12.3, 'Toy_Box_A', collide=True)
        self.put(r, 'Toy_Teddy', 11.2, 3.55, 200.0)
        self.put(r, 'Toy_Box_A_Lid', tb['x1'] - 0.2, tb['z0'] - 0.35, 15.0)
        self.put(r, 'Toy_Racetrack', 11.2, 2.6, 20.0)
        self.put(r, 'Toy_Car_A', 11.0, 2.3, 75.0)
        self.put(r, 'Toy_Car_B', 11.6, 3.0, -30.0)
        self.put(r, 'Toy_Truck', 10.7, 3.4, 140.0)
        self.put(r, 'Toy_Block_Set_A', 12.0, 3.0, 10.0)
        self.put(r, 'Toy_Block_B', 12.2, 2.75, 40.0)
        self.put(r, 'Toy_Muscleman', 10.9, 1.8, -60.0)
        st = self.wall(r, 'W', 2.6, 'Sidetable_B', collide=True)
        self.on(r, st, 'Book_Drawing_Kid', dx=-0.1, yaw=100.0)
        self.on(r, st, 'Pen_Set_A_Kid', dx=0.15)
        self.wall(r, 'W', 2.6, 'Poster_Kid_A', y=1.35)
        self.wall(r, 'E', 3.2, 'Poster_Kid_C', y=1.3)
        self.wall(r, 'S', 11.85, 'Poster_Kid_E', y=1.5)
        self.switch(r, 'S', 11.35)
        self.outlet(r, 'W', 3.4)
        self.ceiling_light(r, 'Lamp_Ceiling_A', 11.5, 2.1, 2.0, 5.0, color=(1.0, 0.92, 0.8))
        self.cam('kids_1', 10.4, 3.9, 12.8, 0.5, -14)

    def bathroom(self):
        r = 'Bathroom'
        x0, z0, x1, z1 = inner(r)
        sw = self.wbox('Shower_Stall_190cm_R_Assembled', -90.0)
        sh = self.wall(r, 'E', z0 + (sw[5] - sw[4]) / 2 + 0.01, 'Shower_Stall_190cm_R_Assembled', collide=True)
        self.wall(r, 'E', sh['z0'] + 0.5, 'Shower', y=0.35)
        vn = self.wall(r, 'S', x0 + 0.95, 'Bathroom_Sink_Body', collide=True)
        self.wall(r, 'S', x0 + 0.95, 'Mirror_Bathroom', y=1.15)
        self.on(r, vn, 'Toothbrush_A', dx=-0.3, dz=-0.15, yaw=80.0)
        self.on(r, vn, 'Handsoap', dx=0.1, dz=-0.18)
        self.on(r, vn, 'Shampoo_Bottle_B', dx=0.5, dz=-0.15)
        self.wall(r, 'W', 6.95, 'Toilet_Assembled', collide=True)
        self.wall(r, 'W', 6.45, 'Toiletpaper_Single', y=0.7)
        self.wall(r, 'W', 7.6, 'Towel_Holder', y=1.3)
        self.wall(r, 'W', 7.6, 'Towel_Hanging_Small', y=0.85, gap=0.04)
        self.switch(r, 'N', 11.85)
        self.ceiling_light(r, 'Lamp_Ceiling_A', 11.4, 6.85, 1.8, 4.0, color=(1.0, 0.95, 0.88))
        self.cam('bathroom_1', 10.7, 5.7, 12.6, 7.9, -20)

    def laundry(self):
        r = 'Laundry'
        x0, z0, x1, z1 = inner(r)
        # the washer is modelled with its front at local -X (control panel along +X): a quarter turn puts the
        # panel against the wall
        wm = self.wall(r, 'S', x1 - 1.0, 'Washingmachine_Assembled', collide=True, extra_yaw=90.0)
        self.on(r, wm, 'Towel_Roll_Set', dx=-0.1)
        self.wall(r, 'E', z0 + 0.6, 'Garage_Shelf_A', collide=True)
        self.put(r, 'Storage_Box_A', x1 - 0.3, z0 + 0.45, -90.0, 0.03)
        self.put(r, 'Cardboard_Box_B', x1 - 0.3, z0 + 0.75, -80.0, 0.03)
        self.wall(r, 'N', 11.3, 'Electricbox_Main', y=1.35)
        self.put(r, 'Laundrybasket', x0 + 0.95, z1 - 0.3, 10.0)
        self.put(r, 'Paintbucket_Set_A', x0 + 1.2, z0 + 0.2, 0.0)
        self.switch(r, 'W', 9.85)
        self.outlet(r, 'S', x1 - 0.5, y=0.9, gfi=True)
        self.ceiling_light(r, 'Lamp_Fluorescent_A', 11.7, 9.15, 1.6, 4.0, color=(0.92, 0.97, 1.0))
        self.cam('laundry_1', 10.7, 8.3, 12.8, 9.9, -20)

    def hall(self):
        r = 'Hall'
        x0, z0, x1, z1 = inner(r)
        sh = self.wall(r, 'S', 8.6, 'Shelf', y=1.1)
        self.on(r, sh, 'Candle_Holder_A', dx=-0.3)
        self.on(r, sh, 'Vase_A', dx=0.25)
        self.wall(r, 'S', 7.2, 'Painting_Medium_A', y=1.25)
        self.wall(r, 'N', 12.2, 'Clock_Wall_Analog', y=1.85)
        self.switch(r, 'N', 12.55)
        self.ceiling_light(r, 'Lamp_Ceiling_B', 8.0, 4.8, 1.4, 4.0)
        self.ceiling_light(r, 'Lamp_Ceiling_B', 11.5, 4.8, 1.4, 4.0)
        self.cam('hall_1', 12.5, 4.8, 6.0, 4.8, -6)
        self.cam('hall_2', 5.2, 4.8, 13.0, 4.8, -6)

    def player(self):
        sandbox = json.load(open(os.path.join(self.repo, 'project', 'scenes', 'Sandbox.json'), encoding='utf-8'))
        spawn = next(e for e in sandbox['empties'] if e['name'] == 'Player Spawn')
        e = json.loads(json.dumps(spawn))
        i = self.s._next()
        e.update({'id': i, 'order': i, 'parentId': -1, 'position': [12.25, 0.1, 4.8], 'rotation': k.yaw_quat(90.0)})
        self.s.empties.append(e)

    def audio(self):
        self.s.empty('Reverb', self.root, (6.5, 1.3, 5.0), **{'Reverb Zone': {
            'Ambience': 'snd.amb.indoor_small', 'Ambience Volume': 0.6, 'Enabled': True, 'Extents': [6.5, 1.3, 5.0],
            'Fade Distance': 0.5, 'Priority': 0, 'Radius': 6.0, 'Reverb Mode': 'Class Default', 'Shape': 'Box',
            'Tail Class': 'Indoor Small', 'Tail Gain': 1.0}})

    def build(self):
        # interior look: no sun, little sky light; the room lights carry it
        # a dim neutral procedural sky: what the mirrors and glossy surfaces reflect, with no windows to show it
        for key in ('skyHdriGuid', 'skyHdriPath', 'skyRotationDegrees'):
            self.d.pop(key, None)
        self.d.update({'skySource': 0, 'skyHorizonColor': [0.34, 0.32, 0.3], 'skyZenithColor': [0.3, 0.29, 0.28]})
        self.d.update({'skyAmbientIntensity': 0.35, 'exposureEV': 0.6, 'autoExposure': False, 'fogEnabled': False,
                       'shadowDistance': 40.0, 'maxPointShadows': 4, 'ssaoEnabled': True, 'ssaoIntensity': 1.0})
        self.shell()
        self.doors()
        for f in (self.living, self.kitchen, self.master, self.ensuite, self.bedroom2, self.kids, self.bathroom,
                  self.laundry, self.hall):
            f()
        self.player()
        self.audio()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('repo')
    ap.add_argument('--out')
    ap.add_argument('--shots')
    args = ap.parse_args()
    repo = os.path.abspath(args.repo)
    b = Builder(repo)
    b.build()
    problems = b.validate()
    for line in problems:
        print('build_apartment: check:', line)
    out = args.out or os.path.join(repo, 'project', 'scenes', 'Apartment_3Bed.json')
    b.s.write(out)
    if args.shots:
        with open(args.shots, 'w') as f:
            f.write(';'.join(b.cams))
    print(f'build_apartment: {len(b.s.models)} models, {len(b.s.boxes)} boxes, {len(b.s.empties)} empties -> {out}')


if __name__ == '__main__':
    main()
