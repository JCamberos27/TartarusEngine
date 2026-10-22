# Dresses the Apartment Hallway's corridor as a derelict squat: peeling, flaking plaster, water-stained ceiling, dark
# splashes by the doorways. It patches project/scenes/Apartment Hallway.json IN PLACE (the scene has hand edits that
# build_apartment_hallway.py doesn't make, e.g. the 301 test room, so never rebuild it over the top):
#
#   python -I tools/scenes/dress_corridor.py [<repo>]
#
#  * the embedded materials of the corridor walls / ceiling / baseboards get the beige plaster's albedo + AO and a
#    peeling-paint grunge map (the same values build_apartment_hallway.py's corridor_* materials use), and a
#    "substrate" block (the PBR material that shows where paint is missing): Wall_Concrete on the walls,
#    Floor_Concrete on the ceiling, PH_Dark_Wooden_Planks on the baseboards (presets, SUBSTRATES below);
#  * a seeded, layered set of decals goes under a "Corridor Wear" root: per patch a big soft base, a peel layer, then
#    small sharp flakes and a crack on top. Only the DA_PeelingPaint_* sets (the tile / board-grid ones are skipped) and
#    a few DA_LiquidSplatter_* floor splashes. Asserted limits: <= 80 decals, <= 22 unique 2K .decal maps, <= 1 4K.
#    Ceiling decals carry a Preset substrate override (Floor_Concrete); wall / skirting decals stay on Auto, so they
#    show the substrate of whatever they land on (concrete on the wall, wood on the baseboard);
#  * a "Decal Grading" entity (Age 0.35, Wear 0.4) under that root.
#
# Idempotent: it first removes the "Decal Lab" (the old demo), "Corridor Wear" and "Apartment Wear" subtrees (run
# dress_apartments.py after it; removing the apartments' keeps the entity ids reproducible), then adds fresh ones, so a
# second run leaves the file byte-identical. Decal textures are Drive assets (docs/DECALS.md); without them the
# decals just don't draw.
#
# Corridor (world; Third Floor offset applied): x -10.6..10.6, wall faces z = -1.0 (north, +Z out) and z = +1.0
# (south, yaw 180); end walls x = -10.45 (yaw +90) / +10.45 (yaw -90) with a window cut-out y 0.75..2.42 around z 0;
# ceiling y 2.7. Doors (0.92 wide, header 2.38): north x -6.9 -1.0 5.9; south x -5.9 3.4 9.3. Dead lamp at x ~ 5.3.
import json
import math
import os
import random
import sys

ROOTS = ('Decal Lab', 'Corridor Wear', 'Apartment Wear')   # dress_apartments.py re-adds the last (it runs after)
MAX_DECALS, MAX_2K, MAX_4K = 80, 22, 1
D = 'assets/Decals/'
PEEL = D + 'Grime/PeelingPaint/DA_PeelingPaint_{:03d}{}.decal'
SPLAT = D + 'Stains/LiquidSplatter/DA_LiquidSplatter_{:03d}.decal'

# Peel map tags (from the contact sheets); the tile / board grids (003 011 017 022 071 073 077 080 084 092) are skipped.
# A subset of each tag keeps the unique-map count under the limit.
POOL = {'blotch': [7, 9, 13, 26], 'hole': [23, 28, 31, 39], 'strip': [1, 4, 8, 12], 'crack': [5, 18, 27],
        'flake': [2, 6, 14, 16]}
HERO_4K = 7     # the one 4K map: a blotch

# Materials (project-relative; guids come from the .meta files). Must match build_apartment_hallway.py.
WALL_DIR = 'assets/Architecture/Building/Textures/PH_Beige_Wall/beige_wall_001_'
GRUNGE_WALL = D + 'Grime/PeelingPaint/T_Grunge_PeelingPaint_036.png'
GRUNGE_WORN = D + 'Grime/PeelingPaint/T_Grunge_PeelingPaint_085.png'

