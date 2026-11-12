# The Forest: a dense stand of mossy cedars at night, the project's main scene to build out from. Layers, from the
# ground up:
#   - a floor of soil and needles, and Grass Areas (GrassRenderer: procedural, GPU-scattered blades) only where the
#     canopy opens: the clearing where the player starts and the glades;
#   - scanned patches of real forest floor in drifts, and low root mounds, over the flat floor;
#   - logs, rocks and stumps, then what gathers round them: moss, ferns in stands of one species, mushrooms;
#     ground cover in patches, flowers and tall grass in the glades, bushes, fallen branches and lichen;
#   - deadfall: trunks lying out from the stumps they broke from, their crowns a litter of branches; fungi on the
#     dead wood; now and then a leaning cedar;
#   - cedars from the SeedMesh pack's full LOD prefabs, packed in clumps with thinner glades between them (spacing
#     from a noise field), in a 120 x 120 m playable square and a wall of more trees round it to the horizon;
#     most of them with the moss, fern and vine cover the pack grew up that variant's trunk;
#   - the Apartment Hallway's night sky, moon and player, and volumetric fog for depth;
#   - a 4 m precast concrete wall round the playable square (posts every 6 m), dressed in places with spalled
#     concrete, soot, old paint patches, rust and water streaks and a grime band, under sodium wall lamps: the decal
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
GROUND = SEEDMESH + 'Terrain_Textures/Dirt/M_Dirt_forest_mat.mat'     # soil, needles and twigs

TREES = [VEG + f'Cedrus_tree/Prefabs/Cedrus_tree_var{n}.prefab' for n in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15)]
GROUND_COVER = ([VEG + f'Ground_cover_plants/Prefabs/ForestClover_var{n}.prefab' for n in (1, 2, 3)] +
                [VEG + f'Ground_cover_plants/Prefabs/Ground_weed_var{n}.prefab' for n in (1, 2, 3, 4, 5, 6)] +
                [VEG + f'Ground_cover_plants/Prefabs/Ground_Leaves_var{n}.prefab' for n in (1, 2)])
# The pack's CedrusPlants_cover_varN is moss, ferns and vines grown up one cedar variant's trunk (4-8 m tall; its
# Cedrus_tree_varN_PC prefab is the pair): it goes on that tree at the tree's own transform, never on open ground.
TREE_COVER = {n: VEG + f'Cedrus_tree/Prefabs/CedrusPlants_cover_var{n}.prefab' for n in (2, 3, 4, 5, 6)}
TREE_COVER_SHARE = 0.8    # (its models' .metas map it to the _Tree materials, whose wind bends with the trunk)
FLOWERS = [VEG + f'Flower_Weeds/Prefabs/Ground_flower_weed_var{n} Variant.prefab' for n in range(1, 6)]
WILD_GRASS = [VEG + f'Grass/Prefabs/Wild_forest_grass_var{n}.prefab' for n in (1, 2, 3)]
MOSS = [VEG + f'Moss_cover/Prefabs/Moss_cover_var{n}.prefab' for n in (1, 2, 3, 4)]
# Ferns grow in stands of one kind: each list is a species, and a patch draws from one.
FERN_SPECIES = [[VEG + f'Ferns/Prefabs/Wild_fern_Var{n}.prefab' for n in (1, 2, 3, 4, 5)],
                [VEG + f'Ferns/Prefabs/Common_fern_var{n}.prefab' for n in (1, 2, 3, 4)],
                [VEG + f'Ferns/Prefabs/Eagle_fern_var{n}.prefab' for n in (1, 2, 3, 4, 5, 6)],
                [VEG + f'Ferns/Prefabs/Cretan_brake_fern_var{n}.prefab' for n in (1, 2, 3, 4, 5)]]
BUSHES = ([VEG + f'Roldana_plant/Prefabs/Roldana_plant_var{n}.prefab' for n in range(1, 12)] +
          [VEG + f'Roldana_plant/Prefabs/Roldana_plant_var{n} Variant.prefab' for n in (12, 13, 14)] +
          [VEG + f'Oreopanax_Xalapensis/Prefabs/Oreopanax_Xalapensis_var{n} Variant.prefab' for n in range(1, 7)])
