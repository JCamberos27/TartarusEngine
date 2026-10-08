# Builds project/scenes/Apartment_3Bed.json: a furnished, lived-in three-bedroom family apartment (master with
# ensuite, a guest room / office, a kid's room, living room, kitchen + dining, bathroom, laundry, hall) from the
# imported model library, lit for a late, dark, uneasy night: a few lamps, the TV and a stuttering tube.
#
# Plan coordinates are metres: X runs east, Z runs south, the floor is y = 0. Walls are listed by centre line and
# built as two half-thickness boxes so each side carries its own room's paint. Every model is placed by its measured
# bounds (scene_kit.model_index), so a placement names where the piece's footprint goes, not where its pivot is.
#
#   python tools/scenes/build_apartment.py <repo> [--out <scene.json>]
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
HINGE = (-0.4, -0.013)        # the leaf's hinge line, door-local (x, z)

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

# Doors: (name, wall axis, fixed, centre, side it swings into: +1 / -1 along the wall's normal, exterior?, opened
# degrees). A door left ajar lets its room's light spill out.
DOORS = [
    ('Front Door', 'z', 13.0, 4.8, -1, True, 0.0),
    ('Master Door', 'x', 4.2, 2.9, -1, False, 70.0),
    ('Ensuite Door', 'z', 4.4, 3.5, +1, False, 35.0),
    ('Bedroom 2 Door', 'x', 4.2, 7.3, -1, False, 25.0),
    ('Kids Bedroom Door', 'x', 4.2, 10.7, -1, False, 15.0),
    ('Bathroom Door', 'x', 5.4, 11.1, +1, False, 0.0),
    ('Laundry Door', 'z', 10.4, 9.15, +1, False, 50.0),
]

# Lights. The scene is dark: these are the few things switched on. Colours are warm incandescent unless noted.
WARM = (1.0, 0.72, 0.45)
LAMP = (1.0, 0.66, 0.38)
TUBE = (0.82, 0.95, 1.0)
TV = (0.55, 0.7, 1.0)
FLICKER = ('Tartarus.Gameplay.LightFlicker', 'assets/Scripts/LightFlicker.cs')
MODES = {'Fluorescent': 0, 'Tv': 1, 'Candle': 2}


def room_at(x, z):
    for name, (x0, z0, x1, z1) in ROOMS.items():
        if x0 < x < x1 and z0 < z < z1:
            return name
    return None


def inner(room):
    """The room's interior rectangle (wall faces)."""
    x0, z0, x1, z1 = ROOMS[room]
    return x0 + T / 2, z0 + T / 2, x1 - T / 2, z1 - T / 2


# Shelf boards (height of each board's top above the piece's bottom), measured on the models with a ray.
BOARDS = {'Bookshelf': [0.144, 0.952, 1.334, 1.717], 'Garage_Shelf_A': [0.142, 0.704, 1.272, 1.837],
          'Bathroom_Shelf_Unit_A': [0.226, 0.517, 0.804], 'Bathroom_Shelf_Unit_B': [0.157, 0.55, 0.94, 1.325],
          'Tv_Stand_Assembled': [0.144], 'Sidetable_C': [0.208]}
