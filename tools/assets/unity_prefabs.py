# Unity prefabs and scenes -> engine .prefab / scene JSON, for unity_import.py.
#
# Spaces. Unity is left-handed and its FBX import mirrors X; the engine keeps the FBX's right-handed frame, so a
# Unity position (x, y, z) is the engine's (-x, y, z) and a rotation (x, y, z, w) is (x, -y, -z, w).
# Unity draws a mesh in its own node's geometric space (the FBX GeometricTranslation / Rotation / Scaling baked in,
# the node's Translation / PreRotation / Rotation kept on the GameObject), while the engine bakes the whole node tree
# into the vertices. So each model gets a root correction in its .meta (pivotOffset, or rootTransform when it turns
# too) that brings it back to the space Unity draws it in, and a Unity transform then carries over as it is. The node
# data comes from the engine's own read of the FBX (--model-nodes, with the pivot chain kept).
import collections
import json
import math
import os
import re
import subprocess

import unity_yaml as uy
from import_model_library import write_json, write_meta

IDENT = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
CLASS_GO, CLASS_TRANSFORM, CLASS_MESHFILTER, CLASS_MESHRENDERER, CLASS_MESH = 1, 4, 33, 23, 43
UNITY_BUILTIN, BUILTIN_CUBE, BUILTIN_PLANE = '0000000000000000e000000000000000', 10202, 10209


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


# --- Unity objects ----------------------------------------------------------------------------------------------
class UObj:
    """A GameObject in a Unity file or expanded from a source prefab / model."""
    def __init__(self, oid, name):
        self.id, self.name, self.active = oid, name, True
        self.pos, self.rot, self.scale = [0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 1.0], [1.0, 1.0, 1.0]
        self.children, self.comps, self.static, self.source = [], [], False, None
        self.comp_ids, self.mat_mods = set(), {}  # component fileIDs (LOD groups name renderers by them)
        self.open_spec = None  # a door / lift clip that moves this object (Converter.open_specs), becomes an Openable
        self.renderer_mods = {}  # instance overrides of the MeshRenderer: m_CastShadows, m_ReceiveShadows, m_Enabled


def vec(d, keys='xyz', default=0.0):
    d = d or {}
    return [float(d.get(k, default) or 0.0) for k in keys]


_FILEIDS = None


def unity_fileids():
    """Unity's real sub-object fileIDs per model guid, dumped from the Unity editor (unity_fileids/*.json). Newer
    imports (fileIdsGeneration 2) leave internalIDToNameTable empty and hash their ids, and older tables go stale
    when a file is re-exported, so this dump is the authority: {guid: {'root', 'ids': [{'id', 'cls', 'name'}]}}."""
    global _FILEIDS
    if _FILEIDS is None:
        _FILEIDS = {}
        d = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'unity_fileids')
        for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            if f.endswith('.json'):
                _FILEIDS.update(json.load(open(os.path.join(d, f), encoding='utf-8')))
    return _FILEIDS


# a GameObject's own placement, name and switch: on a child node of a whole-model instance these can't be folded into
# the model entity (they'd move, rename or hide the whole model)
NODE_PROPS = ('m_LocalPosition', 'm_LocalRotation', 'm_LocalScale', 'm_LocalEulerAnglesHint', 'm_Name', 'm_IsActive')