DEBRIS = ([VEG + f'Cedrus_tree/Prefabs/Dry_cedrus_debris_Var{n}.prefab' for n in range(1, 8)] +
          [GO + f'Short_rotten_branch_var{n}.prefab' for n in range(1, 6)] +
          [GO + f'Rotten_cedrus_branch_var{n}.prefab' for n in range(1, 6)] +
          [GO + 'Generic_dry_leaves_var1.prefab'] +
          [GO + f'Lichen_branch_Debris_{n} Variant.prefab' for n in (1, 2)])
LOGS = ([GO + f'Mossy_log_var{n}.prefab' for n in (9, 10, 11)] +
        [GO + f'Mossy_rotten_log_var{n}.prefab' for n in (3, 4, 7, 8)] +
        [GO + f'Mossy_Rotten_log_var{n}.prefab' for n in (1, 2)] + [GO + 'Rotten_log_var5.prefab', GO + 'Rotten_log_var6.prefab'] +
        [GO + 'Rotten_log_var12.prefab'])
# Ground scans: mossy, rooty patches of real forest floor (1.5-2.3 m, up to 0.4 m of relief) over the flat floor box;
# stretched, they make the low mounds.
SCANS = [GO + f'Forest_ground_scan_var{n}.prefab' for n in (1, 2, 3)]
# Deadfall: a fallen trunk with the stump it broke from. (prefab, half its length, the axis it lies along)
FALLEN = [(GO + 'Large_RottenCedrus_log_1 Variant.prefab', 1.35, 'z'), (GO + 'Mossy_rotten_log_var8.prefab', 3.9, 'x'),
          (GO + 'Mossy_rotten_log_var7.prefab', 2.05, 'x'), (GO + 'Mossy_Rotten_log_var2.prefab', 1.7, 'x'),
          (GO + 'Rotten_log_var5.prefab', 1.55, 'x'), (GO + 'Mossy_rotten_log_var3.prefab', 1.35, 'x')]
CROWN_DEBRIS = ([GO + 'Rotten_cedrus_branchGroup_var1.prefab'] + [GO + f'Lichen_branch_Debris_{n} Variant.prefab' for n in (1, 2)] +
                [GO + f'Rotten_cedrus_branch_var{n}.prefab' for n in range(1, 6)])