# Substrate presets (what shows where paint is missing): .substrate files under assets/Decals/Substrates, which
# materials and decals reference by path instead of embedding the maps and values.
# The skirting band sits just above the 9 cm baseboards, lapping only their top ~3 cm: run down over them, its
# dark peel read as a black-and-white camo strip. The baseboards' own chipping is their material Grunge.
SKIRT_Y = 0.43          # the band's centre (0.75 tall, bottom at 0.055)
SKIRT_HOLE_Y = 0.33     # the extra hole beside it (0.55 tall)

SUBSTRATES = {
    'wall': 'assets/Decals/Substrates/SB_Wall_Concrete.substrate',
    'ceil': 'assets/Decals/Substrates/SB_Floor_Concrete.substrate',
    'trim': 'assets/Decals/Substrates/SB_Dark_Wooden_Planks.substrate',
}


def quat(yaw=0.0, pitch=0.0):
    """Yaw about +Y then pitch about +X, as [x, y, z, w]."""
    cy, sy = math.cos(math.radians(yaw) / 2), math.sin(math.radians(yaw) / 2)
    cp, sp = math.cos(math.radians(pitch) / 2), math.sin(math.radians(pitch) / 2)
    return [round(cy * sp, 6), round(sy * cp, 6), round(-sy * sp, 6), round(cy * cp, 6)]


# wall -> (rotation, fixed axis, fixed value); 'u' runs along x on the north / south walls, along z on the ends
WALLS = {'N': (quat(0.0), 'z', -1.0), 'S': (quat(180.0), 'z', 1.0),
         'W': (quat(90.0), 'x', -10.45), 'E': (quat(-90.0), 'x', 10.45)}
CEIL_Q = quat(0.0, 90.0)     # +Z (out of the surface) -> world -Y
FLOOR_Q = quat(0.0, -90.0)   # -> world +Y


def children(d, roots):
    gone = set(roots)
    grew = True
    while grew:
        grew = False
        for k in ('boxes', 'models', 'empties'):
            for e in d[k]:
                if e['parentId'] in gone and e['id'] not in gone:
                    gone.add(e['id'])
                    grew = True
    return gone


def remove_roots(d, names=ROOTS):
    roots = {e['id'] for k in ('boxes', 'models', 'empties') for e in d[k]
             if e['name'] in names and e['parentId'] == -1}
    gone = children(d, roots)
    for k in ('boxes', 'models', 'empties'):
        d[k] = [e for e in d[k] if e['id'] not in gone]
    return len(gone)


def world_pos(by_id, e):
    x, y, z = e['position']
    while e['parentId'] in by_id:
        e = by_id[e['parentId']]
        assert abs(e['rotation'][3] - 1.0) < 1e-6, f"rotated parent {e['name']}: world maths assumes none"
        x, y, z = x + e['position'][0], y + e['position'][1], z + e['position'][2]
    return x, y, z


def ref(repo, path):
    with open(os.path.join(repo, 'project', path + '.meta'), encoding='utf-8') as f:
        return {'path': path, 'pathGuid': json.load(f)['guid']}


def substrate_block(repo, key):
    """The material "substrate" block for SUBSTRATES[key]: a reference to its preset."""
    assert os.path.isfile(os.path.join(repo, 'project', SUBSTRATES[key])), SUBSTRATES[key]
    return {'preset': SUBSTRATES[key]}


def substrate_override(key):
    """The decal component keys for a Preset substrate override with SUBSTRATES[key]."""
    return {'Substrate': 'Preset', 'Substrate Preset': SUBSTRATES[key]}


def corridor_trims(d, by_id):
    """The baseboards along the corridor (the apartments' are every other 'Baseboard')."""
    trims = []
    for e in d['boxes']:
        if e['name'] == 'Baseboard':
            x, _, z = world_pos(by_id, e)
            if abs(abs(z) - 0.994) < 0.01 or (abs(abs(x) - 10.444) < 0.01 and abs(z) < 1.0):
                trims.append(e)
    return trims


