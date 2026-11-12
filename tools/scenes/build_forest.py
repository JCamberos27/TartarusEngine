# The Forest: a dense stand of mossy cedars at night, knee-deep in grass, the project's main scene to build out
# from. Layers, from the ground up:
#   - a moss floor, Grass Areas (GrassRenderer: procedural, GPU-scattered blades) over all of it, and a shorter,
#     denser lawn over the clearing where the player starts;
#   - ground cover (clover, weeds, leaf litter, cedar seedlings, moss mats), ferns, bushes, fallen branches, rotten
#     logs, mossy rocks, stumps and mushrooms, gathered round the trunks the way a forest floor grows;
#   - cedars from the SeedMesh pack's full LOD prefabs, packed in clumps with thinner glades between them (spacing
#     from a noise field), in a 120 x 120 m playable square and a wall of more trees round it to the horizon;
#   - the Apartment Hallway's night sky, moon and player, and volumetric fog for depth;
#   - a 4 m precast concrete wall round the playable square (posts every 6 m), dressed in places with graffiti,
#     spalled concrete, soot, painted-out patches, rust and water streaks and a grime band, under sodium wall lamps: the decal
#     test surface (bullet holes are the runtime ones: shoot it). Trees and ground pieces keep clear of it.
#
#   python tools/scenes/build_forest.py [<repo>]
#
# Writes project/scenes/Forest.json (+ .meta, its guid kept across rebuilds).
# World (metres): X east, Z south, the ground at y = 0, the player in the clearing at the middle facing north.
#
# Every prefab is checked before it is used: a LOD Group's levels are its first children in order, and each must be
# an active model - the pack's "Background" tree variants have empty placeholders for their two nearest levels, so
# those trees vanished as the player walked up to them. A prefab that fails is reported and left out.
import json
import math
import os
import random
import secrets
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scene_kit as k  # noqa: E402

HALF = 60.0                        # the playable forest: -HALF..HALF on X and Z
OUTER = 100.0                      # trees carry on to here, past the bounds, so the forest never ends in view
CLEARING = (0.0, 6.0, 8.5)         # centre x, z and radius of the clearing the player starts in
SEEDMESH = 'assets/Environments/SeedMesh_Mossy_Cedrus_Forest/Environment/'
VEG, GO = SEEDMESH + 'Vegetation/', SEEDMESH + 'Ground_objects/Prefabs/'
GROUND = SEEDMESH + 'Terrain_Textures/Mossy_Ground_rocks/M_Mossy_rocks_ground.mat'

TREES = [VEG + f'Cedrus_tree/Prefabs/Cedrus_tree_var{n}.prefab' for n in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15)]
GROUND_COVER = ([VEG + f'Ground_cover_plants/Prefabs/ForestClover_var{n}.prefab' for n in (1, 2, 3)] +
                [VEG + f'Ground_cover_plants/Prefabs/Ground_weed_var{n}.prefab' for n in (1, 2, 3, 4, 5, 6)] +
                [VEG + f'Ground_cover_plants/Prefabs/Ground_Leaves_var{n}.prefab' for n in (1, 2)] +
                [VEG + f'Cedrus_tree/Prefabs/CedrusPlants_cover_var{n}.prefab' for n in (2, 3, 4, 5, 6)])
MOSS = [VEG + f'Moss_cover/Prefabs/Moss_cover_var{n}.prefab' for n in (1, 2, 3, 4)]
FERNS = ([VEG + f'Ferns/Prefabs/Wild_fern_Var{n}.prefab' for n in (1, 2, 3, 4, 5)] +
         [VEG + f'Ferns/Prefabs/Common_fern_var{n}.prefab' for n in (1, 2, 3, 4)] +
         [VEG + f'Ferns/Prefabs/Eagle_fern_var{n}.prefab' for n in (1, 2, 3, 4, 5, 6)])
BUSHES = ([VEG + f'Roldana_plant/Prefabs/Roldana_plant_var{n}.prefab' for n in range(1, 12)] +
          [VEG + f'Oreopanax_Xalapensis/Prefabs/Oreopanax_Xalapensis_var{n} Variant.prefab' for n in range(1, 7)])
