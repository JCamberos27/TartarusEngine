# Dresses the six apartments (301-306) of the Apartment Hallway as a derelict squat, the same layered peeling-paint
# recipe as dress_corridor.py (it imports that script's helpers). It patches project/scenes/Apartment Hallway.json IN
# PLACE, so run it AFTER dress_corridor.py:
#
#   python -I tools/scenes/dress_corridor.py [<repo>]
#   python -I tools/scenes/dress_apartments.py [<repo>]
#
#  * materials: the apartment walls (+ the window / arch Reveal pieces that sit in a unit) keep their white baseColor,
#    normal and roughness maps and only gain the PH_Beige_Wall AO map (the shader reads its red channel, so it can't
#    tint) and the T_Grunge_PeelingPaint_036 grunge (0.3, scale 0.25); ceilings get _085 (0.4) and
#    the apartments' baseboards _036 (0.4). They also gain the corridor's "substrate" preset references (dc.SUBSTRATES): Wall_Concrete on walls and
#    reveals, Floor_Concrete on ceilings, PH_Dark_Wooden_Planks on baseboards. 301 is the hand-edited NPC test room: nothing there is touched except these materials and the
#    added decals;
#  * decals go under an "Apartment Wear" root, one "30x Wear" group per unit. Two seeded units are trashed (~60 decals),
#    the rest moderate (~30-40). Per room, as budget allows: door-frame cracks and flakes, a skirting band, mid-wall hero
#    patches (larger and lit rooms), and ceiling clusters under the lamp and in the corners. Bathrooms always get water
#    damage kept light, like the hall: one ceiling cluster, chipped lower walls, one dark DA_LiquidSplatter_* splash.
#    The corridor's DA_PeelingPaint_* pool plus eight flake / crack maps (APT_POOL), for variety and chipping.
#    Ceiling decals get a Preset Floor_Concrete substrate override; the rest stay on Auto (the receiver's substrate);
#  * every decal is placed from geometry read off the scene (wall planes and which side faces into the room, door and
#    window openings, ceiling footprints) and a self-check at the end fails loudly if one is off its wall, faces out of
#    the room, sits outside the room's footprint or covers an opening. Asserted limits: <= MAX_DECALS decals,
#    <= 30 unique 2K maps and <= 1 4K map across the corridor AND the apartments.
#
# Idempotent: it removes "Apartment Wear" and re-adds it, so a second run leaves the file byte-identical.
import json
import math
import os
import random
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))     # python -I drops the script directory
import dress_corridor as dc                                          # noqa: E402

ROOTS = ('Apartment Wear',)
MAX_DECALS, MAX_2K, MAX_4K = 330, 30, 1
# The corridor's pool plus eight more flake / crack maps: more variety, and the chipping the corridor shows.
APT_POOL = {k: v + {'flake': [21, 25, 29, 36], 'crack': [30, 37, 40, 69]}.get(k, []) for k, v in dc.POOL.items()}
UNITS = (301, 302, 303, 304, 305, 306)
SEED = 4242
WALL_H, CEIL_Y = 2.7, 2.7
EYE_Y = 1.5

GRUNGE = {'walls': (0.3, 0.25), 'ceilings': (0.4, 0.25), 'trim': (0.4, None)}      # strength, scale (None = default)
AO_MAP = dc.WALL_DIR + 'AO_2k.jpg'
SPLAT_TINT = [0.1, 0.09, 0.08]
TOL = 5e-3


# ---------------------------------------------------------------------------------------------------- geometry
def normal_of(q):
    """World direction of the local +Z axis for a quaternion [x, y, z, w]."""
    x, y, z, w = q
    return (2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y))