def patch_materials(d, repo):
    """The corridor walls / ceiling / baseboards (by name and world position). Returns (walls, ceilings, trims)."""
    by_id = {e['id']: e for k in ('boxes', 'models', 'empties') for e in d[k]}
    walls = [e for e in d['models'] if e['name'] == 'Wall - Corridor']
    ceils = [e for e in d['models'] if e['name'] == 'Ceiling - Corridor']
    trims = corridor_trims(d, by_id)
    assert len(walls) == 22 and len(ceils) == 1 and len(trims) == 10, (len(walls), len(ceils), len(trims))
    for e in walls:
        m = e['materials'][0]['embedded']
        m['albedoMap'] = ref(repo, WALL_DIR + 'Diffuse_2k.jpg')
        m['aoMap'] = ref(repo, WALL_DIR + 'AO_2k.jpg')
        m['baseColor'] = [0.92, 0.9, 0.86]
        m['grungeMap'] = ref(repo, GRUNGE_WALL)
        m['grungeStrength'] = 0.35
        m['grungeScale'] = 0.25
        m['substrate'] = substrate_block(repo, 'wall')
    for e in ceils:
        m = e['materials'][0]['embedded']
        m['grungeMap'] = ref(repo, GRUNGE_WORN)
        m['grungeStrength'] = 0.45
        m['grungeScale'] = 0.25
        m['substrate'] = substrate_block(repo, 'ceil')
    for e in trims:
        m = e['materials'][0]['embedded']
        m['grungeMap'] = ref(repo, GRUNGE_WALL)    # _085's peel mask is near binary: any strength opened the same
        m['grungeStrength'] = 0.4                   # black blobs; _036 grades, so this is light chipping
        m.pop('grungeScale', None)    # default 0.5
        m['substrate'] = substrate_block(repo, 'trim')
    return len(walls), len(ceils), len(trims)