class Converter:
    def __init__(self, pack, exe):
        self.pack, self.exe = pack, exe
        self.fbx = {}            # unity model guid -> FbxInfo
        self.variants = {}       # (model guid, node) -> engine model path
        self.local_meshes = {}   # the current scene's embedded Mesh fileID -> name
        self.scene_path = None
        self.parent_off = False
        self.embedded = {}       # (scene, mesh name) -> engine model path or None
        self.terrain_mats = {}   # engine path of a terrain OBJ -> its material path
        self.prefab_cache = {}   # unity prefab guid -> (roots, objects by fileID)
        self.mat_guids = {}      # engine mat path -> guid
        self.model_guids = {}    # engine model path -> guid
        self.clips = {}          # unity .anim guid -> clip summary
        self.slot_cache = {}     # engine model path -> engine slot material names

    # models ---------------------------------------------------------------------------------------------------
    def load_fbx(self, guids):
        files = [self.pack.guids[g] for g in guids if g in self.pack.guids and g not in self.fbx]
        raw = read_nodes(self.exe, files)
        for g in guids:
            src = self.pack.guids.get(g)
            r = raw.get(os.path.normcase(os.path.normpath(src))) if src else None
            if r and r['order']:
                self.fbx[g] = FbxInfo(r)
            elif src:
                self.pack.report['FBX the engine could not read (--model-nodes): placed without a correction'].append(os.path.basename(src))

    def mesh_names(self, guid):
        """Unity mesh fileID -> mesh name, and GameObject / Transform fileID -> node name, from the importer table."""
        by_class = collections.defaultdict(dict)
        dumped = unity_fileids().get(guid)
        if dumped:
            for e in dumped['ids']:
                cls, name = int(e['cls']), e['name']
                if cls != CLASS_MESH:  # objects are keyed by node name like the importer table: the root is //RootNode
                    name = name.rsplit('/', 1)[-1] if name else '//RootNode'
                by_class[cls][int(e['id'])] = name
            return by_class
        imp = self.pack.meta(guid).get('ModelImporter') or {}
        for e in imp.get('internalIDToNameTable') or []:
            (k, v), = e['first'].items()
            by_class[int(k)][int(v)] = e['second']
        return by_class

    EMBEDDED_DUMP = os.environ.get('TARTARUS_EMBEDDED_DUMP', 'C:/Users/jacob/unity_truth/embedded')

    def embedded_model(self, name):
        """Engine path of a mesh stored inside the current scene, exported by TartarusDump.EmbeddedMeshes as an OBJ
        (mirrored into the engine's handedness there; one usemtl group sub<i> per submesh). Copied next to the scene."""
        scene = os.path.splitext(os.path.basename(self.scene_path))[0]
        key = (scene, name)
        if key in self.embedded:
            return self.embedded[key]
        rel = os.path.relpath(os.path.dirname(self.scene_path), self.pack.src)
        dst_dir = os.path.join(self.pack.dest, rel, scene + '_embedded')
        top = os.path.relpath(self.pack.src, self.pack.assets).split(os.sep)[0]
        src_dir = os.path.join(self.EMBEDDED_DUMP, top, scene)
        res = None
        idx = os.path.join(src_dir, 'index.json')
        if not os.path.exists(idx) and os.path.exists(os.path.join(src_dir, 'terrain_index.json')):
            idx = os.path.join(src_dir, 'terrain_index.json')
        if os.path.exists(idx):
            # same-named meshes are the same Polybrush copy of one tree LOD; the first export stands for them
            ents = json.load(open(idx, encoding='utf-8'))
            tidx = os.path.join(src_dir, 'terrain_index.json')  # TartarusDump.Terrains: a Unity Terrain as an OBJ grid
            if os.path.exists(tidx):
                ents += json.load(open(tidx, encoding='utf-8'))
            ent = next((e for e in ents if e['name'] == name), None)
            if ent:
                from unity_import import copy_if_changed
                dst = os.path.join(dst_dir, ent['file'])
                os.makedirs(dst_dir, exist_ok=True)
                mtl = os.path.splitext(dst)[0] + '.mtl'
                with open(mtl, 'w') as f:
                    f.write(''.join(f'newmtl sub{i}\nKd 0.8 0.8 0.8\n' for i in range(ent['subMeshes'])))
                txt = open(os.path.join(src_dir, ent['file']), encoding='utf-8').read()
                with open(dst, 'w', encoding='utf-8', newline='\n') as f:
                    f.write(f'mtllib {os.path.basename(mtl)}\n' + txt)
                rel_p = os.path.relpath(dst, os.path.join(self.pack.repo, 'project')).replace(os.sep, '/')
                imp = {'excludeNodes': [], 'globalScale': 1.0, 'importAnimations': False, 'importNormals': True,
                       'importSkeleton': False, 'materialImportMode': 1, 'mergeByMaterial': False, 'optimizeGraph': False}
                write_meta(dst, os.path.dirname(rel_p), 'model', {'importer': imp, 'materialRemap': {}})
                res = rel_p
                if ent.get('albedo') is not None:
                    self.terrain_mats[rel_p] = self.terrain_material(ent, dst)
                self.pack.counts['scene-embedded meshes (OBJ)'] += 1
        self.embedded[key] = res
        return res

    def terrain_material(self, ent, obj_dst):
        """Lit material for a terrain OBJ: the dominant terrain layer's albedo + normal. The OBJ's UVs are already metres
        divided by the layer's tileSize, so the material itself does not tile."""
        from unity_import import tidy, rel_project
        name = 'M_Terrain_' + tidy(ent.get('layer') or 'Ground')
        p = {'_AlbedoMap': '', '_AOMap': '', '_BaseColor': [1.0, 1.0, 1.0], '_DoubleSided': False,
             '_EmissiveColor': [0.0, 0.0, 0.0], '_EmissiveMap': '', '_EmissiveStrength': 1.0, '_Metallic': 0.0,
             '_MetallicMap': '', '_MetallicRoughnessMap': '', '_NormalMap': '', '_Roughness': 0.9, '_RoughnessMap': '',
             '_Triplanar': False, '_TriplanarScale': 1.0, '_ReflectionProbes': True}
        prev, self.pack.cur = getattr(self.pack, 'cur', '?'), 'terrain ' + name
        if ent.get('albedo') in self.pack.guids:
            p['_AlbedoMap'] = self.pack.texture(ent['albedo'], 'rgb', 'albedo')
        if ent.get('normal') in self.pack.guids:
            p['_NormalMap'] = self.pack.texture(ent['normal'], 'rgb', 'normal')
        self.pack.cur = prev
        dst = os.path.join(os.path.dirname(obj_dst), name + '.mat')
        write_json(dst, {'factorsScaleMaps': True, 'matVersion': 2, 'name': name, 'properties': p,
                         'shader': 'engine://Standard.shader'})
        rel = os.path.relpath(os.path.dirname(obj_dst), os.path.join(self.pack.repo, 'project')).replace(os.sep, '/')
        write_meta(dst, rel, 'material')
        mat = rel_project(self.pack.repo, dst)
        self.pack.counts['terrain meshes'] += 1
        self.pack.report['terrain meshes (dominant layer only, no splat blend; UVs baked to tileSize)'].append(
            f"{ent['name']}: {ent['tris']} tris at {ent['spacing']} m, layer {ent.get('layer')}")
        return mat

    def model_entity_path(self, guid, node=None):
        """Engine model path for the whole file (node None) or one mesh node of it (a variant file when needed)."""
        base = self.pack.model(guid)
        if base is None or guid not in self.fbx:
            return None
        info = self.fbx[guid]
        meshes = info.mesh_nodes()
        if node is None or (meshes == [node] and info.collapsed_root() == node):
            self.set_correction(base, info.file_correction() if node is None else info.correction(node), None)
            return base
        key = (guid, node)
        if key not in self.variants:
            src = os.path.join(self.pack.repo, 'project', *base.split('/'))
            stem, ext = os.path.splitext(src)
            dst = f'{stem}@{re.sub(r"[^A-Za-z0-9_]+", "_", node)}{ext}'
            from unity_import import copy_if_changed
            copy_if_changed(src, dst)
            meta = json.load(open(src + '.meta', encoding='utf-8'))
            rel = os.path.relpath(dst, os.path.join(self.pack.repo, 'project')).replace(os.sep, '/')
            write_meta(dst, os.path.dirname(rel), 'model',
                       {'importer': dict(meta['importer']), 'materialRemap': meta.get('materialRemap', {})})
            self.set_correction(rel, info.correction(node), [node])
            self.variants[key] = rel
            self.pack.counts['model variants (one mesh of a multi-mesh FBX)'] += 1
        return self.variants[key]

    def set_correction(self, rel, c, only):
        path = os.path.join(self.pack.repo, 'project', *rel.split('/')) + '.meta'
        meta = json.load(open(path, encoding='utf-8'))
        imp = meta['importer']
        imp.pop('pivotOffset', None)
        imp.pop('rootTransform', None)
        imp.update(correction_meta(c))
        if only is not None:
            imp['meshNodes'] = only
        write_json(path, meta)

    def guid_of(self, rel):
        path = os.path.join(self.pack.repo, 'project', *rel.split('/')) + '.meta'
        return json.load(open(path, encoding='utf-8'))['guid']

    # Unity files ----------------------------------------------------------------------------------------------
    def parse_file(self, path):
        """GameObject tree of a .prefab / .unity: (roots, objects by fileID, scene-level docs)."""
        docs = uy.docs(uy.read(path))
        by_id = {fid: (cls, t, body, stripped) for cls, fid, t, body, stripped in docs}
        if path.lower().endswith('.unity'):  # meshes saved inside the scene (Polybrush edits): fileID -> name
            self.scene_path = path
            self.local_meshes = {fid: v[2].get('m_Name') for fid, v in by_id.items() if v[1] == 'Mesh' and not v[3]}
        objs, tf_of, tf_go = {}, {}, {}
        for fid, (cls, t, body, stripped) in by_id.items():
            if t == 'GameObject' and not stripped:
                o = UObj(fid, body.get('m_Name') or 'GameObject')
                o.active = body.get('m_IsActive', 1) != 0
                o.static = (body.get('m_StaticEditorFlags') or 0) != 0
                objs[fid] = o
                for c in body.get('m_Component') or []:
                    cid = (c.get('component') or {}).get('fileID')
                    if cid in by_id and not by_id[cid][3]:
                        o.comps.append((by_id[cid][1], by_id[cid][2]))
                        o.comp_ids.add(cid)
        for fid, (cls, t, body, stripped) in by_id.items():
            if t in ('Transform', 'RectTransform') and not stripped:
                go = (body.get('m_GameObject') or {}).get('fileID')
                if go in objs:
                    o = objs[go]
                    o.pos = vec(body.get('m_LocalPosition'))
                    o.rot = vec(body.get('m_LocalRotation'), 'xyzw')
                    o.scale = vec(body.get('m_LocalScale'), default=1.0)
                    tf_of[go], tf_go[fid] = fid, go
        # nested prefab / model instances
        instances = {}
        for fid, (cls, t, body, stripped) in by_id.items():
            if t == 'PrefabInstance':
                instances[fid] = self.expand_instance(body)
        # An object inside a nested instance has, in this file, the id (instance id XOR its source id) with the top bit
        # cleared: edits made here, and by whoever nests this file in turn, target it by that id.
        self.nested_ids, self.nested_types = {}, {}
        for fid, inst in instances.items():
            if inst:
                for src_id, o in inst[1].items():
                    nid = ((fid & 0xFFFFFFFFFFFFFFFF) ^ (src_id & 0xFFFFFFFFFFFFFFFF)) & 0x7FFFFFFFFFFFFFFF
                    self.nested_ids.setdefault(nid, o)
                    if src_id in inst[3]:
                        self.nested_types.setdefault(nid, inst[3][src_id])
        # stripped transforms stand for an instance's object: map them so children can attach
        stripped_tf = {}
        for fid, (cls, t, body, stripped) in by_id.items():
            if stripped and t in ('Transform', 'RectTransform', 'GameObject'):
                inst = instances.get((body.get('m_PrefabInstance') or {}).get('fileID'))
                src = (body.get('m_CorrespondingSourceObject') or {}).get('fileID')
                if inst:
                    target = inst[1].get(src)
                    if target is not None:
                        stripped_tf[fid] = target
        # components added to instance objects (stripped GameObject + a real component pointing at it)
        for fid, (cls, t, body, stripped) in by_id.items():
            if stripped or t in ('GameObject', 'Transform', 'PrefabInstance'):
                continue
            go = (body.get('m_GameObject') or {}).get('fileID')
            if go in stripped_tf and go not in objs:
                stripped_tf[go].comps.append((t, body))
                stripped_tf[go].comp_ids.add(fid)
        roots = []
        for fid, (cls, t, body, stripped) in by_id.items():
            if t in ('Transform', 'RectTransform') and not stripped:
                go = tf_go.get(fid)
                father = (body.get('m_Father') or {}).get('fileID') or 0
                if go is None:
                    continue
                if father == 0:
                    roots.append(objs[go])
                elif father in tf_go:
                    objs[tf_go[father]].children.append(objs[go])
                elif father in stripped_tf:
                    stripped_tf[father].children.append(objs[go])
        for fid, inst in instances.items():
            if inst is None:
                continue
            root, _, parent_tf, _ = inst
            if not parent_tf:
                roots.append(root)
            elif parent_tf in tf_go:
                objs[tf_go[parent_tf]].children.append(root)
            elif parent_tf in stripped_tf:
                stripped_tf[parent_tf].children.append(root)
            else:
                roots.append(root)
        # Unity keeps child order in m_Children; sort by it where known
        order = {}
        for fid, (cls, t, body, stripped) in by_id.items():
            if t in ('Transform', 'RectTransform') and not stripped:
                for i, c in enumerate(body.get('m_Children') or []):
                    order[c.get('fileID')] = i
        for o in objs.values():
            o.children.sort(key=lambda c: order.get(tf_of.get(c.id), 1 << 30))
        return roots, objs, by_id

    def expand_instance(self, body):
        """A PrefabInstance -> (root UObj, {source fileID: UObj}, parent transform fileID) with its modifications."""
        mod = body.get('m_Modification') or {}
        src = (body.get('m_SourcePrefab') or {}).get('guid')
        path = self.pack.guids.get(src)
        if not path:
            self.pack.report['missing nested prefabs'].append(src)
            return None
        if path.lower().endswith('.fbx'):
            built = self.model_tree(src)
        elif path.endswith('.prefab'):
            built = self.prefab_tree(src)
        else:
            self.pack.report['instances of unsupported assets'].append(os.path.basename(path))
            return None
        if built is None:
            return None
        root, by_src, types = built
        # removed objects
        for r in mod.get('m_RemovedGameObjects') or []:
            o = by_src.get(r.get('fileID'))
            if o:
                o.active = False
                o.name += ' (removed)'
                o.comps.clear()
                o.children.clear()
        for m in mod.get('m_Modifications') or []:
            target = (m.get('target') or {}).get('fileID')
            o = by_src.get(target)
            prop, val = m.get('propertyPath') or '', m.get('value')
            if o is None:
                if prop.split('.')[0] in NODE_PROPS[:3] + ('m_IsActive',):
                    self.pack.report['instance transform / active edits whose target was not found'].append(
                        f'{os.path.basename(path)}: {target} {prop}')
                continue
            if prop in ('m_CastShadows', 'm_ReceiveShadows', 'm_Enabled') and types.get(target) not in ('MeshRenderer', 'SkinnedMeshRenderer'):
                continue  # a collider's or light's own switch, not the renderer's
            if types.get(target) == 'child node' and prop.split('.')[0] in NODE_PROPS:
                if prop.split('.')[0] not in ('m_LocalEulerAnglesHint', 'm_Name'):
                    self.pack.counts['child-node transform / active edits of whole-model instances, not applied'] += 1
                continue
            ref = m.get('objectReference') or {}
            apply_mod(o, prop, val, ref, self.pack)
        return root, by_src, (mod.get('m_TransformParent') or {}).get('fileID'), types

    def prefab_tree(self, guid):
        """A fresh copy of a source prefab's tree, keyed by its own fileIDs (and its nested instances' ids)."""
        path = self.pack.guids[guid]
        roots, objs, _ = self.parse_file(path)
        if not roots:
            return None
        root = roots[0]
        by_src, types = dict(objs), {}
        for nid, o in self.nested_ids.items():
            by_src.setdefault(nid, o)
        types.update(self.nested_types)
        # Transforms and components resolve to their GameObject: modifications target any of them
        for fid, (cls, t, body, stripped) in self._docs_index(path).items():
            types[fid] = t
            go = (body.get('m_GameObject') or {}).get('fileID')
            if go in objs:
                by_src[fid] = objs[go]
        return root, by_src, types

    def _docs_index(self, path):
        return {fid: (cls, t, body, s) for cls, fid, t, body, s in uy.docs(uy.read(path))}

    def model_tree(self, guid):
        """Unity's model prefab for an FBX: the node tree (root collapsed when it has one child), with ids from the
        importer's internalIDToNameTable. A whole-model instance becomes one engine model; see emit()."""
        if guid not in self.fbx:
            self.load_fbx([guid])
        info = self.fbx.get(guid)
        if info is None:
            self.pack.report['FBX the engine could not read'].append(self.pack.guids.get(guid))
            return None
        names = self.mesh_names(guid)
        id_of = {}
        for cls in (CLASS_GO, CLASS_TRANSFORM, CLASS_MESHFILTER, CLASS_MESHRENDERER, 137):
            for fid, nm in names.get(cls, {}).items():
                id_of.setdefault(nm, []).append(fid)
        top = info.collapsed_root() or info.root
        root = UObj(-1, os.path.splitext(os.path.basename(self.pack.guids[guid]))[0])
        if top != info.root:
            p, r, s = decompose(info.real[top]['local'])
            root.pos, root.rot, root.scale = [-p[0], p[1], p[2]], [r[0], -r[1], -r[2], r[3]], s
        root.source = ('model', guid)
        root.comp_ids.update(names.get(CLASS_MESHRENDERER, {}).keys())
        by_src = {}
        types = {fid: 'MeshRenderer' for fid in names.get(CLASS_MESHRENDERER, {})}
        types.update({fid: 'SkinnedMeshRenderer' for fid in names.get(137, {})})
        for fid in id_of.get('//RootNode', []):
            by_src[fid] = root
        # child objects resolve to the root model entity: renderer / material edits are folded into it, but a child
        # node's own transform, name and active switch are not (see expand_instance)
        exact = guid in unity_fileids()
        for nm, fids in id_of.items():
            for fid in fids:
                if fid not in by_src:
                    by_src[fid] = root
                    if exact and types.get(fid) not in ('MeshRenderer', 'SkinnedMeshRenderer'):
                        types[fid] = 'child node'
        return root, by_src, types

    # engine output --------------------------------------------------------------------------------------------
    def emit(self, roots, out, ids, parent=-1):
        """Append engine entities for a list of UObj trees to out ({'boxes', 'empties', 'models'})."""
        for o in roots:
            self.emit_one(o, out, ids, parent)

    def emit_one(self, o, out, ids, parent):
        pos, rot, scale = to_engine(o.pos, o.rot, o.scale)
        ids[0] += 1
        e = {'id': ids[0], 'name': o.name, 'order': ids[0], 'parentId': parent, 'position': r6(pos),
             'rotation': r6(rot), 'scale': r6(scale)}
        if not o.active or self.parent_off:  # a child of an inactive object is off too: say so on the entity itself
            e['active'] = False
        if o.static:
            e['static'] = True
        comps = collections.defaultdict(list)
        for t, body in o.comps:
            comps[t].append(body)
        model = None
        if o.source and o.source[0] == 'model':
            model = (self.model_entity_path(o.source[1]), None)
        elif comps.get('MeshFilter') or comps.get('SkinnedMeshRenderer'):
            # a skinned mesh (the pack's wind-rigged trees) draws as a static model in its bind pose
            mesh = (comps.get('MeshFilter') or comps['SkinnedMeshRenderer'])[0].get('m_Mesh') or {}
            g = mesh.get('guid')
            if g in self.pack.guids and self.pack.guids[g].lower().endswith('.fbx'):
                if g not in self.fbx:
                    self.load_fbx([g])
                name = self.mesh_names(g).get(CLASS_MESH, {}).get(mesh.get('fileID'))
                # newer imports name meshes by a hash Unity doesn't list: fall back to the only mesh, or the object's name
                node = self.node_for_mesh(g, name) or self.node_for_mesh(g, re.sub(r' \(\d+\)$', '', o.name))
                if node:
                    model = (self.model_entity_path(g, node), node)
                else:
                    self.pack.report['meshes not found in their FBX'].append(f'{o.name}: {name}')
            elif not g and mesh.get('fileID') in self.local_meshes:
                model = (self.embedded_model(self.local_meshes[mesh['fileID']]), None)
                if model[0] is None:
                    self.pack.report['scene-embedded meshes with no exported OBJ (run TartarusDump.EmbeddedMeshes)'].append(
                        f'{o.name}: {self.local_meshes[mesh["fileID"]]}')
            elif g == UNITY_BUILTIN and mesh.get('fileID') == BUILTIN_CUBE:
                model = ('box', None)  # Unity's unit cube is the engine's box (the alley's sun blockers)
            elif g == UNITY_BUILTIN and mesh.get('fileID') == BUILTIN_PLANE and not o.children:
                # Unity's plane is 10 x 10 with no thickness: a flat box (the suburb's water)
                model = ('box', None)
                e['scale'] = r6([e['scale'][0] * 10.0, e['scale'][1] * 0.002, e['scale'][2] * 10.0])
            elif g:
                self.pack.report['non-FBX meshes (Unity built-ins / .asset), skipped'].append(
                    f'{o.name}: {os.path.basename(self.pack.guids.get(g, g))}')
        if model is None and comps.get('Terrain'):
            model = (self.embedded_model(o.name + '_Terrain'), None)
            if model[0] is None:
                self.pack.report['terrains with no exported OBJ (run TartarusDump.Terrains)'].append(o.name)
        renderer = (comps.get('MeshRenderer') or comps.get('SkinnedMeshRenderer') or [None])[0]
        if o.renderer_mods:
            renderer = dict(renderer or {'m_Materials': []}, **o.renderer_mods)
        if model and model[0] == 'box' and (renderer is None or renderer.get('m_Enabled', 1)):
            mats = [self.pack.material(m.get('guid')) for m in (renderer or {}).get('m_Materials') or []]
            e['materials'] = [{'guid': self._mat_guid(m), 'path': m} if m else {} for m in mats[:1]]
            if renderer and renderer.get('m_CastShadows', 1) != 1:
                e['castShadows'] = int(renderer.get('m_CastShadows', 1))
            out['boxes'].append(e)
            self.pack.counts['built-in cubes as boxes'] += 1
        elif model and model[0] and (renderer is None or renderer.get('m_Enabled', 1)):
            e['path'] = model[0]
            e['pathGuid'] = self.guid_of(model[0])
            k = SCALE_OVERRIDES.get(self.pack.name, {}).get(re.sub(r' \(\d+\)$', '', o.name))
            if k:
                e['scale'] = r6([c * k for c in e['scale']])
                self.pack.counts['prefab scale overrides applied'] += 1
            mats = self.material_overrides(model[0], o, renderer)
            if model[0] in self.terrain_mats:
                m = self.terrain_mats[model[0]]
                mats = [{'guid': self._mat_guid(m), 'path': m}]
            if mats:
                e['materials'] = mats
            if renderer and renderer.get('m_CastShadows', 1) != 1:
                e['castShadows'] = int(renderer.get('m_CastShadows', 1))
            if renderer and not renderer.get('m_ReceiveShadows', 1):
                e['receiveShadows'] = False
            out['models'].append(e)
            self.pack.counts['renderers'] += 1
        else:
            out['empties'].append(e)
        self.colliders(o, comps, e, out, ids, is_model='path' in e)
        self.open_specs(o, comps)
        if o.open_spec and 'path' in e:
            self.openable(o, e)
        self.lights(comps, e)
        self.probes(comps, e)
        children = list(o.children)
        lod = (comps.get('LODGroup') or [None])[0]
        if lod is not None:
            children = self.lod_group(o, lod, e, children)
        was_off = self.parent_off
        self.parent_off = was_off or not o.active
        for c in children:
            self.emit_one(c, out, ids, e['id'])
        self.parent_off = was_off

    # doors and lifts --------------------------------------------------------------------------------------------
    def clip(self, guid):
        """{'name', 'length', 'euler_y', 'pos'} of an .anim: per target path, the total Y turn (degrees) and the
        position change of its curves, for the clip's last key."""
        if guid in self.clips:
            return self.clips[guid]
        res = None
        path = self.pack.guids.get(guid)
        if path and path.lower().endswith('.anim'):
            body = next((b for c, _, t, b, _s in uy.docs(uy.read(path)) if t == 'AnimationClip'), None)
            if body:
                def last(curve):
                    keys = (curve.get('curve') or {}).get('m_Curve') or []
                    return keys[-1] if keys else None, keys[0] if keys else None
                by_path = collections.defaultdict(dict)
                length = 0.0
                for c in body.get('m_EulerCurves') or []:
                    hi, lo = last(c)
                    if hi:
                        by_path[c.get('path') or ''].setdefault('euler_y', vec(hi['value'])[1] - vec(lo['value'])[1])
                        length = max(length, float(hi['time']))
                for c in body.get('m_PositionCurves') or []:
                    hi, lo = last(c)
                    if hi:
                        by_path[c.get('path') or '']['pos'] = [a - b for a, b in zip(vec(hi['value']), vec(lo['value']))]
                        length = max(length, float(hi['time']))
                res = {'name': body.get('m_Name', ''), 'length': length, 'paths': dict(by_path)}
        self.clips[guid] = res
        return res

    def open_clip(self, comps):
        """The clip that opens a door: the Animator controller's 'Open' state, or a legacy Animation's clip named for it."""
        guids = []
        for a in comps.get('Animator', []):
            cg = (a.get('m_Controller') or {}).get('guid')
            cpath = self.pack.guids.get(cg)
            if cpath:
                for c, _, t, b, _s in uy.docs(uy.read(cpath)):
                    if t == 'AnimatorState' and str(b.get('m_Name', '')).lower() == 'open':
                        guids.append((b.get('m_Motion') or {}).get('guid'))
        for a in comps.get('Animation', []):
            guids += [(m or {}).get('guid') for m in a.get('m_Animations') or []]
        for g in guids:
            clip = self.clip(g) if g else None
            if clip and 'open' in clip['name'].lower() and 'close' not in clip['name'].lower():
                return clip
        return None

    def open_specs(self, o, comps):
        """Hand the 'open' clip's per-path motion to the objects it moves."""
        if not (comps.get('Animator') or comps.get('Animation')):
            return
        clip = self.open_clip(comps)
        if not clip:
            return
        for path, m in clip['paths'].items():
            target = o
            for part in [x for x in path.split('/') if x]:
                target = next((c for c in target.children if c.name == part), None)
                if target is None:
                    break
            if target is not None:
                target.open_spec = dict(m, length=clip['length'])
                self.pack.counts['openable clips applied'] += 1

    def openable(self, o, e):
        """The engine's Openable (src/Game/Openable.h) for a door or lift leaf the clip moves. The hinge or slide comes
        from the model's own bounds (front +Z), so only which edge and which way are decided here: the leaf sits on the
        engine's +X side of its pivot (a box collider's centre) for Hinge Left, -X for Hinge Right. A Unity turn about Y
        throws that edge toward +Z when the leaf's engine X and sin(turn) agree, else toward -Z (Swing Back)."""
        spec = o.open_spec
        col = e.get('collider') or {}
        side = (col.get('center') or [1.0])[0]
        if 'euler_y' in spec and abs(spec['euler_y']) > 1.0:
            turn = spec['euler_y']
            left = side >= 0
            front = (side * math.sin(math.radians(turn))) > 0
            op = {'Motion': 'Hinge Left' if left else 'Hinge Right', 'Amount': round(abs(turn), 2),
                  'Open Time': round(max(spec['length'], 0.05), 3), 'Start Open': False}
            if not front:
                op['Swing Back'] = True
            op.update({k: ';'.join(f'assets/Audio/Foley/door/{k_}_{i}.wav' for i in range(1, 5))
                       for k, k_ in (('Open Sound', 'open'), ('Close Sound', 'close'), ('Latch Sound', 'latch'))})
        elif 'pos' in spec and abs(spec['pos'][0]) > 0.05:
            dx = -spec['pos'][0]  # Unity -> engine X
            op = {'Motion': 'Slide Right' if dx > 0 else 'Slide Left', 'Amount': round(abs(dx), 3),
                  'Open Time': round(max(spec['length'], 0.05), 3), 'Start Open': False}
        else:
            return
        e['Openable'] = op
        self.pack.counts['openables'] += 1

    def node_for_mesh(self, guid, mesh_name):
        info = self.fbx.get(guid)
        if not info:
            return None
        for n in info.mesh_nodes():
            if n == mesh_name or any(m[0] == mesh_name for m in info.real[n]['meshes']):
                return n
        meshes = info.mesh_nodes()
        if len(meshes) == 1:
            return meshes[0]
        # an unlisted (hashed) id on an object named for its part ('Lockers' for 'Lockers_body'): the one node that
        # carries the object's name as a prefix
        if mesh_name:
            pre = [n for n in meshes if n.startswith(mesh_name + '_')]
            return pre[0] if len(pre) == 1 else None
        return None

    def material_overrides(self, rel, o, renderer):
        """The entity's material list when the renderer's materials differ from the model's .meta remap."""
        if renderer is None:
            return None
        unity = [m.get('guid') for m in renderer.get('m_Materials') or []]
        unity = [o.mat_mods.get(i, g) for i, g in enumerate(unity)]
        if '_embedded/' in rel:  # an exported scene mesh: slot i is submesh i, so the renderer's list is the answer
            paths = [self.pack.material(g) if g else None for g in unity]
            return [{'guid': self._mat_guid(m), 'path': m} if m else {} for m in paths] if any(paths) else None
        meta = json.load(open(os.path.join(self.pack.repo, 'project', *rel.split('/')) + '.meta', encoding='utf-8'))
        remap = meta.get('materialRemap', {})
        guid = self._model_guid_for(rel)
        info = self.fbx.get(guid)
        if info is None:
            return None
        node = rel.split('@')[-1].rsplit('.', 1)[0] if '@' in rel else None
        only = meta['importer'].get('meshNodes')
        nodes = [n for n in info.mesh_nodes() if not only or n in only]
        slots = info.materials(nodes)
        # Unity submesh i of a node <-> that node's i-th material; a whole model's slots run node by node
        by_name = {}
        k = 0
        for n in nodes:
            for _, mat, _ in info.real[n]['meshes']:
                if k < len(unity) and unity[k]:
                    by_name.setdefault(mat, unity[k])
                k += 1
            if node is not None or len(nodes) == 1:
                break
        out, differs = [], False
        for s in slots:
            want = self.pack.material(by_name[s]) if s in by_name else None
            have = remap.get(s)
            path = want or have
            if want and want != have:
                differs = True
            out.append({'guid': self._mat_guid(path), 'path': path} if path else {})
        return out if differs else None

    def _model_guid_for(self, rel):
        base = re.sub(r'@[^/]*(\.[^./]+)$', r'\1', rel)
        for g, p in self.pack.models.items():
            if p == base:
                return g
        return None

    def _mat_guid(self, path):
        if path not in self.mat_guids:
            self.mat_guids[path] = self.guid_of(path)
        return self.mat_guids[path]

    def colliders(self, o, comps, e, out, ids, is_model):
        shapes = []
        for t in ('BoxCollider', 'SphereCollider', 'CapsuleCollider', 'MeshCollider', 'TerrainCollider'):
            for c in comps.get(t, []):
                if c.get('m_Enabled', 1) == 0:
                    continue
                trig = bool(c.get('m_IsTrigger', 0))
                if t == 'BoxCollider':
                    ce, sz = vec(c.get('m_Center')), vec(c.get('m_Size'), default=1.0)
                    shapes.append({'shape': 0, 'center': r6([-ce[0], ce[1], ce[2]]),
                                   'halfExtents': r6([abs(v) / 2 for v in sz]), 'isTrigger': trig})
                elif t == 'SphereCollider':
                    ce = vec(c.get('m_Center'))
                    shapes.append({'shape': 1, 'center': r6([-ce[0], ce[1], ce[2]]),
                                   'halfExtents': [round(c.get('m_Radius', 0.5), 6), 0.0, 0.0], 'isTrigger': trig})
                elif t == 'CapsuleCollider':
                    ce = vec(c.get('m_Center'))
                    rad, h = c.get('m_Radius', 0.5), c.get('m_Height', 2.0)
                    d = c.get('m_Direction', 1)
                    s = {'shape': 2, 'center': r6([-ce[0], ce[1], ce[2]]),
                         'halfExtents': [round(rad, 6), round(max(h / 2 - rad, 0.0), 6), 0.0], 'isTrigger': trig}
                    if d != 1:
                        s['_axis'] = d
                    shapes.append(s)
                else:
                    if not is_model:
                        self.pack.report['mesh colliders without a renderer (skipped)'].append(o.name)
                        continue
                    shapes.append({'shape': 3 if c.get('m_Convex', 0) else 4, 'isTrigger': trig})
                self.pack.counts['colliders'] += 1
        for i, s in enumerate(shapes):
            axis = s.pop('_axis', None)
            s['friction'] = 0.6
            if i == 0 and axis is None and (is_model or s['shape'] < 3):
                e['collider'] = s
                continue
            ids[0] += 1
            rot = [0.0, 0.0, 0.0, 1.0]
            if axis == 0:
                rot = [0.0, 0.0, -0.707107, 0.707107]
            elif axis == 2:
                rot = [0.707107, 0.0, 0.0, 0.707107]
            c = s.pop('center', [0.0, 0.0, 0.0])
            out['empties'].append({'id': ids[0], 'name': f'{o.name} Collider {i}', 'order': ids[0],
                                   'parentId': e['id'], 'position': c, 'rotation': rot, 'scale': [1.0, 1.0, 1.0],
                                   'collider': s, **({'static': True} if o.static else {})})

    def probes(self, comps, e):
        """Unity ReflectionProbe -> the engine's Reflection Probe: the box volume (axes are unsigned, so no mirroring) and
        Importance. The box offset moves the volume off the object in Unity; the engine's probe sits at the object, so
        an offset probe is nudged (X mirrored) by placing it on a child empty."""
        for pr in comps.get('ReflectionProbe', []):
            if not pr.get('m_Enabled', 1) or pr.get('m_Type', 0) != 0:
                continue
            e['Reflection Probe'] = {'Importance': float(pr.get('m_Importance', 1)),
                                     'Size': r6([abs(v) for v in vec(pr.get('m_BoxSize'), default=1.0)])}
            off = vec(pr.get('m_BoxOffset'))
            if any(abs(v) > 1e-4 for v in off):
                e['position'] = r6([a + b for a, b in zip(e['position'], [-off[0], off[1], off[2]])])  # parent frame approx
                self.pack.report['reflection probes with a box offset (moved the probe, ignoring the parent rotation)'].append(e['name'])
            self.pack.counts['reflection probes'] += 1

    HDRP_TO_ENGINE = 3.0 / 120000.0  # HDRP lights are physical (sun ~120k lux, points/spots in candela); engine sun ~3

    def lights(self, comps, e):
        # An HDRP pack (SeedMesh) stores physical intensities: scale every light by one factor so the balance holds.
        k = self.HDRP_TO_ENGINE if self.pack.counts['seedmesh materials'] else 1.0
        for l in comps.get('Light', []):
            kind = {0: 'Spot', 1: 'Directional', 2: 'Point'}.get(l.get('m_Type', 2))
            if kind is None:
                self.pack.report['area lights (baked only in Unity), skipped'].append(e['name'])
                continue
            col = l.get('m_Color') or {}
            sh = (l.get('m_Shadows') or {}).get('m_Type', 0)
            e['Light'] = {'Angular Size': 0.53, 'Cast Shadows': sh != 0, 'Color': r6([col.get('r', 1), col.get('g', 1), col.get('b', 1)]),
                          'ColorTempK': 0.0, 'Culling Mask': -1, 'Intensity': round(float(l.get('m_Intensity', 1.0)) * k, 4),
                          'Range': round(float(l.get('m_Range', 10.0)), 4), 'Shadow Bias': 1.0, 'Shadow Near Plane': 0.05,
                          'Shadow Normal Bias': 1.0, 'Shadow Resolution': 'Follow global', 'Shadow Softness': 1.0,
                          'Shadow Update Mode': 'Dynamic', 'Spot Angle': round(float(l.get('m_SpotAngle', 30.0)) / 2, 4),
                          'Type': kind}
            # Unity lights shine along +Z, the engine's along -Z: turn the light 180 degrees about its own Y (q * Ry180),
            # or every street lamp, spot and the moon lights the sky instead of the ground.
            qx, qy, qz, qw = e['rotation']
            e['rotation'] = r6([-qz, qw, qx, -qy])
            if l.get('m_Lightmapping', 4) == 2:  # Baked only: Unity drew it into lightmaps, not live
                self.pack.counts['baked-only lights (now realtime)'] += 1
            self.pack.counts['lights'] += 1

    def lod_group(self, o, lod, e, children):
        """Unity LODGroup -> engine LOD Group: the level renderers become the first children, in level order."""
        levels = lod.get('m_LODs') or []
        n = len(levels)
        # The engine holds 4 levels: keep the first, the last and evenly spaced ones between. A kept level stays up
        # until the next kept one takes over, so it inherits the threshold of the level just before that one.
        keep = sorted({round(i * (n - 1) / 3) for i in range(4)}) if n > 4 else list(range(n))
        if n > 4:
            self.pack.report['LOD groups with more than 4 levels (kept first, last and evenly spaced)'].append(o.name)
        all_objs = []
        for lv in levels:
            members = []
            for r in lv.get('renderers') or []:
                fid = (r.get('renderer') or {}).get('fileID')
                for c in children:
                    if fid in c.comp_ids:
                        members.append(c)
            all_objs.append(members)
        level_objs = [all_objs[i] for i in keep]
        if not any(level_objs) or any(len(m) != 1 for m in level_objs):
            self.pack.report['LOD groups not converted (levels are not one direct child each)'].append(o.name)
            return children
        ends = [keep[j + 1] - 1 for j in range(len(keep) - 1)] + [keep[-1]]
        thresholds = [round(float(levels[i].get('screenRelativeHeight', 0.0)), 5) for i in ends]
        thresholds += [0.0] * (4 - len(thresholds))
        e['LOD Group'] = {'LOD 0': thresholds[0], 'LOD 1': thresholds[1], 'LOD 2': thresholds[2],
                          'LOD 3': thresholds[3], 'Size': 0.0}
        firsts = [m[0] for m in level_objs]
        for i, m in enumerate(all_objs):  # dropped levels would otherwise draw on top of the kept ones
            if i not in keep:
                for c in m:
                    c.active = False
        self.pack.counts['LOD groups'] += 1
        return firsts + [c for c in children if c not in firsts]


