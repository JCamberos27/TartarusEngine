# The Tartarus Hub: the project's one scene. A rundown three-storey apartment block at the edge of a mossy cedar
# forest, at night just after rain. Floor 3 is the Apartment Hallway floor (corridor + apartments 301-306, the NPC
# tests' rooms) exactly where it has always been; under it a ground floor (lobby spawn, super's office, back door to
# the lot) and a basement (25 m range, armory cage, boiler room), tied together by a stair tower on the corridor's
# east end. Outside: a wet back lot (City Alley kit) with a garage workshop, a treeline fence, and a lit trail loop
# through the cedars.
#
#   python tools/scenes/build_hub.py [<repo>] [--from <scene.json>]
#
# A patcher: it reads scenes/Tartarus Hub.json (or, the first time, scenes/Apartment Hallway.json, or --from),
# removes everything it owns (the root groups named "Hub ...") and builds them again, so hand polish on Floor 3 and on
# anything outside those groups survives a rebuild. Writes scenes/Tartarus Hub.json (+ .meta, its guid kept).
#
# World (metres): X east, Z south, Floor 3's floor at y = 0 (so the NPC tests' coordinates are unchanged), the
# ground floor and the outside ground at y = -3, the basement at y = -6. The sky's sea level sits at the street.
import json
import math
import os
import random
import secrets
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scene_kit as k  # noqa: E402

OWN = 'Hub '                     # every root group this script builds starts with it
STOREY = 3.0                     # floor to floor
G, B = -STOREY, -2 * STOREY      # ground floor and basement floor levels
SLAB = 0.3
HALL = 1.075                     # corridor wall centre lines (as the Apartment Hallway's)
X0, X1 = -10.6, 10.6             # building wall centre lines along X (the corridor's ends)
Z0, Z1 = -9.675, 9.675           # outside wall centre lines along Z
TX, TI = 0.3, 0.15               # outside / inside wall thickness
FACADE = 0.12                    # brick skin outside the outside walls
TOP = 2.7                        # Floor 3's ceiling; the roof slab sits on it
# the stair tower on the corridor's east end: interior rectangle, landings at the west end, flights east and back
TW0, TW1 = X1 + TX / 2, X1 + TX / 2 + 5.3
TZ0, TZ1 = -1.55, 1.55
LAND_A = 1.3                     # west landing depth (the floor-level landing at each storey)
RISERS = 9                       # per flight, two flights per storey
TREAD = 0.3
SITE = (-30.0, -54.0, 30.0, 14.0)  # x0, z0, x1, z1 of the playable site
LOT_Z = -27.0                    # the lot runs from the building's north wall to the treeline here
GARAGE = (-27.0, -24.0, -17.0, -14.5)
TRAIL = (-14.0, -46.0, 14.0, -31.0)  # the trail loop's centre-line rectangle
GLADE = (5.0, -44.0, 12.0, -38.0)

ALLEY = 'assets/Environments/ModularCityAlleyPack/'
FOREST = 'assets/Environments/SeedMesh_Mossy_Cedrus_Forest/Environment/'
TREES = [FOREST + f'Vegetation/Cedrus_tree/Prefabs/Optimized_trees/Cedrus_tree_var{n} Background Variant.prefab'
         for n in (1, 2, 3, 4, 5, 6, 7, 8, 10, 11)]


