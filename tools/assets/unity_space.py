# Unity <-> engine space: 4x4 matrix helpers, the FBX node read (--model-nodes) and each model's root
# correction, and quaternion helpers for the scene passes. Split out of unity_prefabs.py.
import collections
import math
import os
import re
import subprocess


IDENT = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]


# --- matrices (column-major lists of 16) ------------------------------------------------------------------------
def mul(a, b):
    return [sum(a[r + 4 * k] * b[k + 4 * c] for k in range(4)) for c in range(4) for r in range(4)]


def inverse(m):
    a = [[m[r + 4 * c] for c in range(4)] + [1.0 if r == i else 0.0 for i in range(4)] for r in range(4)]
    for i in range(4):
        p = max(range(i, 4), key=lambda r: abs(a[r][i]))
        a[i], a[p] = a[p], a[i]
        d = a[i][i]
        a[i] = [x / d for x in a[i]]
        for r in range(4):
            if r != i and a[r][i]:
                f = a[r][i]
                a[r] = [x - f * y for x, y in zip(a[r], a[i])]
    return [a[r][4 + c] for c in range(4) for r in range(4)]


def near_ident_rot(m, eps=1e-4):
    return all(abs(m[i] - IDENT[i]) < eps for i in (0, 1, 2, 4, 5, 6, 8, 9, 10))


def trs(pos, rot, scale):
    x, y, z, w = rot
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    sx, sy, sz = scale
    return [(1 - 2 * (y * y + z * z)) * sx, (2 * (x * y + z * w)) * sx, (2 * (x * z - y * w)) * sx, 0.0,
            (2 * (x * y - z * w)) * sy, (1 - 2 * (x * x + z * z)) * sy, (2 * (y * z + x * w)) * sy, 0.0,
            (2 * (x * z + y * w)) * sz, (2 * (y * z - x * w)) * sz, (1 - 2 * (x * x + y * y)) * sz, 0.0,
            pos[0], pos[1], pos[2], 1.0]


def decompose(m):
    sx = math.sqrt(m[0] ** 2 + m[1] ** 2 + m[2] ** 2)
    sy = math.sqrt(m[4] ** 2 + m[5] ** 2 + m[6] ** 2)
    sz = math.sqrt(m[8] ** 2 + m[9] ** 2 + m[10] ** 2)
    det = (m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]))
    if det < 0:
        sx = -sx
    r = [m[0] / sx, m[1] / sx, m[2] / sx, m[4] / sy, m[5] / sy, m[6] / sy, m[8] / sz, m[9] / sz, m[10] / sz]
    t = r[0] + r[4] + r[8]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        q = [(r[5] - r[7]) / s, (r[6] - r[2]) / s, (r[1] - r[3]) / s, 0.25 * s]
    elif r[0] > r[4] and r[0] > r[8]:
        s = math.sqrt(1.0 + r[0] - r[4] - r[8]) * 2
        q = [0.25 * s, (r[3] + r[1]) / s, (r[6] + r[2]) / s, (r[5] - r[7]) / s]
    elif r[4] > r[8]:
        s = math.sqrt(1.0 + r[4] - r[0] - r[8]) * 2
        q = [(r[3] + r[1]) / s, 0.25 * s, (r[7] + r[5]) / s, (r[6] - r[2]) / s]
    else:
        s = math.sqrt(1.0 + r[8] - r[0] - r[4]) * 2
        q = [(r[6] + r[2]) / s, (r[7] + r[5]) / s, 0.25 * s, (r[1] - r[3]) / s]
    return [m[12], m[13], m[14]], q, [sx, sy, sz]


def to_engine(pos, rot, scale):
    """Unity local TRS -> engine local TRS (mirror X)."""
    return ([-pos[0], pos[1], pos[2]], [rot[0], -rot[1], -rot[2], rot[3]], list(scale))


def r6(v):
    return [round(x, 6) for x in v]


# --- FBX node data ----------------------------------------------------------------------------------------------
def read_nodes(exe, files):
    """FBX path (normcased) -> {'nodes': {name: node}, 'order': [names]} from --model-nodes with pivots kept."""
    out = {}
    env = dict(os.environ, TARTARUS_NODES_PIVOTS='1')
    files = sorted(set(files))
    for i in range(0, len(files), 150):
        batch = files[i:i + 150]
        for attempt in range(3):   # the exe occasionally drops a batch; ask again for what is missing
            res = subprocess.run([exe, '--model-nodes', *batch], capture_output=True, text=True,
                                 encoding='utf-8', errors='replace', env=env)
            if res.stdout.count(' unitscale ') >= len(batch):
                break
        cur = None
        for line in res.stdout.splitlines():
            m = re.match(r'\[ModelNodes\] (.*) unitscale (\S+)$', line.strip())
            if m:
                cur = out.setdefault(os.path.normcase(os.path.normpath(m.group(1))), {'nodes': {}, 'order': []})
                continue
            m = re.match(r'\[ModelNodes\]   node (.*?) parent (.*?) local((?: \S+){16})(.*)$', line)
            if m and cur is not None:
                meshes = [(a, b, list(map(float, c.split())))
                          for a, b, c in re.findall(r'\| mesh (.*?) material (.*?) aabb((?: \S+){6})', m.group(4))]
                cur['nodes'][m.group(1)] = {'name': m.group(1), 'parent': m.group(2),
                                            'L': list(map(float, m.group(3).split())), 'meshes': meshes}
                cur['order'].append(m.group(1))
    return out