class Line:
    """One wall plane of one room: its facing, its rotation and the rectangles (u0, u1, y0, y1) the wall pieces cover,
    with u running along the plane (x for a wall facing +-Z, z for one facing +-X)."""

    def __init__(self, room, axis, sign, off, rot):
        self.room, self.axis, self.sign, self.off, self.rot = room, axis, sign, off, rot
        self.rects = []
        self.doors = []          # centre u of every door in this wall
        self.taken = []          # (u0, u1, y0, y1) rectangles already dressed

    @property
    def n(self):
        return (self.sign, 0.0, 0.0) if self.axis == 'x' else (0.0, 0.0, self.sign)

    def pos(self, u, y):
        return (self.off, y, u) if self.axis == 'x' else (u, y, self.off)

    def u_of(self, p):
        return p[2] if self.axis == 'x' else p[0]

    @property
    def span(self):
        return min(r[0] for r in self.rects), max(r[1] for r in self.rects)

    def covers(self, u, y, hw, hh, grid=6):
        """True if the whole rectangle (clipped to the wall's height) lies on wall pieces: no doorway, no window."""
        y0, y1 = max(y - hh, 0.0), min(y + hh, WALL_H)
        for i in range(grid + 1):
            for j in range(grid + 1):
                uu, yy = u - hw + 2 * hw * i / grid, y0 + (y1 - y0) * j / grid
                if not any(r[0] - TOL <= uu <= r[1] + TOL and r[2] - TOL <= yy <= r[3] + TOL for r in self.rects):
                    return False
        return True

    def free(self, u, y, hw, hh):
        """No earlier patch overlaps more than 30% of the smaller rectangle."""
        a = (u - hw, u + hw, max(y - hh, 0.0), min(y + hh, WALL_H))
        for b in self.taken:
            ou, oy = min(a[1], b[1]) - max(a[0], b[0]), min(a[3], b[3]) - max(a[2], b[2])
            if ou > 0 and oy > 0 and ou * oy > 0.3 * min((a[1] - a[0]) * (a[3] - a[2]), (b[1] - b[0]) * (b[3] - b[2])):
                return False
        return True


class Room:
    def __init__(self, unit, name, x0, x1, z0, z1):
        self.unit, self.name, self.x0, self.x1, self.z0, self.z1 = unit, name, x0, x1, z0, z1
        self.lines, self.lamps, self.doors = [], [], []      # doors: world (x, z) of each door frame in this room's walls

    area = property(lambda s: (s.x1 - s.x0) * (s.z1 - s.z0))
    label = property(lambda s: f'Apartment {s.unit} - {s.name}')

    def inside(self, x, z, inset=0.0):
        return self.x0 + inset - TOL <= x <= self.x1 - inset + TOL and self.z0 + inset - TOL <= z <= self.z1 - inset + TOL


