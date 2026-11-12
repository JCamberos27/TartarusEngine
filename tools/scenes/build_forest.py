# The Forest: a 120 x 120 m stand of mossy cedars at night, knee-deep in grass, the project's new main scene to
# build out from. A moss floor, Grass Areas (GrassRenderer: procedural, GPU-scattered blades) over all of it with a
# shorter, denser lawn in the clearing where the player starts, a seeded scatter of the SeedMesh pack's LOD cedars
# kept off the clearing, and the Apartment Hallway's night sky, moon and player.
#
#   python tools/scenes/build_forest.py [<repo>]
#
# Writes project/scenes/Forest.json (+ .meta, its guid kept across rebuilds).
# World (metres): X east, Z south, the ground at y = 0, the player in the clearing at the middle facing north.
import json
import math
import os
import random
import secrets
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scene_kit as k  # noqa: E402

HALF = 60.0                        # the forest runs -HALF..HALF on X and Z
CLEARING = (0.0, 6.0, 9.0)         # centre x, z and radius of the clearing the player starts in
FOREST = 'assets/Environments/SeedMesh_Mossy_Cedrus_Forest/Environment/'
TREES = [FOREST + f'Vegetation/Cedrus_tree/Prefabs/Optimized_trees/Cedrus_tree_var{n} Background Variant.prefab'
         for n in (1, 2, 3, 4, 5, 6, 7, 8, 10, 11)]
GROUND = FOREST + 'Terrain_Textures/Mossy_Ground_rocks/M_Mossy_rocks_ground.mat'


class Forest(k.Scene):
    def __init__(self, base, repo):
        super().__init__(base)
        self.repo = repo

    def stamp(self, prefab, parent, pos, yaw=0.0, scale=1.0, name=None):
        """A copy of a .prefab's entities under a new empty at pos (the editor's drop, by hand)."""
        with open(os.path.join(self.repo, 'project', prefab), encoding='utf-8') as f:
            p = json.load(f)
        root = self.empty(name or os.path.splitext(os.path.basename(prefab))[0], parent, pos, yaw=yaw, static=True)
        if scale != 1.0:
            next(e for e in self.empties if e['id'] == root)['scale'] = [scale] * 3
        remap = {e['id']: self._next() for key in ('empties', 'boxes', 'models') for e in p.get(key, [])}
        for key in ('empties', 'boxes', 'models'):
            for e in p.get(key, []):
                c = json.loads(json.dumps(e))
                c['id'] = c['order'] = remap[e['id']]
                c['parentId'] = root if e['parentId'] == -1 else remap[e['parentId']]
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
        remap = {i: self._next() for i in sorted(keep)}
        for key in ('empties', 'boxes', 'models'):
            for e in base.get(key, []):
                if e['id'] in keep:
                    c = json.loads(json.dumps(e))
                    c['id'] = c['order'] = remap[e['id']]
                    c['parentId'] = -1 if e['parentId'] == -1 else remap[e['parentId']]
                    getattr(self, key).append(c)
        return remap


def grass(name, size_x, size_z, **kw):
    g = {'Size X': size_x, 'Size Z': size_z, 'Density': 260.0, 'Height': 0.5, 'Height Variation': 0.4,
         'Width': 0.04, 'Bend': 0.35, 'Base Color': [0.035, 0.07, 0.025], 'Tip Color': [0.26, 0.34, 0.11],
         'Dry Color': [0.32, 0.29, 0.15], 'Color Variation': 0.25, 'Patchiness': 0.55, 'Wind Strength': 0.45,
         'Wind Speed': 0.9, 'Near Distance': 14.0, 'Draw Distance': 55.0, 'Edge Fade': 1.5, 'Seed': 1,
         'Receive Shadows': True}
    g.update(kw)
    return {'Grass Area': g}


