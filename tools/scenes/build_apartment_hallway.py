# The Apartment Hallway: a third-floor corridor at night with six empty apartments off it (a studio, a one-bed and a
# two-bed on each side), dark plank floors, white plaster walls, glass windows onto the physical night sky and its big
# moon, dim and flickering ceiling lights, doors that open with Interact (E), and the player in black classic pants,
# classic shoes and a balaclava with the AKS-74U and Remington 870.
#
#   python tools/scenes/build_apartment_hallway.py [<repo>]
#
# Writes project/scenes/Apartment Hallway.json (+ .meta, its guid kept across rebuilds). Render settings and the
# player's rig come from scenes/Arena.json.
#
# See-through walls: every wall face is a single-sided plane (Plane_1m) facing into its own room, so from inside the
# walls are solid and from outside (the editor's scene view) back-face culling shows straight into the rooms. Inside
# each wall sits an invisible box (Shadows Only) that collides and casts shadows, so neither the player nor the light
# gets through. Ceilings are planes facing down under an invisible shadow-casting roof slab.
#
# Plan (metres, X east along the corridor, Z south, floor y = 0): the corridor runs x 0..L between the end walls,
# its walls' centre lines at z = +-HALL; the apartments are D deep beyond them, the north row's front doors on
# z = -HALL and the south row's on z = +HALL. Rooms are centre-line rectangles; wall faces sit half a wall in.
import json
import math
import os
import secrets
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scene_kit as k  # noqa: E402

H = 2.7               # floor to ceiling
T = 0.15              # partition thickness
TX = 0.3              # outside walls
HALL = 1.075          # corridor wall centre lines at z = +-HALL: 2.0 m clear
D = 8.6               # apartment depth, corridor wall to outside wall (centre lines)
DOOR_W, DOOR_H = 0.92, 2.06      # opening for Door_Op_1's frame (the frame's own trim covers the gap)
DOOR_WALL_Z = 0.032              # the frame's wall centre, door-local +Z of its origin
WIN_STEM, WIN_SCALE = 'Window_Tall_Fake', 1.15   # 1.0 x 1.77 m
WIN_W, WIN_H, WIN_FRAME, WIN_SILL = 0.87 * WIN_SCALE, 1.54 * WIN_SCALE, 0.045 * WIN_SCALE, 0.7
PLANE = 'assets/Architecture/Building/Models/Plane_1m.obj'
DOOR_AUDIO = 'assets/Audio/Foley/door'

# Unit plans in their own (u, v): u along the corridor from the unit's start, v from the corridor wall (0) to the
# outside wall (D). rooms: name -> (u0, v0, u1, v1). doors: (room it swings into, axis, line, centre, interior?);
# axis 'u' = the wall runs along u at v = line. arches: open passages (axis, line, centre, width, height).
# windows: (axis, line, centre). The two-bed's u = W side is the building's end wall (its second bedroom's window).
UNITS = {
    'Studio': dict(W=5.0, rooms={
        'Bathroom': (0.0, 0.0, 2.4, 2.7), 'Entry': (2.4, 0.0, 5.0, 2.7), 'Living Room': (0.0, 2.7, 5.0, D)},
        doors=[('Entry', 'u', 0.0, 3.7, False), ('Bathroom', 'v', 2.4, 1.35, True)],
        arches=[('u', 2.7, 3.7, 1.6, 2.3)],
        windows=[('u', D, 1.55), ('u', D, 3.45)]),
    '1 Bed': dict(W=6.8, rooms={
        'Bathroom': (0.0, 0.0, 2.5, 2.8), 'Entry': (2.5, 0.0, 6.8, 2.8), 'Bedroom': (0.0, 2.8, 3.4, D),
        'Living Room': (3.4, 2.8, 6.8, D)},
        doors=[('Entry', 'u', 0.0, 4.6, False), ('Bathroom', 'v', 2.5, 1.4, True), ('Bedroom', 'v', 3.4, 3.8, True)],
        arches=[('u', 2.8, 5.2, 1.4, 2.3)],
        windows=[('u', D, 1.7), ('u', D, 4.3), ('u', D, 5.9)]),
    '2 Bed': dict(W=9.4, rooms={
        'Bathroom': (0.0, 0.0, 3.6, 3.0), 'Bedroom 1': (0.0, 3.0, 3.6, D), 'Entry': (3.6, 0.0, 5.8, 3.6),
        'Bedroom 2': (5.8, 0.0, 9.4, 3.6), 'Living Room': (3.6, 3.6, 9.4, D)},
        doors=[('Entry', 'u', 0.0, 4.7, False), ('Bathroom', 'v', 3.6, 1.5, True), ('Bedroom 1', 'v', 3.6, 4.4, True),
               ('Bedroom 2', 'v', 5.8, 2.4, True)],
        arches=[('u', 3.6, 4.7, 1.4, 2.3)],
        windows=[('u', D, 1.8), ('u', D, 5.6), ('u', D, 7.8), ('v', 9.4, 1.8)]),
}
# (number, plan, x of u = 0, u direction, row: -1 north / +1 south). The two-beds sit at the ends, their u = W side
# on the end walls.
L = sum(UNITS[p]['W'] for p in ('Studio', '1 Bed', '2 Bed'))
PLACEMENT = [(301, 'Studio', 0.0, 1, -1), (302, '1 Bed', 5.0, 1, -1), (303, '2 Bed', 11.8, 1, -1),
             (304, '2 Bed', 9.4, -1, 1), (305, '1 Bed', 9.4, 1, 1), (306, 'Studio', 16.2, 1, 1)]