class Hub(k.Scene):
    """A k.Scene that keeps the base scene's entities (everything not owned by this script)."""

    def __init__(self, base, repo):
        super().__init__(base)
        self.repo = repo
        ents = {e['id']: e for key in ('boxes', 'empties', 'models') for e in base.get(key, [])}
        owned = {i for i, e in ents.items() if e['parentId'] == -1 and e['name'].startswith(OWN)}
        changed = True
        while changed:  # every descendant of an owned root
            changed = False
            for i, e in ents.items():
                if i not in owned and e['parentId'] in owned:
                    owned.add(i)
                    changed = True
        self.boxes = [e for e in base.get('boxes', []) if e['id'] not in owned]
        self.empties = [e for e in base.get('empties', []) if e['id'] not in owned]
        self.models = [e for e in base.get('models', []) if e['id'] not in owned]
        self.removed = len(owned)
        self._id = max([0] + [e['id'] for e in self.boxes + self.empties + self.models])
        self.mats = {}

    # --- lookups -----------------------------------------------------------------------------------------------
    def all(self):
        return self.boxes + self.empties + self.models

    def world(self, e):
        ents = {x['id']: x for x in self.all()}
        p = list(e['position'])
        q = e
        while q['parentId'] != -1:  # Floor 3's groups are translated only, never turned or scaled
            q = ents[q['parentId']]
            p = [a + b for a, b in zip(p, q['position'])]
        return p

    def drop(self, pred):
        n = 0
        for key in ('boxes', 'empties', 'models'):
            keep = [e for e in getattr(self, key) if not pred(e)]
            n += len(getattr(self, key)) - len(keep)
            setattr(self, key, keep)
        return n

    # --- materials ---------------------------------------------------------------------------------------------
    def mat(self, key, path, scale=0.5, **kw):
        if key not in self.mats:
            self.mats[key] = k.from_mat(self.repo, path, triplanar=True, triplanarScale=scale, **kw)
        return self.mats[key]

    def materials(self):
        A = ALLEY + 'Materials/'
        return {
            'brick': self.mat('brick', A + 'Walls/M_Wall_Brickwall_Dark.mat', 0.33),
            'brick_red': self.mat('brick_red', A + 'Walls/M_Wall_Brickwall_RedDark.mat', 0.33),
            'concrete': self.mat('concrete', A + 'Walls/M_Midwall_ConcreteDirty.mat', 0.4),
            'painted': self.mat('painted', A + 'Walls/M_Wall_ConcretePainted.mat', 0.4),
            'floor': self.mat('floor', A + 'Floors/M_Floor_Concrete.mat', 0.35),
            'asphalt': self.mat('asphalt', A + 'Floors/M_Floor_AsphaltRoad.mat', 0.2),
            'plates': self.mat('plates', A + 'Floors/M_Floor_StreetPlatesConcrete.mat', 0.3),
            'roof': self.mat('roof', A + 'Miscellaneous/M_Roof_Metal.mat', 0.4),
            'rust': self.mat('rust', A + 'Miscellaneous/M_RustyMetalTiled.mat', 0.5),
            'moss': self.mat('moss', FOREST + 'Terrain_Textures/Mossy_Ground_rocks/M_Mossy_rocks_ground.mat', 0.25),
            'dirt': self.mat('dirt', FOREST + 'Terrain_Textures/Dirt/M_Dirt_forest_mat.mat', 0.3),
        }

    # --- pieces ------------------------------------------------------------------------------------------------
    def slab(self, name, parent, x0, z0, x1, z1, y_top, thick, mat, collider=True, **extra):
        return self.box(name, parent, ((x0 + x1) / 2, y_top - thick / 2, (z0 + z1) / 2),
                        (x1 - x0, thick, z1 - z0), mat, collider=collider, static=True, **extra)

    def wall(self, name, parent, axis, line, a, b, y0, y1, thick, mat, openings=()):
        """A wall along X (axis 'x', at z = line) or along Z (axis 'z', at x = line) from a to b, floor y0 to top y1,
        cut by openings [(centre, width, sill, head)]: full-height pieces between them, a header above each, a
        breast below each that has a sill."""
        def piece(c0, c1, h0, h1):
            if c1 - c0 < 0.01 or h1 - h0 < 0.01:
                return
            mid, ln, y = (c0 + c1) / 2, c1 - c0, (h0 + h1) / 2
            if axis == 'x':
                self.box(name, parent, (mid, y, line), (ln, h1 - h0, thick), mat, static=True)
            else:
                self.box(name, parent, (line, y, mid), (thick, h1 - h0, ln), mat, static=True)
        cuts = sorted((c - w / 2, c + w / 2, s, h) for c, w, s, h in openings)
        at = a
        for c0, c1, sill, head in cuts:
            piece(at, c0, y0, y1)
            piece(c0, c1, y0 + head, y1)
            piece(c0, c1, y0, y0 + sill)
            at = c1
        piece(at, b, y0, y1)

    def stamp(self, prefab, parent, pos, yaw=0.0, scale=1.0, name=None):
        """A copy of a .prefab's entities under a new empty at pos (the editor's drop, by hand)."""
        with open(os.path.join(self.repo, 'project', prefab), encoding='utf-8') as f:
            p = json.load(f)
        root = self.empty(name or os.path.splitext(os.path.basename(prefab))[0], parent, pos, yaw=yaw, static=True)
        if scale != 1.0:
            next(e for e in self.empties if e['id'] == root)['scale'] = [scale] * 3
        remap = {}
        for key in ('empties', 'boxes', 'models'):
            for e in p.get(key, []):
                remap[e['id']] = self._next()
        for key in ('empties', 'boxes', 'models'):
            for e in p.get(key, []):
                c = json.loads(json.dumps(e))
                c['id'] = remap[e['id']]
                c['order'] = c['id']
                c['parentId'] = remap.get(e['parentId'], root) if e['parentId'] != -1 else root
                getattr(self, key).append(c)
        return root

    def spot(self, name, parent, pos, intensity, rng, angle, shadows, kelvin, pitch=-90.0, yaw=0.0, **extra):
        return self.light(name, parent, pos, intensity, rng, shadows=shadows, kelvin=kelvin, spot=angle, pitch=pitch,
                          yaw=yaw, **extra)

    def fill(self, name, parent, pos, intensity, rng, kelvin=2700.0):
        return self.light(name, parent, pos, intensity, rng, kelvin=kelvin)