def build(repo):
    with open(os.path.join(repo, 'project', 'scenes', 'Apartment Hallway.json'), encoding='utf-8') as f:
        base = json.load(f)
    s = Forest(base, repo)
    s.keep_from(base, {'Moon', 'Player Spawn'})
    d = s.data
    d['sky'].update({'seaLevel': 0.0, 'clouds': True, 'cloudCoverage': 0.3, 'cloudType': 0.35, 'cloudQuality': 0,
                     'cloudShadows': False, 'cloudWindSpeed': 4.0})
    d.update({'shadowDistance': 60.0, 'maxSpotShadows': 8, 'fogEnabled': False})

    root = s.empty('Forest', -1, (0, 0, 0), static=True)
    ground = k.from_mat(repo, GROUND, triplanar=True, triplanarScale=0.25)
    s.box('Forest Floor', root, (0.0, -0.2, 0.0), (HALF * 2 + 80.0, 0.4, HALF * 2 + 80.0), ground, static=True)
    for name, c, size in (('Bound N', (0.0, 2.0, -HALF), (2 * HALF, 4.0, 0.3)),
                          ('Bound S', (0.0, 2.0, HALF), (2 * HALF, 4.0, 0.3)),
                          ('Bound W', (-HALF, 2.0, 0.0), (0.3, 4.0, 2 * HALF)),
                          ('Bound E', (HALF, 2.0, 0.0), (0.3, 4.0, 2 * HALF))):
        s.box(name, root, c, size, ground, static=True, **{'Mesh Renderer': {'Enabled': False}})

    # Grass: the whole floor, plus a shorter, denser, greener lawn over the clearing
    g = s.empty('Grass', root, (0, 0, 0), static=True)
    s.empty('Forest Grass', g, (0.0, 0.0, 0.0), static=True, **grass('Forest Grass', 2 * HALF + 20.0, 2 * HALF + 20.0))
    cx, cz, cr = CLEARING
    s.empty('Clearing Grass', g, (cx, 0.002, cz), static=True,
            **grass('Clearing Grass', 2 * cr, 2 * cr, Density=380.0, Height=0.32, Patchiness=0.3, Seed=2,
                    **{'Tip Color': [0.3, 0.4, 0.13], 'Edge Fade': 3.0}))

    # Cedars: a seeded scatter, none in the clearing, a few metres apart
    rng = random.Random(11)
    trees = s.empty('Cedars', root, (0, 0, 0), static=True)
    placed = []
    tries = 0
    while len(placed) < 260 and tries < 40000:
        tries += 1
        x, z = rng.uniform(-HALF - 8.0, HALF + 8.0), rng.uniform(-HALF - 8.0, HALF + 8.0)
        if math.hypot(x - cx, z - cz) < cr + 2.5:
            continue
        if any((x - px) ** 2 + (z - pz) ** 2 < 5.2 ** 2 for px, pz in placed):
            continue
        placed.append((x, z))
        s.stamp(rng.choice(TREES), trees, (round(x, 2), 0.0, round(z, 2)), yaw=round(rng.uniform(0, 360), 1),
                scale=round(rng.uniform(0.85, 1.2), 3), name='Cedar')

    # The player in the clearing, facing north into the trees
    for e in s.empties:
        if e['parentId'] == -1 and 'First Person Controller' in e:
            e['position'] = [cx, 0.0, cz + 4.0]
            e['rotation'] = k.euler_quat(0.0)
    return s, len(placed)


def write_meta(path):
    meta = path + '.meta'
    if os.path.exists(meta):
        return
    with open(meta, 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'guid': secrets.token_hex(8), 'metaVersion': 1, 'type': 'scene'}, f, indent=2)
        f.write('\n')


def main():
    repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..'))
    s, trees = build(repo)
    out = os.path.join(repo, 'project', 'scenes', 'Forest.json')
    s.write(out)
    write_meta(out)
    print(f'build_forest: {trees} cedars; {len(s.boxes)} boxes, {len(s.models)} models, {len(s.empties)} empties -> {out}')


if __name__ == '__main__':
    main()