def read_geometry(d):
    by_id = {e['id']: e for k in ('boxes', 'models', 'empties') for e in d[k]}
    rooms = {}
    for e in d['models']:
        m = re.fullmatch(r'Ceiling - Apartment (\d+) - (.+)', e['name'])
        if m:
            x, y, z = dc.world_pos(by_id, e)
            assert normal_of(e['rotation'])[1] < -0.99 and abs(y - CEIL_Y) < 1e-6, f"{e['name']} is not a ceiling"
            rooms[(int(m[1]), m[2])] = Room(int(m[1]), m[2], x - e['scale'][0] / 2, x + e['scale'][0] / 2,
                                            z - e['scale'][1] / 2, z + e['scale'][1] / 2)
    assert len(rooms) == 24, len(rooms)

    # wall pieces -> one Line per (room, facing, plane); which side faces into the room comes from the ceiling footprint
    lines = {}
    for e in d['models']:
        m = re.fullmatch(r'Wall - Apartment (\d+) - (.+)', e['name'])
        if not m:
            continue
        room = rooms[(int(m[1]), m[2])]
        x, y, z = dc.world_pos(by_id, e)
        n = normal_of(e['rotation'])
        axis = 'x' if abs(n[0]) > 0.99 else 'z'
        assert abs(n[1]) < 1e-3 and (abs(n[0]) > 0.99 or abs(n[2]) > 0.99), f"{e['name']} is not a vertical axis-aligned wall"
        sign = 1.0 if (n[0] if axis == 'x' else n[2]) > 0 else -1.0
        assert room.inside(x + n[0] * 0.1, z + n[2] * 0.1), f"{e['name']} at {x:.3f},{z:.3f} faces out of its room"
        off = x if axis == 'x' else z
        u = z if axis == 'x' else x
        line = lines.setdefault((id(room), axis, sign), Line(room, axis, sign, off, list(e['rotation'])))
        assert abs(line.off - off) < TOL, f"{e['name']}: two parallel planes {line.off} / {off}"
        line.rects.append((u - e['scale'][0] / 2, u + e['scale'][0] / 2, y - e['scale'][1] / 2, y + e['scale'][1] / 2))
        if line not in room.lines:
            room.lines.append(line)

    # lamps (a lit room), by footprint
    for e in d['models']:
        if e['name'] in ('Lamp_Ceiling_A', 'Lamp_Fluorescent_A'):
            x, _, z = dc.world_pos(by_id, e)
            for room in rooms.values():
                if room.inside(x, z):
                    room.lamps.append((x, z))

    # door frames -> the wall lines they open (a header piece above the opening on that line)
    for e in d['models']:
        if e['name'] != 'Frame' or not re.fullmatch(r'\d+ .*Door', by_id[e['parentId']]['name']):
            continue
        x, _, z = dc.world_pos(by_id, e)
        n = normal_of(e['rotation'])
        axis = 'x' if abs(n[0]) > 0.99 else 'z'
        fo, fu = (x, z) if axis == 'x' else (z, x)
        hit = 0
        for line in lines.values():
            if line.axis != axis or abs(line.off - fo) > 0.3:
                continue
            for r in line.rects:
                if r[2] >= 2.0 and r[3] >= WALL_H - 0.01 and r[0] - 0.1 <= fu <= r[1] + 0.1 and r[1] - r[0] < 1.3:
                    line.doors.append(round((r[0] + r[1]) / 2, 4))
                    line.room.doors.append((x, z))
                    hit += 1
        assert hit >= 1, f"door {by_id[e['parentId']]['name']} found no wall header"

    for room in rooms.values():
        room.lines.sort(key=lambda l: (l.axis, l.sign))
        room.lamps.sort()
        assert len(room.lines) >= 3, room.label
    return by_id, rooms


def reveal_unit(rooms, x, z):
    """The unit a window / arch reveal piece belongs to (within 20 cm of its rooms), or None."""
    units = {r.unit for r in rooms.values()
             if r.x0 - 0.2 <= x <= r.x1 + 0.2 and r.z0 - 0.2 <= z <= r.z1 + 0.2}
    return units.pop() if len(units) == 1 else None