# --- Floor 3: the corridor's east window becomes the stair tower's doorway --------------------------------------------
def open_east_end(s):
    near = lambda e, x0, x1: x0 <= s.world(e)[0] <= x1 and abs(s.world(e)[2]) < 0.6  # noqa: E731
    gone = s.drop(lambda e: (
        (e['name'] in ('Window', 'Sill', 'Window Mullion (shadow)', 'Window Transom (shadow)', 'Reveal Sill')
         and near(e, 10.3, 10.8))
        or (e['name'] in ('Wall', 'Wall - Corridor') and near(e, 10.3, 10.8) and s.world(e)[1] < 0.6)))
    for e in s.all():
        if e['name'] == 'Reveal' and near(e, 10.3, 10.8) and e['scale'][1] < 2.3:  # run the jambs down to the floor
            head = e['position'][1] + e['scale'][1] / 2
            e['position'][1] = round(head / 2, 4)
            e['scale'][1] = round(head, 4)
        if e['name'] == 'Baseboard' and near(e, 10.3, 10.8) and e['scale'][2] > 1.5:  # split it either side
            half = dict(e)
            e['scale'] = [e['scale'][0], e['scale'][1], 0.55]
            e['position'] = [e['position'][0], e['position'][1], -0.725]
            half = json.loads(json.dumps(e))
            half['id'] = s._next()
            half['order'] = half['id']
            half['position'][2] = 0.725
            s.boxes.append(half)
    return gone


# --- the stair tower ------------------------------------------------------------------------------------------------
def stair_tower(s, m):
    root = s.empty('Hub Stair Tower', -1, (0, 0, 0), static=True)
    rise = STOREY / (2 * RISERS)
    xa = TW0 + LAND_A                    # flights start here
    xb = xa + RISERS * TREAD             # and end at the east landing
    run_w = (TZ1 - TZ0) / 2 - 0.05       # each flight's width (a spine wall between them)
    # walls: north, south, east full height; west only around the doorways (the building's end wall is there)
    y_bot, y_top = B - SLAB, TOP + SLAB
    s.wall('Tower Wall', root, 'x', TZ0 - TI / 2, TW0, TW1 + TI, y_bot, y_top, TI, m['concrete'])
    s.wall('Tower Wall', root, 'x', TZ1 + TI / 2, TW0, TW1 + TI, y_bot, y_top, TI, m['concrete'])
    s.wall('Tower Wall', root, 'z', TW1 + TI / 2, TZ0, TZ1, y_bot, y_top, TI, m['concrete'])
    # brick skin outside
    for z, sign in ((TZ0 - TI, -1), (TZ1 + TI, 1)):
        s.wall('Tower Facade', root, 'x', z + sign * FACADE / 2, TW0 - TX / 2, TW1 + TI + FACADE, G - 0.2, y_top + 0.5,
               FACADE, m['brick'])
    s.wall('Tower Facade', root, 'z', TW1 + TI + FACADE / 2, TZ0 - TI - FACADE, TZ1 + TI + FACADE, G - 0.2,
           y_top + 0.5, FACADE, m['brick'])
    s.slab('Tower Roof', root, TW0 - TX / 2, TZ0 - TI - FACADE, TW1 + TI + FACADE, TZ1 + TI + FACADE, y_top + 0.5,
           0.5, m['roof'])
    s.slab('Tower Pit', root, TW0, TZ0, TW1, TZ1, B, SLAB, m['floor'])
    # the spine between the flights
    s.box('Stair Spine', root, ((xa + xb) / 2, (B + TOP) / 2, 0.0), (xb - xa, TOP - B, 0.1), m['painted'], static=True)
    for level in (0.0, G):  # each storey down from `level`: north flight east, landing, south flight west
        s.slab('Landing', root, TW0, TZ0, xa, TZ1, level, 0.2, m['floor'])
        mid = level - STOREY / 2
        s.slab('Mid Landing', root, xb, TZ0, TW1, TZ1, mid, 0.2, m['floor'])
        for i in range(RISERS):
            # north flight: going east, down from level; each tread a solid block down to the flight's underside
            top = level - (i + 1) * rise
            x = xa + i * TREAD
            s.box('Tread', root, (x + TREAD / 2, top - 0.1, TZ0 + run_w / 2), (TREAD, 0.2, run_w), m['floor'],
                  static=True)
            # south flight: going west from the mid landing, down to the next level
            top2 = mid - (i + 1) * rise
            x2 = xb - (i + 1) * TREAD
            s.box('Tread', root, (x2 + TREAD / 2, top2 - 0.1, TZ1 - run_w / 2), (TREAD, 0.2, run_w), m['floor'],
                  static=True)
    s.slab('Landing', root, TW0, TZ0, xa, TZ1, B, 0.2, m['floor'])
    # lights: a caged bulb on each landing (fills; the stairwell is short-range)
    for level, label in ((0.0, '3'), (G, 'G'), (B, 'B')):
        s.fill(f'Stair Bulb {label}', root, (TW0 + 0.6, level + 2.4, 0.0), 0.5, 4.0, kelvin=3000.0)
    return root