DEBRIS = ([VEG + f'Cedrus_tree/Prefabs/Dry_cedrus_debris_Var{n}.prefab' for n in range(1, 8)] +
          [GO + f'Short_rotten_branch_var{n}.prefab' for n in range(1, 6)] +
          [GO + f'Rotten_cedrus_branch_var{n}.prefab' for n in range(1, 6)] +
          [GO + 'Generic_dry_leaves_var1.prefab'])
LOGS = ([GO + f'Mossy_log_var{n}.prefab' for n in (9, 10, 11)] +
        [GO + f'Mossy_rotten_log_var{n}.prefab' for n in (3, 4, 7, 8)] +
        [GO + f'Mossy_Rotten_log_var{n}.prefab' for n in (1, 2)] + [GO + 'Rotten_log_var5.prefab', GO + 'Rotten_log_var6.prefab'])
ROCKS = ([GO + f'Mossy_rock_var{n}.prefab' for n in (1, 2, 5, 6, 7, 8)] +
         [GO + f'Mossy_Rock_var{n}.prefab' for n in (3, 4)] + [GO + 'Volcanic_Mossy_Rocks.prefab'])
STUMPS = [GO + 'Cedrus_stump_1.prefab'] + [GO + f'Tree_stump_var{n}.prefab' for n in (2, 3, 4, 5)]
MUSHROOMS = ([SEEDMESH + f'Mushrooms/Prefabs/{n}_VMushroom.prefab'
              for n in ('Aged', 'Damaged', 'Dark_1', 'Dark_2', 'Red_Aged_1', 'Red_Couple', 'SmallOrange_1', 'SmallOrange_2')])


# LOD switch points (screen-height share where each level hands over; past the last, the piece isn't drawn). The
# pack's own dropped a 1 m fern at ~9 m: here each level holds out far enough that the swap happens in the fog
# (World::LodSmooth cross-fades it), trees never vanish in view, and small plants stay to ~100 m.
LODS = {
    'tree': (1.2, 0.25, 0.09, 0.012),
    'plant': (0.45, 0.16, 0.05, 0.008),
    'small': (0.4, 0.14, 0.04, 0.012),
    'solid': (0.6, 0.22, 0.07, 0.01),
}


def prefab_problem(p):
    """Why a prefab can't be stamped as is, or None: one root; its LOD Group's levels all active models."""
    ents = [e for key in ('empties', 'boxes', 'models') for e in p.get(key, [])]
    roots = [e for e in ents if e['parentId'] == -1]
    if len(roots) != 1:
        return f'{len(roots)} roots'
    root = roots[0]
    if not any('path' in e and e.get('active', True) for e in ents):
        return 'no active model'
    lod = root.get('LOD Group')
    if not lod:
        return None
    levels = sum(1 for key, v in lod.items() if key.startswith('LOD ') and v > 0)
    kids = sorted((e for e in ents if e['parentId'] == root['id']), key=lambda e: (e.get('order', e['id']), e['id']))
    bad = [i for i in range(levels) if i >= len(kids) or 'path' not in kids[i] or not kids[i].get('active', True)]
    return f'LOD levels {bad} are not active models' if bad else None