class Wear:
    MIDWALL_STRIP = 0.5     # chance a non-skirting patch's peel layer is a strip (else a hole)

    def __init__(self, d, root='Corridor Wear'):
        self.d = d
        self.next = 1 + max(e['id'] for k in ('boxes', 'models', 'empties') for e in d[k])
        self.root = self.empty(root, -1, (0.0, 0.0, 0.0))
        self.decals = []

    def empty(self, name, parent, pos, rot=(0.0, 0.0, 0.0, 1.0), **comps):
        i = self.next
        self.next += 1
        e = {'id': i, 'name': name, 'order': i, 'parentId': parent, 'position': [round(v, 4) for v in pos],
             'rotation': list(rot), 'scale': [1.0, 1.0, 1.0]}
        e.update(comps)
        self.d['empties'].append(e)
        return i

    def decal(self, asset, pos, size, rot, seed, sort, opacity=1.0, tint=None, jitter=(0.0, 0.0), flip=True,
              substrate=None):
        dc = {'Asset': asset, 'Size': [round(v, 3) for v in size], 'Seed': seed, 'Sort Order': sort,
              'Opacity': round(opacity, 3), 'Affect Static': True, 'Affect Dynamic': True,
              'Rotation Jitter': jitter[0], 'Scale Jitter': jitter[1], 'Random Flip': flip}
        if tint:
            dc['Tint'] = list(tint)
        if substrate:
            dc.update(substrate_override(substrate))     # 'wall' / 'ceil' / 'trim'; omitted = Auto (the receiver's)
        self.empty(f'Wear {len(self.decals) + 1:02d}', self.root, pos, rot, Decal=dc)
        self.decals.append(asset)

    def pick(self, r, tag):
        return PEEL.format(POOL[tag][int(r.random() * len(POOL[tag]))], '')

    def spot(self, wall, u, y, off=0.0):
        rot, axis, v = WALLS[wall]
        return ((u, y, v) if axis == 'z' else (v, y, u)), rot

    def patch(self, wall, u, y, w, h, seed, hero=False, flakes=2, crack=True, skirt=False):
        """One layered patch on a wall: soft base, peel layer, small sharp flakes, a crack on top."""
        r = random.Random(seed)
        j = (0.0, 0.1) if skirt else (8.0, 0.1)
        base = PEEL.format(HERO_4K, '_4K') if hero else self.pick(r, 'blotch')
        pos, rot = self.spot(wall, u, y)
        self.decal(base, pos, (w, h, 0.25), rot, seed, 0, 0.75 if skirt else 0.55, jitter=j)
        peel_tag = 'strip' if (skirt or r.random() < self.MIDWALL_STRIP) else 'hole'
        pos, rot = self.spot(wall, u + (r.random() - 0.5) * 0.2 * w, y + (r.random() - 0.5) * 0.1 * h)
        self.decal(self.pick(r, peel_tag), pos, (w * 0.75, h * 0.75, 0.25), rot, seed + 1, 1, 0.85, jitter=j)
        for i in range(flakes):
            s = 0.3 + r.random() * 0.3
            pos, rot = self.spot(wall, u + (r.random() - 0.5) * 0.7 * w, y + (r.random() - 0.5) * 0.7 * h)
            self.decal(self.pick(r, 'flake'), pos, (s, s, 0.25), rot, seed + 2 + i, 2 + i, 0.9, jitter=(180.0, 0.15))
        if crack:
            s = 0.45 + r.random() * 0.3
            pos, rot = self.spot(wall, u + (r.random() - 0.5) * 0.4 * w, y + (r.random() - 0.5) * 0.4 * h)
            self.decal(self.pick(r, 'crack'), pos, (s, s, 0.25), rot, seed + 9, 6, 0.8, jitter=(180.0, 0.15))

    def ceiling(self, x, z, w, h, seed, tags):
        r = random.Random(seed)
        for i, (tag, scale, op) in enumerate(tags):
            ox, oz = (r.random() - 0.5) * 0.4 * w, (r.random() - 0.5) * 0.4 * h
            self.decal(self.pick(r, tag), (x + ox, 2.7, z + oz), (w * scale, h * scale, 0.25), CEIL_Q, seed + i, i, op,
                       jitter=(180.0, 0.12), substrate='ceil')