ZOUT = HALL + D       # outside wall lines z = +-ZOUT
HALL_WINDOWS = [('v', 0.0, 0.0), ('v', L, 0.0)]   # an end window each end of the corridor (world axis/line/centre)

TUBE_K, BULB_K = 4000.0, 2700.0
TEST_ROOM = 'Apartment 301/Living Room'   # the wounded-animation test room (Hallway.test_npcs)
NORTH_TARGET = (0.74, 0.42, 0.52)   # where the moon should hang (direction to it): 35 deg south of east, 25 deg up


def ref(repo, path):
    with open(os.path.join(repo, 'project', path + '.meta'), encoding='utf-8') as f:
        return {'path': path, 'pathGuid': json.load(f)['guid']}


# --- the moon: the engine's TimeOfDay::MoonDirection, to aim the sky so the moon shines in the south windows -----
def moon_dir(hours, day, lat, north, phase):
    sun_dec = math.radians(-23.44) * math.cos(2 * math.pi * (day + 10) / 365)
    dec = sun_dec * math.cos(2 * math.pi * phase) + math.radians(5.14) * math.sin(2 * math.pi * day / 27.21)
    ha = math.radians(15 * (hours - 12)) - 2 * math.pi * phase
    phi = math.radians(lat)
    east = -math.cos(dec) * math.sin(ha)
    nth = math.sin(dec) * math.cos(phi) - math.cos(dec) * math.cos(ha) * math.sin(phi)
    up = math.sin(dec) * math.sin(phi) + math.cos(dec) * math.cos(ha) * math.cos(phi)
    a = math.radians(north)
    x, z = east, -nth
    v = (math.cos(a) * x + math.sin(a) * z, up, -math.sin(a) * x + math.cos(a) * z)
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v)


def aim_moon(day, lat, phase_offset):
    """(hours, north offset): a full moon NORTH_TARGET's height up, turned to NORTH_TARGET's bearing."""
    phase = lambda h: ((day + h / 24.0) / 29.530588 + phase_offset) % 1.0  # noqa: E731
    want_up = NORTH_TARGET[1] / math.sqrt(sum(c * c for c in NORTH_TARGET))
    hours = min((h / 20.0 for h in range(18 * 20, 24 * 20)), key=lambda h: abs(moon_dir(h, day, lat, 0, phase(h))[1] - want_up))
    best = min((o / 2.0 for o in range(720)), key=lambda o: -sum(
        a * b for a, b in zip(moon_dir(hours, day, lat, o, phase(hours)), NORTH_TARGET)))
    return hours, best