class Forest(k.Scene):
    def __init__(self, base, repo):
        super().__init__(base)
        self.repo = repo
        self.cache = {}
        self.rejected = {}

    def prefab(self, path):
        if path not in self.cache:
            full = os.path.join(self.repo, 'project', path)
            if not os.path.exists(full):
                self.cache[path] = None
                self.rejected[path] = 'missing'
            else:
                with open(full, encoding='utf-8') as f:
                    p = json.load(f)
                why = prefab_problem(p)
                self.cache[path] = None if why else p
                if why:
                    self.rejected[path] = why
        return self.cache[path]

    def usable(self, paths):
        return [p for p in paths if self.prefab(p)]

    def stamp(self, path, parent, pos, yaw=0.0, scale=1.0, name=None, lod=None):
        """A copy of a .prefab's entities under a new empty at pos (the editor's drop, by hand). New ids follow the
        prefab's own order, so children keep their order - a LOD Group's levels are its children in order."""
        p = self.prefab(path)
        root = self.empty(name or os.path.splitext(os.path.basename(path))[0], parent, pos, yaw=yaw, static=True)
        if scale != 1.0:
            next(e for e in self.empties if e['id'] == root)['scale'] = [round(scale, 3)] * 3
        ents = sorted(((key, e) for key in ('empties', 'boxes', 'models') for e in p.get(key, [])),
                      key=lambda ke: (ke[1].get('order', ke[1]['id']), ke[1]['id']))
        remap = {e['id']: self._next() for _, e in ents}
        for key, e in ents:
            c = json.loads(json.dumps(e))
            c['id'] = c['order'] = remap[e['id']]
            c['parentId'] = root if e['parentId'] == -1 else remap[e['parentId']]
            if lod and e['parentId'] == -1 and 'LOD Group' in c:
                # Our own screen-height switch points for the levels the prefab has (see LODS); its last level
                # always takes the profile's last, so a two-level prefab isn't dropped at a mid-range size.
                n = sum(1 for i in range(4) if c['LOD Group'].get(f'LOD {i}', 0.0) > 0.0)
                points = list(lod[:max(n - 1, 0)]) + [lod[-1]] if n else []
                for i, v in enumerate(points):
                    c['LOD Group'][f'LOD {i}'] = v
            getattr(self, key).append(c)
        return root

    def keep_from(self, base, names):
        """Copy the base scene's root entities named in `names`, with everything under them, ids renumbered."""
        ents = [e for key in ('empties', 'boxes', 'models') for e in base.get(key, [])]
        keep = {e['id'] for e in ents if e['parentId'] == -1 and e['name'] in names}
        grew = True
        while grew:
            grew = False
            for e in ents:
                if e['id'] not in keep and e['parentId'] in keep:
                    keep.add(e['id'])
                    grew = True
        remap = {i: self._next() for i in sorted(keep, key=lambda i: next(e.get('order', i) for e in ents if e['id'] == i))}
        for key in ('empties', 'boxes', 'models'):
            for e in base.get(key, []):
                if e['id'] in keep:
                    c = json.loads(json.dumps(e))
                    c['id'] = c['order'] = remap[e['id']]
                    c['parentId'] = -1 if e['parentId'] == -1 else remap[e['parentId']]
                    getattr(self, key).append(c)


def grass(**kw):
    g = {'Size X': 2 * OUTER, 'Size Z': 2 * OUTER, 'Density': 320.0, 'Height': 0.55, 'Height Variation': 0.45,
         'Width': 0.038, 'Bend': 0.4, 'Base Color': [0.03, 0.06, 0.022], 'Tip Color': [0.21, 0.29, 0.09],
         'Dry Color': [0.28, 0.25, 0.13], 'Color Variation': 0.28, 'Patchiness': 0.6, 'Wind Strength': 0.4,
         'Wind Speed': 0.85, 'Near Distance': 16.0, 'Draw Distance': 60.0, 'Edge Fade': 1.5, 'Seed': 1,
         'Receive Shadows': True}
    g.update(kw)
    return {'Grass Area': g}


def noise2(x, z, seed, scale):
    """Smooth value noise in [0, 1] (the clumping of the trees)."""
    def h(i, j):
        n = (i * 374761393 + j * 668265263 + seed * 2147483647) & 0xffffffff
        n = ((n ^ (n >> 13)) * 1274126177) & 0xffffffff
        return ((n ^ (n >> 16)) & 0xffffff) / 16777215.0
    x, z = x / scale, z / scale
    i, j = math.floor(x), math.floor(z)
    fx, fz = x - i, z - j
    fx, fz = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
    a, b, c, d = h(i, j), h(i + 1, j), h(i, j + 1), h(i + 1, j + 1)
    return (a + (b - a) * fx) * (1 - fz) + (c + (d - c) * fx) * fz