def build(d):
    s = Wear(d)
    # skirting band: heavy peeling low on the wall between the doors
    for n, (wall, u) in enumerate([('N', -8.9), ('N', -4.0), ('N', 2.4), ('N', 8.3), ('S', -8.4), ('S', -1.2),
                                   ('S', 6.3)]):
        s.patch(wall, u, SKIRT_Y, 2.2, 0.75, 1000 + 20 * n, flakes=0, crack=False, skirt=True)
        pos, rot = s.spot(wall, u + (0.6 if n % 2 else -0.6), SKIRT_HOLE_Y)
        r = random.Random(1000 + 20 * n + 5)
        s.decal(s.pick(r, 'hole'), pos, (0.8, 0.55, 0.25), rot, 1000 + 20 * n + 5, 3, 0.9, jitter=(180.0, 0.15))
    # door surrounds: a crack along the header and flaking beside the jamb
    for n, (wall, x) in enumerate([('N', -6.9), ('N', -1.0), ('N', 5.9), ('S', -5.9), ('S', 3.4), ('S', 9.3)]):
        r = random.Random(2000 + n)
        side = 1 if n % 2 else -1
        pos, rot = s.spot(wall, x + (r.random() - 0.5) * 0.3, 2.5)
        s.decal(s.pick(r, 'crack'), pos, (1.1, 0.5, 0.25), rot, 2000 + n, 4, 0.85, jitter=(0.0, 0.1))
        pos, rot = s.spot(wall, x + side * 0.62, 1.3 + r.random() * 0.5)
        s.decal(s.pick(r, 'flake'), pos, (0.5, 0.5, 0.25), rot, 2100 + n, 5, 0.9, jitter=(180.0, 0.15))
    # hero patches mid-wall, two a side; the first is the one 4K map
    for n, (wall, u, y, w, h) in enumerate([('N', 2.5, 1.35, 2.4, 2.1), ('N', -3.5, 1.55, 2.0, 1.7),
                                             ('S', -2.0, 1.45, 2.1, 1.8), ('S', 6.2, 1.25, 2.0, 1.6)]):
        s.patch(wall, u, y, w, h, 3000 + 40 * n, hero=(n == 0), flakes=1, crack=True)
    # ceiling: water damage round the dead lamp, and in the corners
    s.ceiling(5.3, 0.0, 1.7, 1.7, 4000, [('blotch', 1.0, 0.6), ('strip', 0.8, 0.8), ('flake', 0.3, 0.9),
                                         ('flake', 0.25, 0.9), ('crack', 0.4, 0.8)])
    s.ceiling(-9.6, -0.5, 1.5, 1.5, 4100, [('blotch', 1.0, 0.55), ('crack', 0.5, 0.8)])
    s.ceiling(9.6, 0.5, 1.5, 1.5, 4200, [('blotch', 1.0, 0.55), ('flake', 0.4, 0.9)])
    # end walls: round the window cut-outs, on the side panels (z +-0.72)
    for n, (wall, z) in enumerate([('W', -0.72), ('W', 0.72), ('E', -0.72), ('E', 0.72)]):
        r = random.Random(5000 + n)
        pos, rot = s.spot(wall, z, 1.5)
        s.decal(s.pick(r, 'blotch'), pos, (0.6, 2.0, 0.25), rot, 5000 + n, 0, 0.55, jitter=(0.0, 0.1))
        pos, rot = s.spot(wall, z, 0.5 + r.random())
        s.decal(s.pick(r, 'strip'), pos, (0.55, 1.2, 0.25), rot, 5100 + n, 1, 0.8, jitter=(0.0, 0.1))
        pos, rot = s.spot(wall, z, 2.0 + r.random() * 0.4)
        s.decal(s.pick(r, 'flake'), pos, (0.35, 0.35, 0.25), rot, 5200 + n, 2, 0.9, jitter=(180.0, 0.15))
    # dark splashes on the floor by the doorways
    for n, (x, z, size) in enumerate([(-6.9, -0.6, 1.1), (-1.0, -0.55, 0.9), (3.4, 0.6, 1.2), (9.3, 0.55, 1.0)]):
        s.decal(SPLAT.format(1 + n % 3), (x, 0.0, z), (size, size, 0.25), FLOOR_Q, 6000 + n, 0, 0.45,
                tint=[0.2, 0.11, 0.07], jitter=(180.0, 0.15))
    s.empty('Decal Grading', s.root, (0.0, 0.0, 0.0), **{'Decal Grading': {'Age': 0.35, 'Wear': 0.4}})

    two_k = {a for a in s.decals if '_4K' not in a}
    four_k = {a for a in s.decals if '_4K' in a}
    assert len(s.decals) <= MAX_DECALS, len(s.decals)
    assert len(two_k) <= MAX_2K and len(four_k) <= MAX_4K, (len(two_k), len(four_k))
    return len(s.decals), len(two_k), len(four_k)


def main():
    repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..'))
    path = os.path.join(repo, 'project', 'scenes', 'Apartment Hallway.json')
    with open(path, encoding='utf-8') as f:
        d = json.load(f)
    removed = remove_roots(d)
    mats = patch_materials(d, repo)
    n, two_k, four_k = build(d)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(d, f, indent=2, sort_keys=True)
        f.write('\n')
    print(f'{path}: removed {removed} old entities; patched materials (walls, ceilings, baseboards) {mats}; '
          f'{n} decals, {two_k} unique 2K maps, {four_k} 4K, + {len(SUBSTRATES)} substrate sets (a 2K slot each)')


if __name__ == '__main__':
    main()
