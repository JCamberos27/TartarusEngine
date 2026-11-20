# Shared pieces of the prototype-grid test scenes (tools/gen_arena_scene.py, gen_blood_scene.py,
# gen_perf_stress_scene.py): ids counted from 0, the prototype grid materials, and entities copied out of
# sandbox_template.json (the player rig, the Sun) with their whole subtree.
import json
import os
import sys

from scene_kit import SCENE_FORMAT_VERSION, euler_quat  # noqa: F401  (re-exported for the generators)

KINDS = ('boxes', 'empties', 'models')
SANDBOX = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sandbox_template.json')

TEX = {'light': ('assets/Materials/Prototype/proto_grid_light.png', 'c90849c8fc6c88b0'),
       'dark': ('assets/Materials/Prototype/proto_grid_dark.png', '2b60657ce9aa4555'),
       'orange': ('assets/Materials/Prototype/proto_grid_orange.png', 'c389fb6bc574bb89')}


def grid_mat(tex, rough=0.85, color=(1, 1, 1), metallic=0.0):
    """A one-slot embedded material on a triplanar prototype grid ('light', 'dark' or 'orange')."""
    path, guid = TEX[tex]
    return [{'embedded': {'albedoMap': {'path': path, 'pathGuid': guid}, 'aoMap': '', 'baseColor': list(color),
                          'emissiveColor': [0, 0, 0], 'emissiveMap': '', 'emissiveStrength': 1.0,
                          'factorsScaleMaps': True, 'metallic': metallic, 'metallicMap': '',
                          'metallicRoughnessMap': '', 'normalMap': '', 'roughness': rough, 'roughnessMap': '',
                          'triplanar': True, 'triplanarScale': 0.25}}]


class ProtoScene:
    """Entity lists plus an id counter from 0 (id and order are the same number)."""

    def __init__(self, tool, wall):
        self.tool = tool
        self.wall = wall   # box() material when none is given
        self.ents = {k: [] for k in KINDS}
        self._next = 0

    def fail(self, msg):
        sys.exit(f'{self.tool}: {msg}')

    def nid(self):
        self._next += 1
        return self._next - 1

    def add(self, kind, e):
        self.ents[kind].append(e)
        return e['id']

    def node(self, name, parent=-1, pos=(0, 0, 0), rotation=(0, 0, 0, 1), scale=(1, 1, 1), **components):
        """An empty; components are added as-is (the scene JSON keys sort them on write)."""
        i = self.nid()
        return self.add('empties', {'id': i, 'name': name, 'order': i, 'parentId': parent, 'position': list(pos),
                                    'rotation': list(rotation), 'scale': list(scale), **components})

    def group(self, name, parent=-1, pos=(0, 0, 0)):
        return self.node(name, parent, pos)

    def box(self, name, center, size, parent, m=None, yaw=0.0, pitch=0.0, friction=0.8, **components):
        """A cube primitive: mesh + Box collider + level geometry. center is the box's middle."""
        i = self.nid()
        return self.add('boxes', {'collider': {'friction': friction, 'isTrigger': False}, 'color': [1.0, 1.0, 1.0],
                                  'id': i, 'materials': m or self.wall, 'name': name, 'order': i, 'parentId': parent,
                                  'position': [round(v, 4) for v in center], 'rotation': euler_quat(yaw, pitch),
                                  'scale': [round(v, 4) for v in size], **components})

    def block(self, name, x, z, w, h, d, parent, m=None, yaw=0.0, y0=0.0):
        """A box standing on the ground (or on y0): x/z is the footprint centre."""
        return self.box(name, (x, y0 + h * 0.5, z), (w, h, d), parent, m, yaw)


class Sandbox:
    """sandbox_template.json indexed by id and parent, for finding and copying entities by name."""

    def __init__(self, scene):
        self.scene = scene
        with open(SANDBOX, encoding='utf-8') as f:
            self.data = json.load(f)
        self.src = {e['id']: (kind, e) for kind in KINDS for e in self.data[kind]}
        self.kids = {}
        for i, (_, e) in sorted(self.src.items()):
            self.kids.setdefault(e.get('parentId'), []).append(i)

    def settings(self):
        """The scene-wide settings (sky, post, shadows, fog...); not the entities or the asset library lists."""
        skip = set(KINDS) | {'libraryMaterials', 'libraryModels', 'libraryPrefabs', 'librarySounds', 'libraryTextures'}
        out = {k: v for k, v in self.data.items() if k not in skip}
        out['formatVersion'] = SCENE_FORMAT_VERSION
        return out

    def find(self, name):
        hits = [i for i, (_, e) in self.src.items() if e['name'] == name]
        if len(hits) != 1:
            self.scene.fail(f'expected one "{name}" in sandbox_template.json, found {len(hits)}')
        return hits[0]

    def entity(self, i):
        return self.src[i][1]

    def children(self, i):
        return [self.src[k][1] for k in self.kids.get(i, [])]

    def subtree(self, top):
        """top, then its descendants (each node's children in id order, deepest-last branch first)."""
        ids, frontier = [top], [top]
        while frontier:
            kids = self.kids.get(frontier.pop(), [])
            ids += kids
            frontier += kids
        return ids

    def copy_tree(self, top, parent=-1, **overrides):
        """Copies top and its subtree into the scene under parent with fresh ids; overrides patch the top entity."""
        ids = self.subtree(top)
        remap = {old: self.scene.nid() for old in ids}
        for old in ids:
            kind, e = self.src[old]
            c = json.loads(json.dumps(e))
            c['id'] = c['order'] = remap[old]
            c['parentId'] = parent if old == top else remap[e['parentId']]
            if old == top:
                c.update(overrides)
            self.scene.ents[kind].append(c)
        return remap[top]


def write_sorted(path, data, ents):
    """Writes the scene (sorted keys, 2-space indent, LF) and prints the entity counts."""
    data.update(ents)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write('\n')
    print(f'wrote {path}: {len(ents["boxes"])} boxes, {len(ents["empties"])} empties, {len(ents["models"])} models')