# ---------------------------------------------------------------------------------------------------- materials
def patch_materials(d, repo, by_id, rooms):
    """Returns (walls, reveals, ceilings, baseboards) patched."""
    ao = dc.ref(repo, AO_MAP)
    grunge_wall, grunge_worn = dc.ref(repo, dc.GRUNGE_WALL), dc.ref(repo, dc.GRUNGE_WORN)
    walls = [e for e in d['models'] if re.fullmatch(r'Wall - Apartment \d+ - .+', e['name'])]
    reveals = []
    for e in d['models']:
        if e['name'] in ('Reveal', 'Reveal Head', 'Reveal Sill'):
            x, _, z = dc.world_pos(by_id, e)
            if reveal_unit(rooms, x, z) is not None:
                reveals.append(e)
    ceils = [e for e in d['models'] if re.fullmatch(r'Ceiling - Apartment \d+ - .+', e['name'])]
    corridor = {id(e) for e in dc.corridor_trims(d, by_id)}
    trims = [e for e in d['boxes'] if e['name'] == 'Baseboard' and id(e) not in corridor]
    assert len(walls) == 234 and len(ceils) == 24 and len(trims) == 138, (len(walls), len(ceils), len(trims))
    for e in walls + reveals:
        m = e['materials'][0]['embedded']      # keep baseColor / normalMap / roughnessMap: the walls stay white
        m['aoMap'] = ao
        m['grungeMap'] = grunge_wall
        m['grungeStrength'], m['grungeScale'] = GRUNGE['walls']
        m['substrate'] = dc.substrate_block(repo, 'wall')
    for e in ceils:
        m = e['materials'][0]['embedded']
        m['grungeMap'] = grunge_worn
        m['grungeStrength'], m['grungeScale'] = GRUNGE['ceilings']
        m['substrate'] = dc.substrate_block(repo, 'ceil')
    for e in trims:
        m = e['materials'][0]['embedded']
        m['grungeMap'] = grunge_wall               # as the corridor's: _085 opened black blobs at any strength
        m['grungeStrength'] = GRUNGE['trim'][0]
        m.pop('grungeScale', None)
        m['substrate'] = dc.substrate_block(repo, 'trim')
    return len(walls), len(reveals), len(ceils), len(trims)


# ---------------------------------------------------------------------------------------------------- decals
class AptWear(dc.Wear):
    """dc.Wear with wall 'names' being Line objects. decal() buffers into .pending so a feature can be costed and
    dropped; commit() writes the entities."""
    MIDWALL_STRIP = 0.0     # strips read as boxes mid-wall on white plaster: holes only, strips stay on skirting

    def pick(self, r, tag):
        return dc.PEEL.format(APT_POOL[tag][int(r.random() * len(APT_POOL[tag]))], '')

    def __init__(self, d):
        super().__init__(d, 'Apartment Wear')
        self.apt_root = self.root
        self.pending, self.cur = [], None
        self.meta = {}                  # entity id -> (kind, room, line, tag, info)
        self.heroes = []                # (room, line, u, y, w, h)

    def start_unit(self, unit):
        self.root = self.empty(f'{unit} Wear', self.apt_root, (0.0, 0.0, 0.0))
        self.decals = []

    def spot(self, line, u, y, off=0.0):
        return line.pos(u, y), line.rot

    def decal(self, *a, **k):
        self.pending.append((a, k, self.cur))

    def commit(self, items):
        for a, k, cur in items:
            dc.Wear.decal(self, *a, **k)
            self.meta[self.next - 1] = cur
            if cur[3] == 'hero':
                self.heroes.append((cur[1],) + cur[4])


def find_u(line, r, hw, y, hh, tries=40):
    lo, hi = line.span
    if hi - lo < 2 * hw:
        return None
    for _ in range(tries):
        u = lo + hw + r.random() * (hi - lo - 2 * hw)
        if line.covers(u, y, hw, hh) and line.free(u, y, hw, hh):
            return u
    return None


def shuffled(r, items):
    items = list(items)
    r.shuffle(items)
    return items


def f_skirt(S, room, seed, trash):
    r = random.Random(seed)
    for line in shuffled(r, room.lines):
        w = 1.3 + r.random() * 0.9
        hw = max(w / 2 + 0.2, 1.05)           # the extra hole sits 0.6 off centre and is 0.8 wide
        u = find_u(line, r, hw, dc.SKIRT_Y, 0.38)
        if u is None:
            continue
        S.cur = ('wall', room, line, 'skirt', None)
        S.patch(line, u, dc.SKIRT_Y, w, 0.75, seed, flakes=1, crack=False, skirt=True)
        pos, rot = S.spot(line, u + (0.6 if r.random() < 0.5 else -0.6), dc.SKIRT_HOLE_Y)
        S.decal(S.pick(r, 'hole'), pos, (0.8, 0.55, 0.25), rot, seed + 5, 3, 0.9, jitter=(180.0, 0.15))
        line.taken.append((u - hw, u + hw, 0.0, dc.SKIRT_Y + 0.38))
        return True
    return False