class FbxInfo:
    """One FBX's real nodes (the $AssimpFbx$ pivot helpers folded back into the node they belong to)."""

    def __init__(self, raw):
        nodes = raw['nodes']
        self.root = raw['order'][0] if raw['order'] else 'RootNode'
        self.real = collections.OrderedDict()
        for name in raw['order']:
            if '$AssimpFbx$' in name:
                continue
            n = nodes[name]
            chain = []
            p = n['parent']
            while p in nodes and '$AssimpFbx$' in p:
                chain.insert(0, nodes[p])
                p = nodes[p]['parent']
            non_geo, geo = list(IDENT), list(IDENT)
            for h in chain:
                if '_$AssimpFbx$_Geometric' in h['name']:
                    # a node's geometric transform is its own: the parent's matching *Inverse helpers hang in this
                    # chain too, and hand Unity's children back an undisturbed frame, so they are not part of it
                    if 'Inverse' not in h['name']:
                        geo = mul(geo, h['L'])
                else:
                    non_geo = mul(non_geo, h['L'])
            # The node's own matrix is its Translation / Rotation / Scaling (Assimp splits a geometric transform off into
            # helpers, so none of it is geometric): Unity keeps it on the GameObject, e.g. a sign exported scaled x2.07.
            non_geo = mul(non_geo, n['L'])
            # What the engine's own load bakes for this node: it folds the pivot chain, and the folded node keeps its
            # geometric rotation but not the matching inverse that hands the children back upright.
            eng = list(IDENT)
            for h in chain:
                if 'Inverse' not in h['name']:
                    eng = mul(eng, h['L'])
            eng = mul(eng, n['L'])
            self.real[name] = {'name': name, 'parent': p if p in nodes else None, 'local': non_geo, 'geo': geo,
                               'meshes': n['meshes'], 'eng': eng}
        # The root node's own matrix (Assimp's axis conversion; Rx(90) in most Blender exports) sits in 'geo' of the
        # root because it has no pivot helpers. It is part of what the engine bakes, so it belongs to the parent chain.
        self.root_L = list(nodes[self.root]['L']) if self.root in nodes else list(IDENT)
        if self.root in self.real:
            self.real[self.root]['local'], self.real[self.root]['geo'] = list(self.root_L), list(IDENT)
        # Unity keeps unit scale but drops the axis rotation (it is baked into the mesh): the scale-only part of the root.
        self.root_S = [0.0] * 16
        for c in range(3):
            self.root_S[c * 5] = math.sqrt(sum(self.root_L[c * 4 + r] ** 2 for r in range(3))) or 1.0
        self.root_S[15] = 1.0
        self.children = collections.defaultdict(list)
        for name, n in self.real.items():
            if n['parent'] is not None:
                self.children[n['parent']].append(name)

    def global_of(self, name, geo=True):
        n = self.real[name]
        m = mul(n['local'], n['geo']) if geo else n['local']
        p = n['parent']
        while p is not None:  # a parent's geometric transform is its own: it doesn't reach its children
            m = mul(self.real[p]['local'], m)
            p = self.real[p]['parent']
        return m

    def engine_global(self, name):
        """The transform the engine bakes into <name>'s vertices (every ancestor's folded transform, then its own)."""
        m = self.real[name]['eng']
        p = self.real[name]['parent']
        while p is not None:
            m = mul(self.real[p]['eng'], m)
            p = self.real[p]['parent']
        return m

    def mesh_nodes(self):
        return [n for n in self.real if self.real[n]['meshes']]

    def collapsed_root(self):
        """Unity's model root GameObject: the FBX root's only child when it has just one (Unity drops the root)."""
        kids = self.children.get(self.root, [])
        return kids[0] if len(kids) == 1 else None

    def materials(self, names=None):
        """Engine material slot order (first appearance, depth first), optionally only some mesh nodes."""
        out = []
        for n in self.real:
            if names is not None and n not in names:
                continue
            for _, mat, _ in self.real[n]['meshes']:
                if mat not in out:
                    out.append(mat)
        return out

    def correction(self, node):
        """Root correction so the model draws in Unity's space for <node>: its geometric space, unit scale kept."""
        full = self.engine_global(node)  # includes the root
        geo = self.real[node]['geo']
        return mul(mul(self.root_S, geo), inverse(full))

    def file_correction(self):
        r = self.collapsed_root()
        if r is None:
            return list(IDENT)
        # the collapsed node drawn in its own geometric space (root axis rotation and own transform undone)
        return self.correction(r)


def correction_meta(c):
    if near_ident_rot(c):
        off = r6(c[12:15])
        return {'pivotOffset': off} if any(abs(v) > 1e-6 for v in off) else {}
    return {'rootTransform': r6(c)}


def _qmul(a, b):
    x1, y1, z1, w1 = a
    x2, y2, z2, w2 = b
    return (w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2, w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2, w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2)


def _qrot(q, v):
    x, y, z, w = q
    t = (2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0]))
    return (v[0] + w * t[0] + (y * t[2] - z * t[1]), v[1] + w * t[1] + (z * t[0] - x * t[2]),
            v[2] + w * t[2] + (x * t[1] - y * t[0]))


def _world_of(out):
    """A function: entity -> its world (position, rotation, scale) in out (rotations and scales composed down the parents)."""
    ents = {e['id']: e for k in ('models', 'empties', 'boxes') for e in out[k]}
    cache = {}

    def world(e):
        if e['id'] not in cache:
            par = ents.get(e['parentId'])
            if par is None:
                cache[e['id']] = (tuple(e['position']), tuple(e['rotation']), tuple(e['scale']))
            else:
                pp, pq, ps = world(par)
                v = _qrot(pq, tuple(a * b for a, b in zip(e['position'], ps)))
                cache[e['id']] = (tuple(a + b for a, b in zip(pp, v)), _qmul(pq, tuple(e['rotation'])),
                                  tuple(a * b for a, b in zip(ps, e['scale'])))
        return cache[e['id']]
    world.ents = ents
    return world