def apply_mod(o, prop, val, ref, pack):
    def num(v):
        try:
            return float(v)
        except (TypeError, ValueError):
            return 0.0
    m = re.match(r'm_Local(Position|Rotation|Scale)\.([xyzw])$', prop)
    if m:
        arr = {'Position': o.pos, 'Rotation': o.rot, 'Scale': o.scale}[m.group(1)]
        arr['xyzw'.index(m.group(2))] = num(val)
        return
    if prop == 'm_Name':
        o.name = str(val)
    elif prop == 'm_IsActive':
        o.active = num(val) != 0
    elif prop == 'm_StaticEditorFlags':
        o.static = num(val) != 0
    elif prop in ('m_CastShadows', 'm_ReceiveShadows', 'm_Enabled'):
        o.renderer_mods[prop] = int(num(val))
    else:
        m = re.match(r'm_Materials\.Array\.data\[(\d+)\]$', prop)
        if m and ref.get('guid'):
            o.mat_mods[int(m.group(1))] = ref['guid']
            pack.counts['material overrides on instances'] += 1


def write_report(pack):
    lines = [f'# {pack.name}: Unity import report', '', f'Source: `{pack.src}`', '', '## Counts', '']
    for k, v in sorted(pack.counts.items()):
        lines.append(f'- {k}: {v}')
    lines += ['', '## Not converted / approximated', '']
    if not pack.report:
        lines.append('Nothing.')
    for k, items in sorted(pack.report.items()):
        uniq = sorted(set(map(str, items)))
        lines.append(f'### {k} ({len(uniq)})')
        lines += [f'- {x}' for x in uniq[:200]]
        if len(uniq) > 200:
            lines.append(f'- ... {len(uniq) - 200} more')
        lines.append('')
    os.makedirs(pack.dest, exist_ok=True)
    with open(os.path.join(pack.dest, 'unity_import_report.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


# --- driver -----------------------------------------------------------------------------------------------------
SKIP_COMPONENTS = ('Camera', 'CharacterController', 'AudioListener')
SIMPLE_MODS = re.compile(r'^(m_Local(Position|Rotation|Scale|EulerAnglesHint)\.[xyzw]|m_Name|m_RootOrder|'
                         r'm_StaticEditorFlags|m_IsActive|m_ConstrainProportionsScale)$')


def pack_files(pack, ext):
    out = []
    for folder, _, files in os.walk(pack.src):
        out += [os.path.join(folder, f) for f in files if f.lower().endswith(ext)]
    return sorted(out)


def guid_for_path(pack, path):
    with open(path + '.meta', encoding='utf-8', errors='replace') as f:
        return re.search(r'^guid: (\w+)', f.read(4096), re.M).group(1)


def strip_skipped(pack, roots, where):
    keep = []
    for o in roots:
        kinds = {t for t, _ in o.comps}
        if kinds & set(SKIP_COMPONENTS):
            pack.report['player rigs / cameras left out (the engine has its own)'].append(f'{where}: {o.name}')
            continue
        if 'ParticleSystem' in kinds:
            pack.report['particle systems left out (rebuild with the engine\'s Particle System)'].append(
                f'{where}: {o.name}')
        if 'Animator' in kinds or 'Animation' in kinds:
            pack.report['animated objects (doors and lifts became Openables; the rest stay static)'].append(
                f'{where}: {o.name}')
        if any(t == 'MonoBehaviour' for t, _ in o.comps):
            pack.counts['scripts dropped (MonoBehaviour)'] += 1
        o.children = strip_skipped(pack, o.children, where)
        keep.append(o)
    return keep


# Per-pack prefab scale overrides: prefab (root object) name -> uniform scale, applied wherever the prefab is emitted so the
# prefab files and every scene instance agree (colliders and child parts ride on the root entity). Measured against the
# 1.81 m Quantum rig: the office's metal desk and leather chair are too tall.
SCALE_OVERRIDES = {
    'OfficeAndPoliceStation': {'Desk_metal': 0.75 / 0.90, 'Chair_office_leather': 1.10 / 1.28},
}


# Architecture scale: the alley's building kit is 1.2-1.3x too big for the 1.81 m Quantum rig (doors 2.49 m, the narrowest
# alley 4+ m wide). A uniform factor shrinks the building pieces, and the whole scene layout scales about the world origin
# by the same factor, so the walls still tile and the props (kept at 1.0) still sit on the floors. 2.49 m x 0.82 = 2.04 m.
ARCH_SCALE = {'ModularCityAlleyPack': 0.82}
ARCH_FOLDERS = {'ModularCityAlleyPack': {'Walls', 'Floors', 'Roofs', 'Decals'}}
# building-mounted kit that lives in the Props folder (free-standing props stay out)
ARCH_PROP_RE = {'ModularCityAlleyPack': re.compile(
    r'^(Airduct|Awning|Balcony|Commercial$|Distant_Building|FireEscape|MetalPlateRoof_|MetalPlate_A$|Neon_Sign_|Sign_|Pipe_|'
    r'PlasticHanging|PowerLines_|Window_MetalBars_|WindowShutter|Door_MetalBars_|Tree_Groundplate|WireStraight)')}


def arch_class(pack_name, e):
    """True for a building piece, False for a free-standing prop; None when the entity is not a model."""
    path = e.get('path')
    if not path:
        return None
    seg = path.replace(chr(92), '/').split('/Meshes/')[-1].split('/')
    name = re.sub(r' \(\d+\)$', '', e['name'])
    if seg[0] in ARCH_FOLDERS[pack_name]:
        return True
    return bool(ARCH_PROP_RE[pack_name].match(name))


def arch_rescale(pack, out, prefab):
    """Apply ARCH_SCALE to an emitted prefab (root scale only) or scene (every entity directly under the scene or an
    identity group: building pieces get the scale, their positions all scale about the origin; props keep size)."""
    s = ARCH_SCALE.get(pack.name)
    if s is None:
        return
    ents = out['boxes'] + out['empties'] + out['models']
    ids = {e['id']: e for e in ents}
    skip = set()
    for e in ents:  # the spawn's rig and the template Sun are not part of the layout
        if e['name'] in ('Player Spawn', 'Sun'):
            skip.add(e['id'])
    for e in ents:
        q = e
        while q is not None and q['parentId'] in ids:
            q = ids[q['parentId']]
            if q['id'] in skip:
                skip.add(e['id'])
                break
    for e in ents:
        par = ids.get(e['parentId'])
        top = par is None or 'path' not in par
        if prefab:
            if par is not None or 'path' not in e:
                continue
        elif e['id'] in skip:
            if e['name'] == 'Player Spawn':
                e['position'] = r6([c * s for c in e['position']])
            continue
        elif top:
            e['position'] = r6([c * s for c in e['position']])
        if not top and not prefab:
            continue
        kind = arch_class(pack.name, e)
        if kind is None and e in out['boxes']:
            kind = True
        if kind:
            e['scale'] = r6([c * s for c in e['scale']])
        if kind is not None and not prefab:
            pack.counts[f'architecture scaled x{s}' if kind else 'free-standing props kept at 1.0'] += 1


# Player Spawn floor tops (world y of the surface under the spawn's x/z, taken from --scene-bounds): Unity's rig starts
# inside the geometry and its CharacterController pushes it out, which the engine's player doesn't do. (The alley spawn is
# on a sloped hill tile whose AABB top is 0.43 m above the real surface, so it keeps Unity's height.)
SPAWN_FLOOR_Y = {
    ('SuburbNeighborhoodHousePack', 'Demoscene_Day'): 0.5,
    ('SuburbNeighborhoodHousePack', 'Demoscene_Night'): 0.5,
    ('SuburbNeighborhoodHousePack', 'Showcase'): 0.5,
    ('OfficeAndPoliceStation', 'Showcase'): 0.0,
}


# The AAA look, so regenerating a scene keeps it. Every pack scene gets the physical sky, TAA, bloom, auto exposure and a
# gritty grade, with the lens effects (grain, vignette, CA, motion blur, DOF) off. The playable scenes then get their own
# time of day on top; the catalog scenes (Showcase, Prefabs, Asset_list) keep the base look only.
LOOK_BASE = dict(
    skySource=2, antiAliasing='TAA', msaaSamples=1, fxaa=False, tonemapOperator=1,
    filmGrain=0.0, vignetteIntensity=0.0, chromaticAberration=0.0, motionBlur=False, depthOfField=False,
    bloomEnabled=True, bloomThreshold=1.0, bloomKnee=0.5, bloomIntensity=0.14,
    autoExposure=True, autoExposureMinEV=-3.0, autoExposureMaxEV=3.0, autoExposureSpeedUp=1.5, autoExposureSpeedDown=1.0,
    gradeContrast=14.0, gradeSaturation=-10.0, gradeTint=0.0,
)
LOOK_DAY = dict(
    exposureEV=-0.3, gradeTemperature=2.0, gradeColorFilter=[1.0, 0.99, 0.97],
    volumetricEnabled=True, volDensity=0.004, volAnisotropy=0.7, volHeightFalloff=0.08, volDistance=96.0, volQuality=1,
)
# Dark and lamp-lit: a dim moon, the street lamps and windows carry the light, the streets are wet.
LOOK_NIGHT = dict(
    exposureEV=1.0, autoExposureMinEV=-1.0, autoExposureMaxEV=4.0, gradeTemperature=-10.0,
    gradeColorFilter=[0.9, 0.97, 1.05], bloomIntensity=0.2,
    volumetricEnabled=True, volDensity=0.008, volAnisotropy=0.6, volHeightFalloff=0.1, volDistance=64.0, volQuality=1,
    wetness=0.85, puddleAmount=0.45, puddleScale=4.0,
)
LOOK_SCENES = {
    ('ModularCityAlleyPack', 'Example_Scene_Day'): LOOK_DAY,
    ('ModularCityAlleyPack', 'Example_Scene_Night'): LOOK_NIGHT,
    ('OfficeAndPoliceStation', 'OfficePack_Example'): dict(
        exposureEV=0.0, gradeTemperature=-2.0, gradeColorFilter=[0.97, 1.0, 1.0], volumetricEnabled=False),
    # ~100 real-time lamps: 2 sun/moon cascades keep the street in budget (4 cost 1.7 ms more for no visible gain).
    ('SuburbNeighborhoodHousePack', 'Demoscene_Day'): dict(LOOK_DAY, shadowCascades=2),
    ('SuburbNeighborhoodHousePack', 'Demoscene_Night'): dict(LOOK_NIGHT, shadowCascades=2),
    # Low ground mist hugging the trail (the terrain sits around y 3-4), lit through the canopy for god rays.
    ('SeedMesh_Mossy_Cedrus_Forest', 'Mossy_forest_trail'): dict(
        LOOK_DAY, gradeTemperature=0.0, gradeColorFilter=[0.97, 1.0, 0.96],
        volDensity=0.08, volHeightFalloff=0.4, volBaseHeight=3.0, volAnisotropy=0.75, volDistance=80.0, shadowCascades=2),
}
NIGHT_MOON_SKY = 0.006  # sky moonBrightness: dim, though the full high moon from the night offset is still there to see by
NIGHT_SHADOW_RES = 2048  # world shadowResolution for night scenes (moon map); 4096 default
NIGHT_CLOUD_QUALITY = 0 # sky cloudQuality 0 = Low
NIGHT_MOON_LIGHT = 0.35 # the moon's directional light (the Unity scene's own, or the template Sun standing in for it)
NIGHT_LAMP_VOLUMETRIC = 0.5  # point/spot lights' Volumetric Intensity: halves the street-lamp beams in the night fog


def apply_look(pack, stem, scene, empties):
    scene.update(LOOK_BASE)
    look = LOOK_SCENES.get((pack.name, stem))
    if look is None:
        return
    scene.update(look)
    if 'night' in stem.lower():
        scene['sky'] = dict(scene['sky'], moonBrightness=NIGHT_MOON_SKY, cloudQuality=NIGHT_CLOUD_QUALITY)
        scene['shadowResolution'] = NIGHT_SHADOW_RES
        for e in empties:
            light = e.get('Light') or {}
            if light.get('Type') == 'Directional':
                light['Intensity'] = NIGHT_MOON_LIGHT
            elif light.get('Type') in ('Point', 'Spot'):
                light['Volumetric Intensity'] = NIGHT_LAMP_VOLUMETRIC
    pack.counts['scenes given the AAA look preset'] += 1


def convert(pack, exe):
    conv = Converter(pack, exe)
    fbx = [guid_for_path(pack, p) for p in pack_files(pack, '.fbx')]
    conv.load_fbx(fbx)
    for g in fbx:  # every model, used by a prefab or not
        conv.model_entity_path(g)
    for p in pack_files(pack, '.mat'):
        pack.material(guid_for_path(pack, p))
    for p in pack_files(pack, '.prefab'):
        roots, _, _ = conv.parse_file(p)
        roots = strip_skipped(pack, roots, os.path.basename(p))
        if not roots:
            continue
        out = {'formatVersion': 4, 'boxes': [], 'empties': [], 'models': []}
        conv.emit(roots[:1], out, [-1])
        arch_rescale(pack, out, True)
        dst = pack.out_path(p, '.prefab')
        write_json(dst, out)
        write_meta(dst, os.path.dirname(os.path.relpath(dst, os.path.join(pack.repo, 'project')).replace(os.sep, '/')),
                   'prefab')
        pack.counts['prefabs'] += 1
    for p in pack_files(pack, '.unity'):
        write_scene(conv, p)
    return conv


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


def dedupe_openables(out):
    """Two prefabs can each carry the same door in the same place (an elevator car and the wall around its shaft). Only
    one of them opens under Interact, so the twin would stay shut, in view and in the way: the later one is removed.
    (A leaf has no children; the removed entity's id is simply left unused.)"""
    ents = {e['id']: e for k in ('models', 'empties') for e in out[k]}
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
    kept, twins = [], []
    for e in out['models']:
        if 'Openable' not in e:
            continue
        pos, rot, _ = world(e)
        twin = any(k[0] == e['path'] and k[3] == e['Openable']['Motion'] and
                   max(abs(a - b) for a, b in zip(k[1], pos)) < 0.1 and abs(sum(a * b for a, b in zip(k[2], rot))) > 0.999
                   for k in kept)
        if twin:
            twins.append(e)
        else:
            kept.append((e['path'], pos, rot, e['Openable']['Motion']))
    out['models'] = [e for e in out['models'] if not any(e is t for t in twins)]
    dropped = len(twins)
    return dropped


def _walk(objs):
    for o in objs:
        yield o
        yield from _walk(o.children)


def spawn_on_terrain(conv, pack, out, keep):
    """The player spawn must stand above the scene's terrain mesh: one outside the terrain (the Unity rig can sit anywhere)
    moves to a point a quarter of the way across it, and one below the surface is lifted to a metre above it."""
    ter = next((e for e in out['models'] if e.get('path') in conv.terrain_mats and e['parentId'] == -1), None)
    spawn = next((e for e in keep if e['name'] == 'Player Spawn'), None)
    if ter is None or spawn is None:
        return
    vs = []
    with open(os.path.join(pack.repo, 'project', *ter['path'].split('/')), encoding='utf-8') as f:
        for line in f:
            if line.startswith('v '):
                _, x, y, z = line.split()
                vs.append((float(x), float(y), float(z)))
    nx = next(i for i, v in enumerate(vs) if v[2] != vs[0][2])
    nz = len(vs) // nx
    x0, x1, z0, z1 = vs[nx - 1][0], vs[0][0], vs[0][2], vs[-1][2]   # the OBJ is mirrored: x runs from 0 down to -width

    def height(x, z):
        i = min(max(round((x1 - x) / (x1 - x0) * (nx - 1)), 0), nx - 1)
        j = min(max(round((z - z0) / (z1 - z0) * (nz - 1)), 0), nz - 1)
        return vs[j * nx + i][1] * ter['scale'][1] + ter['position'][1]
    lx, lz = spawn['position'][0] - ter['position'][0], spawn['position'][2] - ter['position'][2]
    if not (x0 <= lx <= x1 and z0 <= lz <= z1):
        lx, lz = x1 + (x0 - x1) * 0.25, z0 + (z1 - z0) * 0.25
        spawn['position'][0], spawn['position'][2] = r6([lx + ter['position'][0]])[0], r6([lz + ter['position'][2]])[0]
        pack.report['player spawn outside the terrain (moved a quarter of the way across it)'].append(pack.name)
    ground = height(lx, lz)
    if spawn['position'][1] < ground + 1.0:
        spawn['position'][1] = round(ground + 1.0, 6)
        pack.report['player spawn below the terrain (lifted to 1 m above it)'].append(pack.name)


def scene_bounds(conv, scene):
    """World AABB of every model/box entity of an in-memory scene, by entity id (the engine's --scene-bounds on a temp copy
    whose names carry the id, since bounds come back in registry order)."""
    import copy
    import shutil
    import tempfile
    tmp = tempfile.mkdtemp(prefix='unity_bounds_')
    try:
        sc = copy.deepcopy(scene)
        for k in ('models', 'boxes'):
            for e in sc[k]:
                e['name'] = f"{e['name']}~{e['id']}"
        src = os.path.join(tmp, 'bounds_tmp.json')
        write_json(src, sc)
        subprocess.run([conv.exe, '--project', os.path.join(conv.pack.repo, 'project'), '--scene-bounds', src, '--out', tmp],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
        res = json.load(open(os.path.join(tmp, 'scenes', 'bounds_tmp.json'), encoding='utf-8'))
        out = {}
        for x in res['entities']:
            out[int(x['path'].split('/')[-1].rsplit('~', 1)[1])] = (list(x['min']), list(x['max']))
        return out
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def settle_props(pack, out, b0, b1):
    """After ARCH_SCALE scaled the layout about the origin, put every free-standing prop back on what it rested on in the
    unscaled scene (--scene-bounds before/after): its pivot may not be at its base, and a prop stacked on another prop
    (a box on a table) must follow that prop, which kept its size. The gap above the support scales with a building
    support and stays as is on a prop."""
    s = ARCH_SCALE[pack.name]
    ents = out['models'] + out['boxes']
    ids = {e['id']: e for e in out['boxes'] + out['empties'] + out['models']}
    skip = {e['id'] for e in ids.values() if e['name'] in ('Player Spawn', 'Sun')}
    for e in ids.values():
        q = e
        while q['parentId'] in ids:
            q = ids[q['parentId']]
            if q['id'] in skip:
                skip.add(e['id'])
                break
    box_ids = {e['id'] for e in out['boxes']}
    cls = {}
    for e in out['models']:
        if e['id'] in skip or e['id'] not in b0 or e['id'] not in b1:
            continue
        par = ids.get(e['parentId'])
        cls[e['id']] = None if (par is not None and 'path' in par) else arch_class(pack.name, e)
    cur = {i: [list(b1[i][0]), list(b1[i][1])] for i in cls}
    decals = {i for i, e in ((i, ids[i]) for i in cls) if (e['path'].replace(chr(92), '/').split('/Meshes/')[-1].split('/')[0]
                                                         in ('Decals',))}
    supports = [i for i in cls if i not in decals and i not in box_ids]
    props = sorted((i for i in cls if cls[i] is False), key=lambda i: b0[i][0][1])
    fixed = skipped = 0
    for i in props:
        lo, hi = b0[i]
        cx, cz = (lo[0] + hi[0]) / 2, (lo[2] + hi[2]) / 2
        best = None
        for j in supports:
            if j == i:
                continue
            slo, shi = b0[j]
            if slo[0] <= cx <= shi[0] and slo[2] <= cz <= shi[2] and shi[1] <= lo[1] + 0.05 and (best is None or shi[1] > b0[best][1][1]):
                best = j
        if best is None or lo[1] - b0[best][1][1] > 0.3:
            continue
        gap = lo[1] - b0[best][1][1]
        want = cur[best][1][1] + (s if cls[best] else 1.0) * gap
        dy = want - cur[i][0][1]
        if abs(dy) <= 0.004:
            continue
        if abs(dy) > 0.4:
            skipped += 1
            continue
        ids[i]['position'][1] = round(ids[i]['position'][1] + dy, 6)
        cur[i][0][1] += dy
        cur[i][1][1] += dy
        fixed += 1
    pack.counts['props re-seated on their floor after the architecture scale'] += fixed
    if skipped:
        pack.counts['props left unseated (> 0.4 m off)'] += skipped


def write_scene(conv, path):
    pack = conv.pack
    roots, _, _ = conv.parse_file(path)
    stem = os.path.splitext(os.path.basename(path))[0]
    spawn = None

    def find_rig(objs):
        nonlocal spawn
        for o in objs:
            if spawn is None and {t for t, _ in o.comps} & {'CharacterController', 'Camera'}:
                spawn = o
            find_rig(o.children)
    find_rig(roots)
    roots = strip_skipped(pack, roots, stem)
    tpl_path = os.path.join(pack.repo, 'tools', 'scenes', 'sandbox_template.json')
    tpl = json.load(open(tpl_path, encoding='utf-8'))
    scene = {k: v for k, v in tpl.items() if k not in ('boxes', 'empties', 'models', '_comment')}
    keep = [e for e in tpl.get('empties', []) if e['name'] in ('Player Spawn', 'Sun', 'Player Body')]
    ids = {e['id'] for e in keep}
    for e in tpl.get('empties', []):  # the spawn's subtree (player body, weapons) comes with it
        if e.get('parentId') in ids and e['id'] not in ids:
            keep.append(e)
            ids.add(e['id'])
    models = [m for m in tpl.get('models', []) if m.get('parentId') in ids]
    if spawn is not None:
        for e in keep:
            if e['name'] == 'Player Spawn':
                p, _, _ = to_engine(spawn.pos, spawn.rot, spawn.scale)
                e['position'] = r6(p)
    floor = SPAWN_FLOOR_Y.get((pack.name, stem))
    if floor is not None:  # a scene with no Unity rig keeps the template's spawn x/z, but still stands on the floor
        for e in keep:
            if e['name'] == 'Player Spawn':
                e['position'][1] = floor
    if 'night' in stem.lower():
        # 23:00 on the template's day/latitude puts the moon below the horizon and a third lit, so the whole scene went
        # black. Offset the lunar phase to a high, full moon (its light drives the scene), expose a little up from the
        # template, and match the template Sun (3.0, a daylight value) to the 0.7 a Unity night scene uses for its moon.
        scene['sky'] = dict(scene['sky'], timeOfDayHours=23.0, moonPhaseOffset=0.68, moonBrightness=0.02)
        scene['exposureEV'] = 0.5
        for e in keep:
            if e['name'] == 'Sun' and e.get('Light'):
                e['Light']['Intensity'] = 0.7
    out = {'boxes': [], 'empties': keep, 'models': models}
    start = max([e['id'] for e in keep + models] + [0])
    conv.emit(roots, out, [start])
    pack.counts['duplicate openables dropped (a twin door within 10 cm)'] += dedupe_openables(out)
    # The scene's own directional light replaces the template's Sun (two suns blow every surface out to white).
    if any((e.get('Light') or {}).get('Type') == 'Directional' for e in out['empties'] if e['name'] != 'Sun'):
        out['empties'] = [e for e in out['empties'] if e['name'] != 'Sun']
    apply_look(pack, stem, scene, out['empties'])
    # The template's physical sky draws a grey planet below the horizon, which reads as a floor that Unity's scene doesn't
    # have (HDRP shows sky all round). A scene with no terrain of its own turns that planet off; one with a Unity terrain
    # (not converted yet) keeps it so the trail still looks like it sits on something.
    if pack.name.startswith('SeedMesh') and not any(t == 'Terrain' for o in _walk(roots) for t, _ in o.comps):
        scene['sky'] = dict(scene['sky'], showGround=False)
    spawn_on_terrain(conv, pack, out, keep)
    scene.update(out)
    if pack.name in ARCH_SCALE:
        b0 = scene_bounds(conv, scene)
        arch_rescale(pack, out, False)
        settle_props(pack, out, b0, scene_bounds(conv, scene))
    scene['formatVersion'] = 4
    dst = os.path.join(pack.repo, 'project', 'scenes', f'{pack.name}_{stem}.json')
    write_json(dst, scene)
    write_meta(dst, 'scenes', 'scene')
    pack.counts['scenes'] += 1