# --- ground floor and basement --------------------------------------------------------------------------------------
def storey_shell(s, m, name, y, rooms_n, rooms_s, doors_n, doors_s, east_door=True, extra_west=None):
    """One storey under Floor 3: slab, outside walls (with the doorway to the stair tower), the corridor's walls with
    their doors, and the partitions. rooms_*: x lines of the partitions north / south of the corridor."""
    root = s.empty(name, -1, (0, 0, 0), static=True)
    head = (TOP if y == G else STOREY) - SLAB - 0.01
    ceil = y + STOREY - SLAB - 0.01 if y == G else y + STOREY - SLAB
    s.slab('Floor', root, X0 - TX / 2, Z0 - TX / 2, X1 + TX / 2, Z1 + TX / 2, y, SLAB, m['floor'])
    s.slab('Ceiling Slab', root, X0 - TX / 2, Z0 - TX / 2, X1 + TX / 2, Z1 + TX / 2, ceil + SLAB, SLAB,
           m['painted'])
    h = ceil - y
    door = (0.95, 0.0, 2.1)
    s.wall('Outside Wall', root, 'z', X1, Z0, Z1, y, ceil, TX, m['painted'],
           [(0.0, 0.95, 0.0, 2.1)] if east_door else [])
    if extra_west is None:
        s.wall('Outside Wall', root, 'z', X0, Z0, Z1, y, ceil, TX, m['painted'])
    for line, rooms, doors in ((-HALL, rooms_n, doors_n), (HALL, rooms_s, doors_s)):
        s.wall('Corridor Wall', root, 'x', line, X0, X1, y, ceil, TI, m['painted'],
               [(c, w, 0.0, 2.1 if w < 1.2 else 2.3) for c, w in doors])
        z_out = Z0 if line < 0 else Z1
        for x in rooms:
            s.wall('Partition', root, 'z', x, min(line, z_out), max(line, z_out), y, ceil, TI, m['painted'])
    return root, ceil, h, door


def ground_floor(s, m):
    root, ceil, h, _ = storey_shell(
        s, m, 'Hub Ground Floor', G,
        rooms_n=(-4.0, 0.6), rooms_s=(-4.6, 4.6),
        doors_n=[(-7.3, 0.95), (-1.7, 1.6), (5.6, 0.95)],
        doors_s=[(0.0, 3.0), (7.6, 0.95)])          # unit 101 has no door: a locked shell for now
    # outside walls along Z: the back door (vestibule, north) and the street door (lobby, south, locked: a gap the
    # dressing pass fills with a door that doesn't open)
    s.wall('Outside Wall', root, 'x', Z0, X0, X1, G, ceil, TX, m['painted'], [(-1.7, 1.0, 0.0, 2.15)])
    s.wall('Outside Wall', root, 'x', Z1, X0, X1, G, ceil, TX, m['painted'], [])
    # the lobby's light: a flickering sconce over the mailboxes (shadowed, spot down the wall) and a dim fill
    s.spot('Lobby Sconce', root, (-3.2, G + 2.2, 2.0), 3.0, 7.0, 55.0, True, 3200.0, pitch=-60.0, yaw=180.0,
           **k.light_flicker(Mode=0, MinInterval=2.0, MaxInterval=8.0, Depth=0.8, Seed=41))
    s.fill('Lobby Fill', root, (0.0, G + 2.2, 5.0), 0.35, 6.5)
    s.fill('Office Desk Lamp', root, (8.4, G + 1.0, 6.5), 0.5, 4.5)
    s.fill('Vestibule Fill', root, (-1.7, G + 2.3, -5.0), 0.3, 5.0)
    s.fill('Corridor G Fill', root, (5.0, G + 2.3, 0.0), 0.25, 6.0, kelvin=4000.0)
    s.fill('Corridor G Fill', root, (-6.0, G + 2.3, 0.0), 0.25, 6.0, kelvin=4000.0)
    return root