def f_hero(S, room, four_k, seed, trash):
    r = random.Random(seed)
    for scale in ((0.85, 0.65, 0.5) if room.area < 10 else (1.0, 0.8, 0.6)):     # shrink to fit between the openings
        for line in shuffled(r, room.lines):
            w, h = (1.6 + r.random() * 0.7) * scale, (1.3 + r.random() * 0.6) * scale
            y = 1.15 + r.random() * 0.4
            u = find_u(line, r, w / 2 + 0.3, y, h / 2 + 0.3)
            if u is None:
                continue
            S.cur = ('wall', room, line, 'hero', (line, u, y, w, h))
            S.patch(line, u, y, w, h, seed, hero=four_k, flakes=3 if trash else 2, crack=True)
            line.taken.append((u - w / 2 - 0.3, u + w / 2 + 0.3, y - h / 2 - 0.3, y + h / 2 + 0.3))
            return True
    return False


def f_door(S, room, line, uc, seed, trash):
    """A crack along the header and flaking beside the jamb (and a low hole when trashed)."""
    r = random.Random(seed)
    placed = False
    u = uc + (r.random() - 0.5) * 0.3
    for width in (1.1, 0.9):
        if line.covers(u, 2.45, width / 2, 0.25):
            S.cur = ('wall', room, line, 'door', None)
            pos, rot = S.spot(line, u, 2.45)
            S.decal(S.pick(r, 'crack'), pos, (width, 0.5, 0.25), rot, seed, 4, 0.85, jitter=(0.0, 0.1))
            placed = True
            break
    side = 1 if r.random() < 0.5 else -1
    y = 1.3 + r.random() * 0.5
    for sd in (side, -side):
        um = uc + sd * 0.78
        if line.covers(um, y, 0.25, 0.25) and line.free(um, y, 0.25, 0.25):
            S.cur = ('wall', room, line, 'door', None)
            pos, rot = S.spot(line, um, y)
            S.decal(S.pick(r, 'flake'), pos, (0.45, 0.45, 0.25), rot, seed + 1, 5, 0.9, jitter=(180.0, 0.15))
            line.taken.append((um - 0.25, um + 0.25, y - 0.25, y + 0.25))
            placed = True
            if trash:
                um2, y2 = uc - sd * 0.8, 0.4
                if line.covers(um2, y2, 0.35, 0.3) and line.free(um2, y2, 0.35, 0.3):
                    pos, rot = S.spot(line, um2, y2)
                    S.decal(S.pick(r, 'hole'), pos, (0.7, 0.6, 0.25), rot, seed + 2, 3, 0.9, jitter=(180.0, 0.15))
                    line.taken.append((um2 - 0.35, um2 + 0.35, 0.1, 0.7))
            break
    return placed


def ceil_decals(S, room, cx, cz, w, h, seed, tags):
    """Like dc.Wear.ceiling, but each decal is clamped so it stays inside the room's ceiling."""
    r = random.Random(seed)
    w, h = min(w, room.x1 - room.x0 - 0.1), min(h, room.z1 - room.z0 - 0.1)
    S.cur = ('ceil', room, None, 'ceil', None)
    for i, (tag, scale, op) in enumerate(tags):
        ox, oz = (r.random() - 0.5) * 0.4 * w, (r.random() - 0.5) * 0.4 * h
        dw, dh = w * scale, h * scale
        x = min(max(cx + ox, room.x0 + dw / 2 + 0.03), room.x1 - dw / 2 - 0.03)
        z = min(max(cz + oz, room.z0 + dh / 2 + 0.03), room.z1 - dh / 2 - 0.03)
        S.decal(S.pick(r, tag), (x, CEIL_Y, z), (dw, dh, 0.25), dc.CEIL_Q, seed + i, i, op, jitter=(180.0, 0.12),
                substrate='ceil')
    return True


