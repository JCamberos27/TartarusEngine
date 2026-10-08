# Helpers for scene generators: the model index (path, guid and engine-measured bounds of every imported model)
# and the scene-JSON pieces (empties, models, boxes, lights) in the format SceneSerializer reads.
import json
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'assets'))
from import_model_library import model_report, write_json  # noqa: E402

ROOTS = ['assets/Architecture', 'assets/Furniture', 'assets/Props']


def model_index(repo, exe=None):
    """stem -> {'path', 'guid', 'min', 'max'}; bounds are after the .meta's scale / axis / pivot."""
    project = os.path.join(repo, 'project')
    exe = exe or os.path.join(repo, 'build', 'Release', 'TartarusEngine.exe')
    cwd = os.getcwd()
    os.chdir(repo)
    try:
        report = model_report(exe, ['project/' + r for r in ROOTS])
    finally:
        os.chdir(cwd)
    index = {}
    for root in ROOTS:
        for folder, _, files in os.walk(os.path.join(project, *root.split('/'))):
            for name in files:
                if not name.lower().endswith('.fbx'):
                    continue
                full = os.path.join(folder, name)
                # the report's keys are the paths it was given, normcased
                r = report.get(os.path.normcase(os.path.normpath(os.path.relpath(full, repo))))
                if not r:
                    continue
                with open(full + '.meta', encoding='utf-8') as f:
                    g = json.load(f)['guid']
                rel = os.path.relpath(full, project).replace(os.sep, '/')
                index[os.path.splitext(name)[0]] = {'path': rel, 'guid': g, 'min': r['min'], 'max': r['max'],
                                                    'meshes': r['meshes']}
    return index


def yaw_quat(deg):
    h = math.radians(deg) / 2
    return [0.0, round(math.sin(h), 6), 0.0, round(math.cos(h), 6)]


def euler_quat(yaw=0.0, pitch=0.0, roll=0.0):
    """[x, y, z, w] for yaw about Y, then pitch about X, then roll about Z (degrees; each in the frame left by the
    one before, so yaw turns the piece and pitch / roll tip it about its own axes)."""
    def mul(a, b):
        ax, ay, az, aw = a
        bx, by, bz, bw = b
        return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]
    hy, hp, hr = (math.radians(v) / 2 for v in (yaw, pitch, roll))
    q = mul(mul([0.0, math.sin(hy), 0.0, math.cos(hy)], [math.sin(hp), 0.0, 0.0, math.cos(hp)]),
            [0.0, 0.0, math.sin(hr), math.cos(hr)])
    return [round(v, 6) for v in q]


class Scene:
    def __init__(self, base):
        skip = {'boxes', 'empties', 'models', 'libraryMaterials', 'libraryModels', 'libraryPrefabs', 'librarySounds',
                'libraryTextures'}
        self.data = {k: v for k, v in base.items() if k not in skip}
        self.data['formatVersion'] = 4
        self.boxes, self.empties, self.models = [], [], []
        self._id = 0

    def _next(self):
        self._id += 1
        return self._id

    def _common(self, name, parent, pos, yaw, scale=(1.0, 1.0, 1.0), pitch=0.0, roll=0.0):
        i = self._next()
        return {'id': i, 'name': name, 'order': i, 'parentId': parent, 'position': [round(v, 4) for v in pos],
                'rotation': euler_quat(yaw, pitch, roll), 'scale': list(scale)}

    def empty(self, name, parent=-1, pos=(0, 0, 0), yaw=0.0, pitch=0.0, **components):
        e = self._common(name, parent, pos, yaw, pitch=pitch)
        e.update(components)
        self.empties.append(e)
        return e['id']

    def model(self, index, stem, parent, pos, yaw=0.0, name=None, scale=1.0, pitch=0.0, roll=0.0, **extra):
        m = index[stem]
        e = self._common(name or stem, parent, pos, yaw, (scale, scale, scale), pitch, roll)
        e.update({'path': m['path'], 'pathGuid': m['guid']})
        e.update(extra)
        self.models.append(e)
        return e

    def box(self, name, parent, center, size, material, yaw=0.0, collider=True, **extra):
        e = self._common(name, parent, center, yaw, scale=[round(v, 4) for v in size])
        e.update({'color': [1.0, 1.0, 1.0], 'materials': [{'embedded': material}]})
        if collider:
            e['collider'] = {'friction': 0.8, 'isTrigger': False}
        e.update(extra)
        self.boxes.append(e)
        return e

    def light(self, name, parent, pos, intensity=2.2, rng=4.5, color=(1.0, 0.86, 0.7), shadows=False, kelvin=0.0,
              spot=None, pitch=-90.0, yaw=0.0, softness=1.0, **extra):
        """A point light, or with spot=<half-angle degrees> a spot aimed along local -Z: the default pitch -90 aims
        it straight down. kelvin > 0 drives the colour from a temperature instead of `color`."""
        return self.empty(name, parent, pos, yaw=yaw if spot else 0.0, pitch=pitch if spot else 0.0, Light={
            'Angular Size': 0.53, 'Cast Shadows': shadows, 'Color': list(color), 'ColorTempK': kelvin, 'Culling Mask': -1,
            'Intensity': intensity, 'Range': rng, 'Shadow Bias': 1.0, 'Shadow Near Plane': 0.05, 'Shadow Normal Bias': 1.0,
            'Shadow Resolution': 'Follow global', 'Shadow Softness': softness, 'Shadow Update Mode': 'Dynamic',
            'Spot Angle': spot or 30.0, 'Type': 'Spot' if spot else 'Point'}, **extra)

    def write(self, path):
        self.data.update({'boxes': self.boxes, 'empties': self.empties, 'models': self.models})
        write_json(path, self.data)