def basement(s, m):
    rx0 = -18.6  # the range runs west past the building, under the side yard
    root, ceil, h, _ = storey_shell(
        s, m, 'Hub Basement', B,
        rooms_n=(-3.0, 3.0), rooms_s=(),
        doors_n=[(-7.0, 0.95), (0.0, 1.8), (6.8, 0.95)],
        doors_s=[(8.6, 1.8)], extra_west=rx0)
    # the west wall: the corridor and north rooms end at X0; the range carries on to rx0
    s.wall('Outside Wall', root, 'z', X0, Z0, HALL, B, ceil, TX, m['painted'])
    s.wall('Outside Wall', root, 'x', Z0, X0, X1, B, ceil, TX, m['concrete'])
    s.wall('Range Wall', root, 'x', Z1, rx0, X1, B, ceil, TX, m['concrete'])
    s.wall('Range Wall', root, 'x', HALL, rx0, X0, B, ceil, TX, m['concrete'])
    s.wall('Range Wall', root, 'z', rx0, HALL, Z1, B, ceil, TX, m['concrete'])
    s.slab('Range Floor', root, rx0 - TX / 2, HALL, X0 - TX / 2, Z1 + TX / 2, B, SLAB, m['floor'])
    s.slab('Range Ceiling', root, rx0 - TX / 2, HALL, X0 - TX / 2, Z1 + TX / 2, ceil + SLAB, SLAB, m['concrete'])
    # firing line: a bench and booth dividers across the range at x = 6.6; four lanes
    lanes = 4
    for i in range(lanes + 1):
        z = HALL + 0.15 + i * (Z1 - HALL - 0.3) / lanes
        s.box('Booth Divider', root, (6.6, B + 0.9, z), (0.9, 1.8, 0.06), m['rust'], static=True)
    s.box('Firing Bench', root, (6.2, B + 0.45, (HALL + Z1) / 2), (0.5, 0.9, Z1 - HALL - 0.4), m['rust'],
          static=True)
    s.box('Backstop', root, (rx0 + 0.6, B + 1.3, (HALL + Z1) / 2), (0.6, 2.6, Z1 - HALL - 0.2), m['rust'],
          static=True)
    for i in range(lanes):  # placeholder target plates, 25 m and 10 m
        z = HALL + 0.15 + (i + 0.5) * (Z1 - HALL - 0.3) / lanes
        s.box('Target 25m', root, (6.6 - 25.0, B + 1.4, z), (0.03, 0.6, 0.45), m['rust'], static=True)
        s.box('Target 10m', root, (6.6 - 10.0, B + 1.4, z), (0.03, 0.45, 0.3), m['rust'], static=True)
    # lights: one shadowed worklight down the lanes, fills at the line, the armory and the boiler room
    s.spot('Range Worklight', root, (-5.0, ceil - 0.15, (HALL + Z1) / 2), 6.0, 16.0, 70.0, True, 4500.0,
           pitch=-90.0)
    s.fill('Firing Line Fill', root, (8.5, ceil - 0.3, (HALL + Z1) / 2), 0.6, 5.5, kelvin=4500.0)
    s.fill('Armory Fill', root, (0.0, ceil - 0.3, -5.4), 0.5, 5.0, kelvin=4000.0)
    s.fill('Boiler Worklight', root, (-7.0, ceil - 0.3, -5.4), 0.45, 5.5, kelvin=3500.0)
    s.fill('Corridor B Fill', root, (0.0, ceil - 0.3, 0.0), 0.25, 7.0, kelvin=4000.0)
    return root