def f_ceil(S, room, seed, trash):
    """A cluster under the lamp (or room centre), with a couple of extra flakes when trashed."""
    r = random.Random(seed)
    cx, cz = room.lamps[0] if room.lamps else ((room.x0 + room.x1) / 2, (room.z0 + room.z1) / 2)
    cx, cz = cx + (r.random() - 0.5) * 0.8, cz + (r.random() - 0.5) * 0.8
    tags = [('blotch', 1.0, 0.6), ('strip', 0.8, 0.8), (r.choice(('flake', 'crack')), 0.4, 0.85)]
    if trash:
        tags += [('hole', 0.35, 0.8)]
    return ceil_decals(S, room, cx, cz, 1.3 + r.random() * 0.5, 1.3 + r.random() * 0.5, seed, tags)


def f_corner(S, room, seed, trash):
    r = random.Random(seed)
    w = 1.2 + r.random() * 0.4
    cx = room.x0 + w / 2 if r.random() < 0.5 else room.x1 - w / 2
    cz = room.z0 + w / 2 if r.random() < 0.5 else room.z1 - w / 2
    tags = [('blotch', 1.0, 0.55), (r.choice(('crack', 'flake')), 0.5, 0.8)]
    if trash:
        tags.append(('strip', 0.7, 0.75))
    return ceil_decals(S, room, cx, cz, w, w, seed, tags)


def f_bath(S, room, seed, trash):
    """Bathrooms, dressed like the hall: a ceiling cluster, chipped lower walls and one dark splash on the floor."""
    r = random.Random(seed)
    n = 0
    for k in range(2 if trash else 1):
        cx = room.x0 + 0.6 + r.random() * (room.x1 - room.x0 - 1.2)
        cz = room.z0 + 0.6 + r.random() * (room.z1 - room.z0 - 1.2)
        ceil_decals(S, room, cx, cz, 1.6 + r.random() * 0.4, 1.6 + r.random() * 0.4, seed + 20 * k + 100,
                    [('blotch', 1.0, 0.55), (r.choice(('flake', 'crack')), 0.4, 0.9)])
    for line in shuffled(r, room.lines):
        if n >= (3 if trash else 2):
            break
        w, h = 1.2 + r.random() * 0.8, 1.0 + r.random() * 0.3
        u = find_u(line, r, w / 2 + 0.3, 0.55, h / 2 + 0.3)
        if u is None:
            continue
        S.cur = ('wall', room, line, 'water', None)
        S.patch(line, u, 0.55, w, h, seed + 40 * n + 7, flakes=2, crack=True)
        line.taken.append((u - w / 2 - 0.3, u + w / 2 + 0.3, 0.0, 0.55 + h / 2 + 0.3))
        n += 1
    placed = 0
    for k in range(1):
        for _ in range(60):
            size = 0.9 + r.random() * 0.5
            x = room.x0 + size / 2 + 0.2 + r.random() * (room.x1 - room.x0 - size - 0.4)
            z = room.z0 + size / 2 + 0.2 + r.random() * (room.z1 - room.z0 - size - 0.4)
            if all(math.hypot(x - dx, z - dz) >= size / 2 + 0.55 for dx, dz in room.doors):
                S.cur = ('floor', room, None, 'splat', None)
                S.decal(dc.SPLAT.format(1 + int(r.random() * 3)), (x, 0.0, z), (size, size, 0.25), dc.FLOOR_Q,
                        seed + 300 + k, 0, 0.3, tint=SPLAT_TINT, jitter=(180.0, 0.15))
                placed += 1
                break
    return n >= 1 and placed >= 1