class Grid:
    """A spatial hash for min-distance scatter."""
    def __init__(self, cell=4.0):
        self.cell, self.cells = cell, {}

    def near(self, x, z, r):
        c = self.cell
        for i in range(int((x - r) // c), int((x + r) // c) + 1):
            for j in range(int((z - r) // c), int((z + r) // c) + 1):
                for px, pz, pr in self.cells.get((i, j), ()):
                    if (x - px) ** 2 + (z - pz) ** 2 < (max(r, pr)) ** 2:
                        return True
        return False

    def add(self, x, z, r):
        self.cells.setdefault((int(x // self.cell), int(z // self.cell)), []).append((x, z, r))

# The boundary wall: precast concrete panels between posts, the decal test surface. WALLS: side -> (the axis it sits
# on, its sign); its inner face is WALL_T / 2 in from +-HALF. Trees keep WALL_CLEAR_TREE metres off its line, every
# scattered piece WALL_CLEAR, so nothing pokes through it or hides its decals.
WALLS = {'N': ('z', -1.0), 'S': ('z', 1.0), 'W': ('x', -1.0), 'E': ('x', 1.0)}
WALL_H, WALL_T = 4.0, 0.4
WALL_TINT = (1.3, 1.28, 1.24)    # the floor-concrete maps are dark for a wall
POST_EVERY, POST_W, POST_D, POST_H = 6.0, 0.5, 0.9, 4.3
WALL_CLEAR_TREE, WALL_CLEAR = 2.2, 1.2
_CONCRETE = 'assets/Architecture/Building/Textures/Floor_Concrete/Floor_Concrete_'   # Wall_Concrete's sheet has trim bands
CONCRETE = tuple(_CONCRETE + m + '.png' for m in ('Base_Color', 'Normal', 'Roughness', 'AO'))
D = 'assets/Decals/'
PIECE = D + 'Graffiti/Pieces/DA_Piece_{}.decal'
SHAPE = D + 'Graffiti/Shapes/DA_GraffitiShape_{:03d}.decal'
SPRAY = D + 'Graffiti/Spray/DA_GraffitiSpray_{:03d}.decal'
STAIN = D + 'Stains/Splatters/DA_Stain_{:03d}.decal'
SPLAT = D + 'Stains/LiquidSplatter/DA_LiquidSplatter_{:03d}.decal'
PEEL = D + 'Grime/PeelingPaint/DA_PeelingPaint_{:03d}.decal'
PAINT = {'red': [0.75, 0.07, 0.05], 'yellow': [0.92, 0.7, 0.08], 'white': [0.88, 0.88, 0.85], 'black': [0.04, 0.04, 0.04],
         'blue': [0.08, 0.28, 0.85], 'green': [0.15, 0.65, 0.22], 'pink': [0.92, 0.28, 0.6]}
RUST, GRIME, SPALL, SOOT = [0.3, 0.14, 0.06], [0.08, 0.08, 0.05], [0.07, 0.065, 0.06], [0.02, 0.02, 0.02]
MAX_WALL_DECALS, MAX_WALL_MAPS = 300, 24     # unique maps: 2K slots of the decal texture budget (docs/DECALS.md)


def by_wall(x, z, clear):
    """Is (x, z) within `clear` metres of the boundary wall's line (on either side of it)?"""
    return ((abs(abs(z) - HALF) < clear and abs(x) < HALF + clear) or
            (abs(abs(x) - HALF) < clear and abs(z) < HALF + clear))


class WallDecals:
    """Decals on the boundary wall's inner faces, placed by (side, u, y): u runs along the wall left to right as seen
    from inside, y up from the ground. Each box's +Z points into the forest (the decal projects along -Z)."""
    def __init__(self, s, parent):
        self.s, self.parent, self.maps, self.n = s, parent, set(), 0

    @staticmethod
    def face(side, u, y, out=0.0):
        """(position, yaw) at (u, y) on `side`'s inner face, `out` metres in front of it."""
        axis, sign = WALLS[side]
        d = sign * (HALF - WALL_T / 2 - out)
        if axis == 'z':
            return ((u, y, d), 0.0) if sign < 0 else ((-u, y, d), 180.0)
        return ((d, y, -u), 90.0) if sign < 0 else ((d, y, u), -90.0)

    def put(self, asset, side, u, y, w, h, seed, sort, opacity=1.0, tint=None, spin=0.0, jitter=(0.0, 0.0),
            flip=False, substrate=False):
        pos, yaw = self.face(side, u, y)
        dc = {'Asset': asset, 'Size': [round(w, 3), round(h, 3), 0.25], 'Seed': seed, 'Sort Order': sort,
              'Opacity': round(opacity, 3), 'Affect Static': True, 'Affect Dynamic': False,
              'Rotation Jitter': jitter[0], 'Scale Jitter': jitter[1], 'Random Flip': flip}
        if tint:
            dc['Tint'] = list(tint)
        if substrate:
            dc.update({'Substrate': 'Preset', 'Substrate Preset': D + 'Substrates/SB_Wall_Concrete.substrate'})
        self.n += 1
        self.s.empty(f'Wall Decal {self.n:03d}', self.parent, pos, yaw=yaw, **{'Decal': dc})
        if spin:
            e = self.s.empties[-1]
            e['rotation'] = k.euler_quat(yaw, 0.0, spin)
        self.maps.add(asset)


def panels():
    """Every panel between two posts: (side, u of its middle)."""
    n = int(round(2 * HALF / POST_EVERY))
    return [(side, -HALF + POST_EVERY * (i + 0.5)) for side in WALLS for i in range(n)]


def dress_walls(s, root, repo):
    """The posts, the decals and the lamps on the boundary wall, on a random stream of its own so the forest's
    doesn't move. Returns (decals, lamps)."""
    rng = random.Random(4242)
    concrete = k.textured(repo, *CONCRETE, base=WALL_TINT, triplanar_scale=0.6, roughness=0.9)
    grp = s.empty('Concrete Wall', root, (0, 0, 0), static=True)
    posts = s.empty('Posts', grp, (0, 0, 0), static=True)
    n = int(round(2 * HALF / POST_EVERY))
    for side, (axis, sign) in WALLS.items():
        for i in range(n + 1):
            u = -HALF + POST_EVERY * i
            if axis == 'x' and abs(u) >= HALF:      # the corners are the north and south walls' end posts
                continue
            c = (u, POST_H / 2, sign * HALF) if axis == 'z' else (sign * HALF, POST_H / 2, u)
            size = (POST_W, POST_H, POST_D) if axis == 'z' else (POST_D, POST_H, POST_W)
            s.box('Wall Post', posts, c, size, concrete, static=True)

    dg = s.empty('Wall Decals', grp, (0, 0, 0), static=True)
    w = WallDecals(s, dg)
    span = POST_EVERY - POST_W - 0.5            # what a decal may cover between two posts' faces
    seed = iter(range(7000, 100000))
    # a grime band on every panel: splash-up and damp along the foot, broken up by the map's seed / flip
    for side, u in panels():
        w.put(SPLAT.format(3), side, u, 0.55, span, 1.1, next(seed), 0, rng.uniform(0.35, 0.55), GRIME,
              flip=True, jitter=(0.0, 0.1))

    def chips(side, u, y, spread, count, sort=4):
        """Concrete knocked off in a burst round (u, y): dark spall marks with their rays. (The peeling-paint maps
        don't do here - they reveal concrete, and this is concrete already.)"""
        for _ in range(count):
            a, r = rng.uniform(0, 2 * math.pi), spread * rng.random() ** 0.6
            sz = rng.uniform(0.1, 0.26)
            w.put(STAIN.format(rng.choice((596, 598))), side, u + math.cos(a) * r, y + math.sin(a) * r * 0.7, sz, sz,
                  next(seed), sort, rng.uniform(0.6, 0.85), SPALL, spin=round(rng.uniform(0, 360), 1))

    def streaks(side, u, count, tint=RUST):
        """Rust / water runs down from the top edge."""
        for _ in range(count):
            h = rng.uniform(1.4, 2.6)
            w.put(STAIN.format(594), side, u + rng.uniform(-2.0, 2.0), WALL_H - h / 2, rng.uniform(0.4, 0.7), h,
                  next(seed), 1, rng.uniform(0.6, 0.85), tint)

    # The dressed panels: (side, panel index from the left as seen from inside, kind), a few on every wall.
    sites = [('N', 9, 'piece'), ('N', 4, 'firefight'), ('N', 14, 'buffed'), ('N', 18, 'tags'),
             ('S', 10, 'firefight'), ('S', 6, 'piece'), ('S', 15, 'tags'), ('S', 1, 'stained'),
             ('W', 9, 'buffed'), ('W', 3, 'stained'), ('W', 13, 'firefight'), ('W', 17, 'piece'),
             ('E', 10, 'tags'), ('E', 5, 'firefight'), ('E', 14, 'stained'), ('E', 1, 'buffed')]
    lamps = s.empty('Wall Lamps', grp, (0, 0, 0), static=True)
    housing = k.textured(repo, base=(0.05, 0.05, 0.05), roughness=0.5, metallic=0.8)
    lens = k.textured(repo, base=(1.0, 0.62, 0.3), emissive=(1.0, 0.62, 0.3), strength=6.0)
    pieces = [('Rats', 'red'), ('Crown', None), ('Rats', 'blue')]
    colours = list(PAINT)
    for n_site, (side, i, kind) in enumerate(sites):
        u = -HALF + POST_EVERY * (i + 0.5)
        if kind == 'piece':
            name, colour = pieces[n_site % len(pieces)]
            w.put(PEEL.format(13), side, u + 0.2, 1.7, 4.2, 2.4, next(seed), 1, 0.6, substrate=True)
            w.put(PIECE.format(name), side, u, 1.75, 3.6, 1.8, next(seed), 3, 0.95, PAINT.get(colour))
            w.put(SPRAY.format(16), side, u + 1.4, 0.8, 1.6, 0.8, next(seed), 4, 0.9, PAINT['black'])
            w.put(SHAPE.format(36), side, u - 2.0, 2.3, 0.8, 0.8, next(seed), 5, 0.9, PAINT['white'])
            chips(side, u + 1.6, 2.6, 0.5, 4)
            streaks(side, u, 2)
        elif kind == 'firefight':
            for cu, cy in ((u - 1.2, 1.3), (u + 0.9, 1.6), (u + 0.1, 0.9)):
                chips(side, cu, cy, 0.55, 6)
            w.put(STAIN.format(595), side, u - 0.3, 1.2, 1.8, 1.8, next(seed), 2, 0.6, SOOT,
                  spin=round(rng.uniform(0, 360), 1))
            w.put(SHAPE.format(54), side, u + 1.8, 2.4, 0.7, 0.7, next(seed), 3, 0.9, PAINT['red'])
        elif kind == 'buffed':
            # tags painted out in grey, newer tags over that
            w.put(SPRAY.format(6), side, u - 0.8, 1.5, 2.2, 1.0, next(seed), 1, 0.9, PAINT['green'])
            w.put(SHAPE.format(32), side, u + 1.0, 1.7, 1.0, 1.0, next(seed), 1, 0.9, PAINT['pink'])
            w.put(PEEL.format(7), side, u, 1.6, 4.0, 2.2, next(seed), 2, 0.75, [0.55, 0.55, 0.53], substrate=True)
            w.put(SHAPE.format(46), side, u - 1.4, 1.9, 0.9, 0.9, next(seed), 4, 0.95, PAINT['black'])
            w.put(SPRAY.format(21), side, u + 1.3, 1.4, 0.9, 1.0, next(seed), 4, 0.95, PAINT['yellow'])
            streaks(side, u, 2, GRIME)
        elif kind == 'tags':
            for j, (shape, du, y, sz) in enumerate(((49, -1.8, 2.2, 1.0), (36, 0.0, 1.6, 0.9), (54, 1.6, 1.9, 0.7),
                                                     (32, 0.9, 0.9, 0.8))):
                w.put(SHAPE.format(shape), side, u + du, y, sz, sz, next(seed), 2 + j, 0.95,
                      PAINT[colours[(n_site + j) % len(colours)]], spin=round(rng.uniform(-8, 8), 1))
            w.put(SPRAY.format(33), side, u - 0.6, 0.9, 1.0, 1.2, next(seed), 1, 0.9, PAINT['blue'])
            w.put(SPRAY.format(36), side, u + 2.2, 2.6, 0.4, 1.4, next(seed), 6, 0.9, PAINT['red'])
            chips(side, u - 1.0, 2.9, 0.4, 3)
        elif kind == 'stained':
            streaks(side, u, 6)
            w.put(STAIN.format(595), side, u - 1.2, 0.7, 1.2, 1.2, next(seed), 2, 0.6, RUST,
                  spin=round(rng.uniform(0, 360), 1))
            chips(side, u + 1.5, 1.1, 0.45, 4)
        # the lamp over it: a dark housing on the wall, a glowing lens, a sodium spot down the face
        pos, yaw = WallDecals.face(side, u, 3.65, out=0.15)
        lamp = s.empty('Wall Lamp', lamps, pos, yaw=yaw, static=True)
        s.box('Lamp Housing', lamp, (0.0, 0.0, 0.0), (0.45, 0.16, 0.3), housing, collider=False, static=True)
        s.box('Lamp Lens', lamp, (0.0, -0.085, 0.02), (0.36, 0.02, 0.22), lens, collider=False, static=True)
        s.light('Lamp Light', lamp, (0.0, -0.12, 0.25), intensity=7.0, rng=9.0, kelvin=2100.0, spot=72.0,
                pitch=-65.0)
    s.empty('Decal Grading', grp, (0.0, 0.0, 0.0), **{'Decal Grading': {'Age': 0.25, 'Wear': 0.3}})
    assert w.n <= MAX_WALL_DECALS and len(w.maps) <= MAX_WALL_MAPS, (w.n, len(w.maps))
    return w.n, len(sites)


def build(repo):
    with open(os.path.join(repo, 'tests', 'scenes', 'Apartment Hallway.json'), encoding='utf-8') as f:
        base = json.load(f)
    s = Forest(base, repo)
    s.keep_from(base, {'Moon', 'Player Spawn'})
    d = s.data
    d['sky'].update({'seaLevel': 0.0, 'clouds': True, 'cloudCoverage': 0.3, 'cloudType': 0.35, 'cloudQuality': 0,
                     'cloudShadows': False, 'cloudWindSpeed': 4.0, 'moonBrightness': 0.0164, 'nightBrightness': 4.4})
    # Volumetric fog for the depth between the trunks, not the distance fog (outdoors only - the rule is no fog
    # indoors). The distance fog's settings stay for when it's switched back on.
    d.update({'shadowDistance': 45.0, 'maxSpotShadows': 8, 'fogEnabled': False, 'fogMode': 3, 'fogDensity': 0.02,
              'fogColor': [0.03, 0.038, 0.052], 'fogHeightFalloff': 0.06, 'fogBaseHeight': 0.0,
              'volumetricEnabled': True})
    rng = random.Random(11)
    cx, cz, cr = CLEARING
    counts, dropped = {}, {}

    root = s.empty('Forest', -1, (0, 0, 0), static=True)
    ground = k.from_mat(repo, GROUND, triplanar=True, triplanarScale=0.25)
    s.box('Forest Floor', root, (0.0, -0.2, 0.0), (2 * OUTER + 40.0, 0.4, 2 * OUTER + 40.0), ground, static=True)
    concrete = k.textured(repo, *CONCRETE, base=WALL_TINT, triplanar_scale=0.6, roughness=0.9)
    for side, (axis, sign) in WALLS.items():
        c = (0.0, WALL_H / 2, sign * HALF) if axis == 'z' else (sign * HALF, WALL_H / 2, 0.0)
        size = (2 * HALF, WALL_H, WALL_T) if axis == 'z' else (WALL_T, WALL_H, 2 * HALF)
        s.box(f'Concrete Wall {side}', root, c, size, concrete, static=True)

    g = s.empty('Grass', root, (0, 0, 0), static=True)
    s.empty('Forest Grass', g, (0.0, 0.0, 0.0), static=True, **grass())
    s.empty('Clearing Grass', g, (cx, 0.002, cz), static=True,
            **grass(**{'Size X': 2 * cr, 'Size Z': 2 * cr, 'Density': 420.0, 'Height': 0.36, 'Patchiness': 0.3,
                       'Seed': 2, 'Tip Color': [0.24, 0.33, 0.1], 'Edge Fade': 3.0}))

    # --- cedars: clumps and glades. Spacing runs 3.2 m in a clump to 9 m in a glade, from a noise field; the
    # clearing stays open, with a softer edge of saplings-size trees round it.
    trees = s.usable(TREES)
    grid = Grid(4.0)
    tree_pts = []
    grp = s.empty('Cedars', root, (0, 0, 0), static=True)
    for _ in range(60000):
        x, z = rng.uniform(-OUTER, OUTER), rng.uniform(-OUTER, OUTER)
        dc = math.hypot(x - cx, z - cz)
        if dc < cr + 1.5:
            continue
        clump = noise2(x, z, 3, 22.0)
        spacing = 3.2 + 5.8 * (1.0 - clump) ** 1.5
        if grid.near(x, z, spacing):
            continue
        grid.add(x, z, spacing)
        edge = dc < cr + 6.0
        scale = rng.uniform(0.55, 0.8) if edge else rng.uniform(0.8, 1.25)
        path, yaw = rng.choice(trees), round(rng.uniform(0, 360), 1)
        if by_wall(x, z, WALL_CLEAR_TREE):
            continue
        tree_pts.append((x, z, scale))
        s.stamp(path, grp, (round(x, 2), 0.0, round(z, 2)), yaw=yaw, scale=scale, name='Cedar', lod=LODS['tree'])
    counts['cedars'] = len(tree_pts)
    inside = [(x, z, sc) for x, z, sc in tree_pts if abs(x) < HALF + 6 and abs(z) < HALF + 6]

    def scatter(name, paths, n, near_tree, r_min, r_max, scale, avoid=1.0, spread=HALF + 4.0, clearing_ok=False,
                lod='plant'):
        """`n` copies of `paths`: around a random trunk (between r_min and r_max of it) with probability
        `near_tree`, else anywhere; kept off the clearing (unless clearing_ok) and `avoid` metres apart."""
        group = s.empty(name, root, (0, 0, 0), static=True)
        usable = s.usable(paths)
        if not usable:
            return 0
        local = Grid(2.0)
        placed = 0
        for _ in range(n * 8):
            if placed >= n:
                break
            if inside and rng.random() < near_tree:
                tx, tz, _sc = rng.choice(inside)
                a, r = rng.uniform(0, 2 * math.pi), rng.uniform(r_min, r_max)
                x, z = tx + math.cos(a) * r, tz + math.sin(a) * r
            else:
                x, z = rng.uniform(-spread, spread), rng.uniform(-spread, spread)
            if abs(x) > spread or abs(z) > spread:
                continue
            dc = math.hypot(x - cx, z - cz)
            if not clearing_ok and dc < cr - 0.5:
                continue
            if avoid > 0 and local.near(x, z, avoid):
                continue
            if avoid > 0:
                local.add(x, z, avoid)
            path, yaw, sc = rng.choice(usable), round(rng.uniform(0, 360), 1), rng.uniform(*scale)
            placed += 1     # counted even when the wall strip drops it, so the draws after it don't shift
            if by_wall(x, z, WALL_CLEAR):
                dropped[name] = dropped.get(name, 0) + 1
                continue
            s.stamp(path, group, (round(x, 2), 0.0, round(z, 2)), yaw=yaw, scale=sc, lod=LODS[lod])
        counts[name] = placed - dropped.get(name, 0)
        return placed

    scatter('Moss Mats', MOSS, 260, 0.8, 0.5, 3.5, (0.8, 1.4), avoid=1.2)
    scatter('Ground Cover', GROUND_COVER, 1400, 0.55, 0.6, 7.0, (0.8, 1.3), avoid=0.6, clearing_ok=True)
    scatter('Ferns', FERNS, 1100, 0.75, 0.8, 6.0, (0.75, 1.3), avoid=0.8)
    scatter('Bushes', BUSHES, 260, 0.6, 1.5, 7.0, (0.7, 1.2), avoid=2.2)
    scatter('Debris', DEBRIS, 420, 0.7, 0.5, 6.0, (0.8, 1.2), avoid=0.8, clearing_ok=True, lod='small')
    scatter('Logs', LOGS, 45, 0.4, 2.0, 8.0, (0.8, 1.15), avoid=6.0, lod='solid')
    scatter('Rocks', ROCKS, 70, 0.3, 1.5, 8.0, (0.6, 1.3), avoid=3.5, lod='solid')
    scatter('Stumps', STUMPS, 35, 0.2, 2.0, 8.0, (0.8, 1.2), avoid=5.0, lod='solid')
    scatter('Mushrooms', MUSHROOMS, 160, 0.9, 0.4, 1.6, (0.8, 1.4), avoid=0.4, lod='small')

    counts['wall decals'], counts['wall lamps'] = dress_walls(s, root, repo)

    # The player in the clearing, facing north into the trees
    for e in s.empties:
        if e['parentId'] == -1 and 'First Person Controller' in e:
            e['position'] = [cx, 0.0, cz + 4.0]
            e['rotation'] = k.euler_quat(0.0)
    return s, counts


def write_meta(path):
    meta = path + '.meta'
    if os.path.exists(meta):
        return
    with open(meta, 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'guid': secrets.token_hex(8), 'metaVersion': 1, 'type': 'scene'}, f, indent=2)
        f.write('\n')


def main():
    repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..'))
    s, counts = build(repo)
    out = os.path.join(repo, 'project', 'scenes', 'Forest.json')
    s.write(out)
    write_meta(out)
    for path, why in sorted(s.rejected.items()):
        print(f'build_forest: left out {path}: {why}')
    print('build_forest: ' + ', '.join(f'{v} {k_}' for k_, v in counts.items()) +
          f'; {len(s.boxes)} boxes, {len(s.models)} models, {len(s.empties)} empties -> {out}')


if __name__ == '__main__':
    main()