# --- the building's outside: brick skin, roof, windows kept clear -----------------------------------------------------
def facade(s, m):
    root = s.empty('Hub Facade', -1, (0, 0, 0), static=True)
    # Floor 3's windows (outside walls only) stay clear: openings from its window models
    wins = {'x-': [], 'x+': [], 'z-': [], 'z+': []}
    for e in s.models:
        if e['name'] != 'Window':
            continue
        x, y, z = s.world(e)
        sc = e['scale'][0]
        w, hgt = 0.87 * sc + 0.12, 1.54 * sc + 0.12
        if abs(abs(z) - Z1) < 0.4:
            wins['z+' if z > 0 else 'z-'].append((x, w, y - 0.06 - G, y - 0.06 - G + hgt))
        elif abs(abs(x) - X1) < 0.4 and abs(z) > 0.6:
            wins['x+' if x > 0 else 'x-'].append((z, w, y - 0.06 - G, y - 0.06 - G + hgt))
    y0, y1 = G - 0.25, TOP + SLAB + 0.6   # a parapet above the roof
    xo0, xo1 = X0 - TX / 2 - FACADE, X1 + TX / 2 + FACADE
    zo0, zo1 = Z0 - TX / 2 - FACADE, Z1 + TX / 2 + FACADE
    # north (lot side): the back door at ground floor
    s.wall('Facade N', root, 'x', zo0 + FACADE / 2, xo0, xo1, y0, y1, FACADE, m['brick'],
           wins['z-'] + [(-1.7, 1.0, 0.25, 0.25 + 2.15)])
    s.wall('Facade S', root, 'x', zo1 - FACADE / 2, xo0, xo1, y0, y1, FACADE, m['brick'], wins['z+'])
    s.wall('Facade W', root, 'z', xo0 + FACADE / 2, zo0, zo1, y0, y1, FACADE, m['brick'], wins['x-'])
    s.wall('Facade E', root, 'z', xo1 - FACADE / 2, zo0, zo1, y0, y1, FACADE, m['brick'],
           wins['x+'] + [(0.0, 3.4, 0.0, y1 - y0)])     # the stair tower butts on here
    s.slab('Roof', root, X0 - TX / 2, Z0 - TX / 2, X1 + TX / 2, Z1 + TX / 2, TOP + SLAB + 0.15, 0.15, m['roof'])
    # a band course between the ground floor and Floor 3
    for z, sign in ((zo0, -1), (zo1, 1)):
        s.box('Band Course', root, (0.0, -0.15, z + sign * 0.05), (xo1 - xo0 + 0.2, 0.3, 0.1), m['concrete'],
              static=True)
    return root, wins