def room_queue(room, trash):
    """The features a room can take, in priority order: (kind, arg)."""
    q = []
    hero_ok = room.area >= 10 or bool(room.lamps)
    if hero_ok:
        q.append(('hero', None))
    doors = [(l, u) for l in room.lines for u in l.doors]
    for i, (l, u) in enumerate(doors):
        q.append(('door', (l, u)))
        if i == 0:
            q += [('skirt', None), ('ceil', None)]
    if not doors:
        q += [('skirt', None), ('ceil', None)]
    q += [('skirt', None), ('corner', None), ('skirt', None)]
    if hero_ok and room.area >= 18:
        q.append(('hero', None))
    q += [('skirt', None), ('hero', None), ('corner', None), ('skirt', None), ('hero', None)]
    return q


def dress_unit(S, unit, rooms, trash, use_4k):
    r = random.Random(SEED + unit)
    target = r.randint(56, 64) if trash else r.randint(30, 40)
    mine = sorted((rm for rm in rooms.values() if rm.unit == unit), key=lambda rm: rm.name)
    S.start_unit(unit)
    counter = [0]
    got = {'hero4k': use_4k}

    def attempt(fn, *args):
        """Generate one feature into the buffer; returns (ok, items, wall-space marks to roll back to)."""
        counter[0] += 1
        S.pending, S.cur = [], None
        mark = {id(l): len(l.taken) for m in mine for l in m.lines}
        ok = fn(S, *args, unit * 10000 + counter[0] * 10, trash)
        items, S.pending = S.pending, []
        return ok, items, mark

    def accept(ok, items, mark, limit):
        if ok and items and S.count_cum + len(items) <= limit:
            S.commit(items)
            S.count_cum += len(items)
            return True
        for m in mine:                      # dropped: free the wall space it claimed
            for l in m.lines:
                del l.taken[mark[id(l)]:]
        return False

    S.count_cum = 0
    for rm in mine:                         # bathrooms first: the water damage is not optional
        if rm.name == 'Bathroom':
            assert accept(*attempt(f_bath, rm), 10 ** 9), rm.label
    order = shuffled(r, [rm for rm in mine if rm.name != 'Bathroom'])
    queues = {id(rm): room_queue(rm, trash) for rm in order}
    for rank in range(max(len(q) for q in queues.values())):
        for rm in order:
            if S.count_cum >= target:
                break
            q = queues[id(rm)]
            if rank >= len(q):
                continue
            kind, arg = q[rank]
            if kind == 'hero':
                ok = accept(*attempt(f_hero, rm, got['hero4k']), target + 3)
                got['hero4k'] = got['hero4k'] and not ok
            elif kind == 'door':
                accept(*attempt(f_door, rm, arg[0], arg[1]), target + 3)
            else:
                accept(*attempt({'skirt': f_skirt, 'ceil': f_ceil, 'corner': f_corner}[kind], rm), target + 3)
    return S.count_cum, target


# ---------------------------------------------------------------------------------------------------- self-check
def self_check(d, S, rooms):
    by_id = {e['id']: e for e in d['empties']}
    counts = {'wall': 0, 'ceil': 0, 'floor': 0}
    for eid, (kind, room, line, tag, info) in sorted(S.meta.items()):
        e = by_id[eid]
        assert by_id[e['parentId']]['name'] == f'{room.unit} Wear', (eid, room.label)
        x, y, z = dc.world_pos(by_id, e)
        n = normal_of(e['rotation'])
        w, h, _ = e['Decal']['Size']
        what = f"decal {e['name']} ({room.label}, {tag}) at {x:.3f},{y:.3f},{z:.3f}"
        if kind == 'wall':
            assert math.dist(n, line.n) < 1e-3, f'{what} does not face into its room'
            off, u = (x, z) if line.axis == 'x' else (z, x)
            assert abs(off - line.off) <= 0.02, f'{what} is {abs(off - line.off):.3f} m off its wall plane'
            assert room.inside(x + n[0] * 0.1, z + n[2] * 0.1), f'{what} faces out of the room'
            assert room.inside(x, z, -0.03), f'{what} is outside the ceiling footprint'
            assert line.covers(u, y, 0.0, 0.0, grid=1), f'{what} centre is not on wall (doorway / window?)'
            assert line.covers(u, y, w / 4, h / 4, grid=3), f'{what} spills into an opening'
        elif kind == 'ceil':
            assert n[1] < -0.99 and abs(y - CEIL_Y) <= 0.02, f'{what} is not on the ceiling'
            assert room.inside(x, z, 0.02), f'{what} is outside the ceiling footprint'
            assert room.inside(x - w / 2 + 0.02, z - h / 2 + 0.02) and room.inside(x + w / 2 - 0.02, z + h / 2 - 0.02), \
                f'{what} overhangs the room'
        else:
            assert n[1] > 0.99 and abs(y) <= 0.02, f'{what} is not on the floor'
            assert room.inside(x, z, max(w, h) / 2), f'{what} overhangs the room footprint'
            assert all(math.hypot(x - dx, z - dz) >= max(w, h) / 2 + 0.4 for dx, dz in room.doors), \
                f'{what} is in a doorway'
        counts[kind] += 1
    return counts