FALLING_DEBRIS = [SEEDMESH + f'Effects/Falling_Tree_debris/Prefab/Falling_debris_{n}.prefab' for n in range(1, 10)]
WOOD_MUSHROOMS = [SEEDMESH + f'Mushrooms/Prefabs/Rotten_wood_mushroom_var{n}.prefab' for n in range(1, 6)]
# Shelf fungi go on stumps: (stump prefab, trunk radius where they sit, the height band they sit in), measured.
TREE_MUSHROOMS = [SEEDMESH + f'Mushrooms/Prefabs/Tree_Mushroom_var{n}.prefab' for n in range(1, 5)]
STUMP_TRUNK = {'Cedrus_stump_1': (0.17, 0.25, 0.75), 'Tree_stump_var2': (0.36, 0.3, 0.8),
               'Tree_stump_var3': (0.55, 0.35, 0.95), 'Tree_stump_var4': (0.36, 0.3, 0.8),
               'Tree_stump_var5': (0.5, 0.2, 0.5)}
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

    def stamp(self, path, parent, pos, yaw=0.0, scale=1.0, name=None, lod=None, tilt=(0.0, 0.0)):
        """A copy of a .prefab's entities under a new empty at pos (the editor's drop, by hand). New ids follow the
        prefab's own order, so children keep their order - a LOD Group's levels are its children in order.
        `scale` may be one number or (x, y, z); `tilt` is (pitch, roll) after the yaw."""
        p = self.prefab(path)
        root = self.empty(name or os.path.splitext(os.path.basename(path))[0], parent, pos, yaw=yaw, static=True)
        top = next(e for e in self.empties if e['id'] == root)
        sc = [scale] * 3 if isinstance(scale, (int, float)) else list(scale)
        if sc != [1.0, 1.0, 1.0]:
            top['scale'] = [round(v, 3) for v in sc]
        if tilt != (0.0, 0.0):
            top['rotation'] = k.euler_quat(yaw, tilt[0], tilt[1])
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
    """A Grass Area of meadow grass, for where the canopy opens (under the cedars the floor is moss and litter, no
    grass). Short, olive and dull, with dry blades through it, so it sits in with the moss and bark."""
    g = {'Size X': 12.0, 'Size Z': 12.0, 'Density': 300.0, 'Height': 0.34, 'Height Variation': 0.45,
         'Width': 0.03, 'Bend': 0.5, 'Base Color': [0.026, 0.04, 0.018], 'Tip Color': [0.14, 0.17, 0.07],
         'Dry Color': [0.2, 0.17, 0.1], 'Color Variation': 0.35, 'Patchiness': 0.55, 'Wind Strength': 0.35,
         'Wind Speed': 0.85, 'Near Distance': 16.0, 'Draw Distance': 60.0, 'Edge Fade': 3.0, 'Seed': 1,
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
STAIN = D + 'Stains/Splatters/DA_Stain_{:03d}.decal'
SPLAT = D + 'Stains/LiquidSplatter/DA_LiquidSplatter_{:03d}.decal'
PEEL = D + 'Grime/PeelingPaint/DA_PeelingPaint_{:03d}.decal'
RUST, GRIME, SPALL, SOOT = [0.3, 0.14, 0.06], [0.08, 0.08, 0.05], [0.07, 0.065, 0.06], [0.02, 0.02, 0.02]
MAX_WALL_DECALS, MAX_WALL_MAPS = 300, 12     # unique maps: 2K slots of the decal texture budget (docs/DECALS.md)


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
    sites = [('N', 9, 'patched'), ('N', 4, 'firefight'), ('N', 14, 'stained'), ('N', 18, 'firefight'),
             ('S', 10, 'firefight'), ('S', 6, 'patched'), ('S', 15, 'stained'), ('S', 1, 'firefight'),
             ('W', 9, 'patched'), ('W', 3, 'stained'), ('W', 13, 'firefight'), ('W', 17, 'patched'),
             ('E', 10, 'firefight'), ('E', 5, 'stained'), ('E', 14, 'patched'), ('E', 1, 'firefight')]
    lamps = s.empty('Wall Lamps', grp, (0, 0, 0), static=True)
    housing = k.textured(repo, base=(0.05, 0.05, 0.05), roughness=0.5, metallic=0.8)
    lens = k.textured(repo, base=(1.0, 0.62, 0.3), emissive=(1.0, 0.62, 0.3), strength=6.0)
    for side, i, kind in sites:
        u = -HALF + POST_EVERY * (i + 0.5)
        if kind == 'firefight':
            for cu, cy in ((u - 1.2, 1.3), (u + 0.9, 1.6), (u + 0.1, 0.9)):
                chips(side, cu, cy, 0.55, 6)
            w.put(STAIN.format(595), side, u - 0.3, 1.2, 1.8, 1.8, next(seed), 2, 0.6, SOOT,
                  spin=round(rng.uniform(0, 360), 1))
        elif kind == 'patched':
            # an old grey paint patch, flaking back to the concrete, with grime runs and a few chips
            w.put(PEEL.format(rng.choice((7, 13))), side, u, 1.6, 4.0, 2.2, next(seed), 2, 0.75, [0.55, 0.55, 0.53],
                  substrate=True)
            streaks(side, u, 2, GRIME)
            chips(side, u + 1.5, 2.4, 0.45, 4)
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
                     'cloudShadows': False, 'cloudWindSpeed': 4.0, 'moonBrightness': 0.0328, 'nightBrightness': 4.4,
                     'skyIntensity': 0.4})
    # Volumetric fog for the depth between the trunks, not the distance fog (outdoors only - the rule is no fog
    # indoors). The distance fog's settings stay for when it's switched back on. The shadows' fill is the night sky
    # x skyIntensity x skyAmbientIntensity: dark, but never black.
    d.update({'shadowDistance': 45.0, 'maxSpotShadows': 8, 'fogEnabled': False, 'fogMode': 3, 'fogDensity': 0.02,
              'fogColor': [0.03, 0.038, 0.052], 'fogHeightFalloff': 0.06, 'fogBaseHeight': 0.0,
              'volumetricEnabled': True, 'volDensity': 0.014, 'skyAmbientIntensity': 1.5})
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
    # Grass only where the canopy opens and lets the light in: the clearing, and the glades (after the trees).
    s.empty('Clearing Grass', g, (cx, 0.002, cz), static=True,
            **grass(**{'Size X': 2 * cr, 'Size Z': 2 * cr, 'Seed': 2, 'Patchiness': 0.35}))

    # --- cedars: clumps and glades. Spacing runs 3.2 m in a clump to 9 m in a glade, from a noise field; the
    # clearing stays open, with a softer edge of saplings-size trees round it.
    # Covers and leaning trees draw from streams of their own, so the trees' own layout doesn't move.
    cover_rng, lean_rng = random.Random(23), random.Random(29)
    trees = s.usable(TREES)
    grid = Grid(4.0)
    tree_pts = []
    covered = leaning = 0
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
        # Storm damage: now and then a tree leans, its foot sunk so the lean reads as roots giving way.
        lean = lean_rng.random() < 0.035 and not edge
        tilt = (round(lean_rng.uniform(6.0, 15.0), 1), round(lean_rng.uniform(-4.0, 4.0), 1)) if lean else (0.0, 0.0)
        y = -0.25 if lean else 0.0
        leaning += lean
        tree = s.stamp(path, grp, (round(x, 2), y, round(z, 2)), yaw=yaw, scale=scale, name='Cedar', lod=LODS['tree'],
                       tilt=tilt)
        var = int(path.rsplit('_var', 1)[1].split('.')[0])
        if var in TREE_COVER and cover_rng.random() < TREE_COVER_SHARE and s.prefab(TREE_COVER[var]):
            s.stamp(TREE_COVER[var], tree, (0.0, 0.0, 0.0), lod=LODS['tree'])
            covered += 1
    counts['cedars'], counts['tree covers'], counts['leaning'] = len(tree_pts), covered, leaning

    # Glade grass: a 10 m cell with few trunks within 7 m of its middle gets a meadow patch (12 m, so neighbours
    # overlap and the soft edges hide the grid).
    glades = 0
    for i in range(-6, 6):
        for j in range(-6, 6):
            x, z = (i + 0.5) * 10.0, (j + 0.5) * 10.0
            near = sum(1 for tx, tz, _ in tree_pts if (tx - x) ** 2 + (tz - z) ** 2 < 49.0)
            if near > 1 or math.hypot(x - cx, z - cz) < cr:
                continue
            glades += 1
            s.empty('Glade Grass', g, (x, 0.002, z), static=True,
                    **grass(Seed=10 + glades))
    counts['glade grass'] = glades
    inside = [(x, z, sc) for x, z, sc in tree_pts if abs(x) < HALF + 6 and abs(z) < HALF + 6]

    trunks = Grid(2.0)          # every trunk, for keeping fallen logs out of standing trees
    for x, z, sc in tree_pts:
        trunks.add(x, z, 0.45 * sc)
    mounds = []                 # (x, z, radius): small cover and litter stay off them (they'd be buried)

    def on_mound(x, z):
        return any((x - mx) ** 2 + (z - mz) ** 2 < mr * mr for mx, mz, mr in mounds)

    def ok(x, z, spread=HALF + 4.0, clearing_ok=False, clear=WALL_CLEAR):
        return (abs(x) <= spread and abs(z) <= spread and (clearing_ok or math.hypot(x - cx, z - cz) >= cr - 0.5)
                and not by_wall(x, z, clear))

    def around(pts, r_min, r_max):
        """A point r_min..r_max from a random one of pts ((x, z, ...))."""
        px, pz = rng.choice(pts)[:2]
        a, r = rng.uniform(0, 2 * math.pi), rng.uniform(r_min, r_max)
        return px + math.cos(a) * r, pz + math.sin(a) * r

    def scatter(name, paths, n, near_tree, r_min, r_max, scale, avoid=1.0, spread=HALF + 4.0, clearing_ok=False,
                lod='plant', anchors=None, near_anchor=0.0, a_min=0.5, a_max=2.0, off_mounds=False, group=None):
        """`n` copies of `paths`: round a random anchor (a_min..a_max off it) with probability `near_anchor`, else
        round a random trunk (r_min..r_max) with probability `near_tree`, else anywhere; kept off the clearing
        (unless clearing_ok) and `avoid` metres apart. Returns what it placed: (x, z, path, yaw, scale)."""
        group = group or s.empty(name, root, (0, 0, 0), static=True)
        usable = s.usable(paths)
        pts = []
        if not usable:
            return pts
        local = Grid(2.0)
        placed = 0
        for _ in range(n * 8):
            if placed >= n:
                break
            u = rng.random()
            if anchors and u < near_anchor:
                x, z = around(anchors, a_min, a_max)
            elif inside and u < near_anchor + (1.0 - near_anchor) * near_tree:
                x, z = around(inside, r_min, r_max)
            else:
                x, z = rng.uniform(-spread, spread), rng.uniform(-spread, spread)
            if abs(x) > spread or abs(z) > spread:
                continue
            if not clearing_ok and math.hypot(x - cx, z - cz) < cr - 0.5:
                continue
            if off_mounds and on_mound(x, z):
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
            pts.append((x, z, path, yaw, sc))
        counts[name] = counts.get(name, 0) + placed - dropped.get(name, 0)
        return pts

    def patches(name, species, n_patches, per_patch, radius, scale, anchors, near_anchor, near_tree, avoid=0.7,
                lod='plant', clearing_ok=False, where=None):
        """Stands of one species: a patch centre (by an anchor, by a trunk, or where `where(x, z)` likes), then
        per_patch plants of one list in `species` thrown round it, thicker in the middle."""
        group = s.empty(name, root, (0, 0, 0), static=True)
        kinds = [s.usable(sp) for sp in species]
        kinds = [kd for kd in kinds if kd]
        local, n = Grid(2.0), 0
        for _ in range(n_patches * 20):
            if n_patches <= 0:
                break
            u = rng.random()
            if anchors and u < near_anchor:
                x0, z0 = around(anchors, 0.8, 2.5)
            elif u < near_anchor + near_tree:
                x0, z0 = around(inside, 1.0, 5.0)
            else:
                x0, z0 = rng.uniform(-HALF, HALF), rng.uniform(-HALF, HALF)
                if where and not where(x0, z0):
                    continue
            if not ok(x0, z0, clearing_ok=clearing_ok):
                continue
            n_patches -= 1
            kind = rng.choice(kinds)
            for _ in range(rng.randint(*per_patch)):
                a, r = rng.uniform(0, 2 * math.pi), radius * rng.random() ** 0.7
                x, z = x0 + math.cos(a) * r, z0 + math.sin(a) * r
                if not ok(x, z, clearing_ok=clearing_ok) or local.near(x, z, avoid) or on_mound(x, z):
                    continue
                local.add(x, z, avoid)
                s.stamp(rng.choice(kind), group, (round(x, 2), 0.0, round(z, 2)), yaw=round(rng.uniform(0, 360), 1),
                        scale=rng.uniform(*scale), lod=LODS[lod])
                n += 1
        counts[name] = n

    # --- the floor: real ground scans in drifts (where a noise field says), and low mounds - stretched scans - at
    # tree roots and here and there between them.
    floor = s.empty('Ground Scans', root, (0, 0, 0), static=True)
    scans = s.usable(SCANS)
    n_scan = 0
    for _ in range(6000):
        if n_scan >= 520 or not scans:
            break
        x, z = rng.uniform(-HALF - 4, HALF + 4), rng.uniform(-HALF - 4, HALF + 4)
        if noise2(x, z, 7, 14.0) < 0.5 or not ok(x, z, clearing_ok=True, clear=0.8):
            continue
        s.stamp(rng.choice(scans), floor, (round(x, 2), -0.04, round(z, 2)), yaw=round(rng.uniform(0, 360), 1),
                scale=rng.uniform(0.8, 1.5), lod=LODS['solid'])
        n_scan += 1
    counts['ground scans'] = n_scan
    hump = s.empty('Mounds', root, (0, 0, 0), static=True)
    for _ in range(4000):
        if len(mounds) >= 80 or not scans:
            break
        x, z = around(inside, 0.4, 1.2) if rng.random() < 0.45 else (rng.uniform(-HALF, HALF), rng.uniform(-HALF, HALF))
        if not ok(x, z, clear=2.5) or math.hypot(x - cx, z - cz) < cr + 1.5 or on_mound(x, z):
            continue
        sxz, sy = rng.uniform(2.0, 3.2), rng.uniform(1.0, 1.6)
        s.stamp(rng.choice(scans), hump, (round(x, 2), round(-0.05 * sy, 3), round(z, 2)),
                yaw=round(rng.uniform(0, 360), 1), scale=(sxz, sy, sxz * rng.uniform(0.8, 1.1)), lod=LODS['solid'])
        mounds.append((x, z, 0.75 * sxz))
    counts['mounds'] = len(mounds)

    # --- the solid pieces first: logs, rocks and stumps are what ferns, moss and fungi gather round.
    logs = scatter('Logs', LOGS, 45, 0.4, 2.0, 8.0, (0.8, 1.15), avoid=6.0, lod='solid')
    rocks = scatter('Rocks', ROCKS, 70, 0.3, 1.5, 8.0, (0.6, 1.3), avoid=3.5, lod='solid')
    stumps = scatter('Stumps', STUMPS, 35, 0.2, 2.0, 8.0, (0.8, 1.2), avoid=5.0, lod='solid')

    # --- deadfall: a trunk that broke and fell, lying out from its stump, its crown end a mess of branches and
    # needles, fungi at its foot.
    dead = s.empty('Deadfall', root, (0, 0, 0), static=True)
    fallen = [(p, h, ax) for p, h, ax in FALLEN if s.prefab(p)]
    falls = []
    for _ in range(30000):
        if counts.get('deadfall', 0) >= 16 or not fallen or not stumps:
            break
        x0, z0 = around(inside, 2.0, 4.0)
        a = rng.uniform(0, 2 * math.pi)
        dx, dz = math.cos(a), math.sin(a)
        path, half, axis = rng.choice(fallen)
        sc = rng.uniform(0.9, 1.15)
        length = 2 * half * sc
        line = [(x0 + dx * t, z0 + dz * t) for t in [i * 0.5 for i in range(int((length + 1.4) / 0.5) + 1)]]
        if not all(ok(px, pz, spread=HALF - 2.0, clear=2.0) and not trunks.near(px, pz, 0.5) for px, pz in line):
            continue
        if any(math.hypot(px - fx, pz - fz) < 3.0 for px, pz in line for fx, fz in falls):
            continue
        st = rng.choice(STUMPS)
        if not s.prefab(st):
            continue
        site = s.empty('Fallen Tree', dead, (round(x0, 2), 0.0, round(z0, 2)), static=True)
        st_sc = rng.uniform(0.9, 1.15)
        s.stamp(st, site, (0.0, 0.0, 0.0), yaw=round(rng.uniform(0, 360), 1), scale=st_sc, lod=LODS['solid'])
        stumps.append((x0, z0, st, 0.0, st_sc))
        mid = 0.7 + length / 2          # the log starts just past the stump
        yaw = math.degrees(math.atan2(-dz, dx)) if axis == 'x' else math.degrees(math.atan2(dx, dz))
        s.stamp(path, site, (round(dx * mid, 2), 0.0, round(dz * mid, 2)), yaw=round(yaw, 1), scale=sc,
                lod=LODS['solid'])
        end = 0.7 + length
        crown = [p for p in CROWN_DEBRIS if s.prefab(p)]
        for i in range(rng.randint(2, 4)):
            t = end + rng.uniform(-0.6, 1.2)
            ox, oz = -dz * rng.uniform(-1.4, 1.4), dx * rng.uniform(-1.4, 1.4)
            s.stamp(rng.choice(crown), site, (round(dx * t + ox, 2), 0.0, round(dz * t + oz, 2)),
                    yaw=round(rng.uniform(0, 360), 1), scale=rng.uniform(0.8, 1.2), lod=LODS['small'])
        for i in range(rng.randint(5, 10)):
            t = rng.uniform(0.6, end + 1.5)
            ox, oz = -dz * rng.uniform(-1.2, 1.2), dx * rng.uniform(-1.2, 1.2)
            s.stamp(rng.choice(FALLING_DEBRIS + DEBRIS[:7]), site, (round(dx * t + ox, 2), 0.0, round(dz * t + oz, 2)),
                    yaw=round(rng.uniform(0, 360), 1), scale=rng.uniform(0.9, 1.4), lod=LODS['small'])
        falls += line
        # (as a log lying along its local X, which the fungi below go by)
        logs.append((x0 + dx * mid, z0 + dz * mid, path, yaw if axis == 'x' else yaw - 90.0, sc))
        counts['deadfall'] = counts.get('deadfall', 0) + 1

    wood_pts = [(x, z) for x, z, *_ in logs + stumps]
    anchors = wood_pts + [(x, z) for x, z, *_ in rocks]
    scatter('Moss Mats', MOSS, 260, 0.6, 0.5, 3.5, (0.8, 1.4), avoid=1.2, anchors=anchors, near_anchor=0.35,
            a_min=0.4, a_max=1.6)
    patches('Ground Cover', [GROUND_COVER[0:3], GROUND_COVER[3:9], GROUND_COVER[9:11]], 260, (3, 8), 2.2,
            (0.8, 1.3), anchors, 0.2, 0.45, avoid=0.6, clearing_ok=True)
    patches('Ferns', FERN_SPECIES, 170, (4, 9), 3.0, (0.75, 1.3), anchors, 0.45, 0.35, avoid=0.8)
    # flowers and tall wild grass where the canopy opens: round the clearing's edge and in the glades
    glade = lambda x, z: noise2(x, z, 3, 22.0) < 0.35 or math.hypot(x - cx, z - cz) < cr + 4.0  # noqa: E731
    patches('Flowers', [FLOWERS], 40, (3, 7), 1.6, (0.8, 1.2), None, 0.0, 0.0, avoid=0.5, clearing_ok=True, where=glade)
    patches('Wild Grass', [WILD_GRASS], 45, (2, 6), 1.8, (0.8, 1.3), None, 0.0, 0.0, avoid=0.6, clearing_ok=True,
            where=glade)
    scatter('Bushes', BUSHES, 260, 0.6, 1.5, 7.0, (0.7, 1.2), avoid=2.2)
    scatter('Debris', DEBRIS, 420, 0.75, 0.5, 6.0, (0.8, 1.2), avoid=0.8, clearing_ok=True, lod='small',
            off_mounds=True)
    scatter('Mushrooms', MUSHROOMS, 160, 0.9, 0.4, 1.6, (0.8, 1.4), avoid=0.4, lod='small', anchors=wood_pts,
            near_anchor=0.5, a_min=0.3, a_max=1.1, off_mounds=True)

    # --- fungi on the dead wood: clusters at the foot of each log, shelf fungi up the stumps.
    fungi = s.empty('Wood Fungi', root, (0, 0, 0), static=True)
    wood = s.usable(WOOD_MUSHROOMS)
    n_f = 0
    for x, z, path, yaw, sc in logs:
        if not wood or rng.random() < 0.35:
            continue
        a = math.radians(yaw)
        ax, az = math.cos(a), -math.sin(a)                     # the log's long (local X) axis
        for _ in range(rng.randint(1, 3)):
            t, side = rng.uniform(-0.8, 0.8) * sc, rng.choice((-1, 1)) * 0.2 * sc
            px, pz = x + ax * t - az * side, z + az * t + ax * side
            for _ in range(rng.randint(2, 5)):
                s.stamp(rng.choice(wood), fungi, (round(px + rng.uniform(-0.12, 0.12), 3), 0.0,
                                                  round(pz + rng.uniform(-0.12, 0.12), 3)),
                        yaw=round(rng.uniform(0, 360), 1), scale=rng.uniform(0.8, 1.5), lod=LODS['small'])
                n_f += 1
    shelves = s.usable(TREE_MUSHROOMS)
    for x, z, path, _yaw, sc in stumps:
        trunk = STUMP_TRUNK.get(os.path.splitext(os.path.basename(path))[0])
        if not shelves or not trunk or rng.random() < 0.4:
            continue
        r, lo, hi = trunk
        a0 = rng.uniform(0, 2 * math.pi)
        for i in range(rng.randint(1, 3)):
            a = a0 + rng.uniform(-0.6, 0.6)
            dx, dz = math.cos(a), math.sin(a)
            y = rng.uniform(lo, hi) * sc
            s.stamp(rng.choice(shelves), fungi, (round(x + dx * r * sc * 0.92, 3), round(y, 3), round(z + dz * r * sc * 0.92, 3)),
                    yaw=round(math.degrees(math.atan2(dx, dz)), 1), scale=rng.uniform(0.7, 1.1), lod=LODS['small'])
            n_f += 1
    counts['wood fungi'] = n_f

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
