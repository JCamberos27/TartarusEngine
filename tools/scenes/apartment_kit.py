# The apartment kit behind tools/scenes/build_apartment.py: one three-bedroom apartment plan (master with ensuite,
# two more bedrooms, living room, kitchen + dining, bathroom, laundry, hall), the Builder that lays its shell, doors,
# furniture and lights out from the imported model library, and the layout checks. Who lives in each apartment (what
# goes in the rooms) is in tools/scenes/households/.
#
# Plan coordinates are metres: X runs east, Z runs south, the floor is y = 0. Walls are listed by centre line and
# built as two half-thickness boxes so each side carries its own room's paint. Every model is placed by its measured
# bounds (scene_kit.model_index), so a placement names where the piece's footprint goes, not where its pivot is.
#
# An apartment can be mirrored in X and / or Z on the floor (Builder.xf): households are written in plan
# coordinates, and the Builder mirrors positions and turns, but never a model itself (a door or a shower stall stays
# the way it was made; handed pairs swap, HANDED).
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scene_kit as k  # noqa: E402
from openables import component, kit_parts  # noqa: E402  (tools/assets, on the path scene_kit set)

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

# Windows in the outside walls: (room, wall axis, wall line, centre along it, model, sill height). The model is a
# frame with a glowing pane (sizes measured); it sits on the wall's centre line in an opening a frame's width smaller,
# so the wall's own reveal shows around it.
WINDOWS = [
    ('Living Room', 'z', 0.0, 5.3, 'Window_Tall_Fake', 0.75),
    ('Living Room', 'z', 0.0, 8.9, 'Window_Tall_Fake', 0.75),
    ('Master Bedroom', 'x', 0.0, 1.45, 'Window_Tall_Fake', 0.75),
    ('Bedroom 2', 'x', 0.0, 7.45, 'Window_Fake', 0.95),
    ('Kids Bedroom', 'x', 0.0, 10.85, 'Window_Fake', 0.95),
]
WINDOW_SIZE = {'Window_Fake': (0.87, 0.87), 'Window_Tall_Fake': (0.87, 1.54)}
WINDOW_FRAME = 0.045
for _room, _axis, _c, _along, _stem, _sill in WINDOWS:
    _w, _h = WINDOW_SIZE[_stem]
    next(w for w in WALLS if w[0] == _axis and w[1] == _c and w[2] < _along < w[3])[4].append(
        (_along, _w - 2 * WINDOW_FRAME, _sill + _h - WINDOW_FRAME, _sill + WINDOW_FRAME))
# The moon, the same for the whole building: high in the north-west, so its light runs south-east and down.
MOON_DIR = (0.55, -0.62, 0.56)
MOON = (0.5, 0.6, 0.9)

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

# The floor (--floor): this apartment as 310 and three more of the same plan, on the corridor their front doors open
# onto. Each is mirrored so its bedrooms and living room keep outside walls (windows): east of the corridor in X,
# the second row in Z (living rooms back to back across a party wall, T thick).
CORRIDOR_W = 2.0
UNIT_W, UNIT_D = 13.0, 10.0 + T
FLOOR_ROWS = 2
FURNISHED = 310
CORRIDOR = (UNIT_W, 0.0, UNIT_W + CORRIDOR_W, FLOOR_ROWS * UNIT_D - T)
AREAS = {'Corridor': CORRIDOR}  # rooms outside the apartment plan, in world coordinates


def floor_units():
    """(number, (x, z, sx, sz)) of each apartment: where its plan's origin lands and its mirror signs. Even numbers
    down the west side, odd ones across the corridor."""
    units = []
    for row in range(FLOOR_ROWS):
        sz = 1 if row % 2 == 0 else -1
        oz = row * UNIT_D + (0.0 if sz > 0 else 10.0)
        units.append((FURNISHED + 2 * row, (0.0, oz, 1, sz)))
        units.append((FURNISHED + 1 + 2 * row, (2 * UNIT_W + CORRIDOR_W, oz, -1, sz)))
    return units


# Models made in left- and right-handed versions: a mirrored apartment uses the other one.
HANDED = [('_L_Assembled', '_R_Assembled'), ('_L', '_R'), ('_Door_A_L', '_Door_A_R'), ('_Door_B_L', '_Door_B_R')]

# Lights. The scene is dark: these are the few things switched on. Colours are warm incandescent unless noted.
WARM = (1.0, 0.72, 0.45)
LAMP = (1.0, 0.66, 0.38)
TUBE = (0.82, 0.95, 1.0)
TV = (0.55, 0.7, 1.0)
MODES = {'Fluorescent': 0, 'Tv': 1, 'Candle': 2}


def room_at(x, z):
    for name, (x0, z0, x1, z1) in ROOMS.items():
        if x0 < x < x1 and z0 < z < z1:
            return name
    return None


def inner(room):
    """The room's interior rectangle (wall faces)."""
    x0, z0, x1, z1 = ROOMS[room] if room in ROOMS else AREAS[room]
    return x0 + T / 2, z0 + T / 2, x1 - T / 2, z1 - T / 2