# Pieces hung on or fixed to a wall (they don't need anything under them).
WALL_ITEMS = ('Painting', 'Poster', 'Clock_Wall', 'Light_Switches', 'Outlet', 'Mirror_Bathroom', 'Towel_Holder',
              'Towel_Hanging', 'Shelf', 'Cross', 'Electricbox', 'Shower', 'Toiletpaper_Single', 'Stove_Cooker_Hood',
              'Kitchen_Cabinet_B', 'Lamp_Ceiling', 'Lamp_Chandelier', 'Lamp_Fluorescent', 'Lamp_Wall')


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
        self.supports = []   # boxes small props may rest on that aren't placed pieces (shelf boards)
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
        M = {'wall_white': paint((0.8, 0.78, 0.72)), 'wall_sage': paint((0.6, 0.66, 0.56)),
             'wall_blue': paint((0.3, 0.39, 0.52)), 'tile_wall': paint((0.86, 0.87, 0.86), 0.45),
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
        M['trim'] = k.textured(self.repo, base=(0.82, 0.81, 0.78), roughness=0.45)
        M['exterior'] = k.textured(self.repo, base=(0.5, 0.5, 0.5), roughness=1.0)
        M['screen'] = k.textured(self.repo, base=(0.02, 0.02, 0.03), roughness=0.15, emissive=(0.62, 0.74, 1.0),
                                 strength=2.2)
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
        trim = self.s.empty('Trim', shell)
        for axis, c, a, b, openings in WALLS:
            pieces = []  # solid stretches along the wall, full height or (header) above an opening
            pos = a - T / 2 - 0.01  # 1 cm past each end: walls meeting at a T or an L overlap instead of just touching
            for oc, ow, oh in sorted(openings):
                pieces.append((pos, oc - ow / 2, 0.0, H))
                pieces.append((oc - ow / 2, oc + ow / 2, oh, H))
                pos = oc + ow / 2
            pieces.append((pos, b + T / 2 + 0.01, 0.0, H))
            mid = (a + b) / 2
            for side in (-1, 1):
                px, pz = (mid, c + side * T) if axis == 'x' else (c + side * T, mid)
                room = room_at(px, pz)
                mat = self.mats[self.WALL_PAINT.get(room, 'wall_white')] if room else self.mats['exterior']
                off = c + side * T / 4
                for p0, p1, y0, y1 in pieces:
                    if p1 - p0 < 1e-3:
                        continue
                    # Seal the seams: every piece runs 5 cm into the floor slab / ceiling slab, and a header overlaps
                    # the jambs either side by 2 cm, set 1 mm back so the overlapping faces don't fight.
                    header = y0 > 0.0
                    q0, q1 = (p0 - 0.02, p1 + 0.02) if header else (p0, p1)
                    r0, r1 = (y0 if header else -0.05), y1 + 0.05
                    thick, inset = (T / 2 - 0.002, 0.001) if header else (T / 2, 0.0)
                    o = off - side * inset
                    if axis == 'x':
                        center, size = ((q0 + q1) / 2, (r0 + r1) / 2, o), (q1 - q0, r1 - r0, thick)
                    else:
                        center, size = (o, (r0 + r1) / 2, (q0 + q1) / 2), (thick, r1 - r0, q1 - q0)
                    self.s.box(f'Wall {room or "Exterior"}', walls, center, size, mat)
                    if not room:
                        continue
                    # trim trimmed back from the wall's ends so it doesn't poke through the perpendicular wall
                    b0, b1 = p0 + (T if p0 <= a - T / 2 + 1e-6 else 0), p1 - (T if p1 >= b + T / 2 - 1e-6 else 0)
                    if b1 - b0 < 0.05:
                        continue
                    # baseboard (9 cm, 1.2 cm proud) along the floor; crown (7 cm, 3 cm proud) under the ceiling
                    for ty, th, proud, name in ((0.045, 0.09, 0.012, 'Baseboard'), (H - 0.035, 0.07, 0.03, 'Crown')):
                        if name == 'Baseboard' and y0 > 0.0:
                            continue
                        to = c + side * (T / 2 + proud / 2)
                        if axis == 'x':
                            bc, bs = ((b0 + b1) / 2, ty, to), (b1 - b0, th, proud)
                        else:
                            bc, bs = (to, ty, (b0 + b1) / 2), (proud, th, b1 - b0)
                        self.s.box(name, trim, bc, bs, self.mats['trim'], collider=False, static=True)

    # --- doors -----------------------------------------------------------------------------------------------
    def door_frame(self, axis, c, along, swing):
        """The door's yaw and origin. Its local -Z faces the room it opens into; wall along X: normal is +-Z."""
        if axis == 'x':
            yaw = 0.0 if swing < 0 else 180.0
            wall_pt = (along, c)
        else:
            yaw = 90.0 if swing < 0 else -90.0
            wall_pt = (c, along)
        a = math.radians(yaw)
        fz = (math.sin(a), math.cos(a))  # door-local +Z in the world
        return yaw, (wall_pt[0] - fz[0] * DOOR_WALL_Z, wall_pt[1] - fz[1] * DOOR_WALL_Z)

    def doors(self):
        grp = self.group('Doors')
        for name, axis, c, along, swing, exterior, opened in DOORS:
            yaw, (ox, oz) = self.door_frame(axis, c, along, swing)
            frame = 'Door_Op_1_Frame' if exterior else 'Door_Op_1_Frame_Interior'
            leaf = 'Door_Op_1_Leaf' if exterior else 'Door_Op_1_Leaf_Interior'
            parent = self.s.empty(name, grp)
            self.s.model(self.idx, frame, parent, (ox, 0.0, oz), yaw, name='Frame', static=True)
            if exterior:  # the way out: nothing beyond it, so it stays shut (a plain collider, no hinge)
                self.s.model(self.idx, leaf, parent, (ox, 0.0, oz), yaw, name='Leaf', static=True, collider={
                    'isTrigger': False, 'halfExtents': [0.4, 0.995, 0.024], 'center': [-0.005, 1.004, 0.0]})
                continue
            # left ajar: turn the leaf about its hinge (a positive turn swings the free edge toward local -Z)
            a, b = math.radians(yaw), math.radians(yaw + opened)
            hx = ox + HINGE[0] * math.cos(a) + HINGE[1] * math.sin(a)
            hz = oz - HINGE[0] * math.sin(a) + HINGE[1] * math.cos(a)
            lx = hx - (HINGE[0] * math.cos(b) + HINGE[1] * math.sin(b))
            lz = hz - (-HINGE[0] * math.sin(b) + HINGE[1] * math.cos(b))
            self.s.model(self.idx, leaf, parent, (lx, 0.0, lz), yaw + opened, name='Leaf', **{
                'collider': {'isTrigger': False, 'halfExtents': [0.4, 0.995, 0.024], 'center': [-0.005, 1.004, 0.0],
                             'friction': 0.5},
                'Rigidbody': {'Angular Damping': 2.5, 'Continuous Collision': False, 'Freeze Position X': False,
                              'Freeze Position Y': False, 'Freeze Position Z': False, 'Freeze Rotation X': False,
                              'Freeze Rotation Y': False, 'Freeze Rotation Z': False, 'Initial Velocity': [0.0, 0.0, 0.0],
                              'Interpolate': 'Interpolate', 'Is Kinematic': False, 'Linear Damping': 0.5, 'Mass': 18.0,
                              'Use Gravity': True},
                # limits are relative to the authored pose: the closed position is -opened from here
                'joint': {'anchor': [HINGE[0], 1.0, HINGE[1]], 'axis': [0.0, 1.0, 0.0], 'breakForce': 0.0,
                          'breakTorque': 0.0, 'connectedOrder': -1, 'limitLower': -90.0 - opened,
                          'limitUpper': 90.0 - opened, 'type': 1, 'useLimit': True}})

    def latch_side(self, door_name, room, y=1.2):
        """The light switch on the room side of a door, beside its latch edge (the side away from the hinge)."""
        name, axis, c, along, swing, _, _ = next(d for d in DOORS if d[0] == door_name)
        yaw, (ox, oz) = self.door_frame(axis, c, along, swing)
        a = math.radians(yaw)
        latch = (ox + 0.4 * math.cos(a), oz - 0.4 * math.sin(a))  # door-local +x: away from the hinge
        t = latch[0] if axis == 'x' else latch[1]
        sign = 1 if t > along else -1
        t = along + sign * (DOOR_W / 2 + 0.17)
        x0, z0, x1, z1 = inner(room)
        rc = ((x0 + x1) / 2, (z0 + z1) / 2)
        if axis == 'x':
            side = 'N' if rc[1] > c else 'S'
        else:
            side = 'W' if rc[0] > c else 'E'
        self.switch(room, side, t, y)

    # --- placement ---------------------------------------------------------------------------------------------
    def wbox(self, stem, yaw, scale=1.0):
        """The model's bounds rotated by yaw, relative to its pivot: (x0, x1, y0, y1, z0, z1)."""
        lo, hi = [v * scale for v in self.idx[stem]['min']], [v * scale for v in self.idx[stem]['max']]
        a = math.radians(yaw)
        xs, zs = [], []
        for x in (lo[0], hi[0]):
            for z in (lo[2], hi[2]):
                xs.append(x * math.cos(a) + z * math.sin(a))
                zs.append(-x * math.sin(a) + z * math.cos(a))
        return min(xs), max(xs), lo[1], hi[1], min(zs), max(zs)

    def put(self, room, stem, x, z, yaw=0.0, y=0.0, collide=False, name=None, scale=1.0, wall=False, **extra):
        """Footprint centre at (x, z), bottom at y. Returns the placed piece's world box.
        (The library's loose doors / drawers are laid out beside their bodies, not in place: use the
        *_Assembled bodies from tools/assets/assemble_kits.py instead.)"""
        x0, x1, y0, y1, z0, z1 = self.wbox(stem, yaw, scale)
        px, py, pz = x - (x0 + x1) / 2, y - y0, z - (z0 + z1) / 2
        parent = self.group(room, self.group('Furniture'))
        extra = {'static': True, **extra}
        if collide:
            extra['collider'] = {'friction': 0.8, 'isTrigger': False, 'shape': 4}
        self.s.model(self.idx, stem, parent, (px, py, pz), yaw, name=name, scale=scale, **extra)
        box = {'x0': px + x0, 'x1': px + x1, 'y0': y, 'y1': py + y1, 'z0': pz + z0, 'z1': pz + z1,
               'x': x, 'z': z, 'yaw': yaw, 'stem': stem}
        self.placed.append({'room': room, 'collide': collide,
                            'wall': wall or any(w in stem for w in WALL_ITEMS), **box})
        return box

    SIDE_YAW = {'N': 0.0, 'S': 180.0, 'W': 90.0, 'E': -90.0}  # the front (local +Z) faces into the room

    def wall(self, room, side, t, stem, y=0.0, gap=0.005, collide=False, extra_yaw=0.0, name=None, scale=1.0, **extra):
        """Back against a wall of the room: side N/S/W/E, t = position along the wall (x for N/S, z for W/E)."""
        rx0, rz0, rx1, rz1 = inner(room)
        yaw = self.SIDE_YAW[side] + extra_yaw
        x0, x1, _, _, z0, z1 = self.wbox(stem, yaw, scale)
        w, d = x1 - x0, z1 - z0
        if side == 'N':
            x, z = t, rz0 + gap + d / 2
        elif side == 'S':
            x, z = t, rz1 - gap - d / 2
        elif side == 'W':
            x, z = rx0 + gap + w / 2, t
        else:
            x, z = rx1 - gap - w / 2, t
        return self.put(room, stem, x, z, yaw, y, collide, name, scale, wall=y > 0.05, **extra)

    def hang(self, room, side, t, stem, centre_y, **kw):
        """A picture hung with its centre at centre_y (1.45-1.55 m is eye level; over furniture, ~20 cm above it)."""
        _, _, y0, y1, _, _ = self.wbox(stem, 0.0)
        return self.wall(room, side, t, stem, y=centre_y - (y1 - y0) / 2, gap=0.004, **kw)

    def frame_of(self, base, dx, dz):
        a = math.radians(base['yaw'])
        cx, cz = (base['x0'] + base['x1']) / 2, (base['z0'] + base['z1']) / 2
        return cx + dx * math.cos(a) + dz * math.sin(a), cz - dx * math.sin(a) + dz * math.cos(a)

    def on(self, room, base, stem, dx=0.0, dz=0.0, yaw=None, name=None, y=None, **kw):
        """On top of a placed piece (or at height y above its bottom), offset in its own frame (dx right, dz toward
        its front); yaw is relative to the base's."""
        yaw = base['yaw'] + (yaw or 0.0)
        x, z = self.frame_of(base, dx, dz)
        return self.put(room, stem, x, z, yaw, base['y1'] if y is None else base['y0'] + y, name=name, **kw)

    def shelf_row(self, room, base, board, stems, dz=0.0, start=None, gap=0.02):
        """Line pieces up along one of a shelf piece's boards (BOARDS), left to right from `start`."""
        y = BOARDS[base['stem']][board]
        w = base['x1'] - base['x0'] if base['yaw'] % 180 == 0 else base['z1'] - base['z0']
        x = -w / 2 + 0.05 if start is None else start
        sup = dict(base)
        sup.update({'y1': base['y0'] + y})
        self.supports.append(sup)
        for stem, extra_yaw in stems:  # an empty stem leaves a 25 cm gap
            sw = 0.25
            if stem:
                x0, x1, _, _, _, _ = self.wbox(stem, extra_yaw)
                sw = x1 - x0
                self.on(room, base, stem, dx=x + sw / 2, dz=dz, yaw=extra_yaw, y=y)
            x += sw + gap

    # --- lights ------------------------------------------------------------------------------------------------
    def lamp_glow(self, stem, strength):
        """Material slots setting a lamp model's glow (its shade material at this emission strength; 0 = off). The
        library's 'on' strength is tuned for daylight scenes and blows out at night."""
        meta = json.load(open(os.path.join(self.repo, 'project', self.idx[stem]['path'] + '.meta'), encoding='utf-8'))
        mat = next(iter(meta['materialRemap'].values()))
        over = {'emissive_strength': strength}
        if strength == 0:
            over['emissiveColor'] = [0.0, 0.0, 0.0]
        return {'materials': [{'embedded': k.from_mat(self.repo, mat, **over)}] * self.idx[stem]['meshes']}

    def lamp_off(self, stem):
        return self.lamp_glow(stem, 0.0)

    def light(self, room, name, pos, intensity, rng, color, shadows=True, spot=None, flicker=None, fill=0.14, **kw):
        """A light that's on, plus a wide, faint, shadowless fill of the same colour: the bounce this renderer
        doesn't compute, so the room around a lamp isn't pure black."""
        extra = k.script(*FLICKER, **flicker) if flicker else {}
        self.s.light(f'{room} - {name}', self.group('Lighting'), pos, intensity, rng, color, shadows, spot=spot,
                     softness=1.6, **kw, **extra)
        if fill:
            # halfway to the room's middle and at chest height: a fill hugging the ceiling or a wall paints a hot
            # ellipse on it
            x0, z0, x1, z1 = inner(room)
            fx, fz = (pos[0] + (x0 + x1) / 2) / 2, (pos[2] + (z0 + z1) / 2) / 2
            self.s.light(f'{room} - {name} (bounce)', self.group('Lighting'), (fx, 1.3, fz), intensity * fill,
                         rng * 1.8, color, False)

    def ceiling_fixture(self, room, stem, x, z, on=False, **light):
        x0, x1, y0, y1, z0, z1 = self.wbox(stem, 0.0)
        drop = y1 - y0
        self.put(room, stem, x, z, 0.0, H - drop, **(self.lamp_glow(stem, 1.0) if on else self.lamp_off(stem)))
        if on:
            self.light(room, light.pop('name', 'Ceiling'), (x, H - drop - 0.05, z), **light)

    def table_lamp(self, room, base, stem, on, dx=0.0, dz=0.0, intensity=0.9, rng=3.5, glow=0.9, **kw):
        lamp = self.on(room, base, stem, dx, dz,
                       **({'castShadows': 0, **self.lamp_glow(stem, glow)} if on else self.lamp_off(stem)))
        if on:
            self.light(room, 'Lamp', ((lamp['x0'] + lamp['x1']) / 2, lamp['y1'] - 0.16, (lamp['z0'] + lamp['z1']) / 2),
                       intensity, rng, LAMP, **kw)
        return lamp

    def switch(self, room, side, t, y=1.2):
        # the switch plate is modelled facing -X: turn it a further +90 so it faces into the room
        self.wall(room, side, t, 'Light_Switches_Single', y=y, gap=0.0, extra_yaw=90.0, name='Light Switch')

    def outlet(self, room, side, t, y=0.3, gfi=False):
        self.wall(room, side, t, 'Outlet_Gfi' if gfi else 'Outlet_Electrical', y=y, gap=0.0, extra_yaw=90.0,
                  name='Outlet')

    # --- validation ----------------------------------------------------------------------------------------------
    def validate(self):
        """Layout checks on the placed boxes: anything poking out of its room or through the ceiling, solid furniture
        overlapping solid furniture, solid furniture in a door's swing, anything in a doorway, small props floating
        (nothing under them) and props sunk into each other. Returns the problems."""
        out = []
        for p in self.placed:
            x0, z0, x1, z1 = inner(p['room'])
            if p['x0'] < x0 - 0.02 or p['x1'] > x1 + 0.02 or p['z0'] < z0 - 0.02 or p['z1'] > z1 + 0.02:
                out.append(f"{p['room']}: {p['stem']} pokes out of the room ({p['x0']:.2f}..{p['x1']:.2f}, "
                           f"{p['z0']:.2f}..{p['z1']:.2f})")
            if p['y1'] > H - 0.02 and 'Lamp' not in p['stem']:
                out.append(f"{p['room']}: {p['stem']} goes through the ceiling (top {p['y1']:.2f})")

        def overlap(a, b, m=0.02):
            return (min(a['x1'], b['x1']) - max(a['x0'], b['x0']) > m and min(a['z1'], b['z1']) - max(a['z0'], b['z0']) > m
                    and min(a['y1'], b['y1']) - max(a['y0'], b['y0']) > m)
        solid = [p for p in self.placed if p['collide']]
        tucks = ({'Table_Dining', 'Chair_Dining'}, {'Workdesk_Assembled', 'Chair_Office'})
        for i, a in enumerate(solid):
            for b in solid[i + 1:]:
                if overlap(a, b) and {a['stem'], b['stem']} not in tucks:
                    out.append(f"{a['room']}: {a['stem']} overlaps {b['stem']}")
        for name, axis, c, along, _, _, _ in DOORS:
            if axis == 'x':
                zone = {'x0': along - DOOR_W / 2, 'x1': along + DOOR_W / 2, 'z0': c - 0.95, 'z1': c + 0.95}
                gap = {'x0': along - DOOR_W / 2 - 0.08, 'x1': along + DOOR_W / 2 + 0.08, 'z0': c - 0.2, 'z1': c + 0.2}
            else:
                zone = {'x0': c - 0.95, 'x1': c + 0.95, 'z0': along - DOOR_W / 2, 'z1': along + DOOR_W / 2}
                gap = {'x0': c - 0.2, 'x1': c + 0.2, 'z0': along - DOOR_W / 2 - 0.08, 'z1': along + DOOR_W / 2 + 0.08}
            zone.update({'y0': 0.0, 'y1': 2.0})
            gap.update({'y0': 0.0, 'y1': DOOR_H + 0.05})
            for p in solid:
                if overlap(zone, p):
                    out.append(f"{p['room']}: {p['stem']} is in the swing of the {name}")
            for p in self.placed:
                if overlap(gap, p, 0.005):
                    out.append(f"{p['room']}: {p['stem']} is in the {name}'s doorway / trim")
        # small props: something must be right under them; and two props mustn't sink into each other
        under = self.placed + self.supports
        props = [p for p in self.placed if not p['collide'] and not p['wall']]
        for p in props:
            if p['y0'] < 0.02:
                continue
            rest = [b for b in under if b is not p and abs(b['y1'] - p['y0']) < 0.04
                    and min(b['x1'], p['x1']) > max(b['x0'], p['x0']) and min(b['z1'], p['z1']) > max(b['z0'], p['z0'])]
            if not rest:
                out.append(f"{p['room']}: {p['stem']} floats at {p['y0']:.2f} m (nothing under it)")
        for i, a in enumerate(props):
            for b in props[i + 1:]:
                if a['y0'] > 0.03 and b['y0'] > 0.03 and overlap(a, b, 0.0):
                    ix = min(a['x1'], b['x1']) - max(a['x0'], b['x0'])
                    iz = min(a['z1'], b['z1']) - max(a['z0'], b['z0'])
                    small = min((a['x1'] - a['x0']) * (a['z1'] - a['z0']), (b['x1'] - b['x0']) * (b['z1'] - b['z0']))
                    if abs(a['y0'] - b['y0']) < 0.03 and ix * iz > 0.3 * small:
                        out.append(f"{a['room']}: {a['stem']} and {b['stem']} sit inside each other")
        return out

    # --- rooms -----------------------------------------------------------------------------------------------
    def living(self):
        """Living room: the TV on the west wall, the sofa facing it with its back to the kitchen opening (the walk
        from the hall to the kitchen and the master door runs behind and beside it), a console behind the sofa, a
        reading lamp at its end, armchairs angled in, the bookshelf on the long south wall."""
        r = 'Living Room'
        x0, z0, x1, z1 = inner(r)
        cz = 7.1
        tvs = self.wall(r, 'W', cz, 'Tv_Stand_Assembled', collide=True)
        tv = self.on(r, tvs, 'Tv', dz=-0.04)
        # the picture: an emissive panel on the CRT's face, lit for the night's late show
        tx, tz = self.frame_of(tv, 0.0, (tv['x1'] - tv['x0']) / 2 + 0.004)
        self.s.box('TV Picture', self.group(r, self.group('Furniture')), (tx, tv['y0'] + 0.31, tz), (0.004, 0.4, 0.6),
                   self.mats['screen'], collider=False, static=True, castShadows=0)
        self.light(r, 'TV', (tx + 0.35, tv['y0'] + 0.3, tz), 0.9, 5.0, TV, spot=75.0, pitch=0.0, yaw=-90.0,
                   flicker={'Mode': MODES['Tv'], 'MinInterval': 0.12, 'MaxInterval': 1.4, 'Depth': 0.55, 'Seed': 7},
                   fill=0.12)
        self.shelf_row(r, tvs, 0, [('Books_E', 90.0), ('Books_F', 90.0), ('', 0.0), ('Decorative_Bowl', 0.0)], dz=0.0)
        self.hang(r, 'W', cz, 'Painting_Large_B', 1.75)
        self.put(r, 'Carpet_C', 2.25, cz, 90.0)
        couch = self.put(r, 'Couch', 3.45, cz, -90.0, collide=True)
        coffee = self.put(r, 'Sidetable_C', 2.3, cz, 0.0, collide=True)
        self.on(r, coffee, 'Tv_Remote', dx=0.1, dz=0.12, yaw=35.0)
        self.on(r, coffee, 'Coffee_Mug', dx=-0.14, dz=-0.1, yaw=200.0)
        self.on(r, coffee, 'Magazine_Comic_Kid', dx=0.06, dz=-0.08, yaw=-15.0)
        self.on(r, coffee, 'Bowl', y=BOARDS['Sidetable_C'][0])
        self.supports.append(dict(coffee, y1=coffee['y0'] + BOARDS['Sidetable_C'][0]))
        # a console behind the sofa: lamp (off), flowers, the family Bible, a photo
        console = self.put(r, 'Sidetable_A', couch['x1'] + 0.24, cz, -90.0, collide=True)
        self.table_lamp(r, console, 'Lamp_Table_A_Assembled', False, dx=-0.32)
        self.on(r, console, 'Tulips_A', dx=0.32, dz=-0.04)
        self.on(r, console, 'Holy_Bible', dx=0.02, dz=0.03, yaw=12.0)
        self.on(r, console, 'Painting_Table_A', dx=0.15, dz=-0.08, yaw=180.0)
        # reading lamp at the sofa's south end: one of the few lights on
        fl = self.put(r, 'Lamp_Floor_A_Assembled', couch['x'] - 0.05, couch['z1'] + 0.3, 0.0, castShadows=0,
                      **self.lamp_glow('Lamp_Floor_A_Assembled', 0.9))
        self.light(r, 'Floor Lamp', (fl['x'], fl['y1'] - 0.2, fl['z']), 1.1, 4.5, LAMP)
        self.put(r, 'Arm_Chair_01', 1.55, 5.25, 30.0, collide=True)
        self.put(r, 'Chair_Arm', 1.55, 8.95, 150.0, collide=True)
        # the kid's things drift in: a car and blocks on the rug
        self.put(r, 'Toy_Car_C', 2.85, 6.25, 60.0)
        self.put(r, 'Toy_Block_A', 1.75, 7.95, 20.0)
        self.put(r, 'Toy_Block_C', 1.9, 8.05, -35.0)
        bs = self.wall(r, 'S', 3.15, 'Bookshelf', collide=True)
        self.shelf_row(r, bs, 0, [('Cardboard_Box_A', 90.0), ('Book_Pile_A', 90.0), ('Books_A', 180.0)], dz=0.0)
        self.shelf_row(r, bs, 1, [('Books_B', 180.0), ('Books_C', 180.0), ('Painting_Table_B', 180.0),
                                   ('Vase_A', 0.0)], dz=0.02)
        self.shelf_row(r, bs, 2, [('Candle_Holder_B', 0.0), ('Books_D', 180.0), ('Books_E', 180.0), ('Books_F', 180.0)],
                       dz=0.02)
        self.shelf_row(r, bs, 3, [('Books_A', 180.0), ('Painting_Table_E', 180.0), ('Decorative_Plate', 180.0)],
                       dz=0.02)
        # a small shrine on the south wall: a half-moon table, the statue, candles, the crucifix above
        shrine = self.wall(r, 'S', 5.0, 'Sidetable_B', collide=True)
        self.on(r, shrine, 'David', scale=0.16)
        self.on(r, shrine, 'Candle_A', dx=-0.3, dz=0.02)
        self.on(r, shrine, 'Candle_B', dx=0.3, dz=0.02)
        self.hang(r, 'S', 5.0, 'Cross', 1.55)
        self.wall(r, 'S', 0.45, 'Plant_A', gap=0.15, collide=True)
        self.wall(r, 'W', 5.0, 'Cabinet_A_Assembled', collide=True)
        self.put(r, 'Plant_B', 0.35, 9.55)
        self.hang(r, 'N', 1.2, 'Painting_Large_A', 1.5)
        self.hang(r, 'N', 5.2, 'Clock_Wall_Analog', 2.0)
        self.latch_side('Master Door', r)
        self.outlet(r, 'W', cz - 1.1)
        self.outlet(r, 'S', 1.6)
        self.outlet(r, 'N', 4.1)
        self.ceiling_fixture(r, 'Lamp_Ceiling_B', 2.4, cz)
        self.ceiling_fixture(r, 'Lamp_Ceiling_B', 5.0, 4.85)

    def kitchen(self):
        """Kitchen: fridge, counter, stove, counter along the north wall and the sink run on the east wall - the
        fridge -> sink -> stove triangle - uppers above, the pantry by the opening, the table in the middle with
        last night's dishes still on it. Only the cooker hood's light is on, and one dim bulb over the table."""
        r = 'Kitchen'
        x0, z0, x1, z1 = inner(r)

        def run_n(stem, x, **kw):  # along the north wall, left edge at x
            w = self.idx[stem]['max'][0] - self.idx[stem]['min'][0]
            return self.wall(r, 'N', x + w / 2, stem, **kw)
        fr = run_n('Fridge_Assembled', x0 + 0.02, collide=True)
        c1 = run_n('Kitchen_Counter_A_Assembled', fr['x1'] + 0.02, collide=True)
        st = run_n('Stove', c1['x1'], collide=True)
        c2 = run_n('Kitchen_Counter_A_Assembled', st['x1'], collide=True)
        hood = self.wall(r, 'N', (st['x0'] + st['x1']) / 2, 'Stove_Cooker_Hood', y=1.72)
        self.light(r, 'Hood Light', ((st['x0'] + st['x1']) / 2, hood['y0'] - 0.02, (hood['z0'] + hood['z1']) / 2 + 0.1),
                   0.7, 2.5, WARM, spot=62.0, fill=0.1)
        for base in (c1, c2):
            self.wall(r, 'N', (base['x0'] + base['x1']) / 2, 'Kitchen_Cabinet_B_Assembled', y=1.52, collide=True)
        # coffee corner on the left counter, against the backsplash
        self.on(r, c1, 'Coffeemaker', dx=-0.28, dz=-0.07)
        self.on(r, c1, 'Coffee_Mug', dx=-0.02, dz=0.02, yaw=160.0)
        self.on(r, c1, 'Coffee_Mug', dx=0.1, dz=-0.12, yaw=40.0)
        self.on(r, c1, 'Bottle_A', dx=0.38, dz=-0.18)
        self.on(r, c1, 'Can', dx=0.27, dz=-0.17)
        # cooking: pot and pan on the hob, utensils by it, a knife left out by a plate
        self.on(r, st, 'Cooking_Pot', dx=-0.22, dz=0.08)
        self.on(r, st, 'Frying_Pan', dx=0.22, dz=0.12, yaw=30.0)
        self.on(r, c2, 'Utensil_Holder_Full', dx=-0.38, dz=-0.18)
        self.on(r, c2, 'Plate_Single', dx=-0.1, dz=0.08)
        self.on(r, c2, 'Knife', dx=-0.1, dz=0.08, yaw=70.0, y=c2['y1'] - c2['y0'] + 0.021)
        self.on(r, c2, 'Cooking_Spatula', dx=0.2, dz=0.1, yaw=95.0)
        self.on(r, c2, 'Jar_A', dx=0.18, dz=-0.18)
        self.on(r, c2, 'Jar_B', dx=0.3, dz=-0.18)
        self.on(r, c2, 'Pepper_Mill', dx=0.4, dz=-0.15)
        # east run: sink, then a counter with the drying dishes
        sk = self.wall(r, 'E', z0 + 0.62 + 0.5, 'Kitchen_Counter_Sink_Assembled', collide=True)
        c3 = self.wall(r, 'E', sk['z1'] + 0.5, 'Kitchen_Counter_A_Assembled', collide=True)
        self.on(r, sk, 'Handsoap', dx=-0.38, dz=-0.17)
        self.on(r, c3, 'Plate_Pile', dx=-0.2, dz=-0.05)
        self.on(r, c3, 'Bowl_Pile', dx=0.15, dz=-0.12)
        self.on(r, c3, 'Glass_Drinking_Pile', dx=0.36, dz=0.1)
        self.on(r, c3, 'Teapot', dx=-0.35, dz=0.12, yaw=-60.0)
        self.on(r, c3, 'Macaroni_Package', dx=0.38, dz=-0.2, yaw=-80.0)
        for base in (sk, c3):
            self.wall(r, 'E', (base['z0'] + base['z1']) / 2, 'Kitchen_Cabinet_B_Assembled', y=1.52, collide=True)
        self.wall(r, 'S', x0 + 0.85, 'Kitchen_Cabinet_Large_Assembled', collide=True)
        self.put(r, 'Garbage_Bag_A', x0 + 1.95, z1 - 0.3, 25.0)
        # the table: breakfast never cleared - two cereal bowls, a glass, the mail
        tx, tz = 8.15, 7.95
        tb = self.put(r, 'Table_Dining', tx, tz, 0.0, collide=True)
        for yaw, dx, dz in ((180.0, 0.0, -0.78), (8.0, 0.05, 0.92), (90.0, -0.78, 0.0), (-115.0, 0.82, 0.12)):
            self.put(r, 'Chair_Dining', tx + dx, tz + dz, yaw, collide=True)
        self.on(r, tb, 'Bowl_Cereal', 0.0, -0.42)
        self.on(r, tb, 'Bowl_Cereal', -0.42, 0.05)
        self.on(r, tb, 'Glass_Drinking', -0.2, -0.38)
        self.on(r, tb, 'Papers_Pile_A', 0.35, 0.25, yaw=25.0)
        self.on(r, tb, 'Decorative_Bowl', 0.05, 0.08)
        chand = self.wbox('Lamp_Chandelier_A', 0.0)
        self.put(r, 'Lamp_Chandelier_A', tx, tz, 0.0, H - (chand[3] - chand[2]),
                 **self.lamp_glow('Lamp_Chandelier_A', 0.7))
        self.light(r, 'Table', (tx, H - (chand[3] - chand[2]) + 0.25, tz), 0.55, 3.2, WARM, spot=58.0, fill=0.15)
        self.ceiling_fixture(r, 'Lamp_Ceiling_A', 8.2, 6.3)
        self.hang(r, 'S', 9.3, 'Painting_Medium_E', 1.5)
        self.hang(r, 'W', 9.35, 'Clock_Wall_Analog', 1.95)
        self.switch(r, 'W', 6.05)
        self.outlet(r, 'N', c1['x0'] + 0.2, y=1.12, gfi=True)
        self.outlet(r, 'E', c3['z1'] - 0.2, y=1.12, gfi=True)

    def master(self):
        """Master bedroom: the bed's headboard on the west wall facing the door, a nightstand each side (his lamp on,
        hers off with the Bible), the dresser opposite on the north wall, the long mirror by the ensuite."""
        r = 'Master Bedroom'
        x0, z0, x1, z1 = inner(r)
        bed = self.wall(r, 'W', 2.0, 'Bed', collide=True)
        self.put(r, 'Carpet_A', bed['x1'] - 0.35, 2.0, 90.0)
        ns_n = self.wall(r, 'W', bed['z0'] - 0.34, 'Nightstand_B_Assembled', collide=True)
        ns_s = self.wall(r, 'W', bed['z1'] + 0.34, 'Nightstand_B_Assembled', collide=True)
        self.table_lamp(r, ns_n, 'Lamp_Table_B_Assembled', False, dz=-0.06)
        self.on(r, ns_n, 'Vial_Pain_Killer_02', dx=0.15, dz=0.14)
        self.table_lamp(r, ns_s, 'Lamp_Table_B_Assembled', True, dz=-0.06, intensity=0.8, rng=3.8)
        self.on(r, ns_s, 'Clock_Table_Digital', dx=-0.13, dz=0.14, yaw=-20.0)
        self.on(r, ns_s, 'Glass_Drinking', dx=0.16, dz=0.14)
        self.hang(r, 'W', 2.0, 'Cross', 1.62)
        dr = self.wall(r, 'N', 3.2, 'Dresser_C_Assembled', collide=True)
        self.on(r, dr, 'Vase_B', dx=-0.66, dz=-0.04)
        self.on(r, dr, 'Vase_B_Sticks', dx=-0.5, dz=-0.08)
        self.on(r, dr, 'Money_Usa', dx=-0.22, dz=-0.1, yaw=84.0, scale=0.5)  # half behind the photo
        self.on(r, dr, 'Painting_Table_C', dx=0.1, dz=-0.09)
        self.on(r, dr, 'Holy_Bible', dx=0.35, dz=0.08, yaw=-6.0)
        self.on(r, dr, 'Candle_Holder_B', dx=0.65, dz=-0.05)
        self.hang(r, 'N', 3.2, 'Painting_Large_C', 1.55)
        self.wall(r, 'E', 1.35, 'Mirror_Body', gap=0.06, collide=True)
        self.put(r, 'Laundrybasket', 1.95, z0 + 0.3, 12.0)
        self.put(r, 'Towel_Pile', 1.95, z0 + 0.3, 12.0, y=0.716)
        self.hang(r, 'S', 1.3, 'Painting_Medium_B', 1.5)
        self.latch_side('Master Door', r)
        self.outlet(r, 'W', bed['z0'] - 0.7)
        self.outlet(r, 'N', 1.4)
        self.ceiling_fixture(r, 'Lamp_Ceilingfan', 2.4, 2.1)

    def ensuite(self):
        """Ensuite: shower across the north end, the double vanity on the west wall under the mirror, the toilet on
        the east wall with the paper beside it, a towel rail, a shelf of towels. Its light is on and the door
        ajar, so it spills into the dark bedroom."""
        r = 'Ensuite'
        x0, z0, x1, z1 = inner(r)
        sh = self.wall(r, 'N', (x0 + x1) / 2, 'Shower_Stall_190cm_L_Assembled', collide=True)
        self.wall(r, 'N', (x0 + x1) / 2 + 0.55, 'Shower', y=0.35)
        soap = self.wall(r, 'N', (x0 + x1) / 2 - 0.4, 'Shower_Soap_Holder', y=1.1)
        self.on(r, soap, 'Shampoo_Bottle_A', dx=-0.06)
        self.on(r, soap, 'Shampoo_Bottle_B', dx=0.06, yaw=20.0)
        vn = self.wall(r, 'W', 2.02, 'Bathroom_Sink_Body', collide=True)
        self.wall(r, 'W', 2.02, 'Mirror_Bathroom', y=1.15)
        self.on(r, vn, 'Toothbrush_Mug', dx=-0.05, dz=-0.17)
        self.on(r, vn, 'Toothpaste', dx=0.1, dz=-0.2, yaw=0.0)
        self.on(r, vn, 'Handsoap', dx=0.32, dz=-0.18)
        self.on(r, vn, 'Lotiontube', dx=0.46, dz=-0.17, yaw=30.0)
        self.on(r, vn, 'Syringe_Variation_01', dx=-0.55, dz=0.12, yaw=60.0)
        self.on(r, vn, 'Vial_Insulin_01', dx=-0.48, dz=0.0)
        # the room's own ceiling light is the one on: a downlight from the middle of the ceiling, kept off the walls
        self.ceiling_fixture(r, 'Lamp_Ceiling_A', 5.5, 2.2, on=True, name='Ceiling', intensity=1.0, rng=4.0,
                             color=(1.0, 0.86, 0.68), spot=55.0, fill=0.12)
        self.wall(r, 'E', 3.4, 'Toilet_Assembled', collide=True)
        self.wall(r, 'E', 2.95, 'Toiletpaper_Single', y=0.7)
        unit = self.wall(r, 'E', 2.45, 'Bathroom_Shelf_Unit_B', collide=True)
        self.shelf_row(r, unit, 1, [('Bathroom_Box_A', 90.0)])
        self.shelf_row(r, unit, 2, [('Towel_Pile', 90.0)])
        self.shelf_row(r, unit, 0, [('Toiletpaper_Set', 90.0)])
        self.shelf_row(r, unit, 3, [('Towel_Roll_Set', 90.0)])
        self.wall(r, 'E', 1.55, 'Towel_Holder', y=1.25)
        self.wall(r, 'E', 1.55, 'Towel_Hanging_Large', y=0.75, gap=0.04)
        self.put(r, 'Carpet_D', 5.3, 1.5, 90.0, scale=0.55)
        self.switch(r, 'W', 2.85)

    def bedroom2(self):
        """Guest room / home office: the single bed along the north wall, the desk against the west wall with the
        chair tucked in and the desk lamp on, a dresser, and the boxes nobody unpacked along the south wall."""
        r = 'Bedroom 2'
        x0, z0, x1, z1 = inner(r)
        bed = self.wall(r, 'N', x1 - 0.65, 'Bed_Single_Person', collide=True)
        ns = self.wall(r, 'N', bed['x0'] - 0.26, 'Nightstand_B_Assembled', collide=True)
        self.table_lamp(r, ns, 'Lamp_Table_A_Assembled', False, dz=-0.04)
        desk = self.wall(r, 'W', 1.55, 'Workdesk_Assembled', collide=True)
        self.put(r, 'Chair_Office', desk['x1'] + 0.12, 1.55, -95.0, collide=True)
        self.table_lamp(r, desk, 'Lamp_Table_B_Assembled', True, dx=-0.65, dz=-0.15, intensity=0.75, rng=3.5)
        self.on(r, desk, 'Papers_Pile_B', dx=-0.15, dz=0.05, yaw=8.0)
        self.on(r, desk, 'Papers_Holder_A', dx=0.3, dz=-0.2)
        self.on(r, desk, 'Folders_Set_A', dx=0.6, dz=-0.2)
        self.on(r, desk, 'Pen_Holder_With_Pens', dx=0.12, dz=-0.22)
        self.on(r, desk, 'Scissors', dx=0.2, dz=0.15, yaw=40.0)
        self.on(r, desk, 'Pencil_A', dx=0.4, dz=0.2, yaw=100.0)
        self.on(r, desk, 'Coffee_Mug', dx=-0.42, dz=0.1)
        dr = self.wall(r, 'E', 2.95, 'Dresser_B_Assembled', collide=True)
        self.on(r, dr, 'Folders_Pile_A', dx=-0.22, yaw=8.0)
        self.on(r, dr, 'Books_D', dx=0.15, dz=-0.05)
        self.on(r, dr, 'Plant_B', dx=0.35)
        # the boxes: stacked two high along the south wall, past the door's swing
        b1 = self.wall(r, 'S', 9.55, 'Cardboard_Box_D', collide=True)
        self.on(r, b1, 'Cardboard_Box_A', yaw=10.0)
        b2 = self.wall(r, 'S', 9.0, 'Storage_Box_A', collide=True, extra_yaw=90.0)
        self.on(r, b2, 'Papers_Box_C', yaw=-8.0)
        self.put(r, 'Cardboard_Box_B', 8.55, z1 - 0.3, 75.0)
        self.put(r, 'Carpet_B', 8.05, 2.55, 0.0)
        self.hang(r, 'W', 1.55, 'Painting_Medium_D', 1.55)
        self.hang(r, 'N', bed['x0'] - 0.26, 'Painting_Small_C', 1.45)
        self.latch_side('Bedroom 2 Door', r)
        self.outlet(r, 'W', 2.65)
        self.outlet(r, 'N', 7.4)
        self.ceiling_fixture(r, 'Lamp_Ceiling_A', 8.3, 2.1)

    def kids(self):
        """Kid's room: bunk bed in the corner, play space on the rug in the middle (toys out), dresser with the
        rocket nightlight - the only light, very low - posters, a little table with drawings."""
        r = 'Kids Bedroom'
        x0, z0, x1, z1 = inner(r)
        bunk = self.wall(r, 'N', x1 - 0.76, 'Bed_Bunk_Assembled', collide=True)
        dr = self.wall(r, 'W', 1.05, 'Dresser_D_Assembled', collide=True)
        self.table_lamp(r, dr, 'Lamp_Table_Rocket_Assembled', True, dx=0.15, intensity=0.28, rng=3.0, glow=0.35)
        self.on(r, dr, 'Toy_Dinosaur', dx=-0.18, yaw=60.0)
        self.put(r, 'Carpet_Kid', 11.45, 2.35, 0.0)
        tb = self.wall(r, 'S', 12.35, 'Toy_Box_A', collide=True)
        self.put(r, 'Toy_Box_A_Lid', tb['x0'] - 0.3, tb['z0'] + 0.12, 70.0)
        self.put(r, 'Toy_Teddy', bunk['x0'] - 0.35, bunk['z1'] - 0.25, 200.0)
        self.put(r, 'Toy_Racetrack', 11.2, 2.55, 20.0)
        self.put(r, 'Toy_Car_A', 11.0, 2.25, 75.0)
        self.put(r, 'Toy_Car_B', 11.6, 3.0, -30.0)
        self.put(r, 'Toy_Truck', 10.7, 3.35, 140.0)
        self.put(r, 'Toy_Block_Set_A', 12.0, 3.05, 10.0)
        self.put(r, 'Toy_Block_B', 12.25, 2.75, 40.0)
        self.put(r, 'Toy_Muscleman', 10.9, 1.75, -60.0)
        self.put(r, 'Paper_Kid', 11.85, 1.95, 25.0)
        self.put(r, 'Book_Drawing_Kid', 10.55, 2.95, -40.0)
        st = self.wall(r, 'W', 2.65, 'Sidetable_B', collide=True)
        self.on(r, st, 'Paper_Kid', dx=-0.15, yaw=8.0)
        self.on(r, st, 'Pen_Set_A_Kid', dx=0.18)
        self.on(r, st, 'Jar_Kid', dx=0.3, dz=-0.05)
        self.on(r, st, 'Notebook_Kid', dx=-0.32, yaw=-12.0)
        self.hang(r, 'W', 2.65, 'Poster_Kid_A', 1.5)
        self.hang(r, 'E', 3.2, 'Poster_Kid_C', 1.45)
        self.hang(r, 'S', 11.85, 'Poster_Kid_E', 1.55)
        self.hang(r, 'W', 1.05, 'Poster_Kid_F', 1.65)
        self.latch_side('Kids Bedroom Door', r)
        self.outlet(r, 'W', 3.4)
        self.ceiling_fixture(r, 'Lamp_Ceiling_A', 11.5, 2.1)

    def bathroom(self):
        """Main bathroom: shower on the east wall, vanity on the south wall, toilet on the west. The light is off -
        a black room behind a closed door."""
        r = 'Bathroom'
        x0, z0, x1, z1 = inner(r)
        sw = self.wbox('Shower_Stall_190cm_R_Assembled', -90.0)
        sh = self.wall(r, 'E', z0 + (sw[5] - sw[4]) / 2 + 0.01, 'Shower_Stall_190cm_R_Assembled', collide=True)
        self.wall(r, 'E', sh['z0'] + 0.5, 'Shower', y=0.35)
        vn = self.wall(r, 'S', x0 + 0.95, 'Bathroom_Sink_Body', collide=True)
        self.wall(r, 'S', x0 + 0.95, 'Mirror_Bathroom', y=1.15)
        self.on(r, vn, 'Toothbrush_A', dx=-0.3, dz=-0.15, yaw=80.0)
        self.on(r, vn, 'Handsoap', dx=0.1, dz=-0.18)
        self.on(r, vn, 'Shampoo_Bottle_B', dx=0.55, dz=-0.15)
        self.wall(r, 'W', 6.95, 'Toilet_Assembled', collide=True)
        self.wall(r, 'W', 6.45, 'Toiletpaper_Single', y=0.7)
        self.put(r, 'Toiletpaper_Set', 10.62, 7.42, 90.0)
        self.wall(r, 'W', 7.6, 'Towel_Holder', y=1.3)
        self.wall(r, 'W', 7.6, 'Towel_Hanging_Small', y=0.85, gap=0.04)
        self.put(r, 'Carpet_D', 11.35, 7.15, 0.0, scale=0.55)
        self.put(r, 'Laundrybasket', 11.8, 7.95, -10.0)
        self.switch(r, 'N', 11.85)  # the latch side is jammed in the corner: switch on the hinge side
        self.ceiling_fixture(r, 'Lamp_Ceiling_A', 11.4, 6.85)

    def laundry(self):
        """Laundry / utility: washer against the south wall, the steel shelf of paint, cans and tools, the breaker
        panel. The tube light is on and stutters."""
        r = 'Laundry'
        x0, z0, x1, z1 = inner(r)
        # the washer is modelled with its front at local -X (control panel along +X): a quarter turn puts the
        # panel against the wall
        wm = self.wall(r, 'S', x1 - 1.0, 'Washingmachine_Assembled', collide=True, extra_yaw=90.0)
        self.on(r, wm, 'Towel_Roll_Set', dx=-0.1)
        shelf = self.wall(r, 'E', z0 + 0.62, 'Garage_Shelf_A', collide=True)
        self.shelf_row(r, shelf, 0, [('Paintbucket_A', 0.0), ('Paintbucket_B', 0.0), ('Jerrycan', 90.0)])
        self.shelf_row(r, shelf, 1, [('Storage_Box_B', 90.0), ('Spray_Can_Set_A', -90.0), ('Paint_Thinner', -90.0)])
        self.shelf_row(r, shelf, 2, [('Cardboard_Box_A', 90.0), ('Spray_Can_A', 0.0), ('Spray_Can_B', 0.0),
                                     ('Wrench', 90.0), ('Screwdriver', 90.0)])
        self.shelf_row(r, shelf, 3, [('Storage_Box_C', 90.0), ('Paintbucket_C', 0.0)])
        self.wall(r, 'N', 11.3, 'Electricbox_Main', y=1.35)
        self.put(r, 'Laundrybasket', x0 + 0.9, z1 - 0.3, 10.0)
        self.put(r, 'Cooler_Box', x0 + 1.3, z0 + 0.3, 5.0)
        self.put(r, 'Garbage_Bag_B', x0 + 0.75, z0 + 0.32, -20.0)
        self.latch_side('Laundry Door', r)
        self.outlet(r, 'S', x1 - 0.45, y=0.9, gfi=True)
        tube = self.wbox('Lamp_Fluorescent_A', 0.0)
        self.put(r, 'Lamp_Fluorescent_A', 11.7, 9.15, 0.0, H - (tube[3] - tube[2]),
                 **self.lamp_glow('Lamp_Fluorescent_A', 1.2))
        self.light(r, 'Tube', (11.7, H - 0.12, 9.15), 1.1, 4.5, TUBE, spot=80.0, fill=0.12,
                   flicker={'Mode': MODES['Fluorescent'], 'MinInterval': 3.0, 'MaxInterval': 11.0, 'Depth': 0.95,
                            'Seed': 3})

    def hall(self):
        """Hall: a drop shelf by the front door (keys, mail, a photo), the boxes and bag waiting by the door, a
        family gallery along the long wall, the crucifix over the door. One ceiling light on at the living-room
        end, failing."""
        r = 'Hall'
        x0, z0, x1, z1 = inner(r)
        sh = self.wall(r, 'S', 12.3, 'Shelf', y=1.05, scale=0.8)
        self.on(r, sh, 'Decorative_Bowl', dx=-0.26)
        self.on(r, sh, 'Papers_Pile_A', dx=0.02, yaw=88.0)
        self.on(r, sh, 'Painting_Table_E', dx=0.28, dz=-0.02)
        self.hang(r, 'E', 4.8, 'Cross', 2.25)
        # gallery on the kitchen wall: one centre line at 1.5 m, mixed sizes
        for t, stem in ((6.55, 'Painting_Small_D'), (7.25, 'Painting_Small_A'), (8.05, 'Painting_Medium_G'),
                        (8.85, 'Painting_Small_B'), (9.55, 'Painting_Small_C'), (10.05, 'Painting_Small_E')):
            if stem == 'Painting_Small_E':
                continue
            self.hang(r, 'S', t, stem, 1.5)
        self.hang(r, 'N', 9.15, 'Clock_Wall_Analog', 1.95)
        self.switch(r, 'N', 12.55)
        self.ceiling_fixture(r, 'Lamp_Ceiling_B', 7.4, 4.8, on=True, name='Ceiling', intensity=0.9, rng=4.5,
                             color=(1.0, 0.8, 0.6), spot=70.0,
                             flicker={'Mode': MODES['Fluorescent'], 'MinInterval': 6.0, 'MaxInterval': 18.0, 'Depth': 0.8,
                                      'Seed': 11})
        self.ceiling_fixture(r, 'Lamp_Ceiling_B', 11.5, 4.8)

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
        # Night with most lights off: almost no sky fill, a neutral dim sky for the mirrors to reflect, shadows on
        # every light that's on (the nearest 8 point / 12 spot lights get them), and a slightly crushed, grainy grade.
        for key in ('skyHdriGuid', 'skyHdriPath', 'skyRotationDegrees'):
            self.d.pop(key, None)
        self.d.update({'skySource': 0, 'skyHorizonColor': [0.2, 0.21, 0.24], 'skyZenithColor': [0.16, 0.17, 0.2],
                       'skyAmbientIntensity': 0.06, 'exposureEV': 1.7, 'autoExposure': False, 'fogEnabled': False,
                       'shadowDistance': 40.0, 'maxPointShadows': 8, 'maxSpotShadows': 12, 'ssaoEnabled': True,
                       'ssaoIntensity': 1.25, 'ssaoRadius': 0.45, 'bloomEnabled': True, 'bloomIntensity': 0.12,
                       'bloomThreshold': 1.0, 'vignetteIntensity': 0.32, 'vignetteSmoothness': 0.55,
                       'filmGrain': 0.06, 'gradeContrast': 8.0, 'gradeSaturation': -6.0, 'gradeTemperature': 0.0})
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
    args = ap.parse_args()
    repo = os.path.abspath(args.repo)
    b = Builder(repo)
    b.build()
    for line in b.validate():
        print('build_apartment: check:', line)
    out = args.out or os.path.join(repo, 'project', 'scenes', 'Apartment_3Bed.json')
    b.s.write(out)
    print(f'build_apartment: {len(b.s.models)} models, {len(b.s.boxes)} boxes, {len(b.s.empties)} empties -> {out}')


if __name__ == '__main__':
    main()