# --- outside: lot, garage, treeline, forest, trail ---------------------------------------------------------------------
def outside(s, m, rng):
    root = s.empty('Hub Outside', -1, (0, 0, 0), static=True)
    x0, z0, x1, z1 = SITE
    bx0, bx1 = X0 - TX / 2 - FACADE, X1 + TX / 2 + FACADE
    bz0, bz1 = Z0 - TX / 2 - FACADE, Z1 + TX / 2 + FACADE
    # ground: asphalt lot north of the building, concrete plates around it, moss in the forest
    s.slab('Lot', root, x0, LOT_Z, x1, bz0, G, 0.4, m['asphalt'])
    s.slab('Yard W', root, x0, bz0, bx0, z1, G, 0.4, m['plates'])
    s.slab('Yard E', root, bx1, bz0, x1, z1, G, 0.4, m['plates'])
    s.slab('Forest Floor', root, x0 - 30.0, z0 - 30.0, x1 + 30.0, LOT_Z, G, 0.4, m['moss'])
    # the street side: a closed brick wall with the facade line; nothing to walk past
    s.wall('Street Wall', root, 'x', z1, x0, x1, G, G + 3.2, 0.3, m['brick_red'])
    for x in (x0, x1):
        s.wall('Site Edge', root, 'z', x, LOT_Z, z1, G, G + 3.2, 0.3, m['brick_red'])
    # invisible bounds through the forest
    for name, c, size in (('Bound N', (0.0, G + 2.0, z0), (x1 - x0, 4.0, 0.3)),
                          ('Bound W', (x0, G + 2.0, (z0 + LOT_Z) / 2), (0.3, 4.0, LOT_Z - z0)),
                          ('Bound E', (x1, G + 2.0, (z0 + LOT_Z) / 2), (0.3, 4.0, LOT_Z - z0))):
        s.box(name, root, c, size, m['concrete'], static=True, **{'Mesh Renderer': {'Enabled': False}})
    # the treeline fence: chain-link 3 m panels with a gap onto the trail at x = 0
    fence = s.empty('Treeline Fence', root, (0, 0, 0), static=True)
    x = x0
    while x + 3.0 <= x1 + 0.01:
        if not (-2.0 < x + 1.5 < 2.0):
            s.stamp(ALLEY + 'Prefabs/Prefabs_Props/Fence_MetalChainlink_3x.prefab', fence, (x + 1.5, G, LOT_Z))
        x += 3.0
    # the garage / workshop: three brick walls, a roof, open to the lot on the east
    gx0, gz0, gx1, gz1 = GARAGE
    gar = s.empty('Garage', root, (0, 0, 0), static=True)
    s.wall('Garage Wall', gar, 'x', gz0, gx0, gx1, G, G + 3.4, 0.25, m['brick_red'])
    s.wall('Garage Wall', gar, 'x', gz1, gx0, gx1, G, G + 3.4, 0.25, m['brick_red'])
    s.wall('Garage Wall', gar, 'z', gx0, gz0, gz1, G, G + 3.4, 0.25, m['brick_red'])
    s.wall('Garage Wall', gar, 'z', gx1, gz0, gz1, G, G + 3.4, 0.25, m['brick_red'],
           [((gz0 + gz1) / 2, 5.0, 0.0, 3.0)])
    s.slab('Garage Roof', gar, gx0 - 0.3, gz0 - 0.3, gx1 + 0.3, gz1 + 0.3, G + 3.55, 0.15, m['roof'])
    s.slab('Garage Floor', gar, gx0, gz0, gx1, gz1, G + 0.02, 0.02, m['floor'])
    # lot props: dumpsters by the building, the fire escape on the north wall, the street lamp
    props = s.empty('Lot Props', root, (0, 0, 0), static=True)
    P = ALLEY + 'Prefabs/Prefabs_Props/'
    s.stamp(P + 'Dumpster.prefab', props, (7.5, G, bz0 - 1.3), yaw=180.0)
    s.stamp(P + 'DumpsterOpen.prefab', props, (4.6, G, bz0 - 1.3), yaw=180.0)
    s.stamp(P + 'Pallet.prefab', props, (12.0, G, -13.5), yaw=20.0)
    s.stamp(P + 'Wooden_Crate.prefab', props, (13.0, G, -14.8), yaw=-10.0)
    s.stamp(P + 'FireEscape.prefab', props, (-6.5, 0.0, bz0), yaw=180.0)
    s.stamp(P + 'Lamp_StreetHigh.prefab', props, (-2.0, G, -18.5), yaw=0.0)
    s.stamp(P + 'Lamp_Wall.prefab', props, (-1.7, G + 2.6, bz0), yaw=180.0)
    s.stamp(P + 'AirConditioner_A.prefab', props, (3.0, -0.6, bz0), yaw=180.0)
    # lights outside (shadowed: the sodium lamp, the back door lamp, the garage worklight, one trail lamp)
    L = s.empty('Outside Lights', root, (0, 0, 0), static=True)
    s.spot('Sodium Lamp', L, (-2.0, G + 7.6, -17.6), 9.0, 22.0, 62.0, True, 2000.0, pitch=-80.0, yaw=180.0)
    s.spot('Back Door Lamp', L, (-1.7, G + 2.9, bz0 - 0.35), 2.2, 9.0, 65.0, True, 2700.0, pitch=-60.0, yaw=180.0)
    s.spot('Garage Worklight', L, ((gx0 + gx1) / 2, G + 3.1, (gz0 + gz1) / 2), 4.0, 9.0, 75.0, True, 4200.0)
    s.fill('Lot Bounce', L, (-2.0, G + 1.5, -17.0), 0.35, 11.0, kelvin=2100.0)
    s.fill('Garage Fill', L, ((gx0 + gx1) / 2, G + 1.5, (gz0 + gz1) / 2), 0.3, 7.0, kelvin=4200.0)
    forest(s, m, root, rng)
    return root


def on_trail(x, z, pad):
    tx0, tz0, tx1, tz1 = TRAIL
    inside = tx0 - pad <= x <= tx1 + pad and tz0 - pad <= z <= tz1 + pad
    core = tx0 + pad < x < tx1 - pad and tz0 + pad < z < tz1 - pad
    return inside and not core