# Shelf boards (height of each board's top above the piece's bottom), measured on the models with a ray.
BOARDS = {'Bookshelf': [0.144, 0.952, 1.334, 1.717], 'Garage_Shelf_A': [0.142, 0.704, 1.272, 1.837],
          'Bathroom_Shelf_Unit_A': [0.226, 0.517, 0.804], 'Bathroom_Shelf_Unit_B': [0.157, 0.55, 0.94, 1.325],
          'Tv_Stand_Assembled': [0.144], 'Sidetable_C': [0.208],
          'Shelf_Standing_A': [0.157, 0.551, 0.941, 1.331, 1.76]}
# Pieces hung on or fixed to a wall (they don't need anything under them).
WALL_ITEMS = ('Painting', 'Poster', 'Clock_Wall', 'Light_Switches', 'Outlet', 'Mirror_Bathroom', 'Towel_Holder',
              'Towel_Hanging', 'Shelf', 'Cross', 'Electricbox', 'Shower', 'Toiletpaper_Single', 'Stove_Cooker_Hood',
              'Kitchen_Cabinet_B', 'Lamp_Ceiling', 'Lamp_Chandelier', 'Lamp_Fluorescent', 'Lamp_Wall', 'Window_')


class Builder:
    def __init__(self, repo, floor=False):
        self.repo = repo
        self.floor = floor
        self.xf = (0.0, 0.0, 1, 1)  # the apartment being built: its plan origin in the world, its mirror signs
        self.in_unit = True       # building an apartment's plan (False: the corridor, in world coordinates)
        self.unit = None          # its number
        self.wall_paint, self.floors = {}, {}
        self.door_open = {}       # door name -> degrees it's left open (else DOORS' own)
        self.front_doors = []     # (number, corridor wall side, door centre, number plate centre) along the corridor
        self.idx = k.model_index(repo)
        # scene-wide settings (sky, grade, ...) come from the last generated scene; build() sets the ones it owns
        base = json.load(open(os.path.join(repo, 'project', 'scenes', 'Apartment_3Bed.json'), encoding='utf-8'))
        self.s = k.Scene(base)
        self.d = self.s.data
        self.mats = self._materials()
        self.root = None
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
        # the other households' paint: a widow's dusty rose and cream, a study's olive, rental magnolia gone grey
        M.update({'wall_rose': paint((0.66, 0.53, 0.5)), 'wall_cream': paint((0.78, 0.72, 0.58)),
                  'wall_olive': paint((0.46, 0.47, 0.35)), 'wall_grey': paint((0.56, 0.56, 0.54)),
                  'wall_yellowed': paint((0.66, 0.61, 0.45)), 'wall_teal': paint((0.27, 0.42, 0.42))})
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
        M['screen'] = k.textured(self.repo, base=(0.02, 0.02, 0.03), roughness=0.15, emissive=(0.42, 0.56, 0.86),
                                 strength=0.75)
        # the corridor: tired landlord paint over a bare concrete floor
        M['wall_corridor'] = paint((0.56, 0.54, 0.44))
        M['wall_corridor_low'] = paint((0.33, 0.3, 0.24), 0.7)  # the darker lower half under the chair rail
        M['concrete'] = k.textured(self.repo, **tex('Floor_Concrete', 'Floor_Concrete'), triplanar_scale=0.33)
        return M

    def group(self, name, parent=None):
        if name not in self.groups:
            self.groups[name] = self.s.empty(name, self.root if parent is None else parent)
        return self.groups[name]

    # --- placement in the world --------------------------------------------------------------------------------
    def at(self, x, z):
        """A plan point of the apartment being built -> world (x, z)."""
        ox, oz, sx, sz = self.xf
        return ox + sx * x, oz + sz * z

    def local_of(self, x, z):
        """A world point -> the apartment's plan (mirroring is its own inverse)."""
        ox, oz, sx, sz = self.xf
        return sx * (x - ox), sz * (z - oz)

    def turn(self, yaw):
        """A plan yaw -> world: the direction it faces (local +Z: (sin, cos)), mirrored."""
        _, _, sx, sz = self.xf
        if sx > 0 and sz > 0:
            return yaw
        a = math.radians(yaw)
        return round(math.degrees(math.atan2(sx * math.sin(a), sz * math.cos(a))), 6)

    def hand(self, stem):
        """The model to use for `stem` in the apartment being built: its other-handed twin when it's mirrored."""
        _, _, sx, sz = self.xf
        if sx * sz > 0:
            return stem
        for a, b in HANDED:
            for x, y in ((a, b), (b, a)):
                if stem.endswith(x) and stem[:-len(x)] + y in self.idx:
                    return stem[:-len(x)] + y
        return stem

    def pos(self, p):
        x, z = self.at(p[0], p[2])
        return x, p[1], z

    def side_room(self, x, z):
        """The room a wall face at plan point (x, z) looks into: one of the plan's, the corridor or None (outside)."""
        room = room_at(x, z) if self.in_unit else None
        if room is None and self.floor:
            wx, wz = self.at(x, z)
            cx0, cz0, cx1, cz1 = CORRIDOR
            if cx0 < wx < cx1 and cz0 < wz < cz1:
                room = 'Corridor'
        return room

    # --- shell -----------------------------------------------------------------------------------------------
    def shell(self):
        shell = self.group('Shell')
        floors = self.s.empty('Floors', shell)
        for room, (x0, z0, x1, z1) in ROOMS.items():
            self.s.box(f'Floor - {room}', floors, self.pos(((x0 + x1) / 2, -0.05, (z0 + z1) / 2)),
                       (x1 - x0, 0.1, z1 - z0), self.mats[self.floors.get(room, 'hardwood')])
        self.s.box('Ceiling', shell, self.pos((6.5, H + 0.05, 5.0)), (13.0 + T, 0.1, 10.0 + T), self.mats['ceiling'],
                   castShadows=0)
        walls = self.s.empty('Walls', shell)
        trim = self.s.empty('Trim', shell)
        self.walls(WALLS, walls, trim)

    def walls(self, plan, walls, trim):
        for axis, c, a, b, openings in plan:
            # solid stretches along the wall: full height, the header above an opening, the wall under a window
            pieces = []
            pos = a - T / 2 - 0.01  # 1 cm past each end: walls meeting at a T or an L overlap instead of just touching
            for oc, ow, oh, *sill in sorted(openings):  # (centre, width, top[, sill height for a window])
                pieces.append((pos, oc - ow / 2, 0.0, H))
                pieces.append((oc - ow / 2, oc + ow / 2, oh, H))
                if sill:
                    pieces.append((oc - ow / 2, oc + ow / 2, 0.0, sill[0]))
                pos = oc + ow / 2
            # ...except where it would poke through the outer wall into the corridor: there it stops 1 cm inside it
            end = b + T / 2 + (-0.01 if self.floor and self.in_unit and axis == 'x' and b == UNIT_W else 0.01)
            pieces.append((pos, end, 0.0, H))
            mid = (a + b) / 2
            for side in (-1, 1):
                px, pz = (mid, c + side * T) if axis == 'x' else (c + side * T, mid)
                room = self.side_room(px, pz)
                mat = self.mats[self.wall_paint.get(room, 'wall_white')] if room else self.mats['exterior']
                off = c + side * T / 4
                for p0, p1, y0, y1 in pieces:
                    if p1 - p0 < 1e-3:
                        continue
                    # Seal the seams: every piece runs 5 cm into the floor slab / ceiling slab, and a header overlaps
                    # the jambs either side by 2 cm, set 1 mm back so the overlapping faces don't fight.
                    header, under = y0 > 0.0, y1 < H  # (under a window)
                    q0, q1 = (p0 - 0.02, p1 + 0.02) if header or under else (p0, p1)
                    r0, r1 = (y0 if header else -0.05), (y1 if under else y1 + 0.05)
                    thick, inset = (T / 2 - 0.002, 0.001) if header or under else (T / 2, 0.0)
                    o = off - side * inset
                    if axis == 'x':
                        center, size = ((q0 + q1) / 2, (r0 + r1) / 2, o), (q1 - q0, r1 - r0, thick)
                    else:
                        center, size = (o, (r0 + r1) / 2, (q0 + q1) / 2), (thick, r1 - r0, q1 - q0)
                    self.s.box(f'Wall {room or "Exterior"}', walls, self.pos(center), size, mat)
                    # (an apartment's corridor face runs on past its own walls' ends: corridor() trims it in one run)
                    if not room or (room == 'Corridor' and self.in_unit):
                        continue
                    # trim trimmed back from the wall's ends so it doesn't poke through the perpendicular wall
                    b0, b1 = p0 + (T if p0 <= a - T / 2 + 1e-6 else 0), p1 - (T if p1 >= b + T / 2 - 0.02 else 0)
                    if b1 - b0 < 0.05:
                        continue
                    # baseboard (9 cm, 1.2 cm proud) along the floor; crown (7 cm, 3 cm proud) under the ceiling
                    for ty, th, proud, name in ((0.045, 0.09, 0.012, 'Baseboard'), (H - 0.035, 0.07, 0.03, 'Crown')):
                        if (name == 'Baseboard' and y0 > 0.0) or (name == 'Crown' and y1 < H):
                            continue
                        to = c + side * (T / 2 + proud / 2)
                        if axis == 'x':
                            bc, bs = ((b0 + b1) / 2, ty, to), (b1 - b0, th, proud)
                        else:
                            bc, bs = (to, ty, (b0 + b1) / 2), (proud, th, b1 - b0)
                        self.s.box(name, trim, self.pos(bc), bs, self.mats['trim'], collider=False, static=True)

    def trim_run(self, parent, c, sign, a, b, openings):
        """Baseboard and crown along a wall running along Z at x = c, on its sign side (+1 = its east face), from
        z = a to b; the baseboard breaks at each opening (centre, width)."""
        for ty, th, proud, name in ((0.045, 0.09, 0.012, 'Baseboard'), (H - 0.035, 0.07, 0.03, 'Crown')):
            spans, pos = [], a
            for oc, ow in (sorted(openings) if name == 'Baseboard' else []):
                spans.append((pos, oc - ow / 2))
                pos = oc + ow / 2
            spans.append((pos, b))
            x = c + sign * (T / 2 + proud / 2)
            for s0, s1 in spans:
                self.s.box(name, parent, self.pos((x, ty, (s0 + s1) / 2)), (proud, th, s1 - s0), self.mats['trim'],
                           collider=False, static=True)

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
            opened = self.door_open.get(name, opened)
            # the front door stays shut unless there's a corridor beyond it
            self.door(grp, name, axis, c, along, swing, exterior, opened, fixed=exterior and not self.floor)

    def world_door(self, axis, c, along, swing):
        """A door's plan spec (wall axis, wall line, centre along it, swing side) -> world. The door is then built
        in the world as made, never mirrored: a mirrored apartment just has the other-handed door."""
        ox, oz, sx, sz = self.xf
        if axis == 'x':
            return axis, oz + sz * c, ox + sx * along, swing * sz
        return axis, ox + sx * c, oz + sz * along, swing * sx

    def door(self, grp, name, axis, c, along, swing, exterior, opened, fixed=False):
        yaw, (ox, oz) = self.door_frame(*self.world_door(axis, c, along, swing))
        frame = 'Door_Op_1_Frame' if exterior else 'Door_Op_1_Frame_Interior'
        leaf = 'Door_Op_1_Leaf' if exterior else 'Door_Op_1_Leaf_Interior'
        parent = self.s.empty(name, grp)
        self.s.model(self.idx, frame, parent, (ox, 0.0, oz), yaw, name='Frame', static=True)
        if fixed:  # nothing beyond it: it stays shut (a plain collider, no hinge)
            self.s.model(self.idx, leaf, parent, (ox, 0.0, oz), yaw, name='Leaf', static=True, collider={
                'isTrigger': False, 'halfExtents': [0.4, 0.995, 0.024], 'center': [-0.005, 1.004, 0.0]})
            return
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

    def latch_along(self, axis, c, along, swing):
        """Where a door's latch edge (door-local +x, away from the hinge) is along its wall, in the plan."""
        yaw, (ox, oz) = self.door_frame(*self.world_door(axis, c, along, swing))
        a = math.radians(yaw)
        latch = self.local_of(ox + 0.4 * math.cos(a), oz - 0.4 * math.sin(a))
        return latch[0] if axis == 'x' else latch[1]

    def latch_side(self, door_name, room, y=1.2):
        """The light switch on the room side of a door, beside its latch edge (the side away from the hinge)."""
        name, axis, c, along, swing, _, _ = next(d for d in DOORS if d[0] == door_name)
        t = self.latch_along(axis, c, along, swing)
        sign = 1 if t > along else -1
        t = along + sign * (DOOR_W / 2 + 0.17)
        x0, z0, x1, z1 = inner(room)
        lo, hi = (x0, x1) if axis == 'x' else (z0, z1)
        if not lo + 0.06 < t < hi - 0.06:  # the latch side is jammed in the corner: the hinge side
            t = along - sign * (DOOR_W / 2 + 0.17)
        rc = ((x0 + x1) / 2, (z0 + z1) / 2)
        if axis == 'x':
            side = 'N' if rc[1] > c else 'S'
        else:
            side = 'W' if rc[0] > c else 'E'
        self.switch(room, side, t, y)

    # --- placement ---------------------------------------------------------------------------------------------
    def wbox(self, stem, yaw, scale=1.0, pitch=0.0, roll=0.0):
        """The model's bounds rotated by yaw (and tipped by pitch / roll, scene_kit.euler_quat's order), relative to
        its pivot: (x0, x1, y0, y1, z0, z1)."""
        lo, hi = [v * scale for v in self.idx[stem]['min']], [v * scale for v in self.idx[stem]['max']]
        if pitch or roll:
            p, r, a = math.radians(pitch), math.radians(roll), math.radians(yaw)
            pts = []
            for cx in (lo[0], hi[0]):
                for cy in (lo[1], hi[1]):
                    for cz in (lo[2], hi[2]):
                        x, y = cx * math.cos(r) - cy * math.sin(r), cx * math.sin(r) + cy * math.cos(r)
                        y, z = y * math.cos(p) - cz * math.sin(p), y * math.sin(p) + cz * math.cos(p)
                        x, z = x * math.cos(a) + z * math.sin(a), -x * math.sin(a) + z * math.cos(a)
                        pts.append((x, y, z))
            return (min(q[0] for q in pts), max(q[0] for q in pts), min(q[1] for q in pts), max(q[1] for q in pts),
                    min(q[2] for q in pts), max(q[2] for q in pts))
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
        tip = {'pitch': extra.get('pitch', 0.0), 'roll': extra.get('roll', 0.0)}
        x0, x1, y0, y1, z0, z1 = self.wbox(stem, yaw, scale, **tip)
        px, py, pz = x - (x0 + x1) / 2, y - y0, z - (z0 + z1) / 2
        parent = self.group(room, self.group('Furniture'))
        extra = {'static': True, **extra}
        if not collide and y1 - y0 < 0.35 and 'castShadows' not in extra:
            extra['castShadows'] = 0  # a small loose prop: nothing to see in a shadow, and every shadow map draws it
        if collide:
            extra['collider'] = {'friction': 0.8, 'isTrigger': False, 'shape': 4}
        # in the world: the footprint's centre goes where the plan says, measured on the (mirrored) turn
        wstem, wyaw = self.hand(stem), self.turn(yaw)
        wx, wz = self.at(x, z)
        X0, X1, _, _, Z0, Z1 = self.wbox(wstem, wyaw, scale, **tip)
        body = self.s.model(self.idx, wstem, parent, (wx - (X0 + X1) / 2, py, wz - (Z0 + Z1) / 2), wyaw,
                            name=name or wstem, scale=scale, **extra)
        # its doors, drawers and lids (an assembled kit's parts): children at its origin, each opening in Play
        for part in kit_parts(wstem):
            shadows = {'castShadows': extra['castShadows']} if 'castShadows' in extra else {}
            self.s.model(self.idx, part['stem'], body['id'], (0.0, 0.0, 0.0), 0.0, name=part['stem'][len(wstem) + 1:],
                         Openable=component(part['motion']), **shadows)
        box = {'x0': px + x0, 'x1': px + x1, 'y0': y, 'y1': py + y1, 'z0': pz + z0, 'z1': pz + z1,
               'x': x, 'z': z, 'yaw': yaw, 'stem': stem, 'unit': self.unit, 'tipped': bool(tip['pitch'] or tip['roll'])}
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
        extra = k.light_flicker(**flicker) if flicker else {}
        if 'yaw' in kw:
            kw['yaw'] = self.turn(kw['yaw'])
        self.s.light(f'{room} - {name}', self.group('Lighting'), self.pos(pos), intensity, rng, color, shadows,
                     spot=spot, softness=1.6, **kw, **extra)
        if fill:
            # halfway to the room's middle and at chest height: a fill hugging the ceiling or a wall paints a hot
            # ellipse on it
            x0, z0, x1, z1 = inner(room)
            fx, fz = (pos[0] + (x0 + x1) / 2) / 2, (pos[2] + (z0 + z1) / 2) / 2
            # Its reach fits its own room (a little past the farthest corner): shadowless, it would otherwise light
            # the neighbouring rooms through the walls, and every pixel in reach pays for shading it.
            far = max(math.dist((fx, 1.3, fz), (x, y, z)) for x in (x0, x1) for y in (0.0, H) for z in (z0, z1))
            self.s.light(f'{room} - {name} (bounce)', self.group('Lighting'), self.pos((fx, 1.3, fz)), intensity * fill,
                         min(rng * 1.8, round(far * 1.3, 3)), color, False)

    def ceiling_fixture(self, room, stem, x, z, on=False, glow=0.6, **light):
        x0, x1, y0, y1, z0, z1 = self.wbox(stem, 0.0)
        drop = y1 - y0
        self.put(room, stem, x, z, 0.0, H - drop, **(self.lamp_glow(stem, glow) if on else self.lamp_off(stem)))
        if on:
            self.light(room, light.pop('name', 'Ceiling'), (x, H - drop - 0.05, z), **light)

    def table_lamp(self, room, base, stem, on, dx=0.0, dz=0.0, intensity=0.9, rng=3.5, glow=0.9, **kw):
        lamp = self.on(room, base, stem, dx, dz,
                       **({'castShadows': 0, **self.lamp_glow(stem, glow)} if on else self.lamp_off(stem)))
        if on:
            self.light(room, 'Lamp', ((lamp['x0'] + lamp['x1']) / 2, lamp['y1'] - 0.16, (lamp['z0'] + lamp['z1']) / 2),
                       intensity, rng, LAMP, **kw)
        return lamp

    def room_side(self, room, axis, c):
        """Which of the room's sides (N/S/W/E) is the wall on line c of this axis."""
        x0, z0, x1, z1 = inner(room)
        if axis == 'x':
            return 'N' if (z0 + z1) / 2 > c else 'S'
        return 'W' if (x0 + x1) / 2 > c else 'E'

    def windows(self, curtains=None, moonlit=(), boarded=(), glow=0.8, curtain_tint=None):
        """Every window of the plan (WINDOWS): curtains (room -> curtain model; a room left out has none), boards
        nailed over `boarded` rooms' windows, the moon through `moonlit` rooms' windows (see window())."""
        curtains = curtains or {}
        for room, axis, c, along, stem, sill in WINDOWS:
            self.window(room, axis, c, along, stem, sill, curtain=curtains.get(room), boarded=room in boarded,
                        moonlit=room in moonlit, glow=glow, curtain_tint=curtain_tint)

    def window(self, room, axis, c, along, stem, sill, curtain=None, boarded=False, moonlit=False, glow=0.8,
               curtain_tint=None):
        """A window in the opening its wall was built with: the pane glowing a dim night blue-grey, a sill board and
        casing, optionally curtains, boards nailed across it, and the moon shining in when it is on that side of the
        building."""
        glass = k.from_mat(self.repo, 'assets/Architecture/Building/Materials/M_Window_Light.mat',
                           emissiveColor=[0.32, 0.4, 0.55], emissive_strength=glow)
        trim = self.group('Windows', self.group('Shell'))
        w, h = WINDOW_SIZE[stem]
        side = self.room_side(room, axis, c)
        sign = {'N': 1, 'S': -1, 'W': 1, 'E': -1}[side]  # into the room
        x, z = (along, c) if axis == 'x' else (c, along)
        self.put(room, stem, x, z, self.SIDE_YAW[side], y=sill, name='Window', castShadows=0,
                 materials=[{'embedded': glass}, None])
        # a sill board (from the wall's middle to 3.5 cm proud of it) and a casing round the opening
        ow, top, bot = w - 2 * WINDOW_FRAME, sill + h - WINDOW_FRAME, sill + WINDOW_FRAME
        face = c + sign * T / 2
        for name, a, b, y0, y1, d0, d1 in (
                ('Sill', along - ow / 2 - 0.06, along + ow / 2 + 0.06, bot - 0.025, bot, c, face + sign * 0.035),
                ('Casing', along - ow / 2 - 0.06, along - ow / 2, bot, top + 0.06, face, face + sign * 0.015),
                ('Casing', along + ow / 2, along + ow / 2 + 0.06, bot, top + 0.06, face, face + sign * 0.015),
                ('Casing', along - ow / 2, along + ow / 2, top, top + 0.06, face, face + sign * 0.015)):
            lo, hi = min(d0, d1), max(d0, d1)
            if axis == 'x':
                center, size = ((a + b) / 2, (y0 + y1) / 2, (lo + hi) / 2), (b - a, y1 - y0, hi - lo)
            else:
                center, size = ((lo + hi) / 2, (y0 + y1) / 2, (a + b) / 2), (hi - lo, y1 - y0, b - a)
            self.s.box(name, trim, self.pos(center), size, self.mats['trim'], collider=False, static=True)
        if boarded:
            # planks nailed across the opening on the room side: stood on edge (roll), each a little askew (pitch)
            for i, (f, tilt) in enumerate(((0.2, 7.0), (0.5, -5.0), (0.8, 3.0))):
                d = c + sign * (T / 2 + 0.012)
                bx, bz = (along + (i - 1) * 0.03, d) if axis == 'x' else (d, along + (i - 1) * 0.03)
                self.put(room, 'Woodplank', bx, bz, self.SIDE_YAW[side] + 90.0, y=bot + f * (top - bot) - 0.05,
                         name='Board', wall=True, pitch=tilt, roll=90.0)
        if curtain:
            extra = {}
            if curtain_tint:
                meta = json.load(open(os.path.join(self.repo, 'project', self.idx[curtain]['path'] + '.meta'),
                                      encoding='utf-8'))
                mat = k.from_mat(self.repo, next(iter(meta['materialRemap'].values())), baseColor=curtain_tint)
                extra['materials'] = [{'embedded': mat}] * self.idx[curtain]['meshes']
            ch = self.idx[curtain]['max'][1] - self.idx[curtain]['min'][1]
            # full drapes hang from just under the crown, short ones from just above the window
            y = H - 0.08 - ch if ch > 2.0 else min(top + 0.12, H - 0.08) - ch
            self.wall(room, side, along, curtain, y=max(y, 0.0), gap=0.02, name='Curtains', castShadows=0, **extra)
        if moonlit:
            self.moonlight(room, axis, c, along, sign, bot, top)

    def moonlight(self, room, axis, c, along, sign, bot, top, intensity=14.0):
        """The moon through a window, if it is on that side of the building: a narrow shadowed spot from outside,
        aimed along MOON_DIR, so the window's shape lands on the floor."""
        ox, oz = self.at(0.0, 0.0)
        nx, nz = self.at(*((0.0, sign) if axis == 'x' else (sign, 0.0)))
        nx, nz = nx - ox, nz - oz   # into the room, in the world
        dx, dy, dz = MOON_DIR
        if dx * nx + dz * nz < 0.2:
            return
        wx, wz = self.at(*((along, c) if axis == 'x' else (c, along)))
        dist = 2.5
        pos = (wx - dx * dist, (bot + top) / 2 - dy * dist, wz - dz * dist)
        yaw = math.degrees(math.atan2(-dx, -dz))
        pitch = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
        self.s.light(f'{room} - Moonlight', self.group('Lighting'), pos, intensity, 10.0, MOON, True, spot=16.0,
                     pitch=pitch, yaw=yaw, softness=1.2)

    def switch(self, room, side, t, y=1.2):
        # the switch plate is modelled facing -X: turn it a further +90 so it faces into the room
        self.wall(room, side, t, 'Light_Switches_Single', y=y, gap=0.0, extra_yaw=90.0, name='Light Switch')

    def outlet(self, room, side, t, y=0.3, gfi=False):
        self.wall(room, side, t, 'Outlet_Gfi' if gfi else 'Outlet_Electrical', y=y, gap=0.0, extra_yaw=90.0,
                  name='Outlet')

    # --- validation ----------------------------------------------------------------------------------------------
    def validate(self):
        """Layout checks on the placed boxes, one apartment (or the corridor) at a time, in its plan coordinates.
        Returns the problems."""
        out = []
        for unit in dict.fromkeys(p['unit'] for p in self.placed):
            placed = [p for p in self.placed if p['unit'] == unit]
            supports = [p for p in self.supports if p.get('unit') == unit]
            tag = f'{unit} ' if unit is not None else ''
            out += [tag + line for line in self.validate_unit(placed, supports, plan=unit != 'Corridor')]
        return out

    def validate_unit(self, placed, supports, plan=True):
        """Anything poking out of its room or through the ceiling, solid furniture overlapping solid furniture, solid
        furniture in a door's swing, anything in a doorway, small props floating (nothing under them) and props sunk
        into each other."""
        out = []
        for p in placed:
            x0, z0, x1, z1 = inner(p['room'])
            if p['stem'].startswith('Window_'):  # (set into the wall)
                pass
            elif p['x0'] < x0 - 0.02 or p['x1'] > x1 + 0.02 or p['z0'] < z0 - 0.02 or p['z1'] > z1 + 0.02:
                out.append(f"{p['room']}: {p['stem']} pokes out of the room ({p['x0']:.2f}..{p['x1']:.2f}, "
                           f"{p['z0']:.2f}..{p['z1']:.2f})")
            if p['y1'] > H - 0.02 and 'Lamp' not in p['stem']:
                out.append(f"{p['room']}: {p['stem']} goes through the ceiling (top {p['y1']:.2f})")

        def overlap(a, b, m=0.02):
            return (min(a['x1'], b['x1']) - max(a['x0'], b['x0']) > m and min(a['z1'], b['z1']) - max(a['z0'], b['z0']) > m
                    and min(a['y1'], b['y1']) - max(a['y0'], b['y0']) > m)
        solid = [p for p in placed if p['collide'] or p['stem'].startswith('Curtains')]  # (drapes hang to the floor)
        tucks = ({'Table_Dining', 'Chair_Dining'}, {'Workdesk_Assembled', 'Chair_Office'},
                 {'Desk_Office_Assembled', 'Chair_Office'}, {'Workdesk_Assembled', 'Chair_Workdesk'},
                 {'Table_Dining', 'Chair_Workdesk'})
        for i, a in enumerate(solid):
            for b in solid[i + 1:]:
                if overlap(a, b) and {a['stem'], b['stem']} not in tucks:
                    out.append(f"{a['room']}: {a['stem']} overlaps {b['stem']}")
        if plan:
            out += self.walkable(placed)
        for name, axis, c, along, _, _, _ in DOORS if plan else []:
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
            for p in placed:
                if overlap(gap, p, 0.005) and not p['stem'].startswith('Carpet'):  # rugs pass under a door
                    out.append(f"{p['room']}: {p['stem']} is in the {name}'s doorway / trim")
        # small props: something must be right under them; and two props mustn't sink into each other
        under = placed + supports
        props = [p for p in placed if not p['collide'] and not p['wall']]
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

    # Pieces someone has to get to (the front of a bed, a sofa, a desk, a counter...): checked by walkable().
    # (A nightstand is reached from its bed.)
    USED = ('Bed', 'Bed_Single_Person', 'Bed_Bunk_Assembled', 'Couch', 'Sofa_', 'Chesterfield_Sofa', 'Arm_Chair',
            'Chair_Arm', 'Toilet_Assembled', 'Bathroom_Sink_Body', 'Desk_Office', 'Workdesk_Assembled', 'Stove',
            'Fridge_Assembled', 'Kitchen_Counter', 'Washingmachine_Assembled', 'Dresser_', 'Bookshelf',
            'Cabinet_A_Assembled', 'Kitchen_Cabinet_Large_Assembled', 'Shower_Stall', 'Table_Dining', 'Tv_Stand')

    def walkable(self, placed, radius=0.3, cell=0.05, reach=0.45, picture=None):
        """Can the player (a `radius` circle) walk from the front door to every room, and get within `reach` of
        every piece someone uses? A flood fill over a `cell` grid of the plan, blocked by the walls (doors and
        arches open) and solid furniture below head height. Door leaves are pushed aside, so they don't count."""
        nx, nz = int(UNIT_W / cell) + 1, int(10.0 / cell) + 1
        blocked = [[False] * nz for _ in range(nx)]

        def block(x0, z0, x1, z1):
            for i in range(max(0, int((x0 - radius) / cell)), min(nx, int((x1 + radius) / cell) + 2)):
                for j in range(max(0, int((z0 - radius) / cell)), min(nz, int((z1 + radius) / cell) + 2)):
                    if x0 - radius < i * cell < x1 + radius and z0 - radius < j * cell < z1 + radius:
                        blocked[i][j] = True
        for axis, c, a, b, openings in WALLS:
            pos = a - T / 2
            for oc, ow, oh, *sill in sorted(openings) + [(b + T / 2 + 1.0, 0.0, 0.0)]:
                end = oc - ow / 2 if not sill else oc  # a window doesn't open the wall
                stop = min(end, b + T / 2)
                if axis == 'x':
                    block(pos, c - T / 2, stop, c + T / 2)
                else:
                    block(c - T / 2, pos, c + T / 2, stop)
                pos = oc + ow / 2 if not sill else oc
        for p in placed:
            if p['collide'] and p['y0'] < 1.0:
                block(p['x0'], p['z0'], p['x1'], p['z1'])
        _, _, c, along, _, _, _ = DOORS[0]
        start = (int((c - 0.45) / cell), int(along / cell))
        seen, todo = {start}, [start]
        while todo:
            i, j = todo.pop()
            for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                n = (i + di, j + dj)
                if 0 <= n[0] < nx and 0 <= n[1] < nz and n not in seen and not blocked[n[0]][n[1]]:
                    seen.add(n)
                    todo.append(n)
        if picture:  # a top-down picture of the fill: grey blocked, white reached, black unreached
            from PIL import Image
            im = Image.new('RGB', (nx, nz))
            for i in range(nx):
                for j in range(nz):
                    shade = (90, 90, 90) if blocked[i][j] else (255, 255, 255) if (i, j) in seen else (0, 0, 0)
                    im.putpixel((i, j), shade)
            im.resize((nx * 5, nz * 5), Image.NEAREST).save(picture)
        out = []
        if blocked[start[0]][start[1]]:
            return ['the front door is blocked']
        for room, (x0, z0, x1, z1) in ROOMS.items():
            if not any(x0 < i * cell < x1 and z0 < j * cell < z1 for i, j in seen):
                out.append(f"{room}: can't be walked into from the front door")
        for p in placed:
            if p['collide'] and not p['tipped'] and any(p['stem'].startswith(u) for u in self.USED):
                if not any(p['x0'] - reach < i * cell < p['x1'] + reach and p['z0'] - reach < j * cell < p['z1'] + reach
                           for i, j in seen):
                    out.append(f"{p['room']}: {p['stem']} can't be reached")
        return out

    def from_sandbox(self, name):
        """A copy of the Sandbox entity `name` and everything under it, with fresh ids (a root of this scene)."""
        if not hasattr(self, '_sandbox'):
            self._sandbox = json.load(open(os.path.join(self.repo, 'project', 'scenes', 'Sandbox.json'), encoding='utf-8'))
        lists = {key: self._sandbox.get(key, []) for key in ('empties', 'models', 'boxes')}

        def copy(src, key, parent):
            e = json.loads(json.dumps(src))
            i = self.s._next()
            e.update({'id': i, 'order': i, 'parentId': parent})
            getattr(self.s, key).append(e)
            for ck, items in lists.items():
                for c in items:
                    if c.get('parentId') == src['id']:
                        copy(c, ck, i)
            return e
        return copy(next(e for e in self._sandbox['empties'] if e['name'] == name), 'empties', -1)

    def clear_spot(self, room, margin=0.45):
        """The floor point of `room` furthest from walls and furniture (a 10 cm grid), and its clearance."""
        x0, z0, x1, z1 = inner(room)
        solid = [p for p in self.placed if p['collide'] and p['room'] == room and p['unit'] == self.unit
                 and p['y0'] < 1.0]
        best = (-1.0, None)
        for i in range(int((x1 - x0) / 0.1) + 1):
            for j in range(int((z1 - z0) / 0.1) + 1):
                x, z = x0 + i * 0.1, z0 + j * 0.1
                d = min(x - x0, x1 - x, z - z0, z1 - z)
                for p in solid:
                    dx = max(p['x0'] - x, 0.0, x - p['x1'])
                    dz = max(p['z0'] - z, 0.0, z - p['z1'])
                    d = min(d, math.hypot(dx, dz))
                if d > best[0]:
                    best = (d, (x, z))
        if best[0] < margin:
            raise RuntimeError(f'{room}: no clear floor for an NPC ({best[0]:.2f} m)')
        return best[1], best[0]

    def audio(self):
        # One space for the whole flat, so it sounds the same in every room: a furnished hotel room's recorded IR (RT60
        # ~0.35 s - soft furnishings swallow the tail), well under the Indoor Small class (concrete stairwells, ~2.5 s).
        # The box overhangs the outer walls by more than its fade, so the reverb is at full weight everywhere inside.
        x0 = min(r[0] for r in ROOMS.values()); z0 = min(r[1] for r in ROOMS.values())
        x1 = max(r[2] for r in ROOMS.values()); z1 = max(r[3] for r in ROOMS.values())
        if self.floor:  # (the corridor too: one space for the whole floor)
            x1, z1 = 2 * UNIT_W + CORRIDOR_W, CORRIDOR[3]
        pad = 0.6
        self.s.empty('Reverb', self.root, ((x0 + x1) / 2, H / 2, (z0 + z1) / 2), **{'Reverb Zone': {
            'Ambience': 'snd.amb.indoor_small', 'Ambience Volume': 0.35, 'Enabled': True,
            'Extents': [(x1 - x0) / 2 + pad, H / 2 + pad, (z1 - z0) / 2 + pad], 'Fade Distance': 0.4, 'Priority': 1,
            'Radius': 8.0, 'Reverb Mode': 'Custom', 'Shape': 'Box', 'Tail Class': 'Indoor Small', 'Tail Gain': 0.35,
            'IR': 'assets/Audio/IR/apartment_1.wav', 'Wet dB': -8.0, 'Pre-Delay ms': 2.0, 'HF Damping dB': 2.0,
            'Low Cut (Hz)': 100.0}})

    # --- building ---------------------------------------------------------------------------------------------
    def night(self):
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

    def apartment(self, number, xf, household, parent=-1, name=None):
        """One apartment of the plan at xf (see floor_units), lived in by `household` (a households/ module:
        PAINT, FLOORS, furnish(builder))."""
        self.xf, self.unit, self.groups, self.in_unit = xf, number, {}, True
        self.root = self.s.empty(name or f'Apartment {number}', parent)
        self.wall_paint = {**household.PAINT, 'Corridor': 'wall_corridor'}
        self.floors = household.FLOORS
        self.door_open = getattr(household, 'DOORS_OPEN', {})
        self.shell()
        self.doors()
        household.furnish(self)
        # its number goes on the corridor wall at the front door's latch side
        _, axis, c, along, swing, _, _ = DOORS[0]
        t = self.latch_along(axis, c, along, swing)
        plate = along + math.copysign(DOOR_W / 2 + 0.28, t - along)
        wx, door_z = self.at(c, along)
        self.front_doors.append((number, 'W' if wx < (CORRIDOR[0] + CORRIDOR[2]) / 2 else 'E', door_z,
                                 self.at(c, plate)[1]))

    def common(self, name, build, parent=-1):
        """A space outside the apartments (the corridor), built by build(builder) in world coordinates."""
        self.xf, self.unit, self.groups, self.in_unit = (0.0, 0.0, 1, 1), name, {}, False
        self.root = self.s.empty(name, parent)
        build(self)