def script(cls, source, **fields):
    """The "C# Script" component for a gameplay script class (fields in its Fields JSON)."""
    return {'C# Script': {'Class': cls, 'Source': source, 'Fields JSON': json.dumps(fields), 'Enabled': True,
                          'Scripts': '[]', 'Next Script ID': 1}}


def from_mat(repo, mat_path, **overrides):
    """An embedded material copied from a .mat asset's properties (e.g. a lamp shade's, with its glow switched
    off: emissive_strength=0)."""
    with open(os.path.join(repo, 'project', mat_path), encoding='utf-8') as f:
        p = json.load(f)['properties']

    def ref(path):
        if not path:
            return ''
        with open(os.path.join(repo, 'project', path + '.meta'), encoding='utf-8') as f:
            return {'path': path, 'pathGuid': json.load(f)['guid']}
    m = {'albedoMap': ref(p.get('_AlbedoMap')), 'aoMap': ref(p.get('_AOMap')), 'baseColor': p.get('_BaseColor', [1, 1, 1]),
         'emissiveColor': p.get('_EmissiveColor', [0, 0, 0]), 'emissiveMap': ref(p.get('_EmissiveMap')),
         'emissiveStrength': p.get('_EmissiveStrength', 0.0), 'factorsScaleMaps': True, 'metallic': p.get('_Metallic', 0.0),
         'metallicMap': ref(p.get('_MetallicMap')), 'metallicRoughnessMap': ref(p.get('_MetallicRoughnessMap')),
         'normalMap': ref(p.get('_NormalMap')), 'roughness': p.get('_Roughness', 0.6),
         'roughnessMap': ref(p.get('_RoughnessMap')), 'triplanar': False, 'triplanarScale': 1.0}
    for key, value in overrides.items():
        m[{'emissive_strength': 'emissiveStrength'}.get(key, key)] = value
    return m


def textured(repo, albedo=None, normal=None, rough=None, ao=None, base=(1.0, 1.0, 1.0), roughness=0.8, metallic=0.0,
             triplanar_scale=1.0, emissive=(0.0, 0.0, 0.0), strength=0.0):
    """An embedded box material over imported texture paths (project-relative), triplanar so boxes of any size tile."""
    def ref(p):
        if not p:
            return ''
        with open(os.path.join(repo, 'project', p + '.meta'), encoding='utf-8') as f:
            return {'path': p, 'pathGuid': json.load(f)['guid']}
    return {'albedoMap': ref(albedo), 'aoMap': ref(ao), 'baseColor': list(base), 'emissiveColor': list(emissive),
            'emissiveMap': '', 'emissiveStrength': strength, 'factorsScaleMaps': True, 'metallic': metallic,
            'metallicMap': '', 'metallicRoughnessMap': '', 'normalMap': ref(normal), 'roughness': roughness,
            'roughnessMap': ref(rough), 'triplanar': bool(albedo or normal), 'triplanarScale': triplanar_scale}