def forest(s, m, root, rng):
    grp = s.empty('Forest', root, (0, 0, 0), static=True)
    tx0, tz0, tx1, tz1 = TRAIL
    # the trail: a dirt strip round the loop, joined to the fence gap
    path = s.empty('Trail', grp, (0, 0, 0), static=True)
    w = 1.6
    for name, a, b in (('Trail N', (tx0, tz0), (tx1, tz0)), ('Trail S', (tx0, tz1), (tx1, tz1)),
                       ('Trail W', (tx0, tz0), (tx0, tz1)), ('Trail E', (tx1, tz0), (tx1, tz1)),
                       ('Trail Spur', (0.0, tz1), (0.0, LOT_Z))):
        (ax, az), (bx, bz) = a, b
        s.slab(name, path, min(ax, bx) - w / 2, min(az, bz) - w / 2, max(ax, bx) + w / 2, max(az, bz) + w / 2,
               G + 0.015, 0.02, m['dirt'], collider=False)
    # trees: a seeded scatter, kept off the trail and the glade, denser and bigger out past the site's edge
    x0, z0, x1, z1 = SITE
    placed = []
    want, tries = 110, 0
    while len(placed) < want and tries < 20000:
        tries += 1
        x = rng.uniform(x0 - 18.0, x1 + 18.0)
        z = rng.uniform(z0 - 16.0, LOT_Z - 2.5)
        if on_trail(x, z, 2.4) or (GLADE[0] - 1 <= x <= GLADE[2] + 1 and GLADE[1] - 1 <= z <= GLADE[3] + 1):
            continue
        if abs(x) < 2.2 and z > tz1:  # the spur
            continue
        gap = 5.0 if (x0 <= x <= x1 and z >= z0) else 6.5
        if any((x - px) ** 2 + (z - pz) ** 2 < gap * gap for px, pz in placed):
            continue
        placed.append((x, z))
        s.stamp(rng.choice(TREES), grp, (round(x, 2), G, round(z, 2)), yaw=round(rng.uniform(0, 360), 1),
                scale=round(rng.uniform(0.85, 1.15), 3), name='Cedar')
    # trail lamps: posts with a shadowed lamp at the far bend, fills at the others
    L = s.empty('Trail Lamps', grp, (0, 0, 0), static=True)
    P = ALLEY + 'Prefabs/Prefabs_Props/Lamp_StreetHigh.prefab'
    for i, (x, z, shadow) in enumerate(((tx0 + 0.9, (tz0 + tz1) / 2, False), (0.0, tz0 + 0.9, True),
                                        (tx1 - 0.9, (tz0 + tz1) / 2, False))):
        s.stamp(P, L, (x, G, z), yaw=[90.0, 0.0, -90.0][i], scale=0.8, name='Trail Lamp Post')
        if shadow:
            s.spot('Trail Lamp', L, (x, G + 6.0, z + 0.7), 6.0, 15.0, 65.0, True, 2400.0, pitch=-80.0)
        else:
            s.fill('Trail Lamp', L, (x, G + 4.5, z), 1.2, 9.0, kelvin=2400.0)
    return len(placed)


# --- scene-wide ------------------------------------------------------------------------------------------------------
def look(s):
    d = s.data
    d['sky'].update({'seaLevel': G, 'clouds': True, 'cloudCoverage': 0.32, 'cloudType': 0.35, 'cloudQuality': 0,
                     'cloudShadows': False, 'cloudWindSpeed': 4.0})
    d.update({'shadowDistance': 70.0, 'maxSpotShadows': 8, 'maxPointShadows': 0, 'fogEnabled': False})


def spawn(s):
    """The player starts in the lobby, facing the corridor (north)."""
    for e in s.empties:
        if e['parentId'] == -1 and 'First Person Controller' in e:
            e['position'] = [0.0, G, 6.5]
            e['rotation'] = k.euler_quat(0.0)
            return True
    return False


def write_meta(path):
    meta = path + '.meta'
    if os.path.exists(meta):
        return
    with open(meta, 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'guid': secrets.token_hex(8), 'metaVersion': 1, 'type': 'scene'}, f, indent=2)
        f.write('\n')


def main():
    args = sys.argv[1:]
    src = None
    if '--from' in args:
        i = args.index('--from')
        src = args[i + 1]
        del args[i:i + 2]
    repo = os.path.abspath(args[0] if args else os.path.join(os.path.dirname(__file__), '..', '..'))
    out = os.path.join(repo, 'project', 'scenes', 'Tartarus Hub.json')
    if not src:
        src = out if os.path.exists(out) else os.path.join(repo, 'project', 'scenes', 'Apartment Hallway.json')
    with open(src, encoding='utf-8') as f:
        base = json.load(f)
    s = Hub(base, repo)
    m = s.materials()
    opened = open_east_end(s)
    stair_tower(s, m)
    ground_floor(s, m)
    basement(s, m)
    _, wins = facade(s, m)
    trees = 0
    root = outside(s, m, random.Random(7))
    trees = sum(1 for e in s.empties if e['name'] == 'Cedar')
    look(s)
    spawned = spawn(s)
    s.write(out)
    write_meta(out)
    lights = [e for e in s.empties if 'Light' in e]
    shadowed = sum(1 for e in lights if e['Light'].get('Cast Shadows'))
    print(f'build_hub: from {os.path.basename(src)} (replaced {s.removed} owned entities, opened the east end: '
          f'{opened} pieces); {len(s.boxes)} boxes, {len(s.models)} models, {len(s.empties)} empties; '
          f'{trees} cedars; {len(lights)} lights ({shadowed} shadowed); facade windows '
          f'{sum(len(v) for v in wins.values())}; spawn {"moved to the lobby" if spawned else "NOT FOUND"} -> {out}')


if __name__ == '__main__':
    main()