def camera_poses(S, rooms):
    """One pose per unit, looking at its biggest hero patch, preferring the room with a lamp."""
    poses = []
    for unit in UNITS:
        cand = [h for h in S.heroes if h[0].unit == unit]
        room, line, u, y, w, h = max(cand, key=lambda c: (bool(c[0].lamps), c[4] * c[5]))
        tx, _, tz = line.pos(u, y)
        far = (room.x1 if line.sign > 0 else room.x0) if line.axis == 'x' else (room.z1 if line.sign > 0 else room.z0)
        stand = min(2.6, abs(far - line.off) - 0.4)
        px, pz = tx + line.n[0] * stand, tz + line.n[2] * stand
        yaw = round(math.degrees(math.atan2(-line.n[2], -line.n[0]))) + 0
        pitch = math.degrees(math.atan2(y - EYE_Y, stand))
        poses.append(f'{unit}:{px:.2f},{EYE_Y},{pz:.2f},{yaw},{pitch:.0f}  ({room.name}{" +lamp" if room.lamps else ""})')
    return poses


def main():
    repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..'))
    path = os.path.join(repo, 'project', 'scenes', 'Apartment Hallway.json')
    with open(path, encoding='utf-8') as f:
        d = json.load(f)
    removed = dc.remove_roots(d, ROOTS)
    by_id, rooms = read_geometry(d)
    mats = patch_materials(d, repo, by_id, rooms)

    S = AptWear(d)
    trashed = sorted(random.Random(SEED).sample(UNITS, 2))
    per_unit = {}
    for unit in UNITS:
        per_unit[unit] = dress_unit(S, unit, rooms, unit in trashed, use_4k=unit in trashed)[0]
    total = sum(per_unit.values())
    assert total <= MAX_DECALS, total

    used = [e['Decal']['Asset'] for e in d['empties'] if 'Decal' in e]       # corridor + apartments
    two_k, four_k = {a for a in used if '_4K' not in a}, {a for a in used if '_4K' in a}
    assert len(two_k) <= MAX_2K and len(four_k) <= MAX_4K, (len(two_k), len(four_k))
    checked = self_check(d, S, rooms)

    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(d, f, indent=2, sort_keys=True)
        f.write('\n')
    print(f'{path}: removed {removed} old entities; patched materials (walls, reveals, ceilings, baseboards) {mats}')
    print(f'{total} decals (' + ', '.join(f'{u}: {n}{" trashed" if u in trashed else ""}' for u, n in per_unit.items())
          + f'); corridor + apartments use {len(two_k)} unique 2K maps, {len(four_k)} 4K, + {len(dc.SUBSTRATES)} substrate sets (a 2K slot each)')
    print(f'self-check OK: {checked["wall"]} wall, {checked["ceil"]} ceiling, {checked["floor"]} floor decals')
    print('camera poses:')
    for p in camera_poses(S, rooms):
        print('  ' + p)


if __name__ == '__main__':
    main()