class Hallway(k.Scene):
    def __init__(self, base, repo):
        super().__init__(base)
        self.repo = repo
        self.idx = k.model_index(repo, os.environ.get('TARTARUS_EXE'))  # default: build/Release
        self.plane = ref(repo, PLANE)
        self.mats = self._materials()
        self.rooms = {}        # 'Apartment 301/Bedroom' -> world rect (x0, z0, x1, z1)
        self.room_mat = {}     # same key -> (wall material, floor material)
        self.openings = {}     # (axis, line) -> [(centre, width, top, bottom, kind)]
        self.lines = {}        # (axis, line) -> [(a, b)] stretches of wall
        self.door_specs = []   # (unit, room key, axis, line, centre, sign into the room, interior?)
        self.window_specs = []
        self.sounds = self._door_sounds()

    # --- materials ---------------------------------------------------------------------------------------------
    def _materials(self):
        def ph(folder, stem):
            base = f'assets/Architecture/Building/Textures/{folder}/{stem}_'
            return {'albedo': base + 'Diffuse_2k.jpg', 'normal': base + 'nor_gl_2k.jpg', 'rough': base + 'Rough_2k.jpg',
                    'ao': base + 'AO_2k.jpg'}
        plaster = ph('PH_Beige_Wall', 'beige_wall_001')
        tiles = 'assets/Architecture/Building/Textures/PH_Interior_Tiles/interior_tiles_'
        tile = dict(albedo=tiles + 'diff_2k.jpg', normal=tiles + 'nor_gl_2k.jpg', rough=tiles + 'rough_2k.jpg',
                    ao=tiles + 'ao_2k.jpg')
        ceil = 'assets/Architecture/Building/Textures/Ceiling_Default/Ceiling_Default_'
        r = self.repo
        return {
            # white plaster / drywall: the plaster's normal and roughness under a flat white (the Wall_* sets are UV
            # atlases with a baseboard band in them, which smears when tiled)
            'plaster': k.textured(r, normal=plaster['normal'], rough=plaster['rough'], base=(0.84, 0.83, 0.8),
                                  roughness=0.9, triplanar_scale=0.5),
            'tile_wall': k.textured(r, normal=plaster['normal'], rough=plaster['rough'], base=(0.88, 0.89, 0.88),
                                    roughness=0.45, triplanar_scale=0.5),
            # ~9 boards a tile: 0.9 tiles a metre gives 12 cm boards
            'planks': k.textured(r, **ph('PH_Dark_Wooden_Planks', 'dark_wooden_planks'), base=(1.25, 1.2, 1.15),
                                 triplanar_scale=0.9),
            'tile': k.textured(r, **tile, roughness=1.0, triplanar_scale=1 / 1.9),
            'ceiling': k.textured(r, albedo=ceil + 'Base_Color.png', normal=ceil + 'Normal.png', base=(0.95, 0.95, 0.95),
                                  roughness=0.95, triplanar_scale=0.33),
            'trim': k.textured(r, base=(0.86, 0.85, 0.82), roughness=0.4),
            'hidden': k.textured(r, base=(0.5, 0.5, 0.5), roughness=1.0),
                        'glass': {**k.from_mat(r, 'assets/Architecture/Building/Materials/M_Window_Light.mat'),
                      'baseColor': [0.92, 0.95, 0.97], 'emissiveColor': [0.0, 0.0, 0.0], 'emissiveStrength': 0.0,
                      'emissiveMap': '', 'metallic': 0.0, 'roughness': 0.03, 'transmission': 0.92, 'ior': 1.5,
                      'renderQueue': 2, 'opacity': 0.12},
        }

    def _door_sounds(self):
        """The door takes (Foley/door, CC0, in git): ';'-joined per kind."""
        return {kind: ';'.join(f'{DOOR_AUDIO}/{kind}_{i}.wav' for i in range(1, 5)) for kind in ('open', 'close', 'latch')}

    # --- primitives --------------------------------------------------------------------------------------------
    def face(self, name, parent, center, normal, w, h, mat):
        """A single-sided plane w x h centred at `center`, facing `normal` (+-X, +-Y or +-Z)."""
        nx, ny, nz = normal
        if ny:
            yaw, pitch = 0.0, -90.0 if ny > 0 else 90.0
        else:
            yaw, pitch = math.degrees(math.atan2(nx, nz)), 0.0
        e = self._common(name, parent, center, yaw, (round(w, 4), round(h, 4), 1.0), pitch)
        e.update({**self.plane, 'materials': [{'embedded': mat}], 'castShadows': 0, 'static': True})
        self.models.append(e)
        return e

    def solid(self, name, parent, center, size):
        """An invisible box that collides and casts shadows (the inside of a wall, the roof slab)."""
        return self.box(name, parent, center, size, self.mats['hidden'], castShadows=3, static=True)

    # --- plan ----------------------------------------------------------------------------------------------------
    def add_line(self, axis, line, a, b):
        self.lines.setdefault((axis, round(line, 4)), []).append((min(a, b), max(a, b)))

    def add_opening(self, axis, line, centre, width, top, bottom, kind):
        self.openings.setdefault((axis, round(line, 4)), []).append((centre, width, top, bottom, kind))

    def thick(self, axis, line):
        outside = (axis == 'x' and abs(abs(line) - ZOUT) < 1e-3) or (axis == 'z' and (abs(line) < 1e-3 or abs(line - L) < 1e-3))
        return TX if outside else T

    def plan(self):
        """Every room's world rectangle, wall line, door, arch and window."""
        self.rooms['Corridor'] = (0.0, -HALL, L, HALL)
        self.room_mat['Corridor'] = ('plaster', 'planks')
        for number, plan, x0, sx, row in PLACEMENT:
            p = UNITS[plan]

            def at(u, v):
                return x0 + sx * u, row * (HALL + v)

            def world_line(axis, line):  # unit (axis, line) -> world: 'u' walls run along x, 'v' walls along z
                return ('x', row * (HALL + line)) if axis == 'u' else ('z', x0 + sx * line)

            def world_along(axis, c):
                return x0 + sx * c if axis == 'u' else row * (HALL + c)

            for name, (u0, v0, u1, v1) in p['rooms'].items():
                (ax0, az0), (ax1, az1) = at(u0, v0), at(u1, v1)
                key = f'Apartment {number}/{name}'
                self.rooms[key] = (min(ax0, ax1), min(az0, az1), max(ax0, ax1), max(az0, az1))
                self.room_mat[key] = ('tile_wall', 'tile') if name == 'Bathroom' else ('plaster', 'planks')
            for room, axis, line, c, interior in p['doors']:
                wa, wl = world_line(axis, line)
                along = world_along(axis, c)
                rx0, rz0, rx1, rz1 = self.rooms[f'Apartment {number}/{room}']
                mid = (rz0 + rz1) / 2 if wa == 'x' else (rx0 + rx1) / 2
                self.add_opening(wa, wl, along, DOOR_W, DOOR_H, 0.0, 'door')
                self.door_specs.append((number, room, wa, wl, along, 1 if mid > wl else -1, interior))
            for axis, line, c, w, h in p['arches']:
                wa, wl = world_line(axis, line)
                self.add_opening(wa, wl, world_along(axis, c), w, h, 0.0, 'arch')
            for axis, line, c in p['windows']:
                wa, wl = world_line(axis, line)
                self.window_specs.append((wa, wl, world_along(axis, c)))
        self.window_specs += [(('x' if a == 'u' else 'z'), line, c) for a, line, c in HALL_WINDOWS]
        for wa, wl, c in self.window_specs:
            self.add_opening(wa, wl, c, WIN_W - 2 * WIN_FRAME, WIN_SILL + WIN_H - WIN_FRAME, WIN_SILL + WIN_FRAME, 'window')
        for x0, z0, x1, z1 in self.rooms.values():
            self.add_line('x', z0, x0, x1)
            self.add_line('x', z1, x0, x1)
            self.add_line('z', x0, z0, z1)
            self.add_line('z', x1, z0, z1)

    # --- shell ---------------------------------------------------------------------------------------------------
    def cut(self, axis, line, a, b, y0=0.0, y1=H):
        """The solid pieces (a, b, bottom, top) of a stretch of wall once its openings are cut out."""
        pieces, pos = [], a
        for oc, ow, top, bot, _ in sorted(o for o in self.openings.get((axis, round(line, 4)), []) if a < o[0] < b):
            pieces.append((pos, oc - ow / 2, y0, y1))
            pieces.append((oc - ow / 2, oc + ow / 2, top, y1))
            if bot > 0.0:
                pieces.append((oc - ow / 2, oc + ow / 2, y0, bot))
            pos = oc + ow / 2
        pieces.append((pos, b, y0, y1))
        return [p for p in pieces if p[1] - p[0] > 1e-3 and p[3] - p[2] > 1e-3]

    def shell(self, root):
        walls = self.empty('Walls', root)
        solids = self.empty('Wall Solids (shadows + collision)', root)
        floors = self.empty('Floors', root)
        ceilings = self.empty('Ceilings', root)
        trim = self.empty('Trim', root)
        # inside each wall: one box per merged stretch, cut round its openings, overlapping the crossing walls
        for (axis, line), spans in self.lines.items():
            spans = sorted(spans)
            merged = [list(spans[0])]
            for a, b in spans[1:]:
                if a <= merged[-1][1] + 1e-3:
                    merged[-1][1] = max(merged[-1][1], b)
                else:
                    merged.append([a, b])
            t = self.thick(axis, line)
            for a, b in merged:
                for p0, p1, y0, y1 in self.cut(axis, line, a - t / 2, b + t / 2):
                    c = ((p0 + p1) / 2, (y0 + y1) / 2)
                    if axis == 'x':
                        self.solid('Wall', solids, (c[0], c[1], line), (p1 - p0, y1 - y0, t))
                    else:
                        self.solid('Wall', solids, (line, c[1], c[0]), (t, y1 - y0, p1 - p0))
        # the floor slab and the roof slab over everything
        self.solid('Roof Slab', solids, (L / 2, H + 0.15, 0.0), (L + 2 * TX, 0.3, 2 * ZOUT + TX))
        # The slab is what you walk on: one box, its top just under the floors, so there's no seam to catch on at a
        # doorway (the floors themselves don't collide).
        self.solid('Floor Slab', solids, (L / 2, -0.155, 0.0), (L + 2 * TX, 0.3, 2 * ZOUT + TX))
        # the floor through each door and arch, across the wall's thickness (the rooms' floors stop at the wall faces);
        # a door's in the floor of the room it swings into
        door_floor = {(wa, round(wl, 4), round(c, 4)): self.room_mat[f'Apartment {n}/{room}'][1]
                      for n, room, wa, wl, c, _, _ in self.door_specs}
        for (axis, line), ops in self.openings.items():
            t = self.thick(axis, line)
            for oc, ow, top, bot, kind in ops:
                if bot > 0.0:
                    continue
                mat = self.mats[door_floor.get((axis, line, round(oc, 4)), 'planks')]
                c, size = ((oc, -0.05, line), (ow, 0.1, t)) if axis == 'x' else ((line, -0.05, oc), (t, 0.1, ow))
                self.box('Floor - Threshold', floors, c, size, mat, collider=False, static=True, castShadows=0)
        # each room's own faces: floor, ceiling and four walls, half a wall in from the centre lines
        for key, (x0, z0, x1, z1) in self.rooms.items():
            wall_mat, floor_mat = (self.mats[m] for m in self.room_mat[key])
            ix0, ix1 = x0 + self.thick('z', x0) / 2, x1 - self.thick('z', x1) / 2
            iz0, iz1 = z0 + self.thick('x', z0) / 2, z1 - self.thick('x', z1) / 2
            label = key.replace('/', ' - ')
            self.box(f'Floor - {label}', floors, ((ix0 + ix1) / 2, -0.05, (iz0 + iz1) / 2), (ix1 - ix0, 0.1, iz1 - iz0),
                     self.mats[self.room_mat[key][1]], collider=False, static=True, castShadows=0)
            self.face(f'Ceiling - {label}', ceilings, ((ix0 + ix1) / 2, H, (iz0 + iz1) / 2), (0, -1, 0), ix1 - ix0,
                      iz1 - iz0, self.mats['ceiling'])
            for axis, line, a, b, inward, normal in (
                    ('x', z0, ix0, ix1, iz0, (0, 0, 1)), ('x', z1, ix0, ix1, iz1, (0, 0, -1)),
                    ('z', x0, iz0, iz1, ix0, (1, 0, 0)), ('z', x1, iz0, iz1, ix1, (-1, 0, 0))):
                for p0, p1, y0, y1 in self.cut(axis, line, a, b):
                    mid = ((p0 + p1) / 2, (y0 + y1) / 2)
                    c = (mid[0], mid[1], inward) if axis == 'x' else (inward, mid[1], mid[0])
                    self.face(f'Wall - {label}', walls, c, normal, p1 - p0, y1 - y0, wall_mat)
                self.reveals(walls, axis, line, a, b, inward, normal, wall_mat)
                self.baseboard(trim, axis, line, a, b, inward, normal)

    def reveals(self, parent, axis, line, a, b, inward, normal, mat):
        """The sides of each window and arch opening, from this room's face to the wall's centre line (the room on
        the other side does its own half)."""
        depth = abs(inward - line)
        mid_d = (inward + line) / 2
        for oc, ow, top, bot, kind in self.openings.get((axis, round(line, 4)), []):
            if kind == 'door' or not a < oc < b:
                continue
            h = top - bot
            for sgn in (-1, 1):  # jambs, facing into the opening
                p = oc + sgn * ow / 2
                n = (-sgn, 0, 0) if axis == 'x' else (0, 0, -sgn)
                c = (p, bot + h / 2, mid_d) if axis == 'x' else (mid_d, bot + h / 2, p)
                self.face('Reveal', parent, c, n, depth, h, mat)
            c = (oc, top, mid_d) if axis == 'x' else (mid_d, top, oc)
            self.face('Reveal Head', parent, c, (0, -1, 0), ow if axis == 'x' else depth, depth if axis == 'x' else ow, mat)
            if bot > 0.0:
                c = (oc, bot, mid_d) if axis == 'x' else (mid_d, bot, oc)
                self.face('Reveal Sill', parent, c, (0, 1, 0), ow if axis == 'x' else depth, depth if axis == 'x' else ow, mat)

    def baseboard(self, parent, axis, line, a, b, inward, normal):
        """A 9 cm baseboard, 1.2 cm proud, broken at the doors and arches."""
        spans, pos = [], a
        for oc, ow, top, bot, kind in sorted(self.openings.get((axis, round(line, 4)), [])):
            if bot == 0.0 and a < oc < b:
                spans.append((pos, oc - ow / 2 - (0.03 if kind == 'door' else 0.0)))
                pos = oc + ow / 2 + (0.03 if kind == 'door' else 0.0)
        spans.append((pos, b))
        sgn = normal[2] if axis == 'x' else normal[0]
        d = inward + sgn * 0.006
        for s0, s1 in spans:
            if s1 - s0 < 0.05:
                continue
            c = ((s0 + s1) / 2, 0.045, d) if axis == 'x' else (d, 0.045, (s0 + s1) / 2)
            size = (s1 - s0, 0.09, 0.012) if axis == 'x' else (0.012, 0.09, s1 - s0)
            self.box('Baseboard', parent, c, size, self.mats['trim'], collider=False, static=True)

    # --- doors and windows ------------------------------------------------------------------------------------------
    def doors(self, root):
        grp = self.empty('Doors', root)
        for number, room, axis, line, along, sign, interior in self.door_specs:
            # Door_Op_1 is modelled with its inside (the chain, the door catch, the hinge knuckles, the inner trim) on its
            # back, local -Z: that faces the room it swings into, and it swings that way (Swing Back). The frame sits on
            # the wall's centre line.
            if axis == 'x':
                yaw, wall_pt = (180.0 if sign > 0 else 0.0), (along, line)
            else:
                yaw, wall_pt = (-90.0 if sign > 0 else 90.0), (line, along)
            fz = (math.sin(math.radians(yaw)), math.cos(math.radians(yaw)))
            ox, oz = wall_pt[0] - fz[0] * DOOR_WALL_Z, wall_pt[1] - fz[1] * DOOR_WALL_Z
            suffix = '_Interior' if interior else ''
            parent = self.empty(f'{number} {room} Door' if interior else f'{number} Front Door', grp)
            self.model(self.idx, 'Door_Op_1_Frame' + suffix, parent, (ox, 0.0, oz), yaw, name='Frame', static=True)
            self.model(self.idx, 'Door_Op_1_Leaf' + suffix, parent, (ox, 0.0, oz), yaw, name='Leaf', **{
                'collider': {'isTrigger': False, 'halfExtents': [0.4, 0.995, 0.024], 'center': [-0.005, 1.004, 0.0],
                             'friction': 0.5},
                'Rigidbody': {'Is Kinematic': True, 'Use Gravity': False, 'Mass': 18.0, 'Interpolate': 'Interpolate'},
                'Openable': {'Motion': 'Hinge Left', 'Amount': 95.0, 'Open Time': 0.75, 'Start Open': False, 'Swing Back': True,
                             'Open Sound': self.sounds['open'], 'Close Sound': self.sounds['close'],
                             'Latch Sound': self.sounds['latch']}})
            if not interior:
                self.number_plate(parent, number, axis, line, along, -sign)

    def number_plate(self, parent, number, axis, line, along, toward):
        """The unit number on the corridor wall beside the latch side, at 1.55 m."""
        face = line + toward * T / 2
        digits = str(number)
        step = 0.062
        for i, ch in enumerate(digits):
            # read left to right from the corridor: facing a north wall (toward +Z) that's +X, a south one -X
            off = DOOR_W / 2 + 0.16 + i * step
            if axis == 'x':
                yaw, pos = (0.0 if toward > 0 else 180.0), (along + (off if toward > 0 else -off), 1.5, face + toward * 0.004)
            else:
                yaw, pos = (90.0 if toward > 0 else -90.0), (face + toward * 0.004, 1.5, along + (-off if toward > 0 else off))
            self.model(self.idx, f'Door_Number_{ch}', parent, pos, yaw, name=f'Number {ch}', static=True, castShadows=0)

    def windows(self, root):
        grp = self.empty('Windows', root)
        for axis, line, along in self.window_specs:
            # the window's front (+Z) faces the inside of the building
            if axis == 'x':
                yaw, pos = (0.0 if line < 0 else 180.0), (along, WIN_SILL, line)
            else:
                yaw, pos = (90.0 if line < 1e-3 else -90.0), (line, WIN_SILL, along)
            self.model(self.idx, WIN_STEM, grp, pos, yaw, name='Window', scale=WIN_SCALE, castShadows=0, static=True,
                       materials=[{'embedded': self.mats['glass']}, None])
            # The glass keeps the whole window out of the shadow passes, so its bars cast through invisible stand-ins:
            # the centre mullion and the two transoms (thirds of the pane), on the wall's centre line.
            ow, bot, top = WIN_W - 2 * WIN_FRAME, WIN_SILL + WIN_FRAME, WIN_SILL + WIN_H - WIN_FRAME
            bar = 0.035
            for name, a, y, w, h in (('Mullion', along, (bot + top) / 2, bar, top - bot),
                                     ('Transom', along, bot + (top - bot) / 3, ow, bar),
                                     ('Transom', along, bot + 2 * (top - bot) / 3, ow, bar)):
                if axis == 'x':
                    self.box(f'Window {name} (shadow)', grp, (a, y, line), (w, h, 0.03), self.mats['hidden'], collider=False,
                             castShadows=3, static=True)
                else:
                    self.box(f'Window {name} (shadow)', grp, (line, y, a), (0.03, h, w), self.mats['hidden'], collider=False,
                             castShadows=3, static=True)
            # an interior sill board, 3.5 cm proud
            inward = 1 if (axis == 'x' and line < 0) or (axis == 'z' and line < 1e-3) else -1
            t = self.thick(axis, line)
            ow = WIN_W - 2 * WIN_FRAME
            d0, d1 = line, line + inward * (t / 2 + 0.035)
            y = WIN_SILL + WIN_FRAME - 0.0125
            if axis == 'x':
                c, s = (along, y, (d0 + d1) / 2), (ow + 0.12, 0.025, abs(d1 - d0))
            else:
                c, s = ((d0 + d1) / 2, y, along), (abs(d1 - d0), 0.025, ow + 0.12)
            self.box('Sill', grp, c, s, self.mats['trim'], collider=False, static=True)

    # --- lighting ----------------------------------------------------------------------------------------------------
    def lamp_glow(self, stem, strength):
        meta = json.load(open(os.path.join(self.repo, 'project', self.idx[stem]['path'] + '.meta'), encoding='utf-8'))
        mat = next(iter(meta['materialRemap'].values()))
        over = {'emissive_strength': strength}
        if strength == 0:
            over['emissiveColor'] = [0.0, 0.0, 0.0]
        return [{'embedded': k.from_mat(self.repo, mat, **over)}] * self.idx[stem]['meshes']

    def fixture(self, parent, stem, pos, yaw, kelvin, intensity, rng, glow, flicker=None, fill_room=None):
        """A ceiling fixture and, unless it's dead (intensity 0), its bulb just under it plus a faint shadowless fill
        lower down standing in for the bounce light this renderer doesn't compute."""
        x, z = pos
        drop = -self.idx[stem]['min'][1]
        self.model(self.idx, stem, parent, (x, H, z), yaw, name=stem, static=True, castShadows=0,
                   materials=self.lamp_glow(stem, glow if intensity else 0.0))
        if not intensity:
            return
        extra = k.light_flicker(**flicker) if flicker else {}
        # a ceiling fitting lights the room below it: a wide spot aimed straight down (one shadow view, not a cube's six)
        self.light(f'{stem} Light', parent, (x, H - drop - 0.06, z), intensity, rng, shadows=True, kelvin=kelvin,
                   spot=72.0, softness=1.1, **extra)
        # a tube throws light sideways too: a small shadowless glow lights the ceiling round the fitting
        if stem.startswith('Lamp_Fluorescent'):
            self.light(f'{stem} Ceiling Glow', parent, (x, H - drop - 0.15, z), intensity * 0.3, 2.4, kelvin=kelvin, **extra)
        if fill_room:
            x0, z0, x1, z1 = fill_room
            fx, fz = (x + (x0 + x1) / 2) / 2, (z + (z0 + z1) / 2) / 2
            far = max(math.dist((fx, 1.3, fz), (cx, cy, cz)) for cx in (x0, x1) for cy in (0.0, H) for cz in (z0, z1))
            self.light(f'{stem} Bounce', parent, (fx, 1.3, fz), intensity * 0.12, round(far * 1.2, 3), kelvin=kelvin,
                       **extra)

    def lighting(self, root):
        grp = self.empty('Lighting', root)
        # the corridor: six tube fittings down the middle, one dead, two flickering, all dim
        xs = [L * (i + 0.5) / 6 for i in range(6)]
        for i, x in enumerate(xs):
            dead = i == 4
            flicker = {1: dict(Mode=0, MinInterval=3.0, MaxInterval=9.0, Depth=1.0, Seed=11),
                       3: dict(Mode=0, MinInterval=0.6, MaxInterval=4.0, Depth=0.7, Seed=23)}.get(i)
            self.fixture(grp, 'Lamp_Fluorescent_A', (x, 0.0), 90.0, TUBE_K, 0.0 if dead else 0.9, 5.5, 0.9, flicker)
        # moonlight bouncing off the floors: a faint, cool, shadowless fill in every room but the corridor
        # (only where the moon doesn't reach: the north rooms; the south ones get it straight through the glass), its
        # reach kept inside the room - shadowless, it would light the next room through the wall
        for key, (x0, z0, x1, z1) in self.rooms.items():
            if key == 'Corridor' or z0 > 0.0:
                continue
            reach = math.dist((x0, z0), (x1, z1)) / 2
            self.light(f'Moon Bounce - {key}', grp, ((x0 + x1) / 2, 1.4, (z0 + z1) / 2), 0.07, round(reach, 3),
                       color=(0.55, 0.66, 0.95))
        # a bulb in every entry; 305's is on its last legs
        for number, *_ in PLACEMENT:
            x0, z0, x1, z1 = self.rooms[f'Apartment {number}/Entry']
            flicker = dict(Mode=2, MinInterval=0.2, MaxInterval=1.5, Depth=0.55, Seed=number) if number == 305 else None
            self.fixture(grp, 'Lamp_Ceiling_A', ((x0 + x1) / 2, (z0 + z1) / 2), 0.0, BULB_K, 0.7, 4.5, 0.7, flicker,
                         fill_room=(x0, z0, x1, z1))
        # the test room (test_npcs): 301's living room, lit properly from a ceiling fitting's spot
        x0, z0, x1, z1 = self.rooms[TEST_ROOM]
        self.fixture(grp, 'Lamp_Ceiling_B', ((x0 + x1) / 2, (z0 + z1) / 2), 0.0, BULB_K, 2.2, 6.5, 1.0,
                     fill_room=(x0, z0, x1, z1))

    def test_npcs(self, root, arena):
        """Two training dummies in TEST_ROOM for trying the wounded animations: they never shoot, and a leg or body
        round that would drop one under 20% puts it down (crawling or staggering). One naked with an AK (to see the
        gun fall), one dressed and unarmed. They come back 3 s after dying."""
        x0, z0, x1, z1 = self.rooms[TEST_ROOM]
        squad = next(e['Squad Settings'] for e in arena['empties'] if 'Squad Settings' in e)
        self.empty('Test Squad Settings', root, ((x0 + x1) / 2, 0.0, z0 + 0.5), **{'Squad Settings': {
            **squad, 'Squad Size': 2, 'Respawn': True, 'Respawn Delay': 3.0}})
        # standing in the room's far half, side by side, facing the archway the player walks in through
        for name, u, spawn in (('Test NPC - Naked', 0.3, {'Weapon': 'AKS-74U', 'Bare': True}),
                               ('Test NPC - Unarmed', 0.7, {'Weapon': 'None', 'Bare': False})):
            pos = (x0 + (x1 - x0) * u, 0.0, z0 + (z1 - z0) * 0.62)
            self.empty(name, root, pos, yaw=180.0 if z0 > 0 else 0.0, **{'NPC Spawn': {
                'Brain': 'Training Dummy', 'Outfit Seed': 1, 'Skill': 0.5, 'Squad': 0, **spawn}})

    # --- audio ----------------------------------------------------------------------------------------------------------
    def audio(self, root):
        grp = self.empty('Audio', root)
        zone = lambda name, rect, ir, amb, vol, prio: self.empty(name, grp, ((rect[0] + rect[2]) / 2, H / 2, (rect[1] + rect[3]) / 2), **{  # noqa: E731
            'Reverb Zone': {'Enabled': True, 'Shape': 'Box', 'Extents': [(rect[2] - rect[0]) / 2 + 0.2, H / 2 + 0.3,
                                                                          (rect[3] - rect[1]) / 2 + 0.2],
                            'Radius': 6.0, 'Priority': prio, 'Fade Distance': 0.3, 'Tail Class': 'Indoor Small',
                            'Tail Gain': 0.4, 'Reverb Mode': 'Custom', 'IR': ir, 'Wet dB': -6.0, 'Pre-Delay ms': 3.0,
                            'HF Damping dB': 2.0, 'Low Cut (Hz)': 100.0, 'Ambience': amb, 'Ambience Volume': vol}})
        # the corridor: hard and bare, a longer tail; the units: empty rooms, a little livelier than a furnished flat
        zone('Corridor Reverb', self.rooms['Corridor'], 'assets/Audio/IR/apartment_3.wav', 'snd.amb.indoor_small', 0.3, 2)
        for number, plan, x0, sx, row in PLACEMENT:
            rects = [r for key, r in self.rooms.items() if key.startswith(f'Apartment {number}/')]
            rect = (min(r[0] for r in rects), min(r[1] for r in rects), max(r[2] for r in rects), max(r[3] for r in rects))
            # the night through the windows, faint
            zone(f'Apartment {number} Reverb', rect, 'assets/Audio/IR/apartment_2.wav', 'snd.amb.night_open', 0.18, 1)
        for i, x in enumerate([L * (j + 0.5) / 6 for j in range(6)]):
            if i == 4:
                continue  # the dead tube is silent
            self.empty(f'Tube Hum {i + 1}', grp, (x, H - 0.1, 0.0), **{'Audio Source': {
                'Clip': 'assets/Audio/Ambience/worklight_hum_1.wav', 'Volume': 0.12, 'Loop': True, 'Play On Start': True,
                'Output': 'Ambient', '3D Sound': True, 'Volume Rolloff': 'Linear', 'Min Distance': 0.6,
                'Max Distance': 5.0, 'Doppler Level': 0.0}})
        self.empty('Foley', grp, (0, 0, 0), **{'Foley Audio': {'Footwear': 'Boots', 'NPC Footwear': 'Boots'}})

    # --- player -------------------------------------------------------------------------------------------------------------
    def player(self, arena):
        """Arena's player (controller, AKS-74U + Remington 870, first-person body) at the corridor's west end, facing
        east down it: bare chest, black classic pants and shoes, a black balaclava."""
        r = self.repo
        spawn = next(e for e in arena['empties'] if 'First Person Controller' in e)
        body = next(e for e in arena['empties'] if e.get('parentId') == spawn['id'] and 'First Person Body' in e)
        pieces = [e for e in arena['models'] if e.get('parentId') == body['id']
                  and e.get('Outfit Piece', {}).get('Slot') in {'Torso', 'Head', 'Arms'}]
        anim = pieces[0]['Animator Controller']
        ctrl = {kk: v for kk, v in spawn.items() if kk[0].isupper()}
        fpc = ctrl['First Person Controller']
        fpc['Primary Weapon Prefab'] = 'assets/Weapons/AKS74U/AKS74U.prefab'
        fpc['Secondary Weapon Prefab'] = 'assets/Weapons/Remington870/Remington870.prefab'
        # Arena's 0.3 m capsule (0.7 m wide with PhysX's contact offset) scrapes the 0.87 m a door leaves open; 0.22
        # is a person's shoulders seen from above
        fpc['Capsule Radius'] = 0.22
        sid = self.empty('Player Spawn', -1, (1.0 - L / 2, 0.0, 0.0), yaw=-90.0, **ctrl)  # facing +X
        bid = self.empty('Player Body', sid, tuple(body['position']), **{kk: v for kk, v in body.items() if kk[0].isupper()})
        for p in pieces:
            e = self._common(p['name'], bid, tuple(p['position']), 0.0)
            e['rotation'] = p['rotation']
            e.update({kk: v for kk, v in p.items() if kk in ('path', 'pathGuid', 'materials', 'Outfit Piece', 'Animator Controller')})
            self.models.append(e)
        body_mat = 'assets/Characters/Quantum/Materials/Characters/M_Quantum_Body.mat'
        black = 'assets/Characters/Quantum/Materials/Clothing/Pants/Pants Classic/M_Pants_Classic_Black.mat'
        clothes = 'assets/Characters/Quantum/Models/Clothing/Male/'
        for slot, path, mat, flags, skinned in (
                ('Legs', 'assets/Characters/Quantum/Models/European/Quantum_Legs.fbx', body_mat, 1, True),
                ('Feet', clothes + 'Shoes/Quantum_Feet_Shoes.fbx', body_mat, 1, True),
                ('Pants', clothes + 'Pants/SKM_Pants_Classic.fbx', black, 0, True),
                ('Shoes', clothes + 'Shoes/SKM_Shoes_Classic.fbx', black, 0, True),
                ('Balaclava', clothes + 'Balaclava/SM_Balaclava_Crime.fbx',
                 'assets/Characters/Quantum/Materials/Clothing/Balaclava/M_Balaclava_Crime_Black.mat', 2, False)):
            m = ref(r, path)
            e = self._common(slot, bid, (0.0, 0.0, 0.0), 0.0)
            e.update({**m, 'materials': [{'guid': ref(r, mat)['pathGuid'], 'path': mat}],
                      'Outfit Piece': {'Flags': flags, 'Item': m, 'Slot': slot}})
            if skinned:
                e['Animator Controller'] = anim
            self.models.append(e)


def build(repo):
    with open(os.path.join(repo, 'project', 'scenes', 'Arena.json'), encoding='utf-8') as f:
        arena = json.load(f)
    s = Hallway(arena, repo)
    d = s.data
    # --- Look: physical night sky with a big full moon, aimed into the south apartments and down the corridor -------
    day, lat = 355.0, 38.0
    hours0 = 21.0
    phase_offset = (0.5 - ((day + hours0 / 24.0) / 29.530588 % 1.0)) % 1.0
    hours, north = aim_moon(day, lat, phase_offset)
    d['skySource'] = 2
    d['sky'].update({'timeOfDay': True, 'timeOfDayHours': round(hours, 3), 'dayOfYear': day, 'latitude': lat,
                     'northOffset': north, 'moon': True, 'moonDiscScale': 20.0, 'moonBrightness': 0.09,
                     'moonPhaseOffset': round(phase_offset, 4), 'starsBrightness': 2.5, 'nightGlow': 0.02, 'skyIntensity': 0.15, 'groundAlbedo': [0.04, 0.04, 0.045],
                     'nightBrightness': 5.0, 'clouds': False, 'cloudShadows': False, 'cirrusCoverage': 0.0,
                     'animateInEditor': False, 'dayLengthMinutes': 0.0})
    d.update({'exposureEV': 1.8, 'autoExposure': False, 'tonemapOperator': 1, 'skyAmbientIntensity': 0.08,
              'bloomEnabled': True, 'bloomThreshold': 1.0, 'bloomKnee': 0.5, 'bloomIntensity': 0.12,
              'fogEnabled': False, 'ssaoEnabled': True, 'ssaoIntensity': 1.25, 'ssaoRadius': 0.45,
              'shadowsEnabled': True, 'shadowResolution': 4096, 'shadowCascades': 3, 'shadowDistance': 60.0,
              'maxSpotShadows': 12, 'maxPointShadows': 0, 'vignetteIntensity': 0.3, 'vignetteSmoothness': 0.55,
              'filmGrain': 0.0, 'motionBlur': False, 'depthOfField': False, 'chromaticAberration': 0.0,
              'gradeContrast': 6.0, 'gradeSaturation': -6.0, 'gradeTemperature': 0.0})
    d['skyHdriPath'] = ''
    d.pop('skyHdriGuid', None)
    s.empty('Moon', -1, (0, 30, 0), pitch=-25.0, yaw=-120.0, Light={
        'Type': 'Directional', 'Color': [0.78, 0.86, 1.0], 'Intensity': 0.8, 'Range': 200.0, 'Spot Angle': 35.0,
        'Angular Size': 0.53, 'Cast Shadows': True, 'Shadow Bias': 1.0, 'Shadow Normal Bias': 1.0,
        'Shadow Softness': 0.05, 'Shadow Resolution': 'Follow global', 'Shadow Update Mode': 'Dynamic', 'ColorTempK': 0.0,
        'Culling Mask': -1})
    s.plan()
    # centred on the origin (the editor's default and smoke-test cameras orbit it); the plan stays in corridor coordinates
    root = s.empty('Third Floor', -1, (-L / 2, 0.0, 0.0))
    s.shell(root)
    s.doors(root)
    s.windows(root)
    s.lighting(root)
    s.test_npcs(root, arena)
    s.audio(root)
    s.player(arena)
    return s


def write_meta(path):
    meta = path + '.meta'
    if os.path.exists(meta):
        return  # keep the guid: settings.json points at it
    with open(meta, 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'guid': secrets.token_hex(8), 'metaVersion': 1, 'type': 'scene'}, f, indent=2)
        f.write('\n')


def main():
    repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..'))
    s = build(repo)
    out = os.path.join(repo, 'project', 'scenes', 'Apartment Hallway.json')
    s.write(out)
    write_meta(out)
    print(f'{out}: {len(s.boxes)} boxes, {len(s.models)} models, {len(s.empties)} empties; corridor {L:.1f} m; '
          f'moon {s.data["sky"]["timeOfDayHours"]} h, north offset {s.data["sky"]["northOffset"]}')


if __name__ == '__main__':
    main()
