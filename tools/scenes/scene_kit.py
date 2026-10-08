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
                index[os.path.splitext(name)[0]] = {'path': rel, 'guid': g, 'min': r['min'], 'max': r['max']}
    return index


def yaw_quat(deg):
    h = math.radians(deg) / 2
    return [0.0, round(math.sin(h), 6), 0.0, round(math.cos(h), 6)]


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

    def _common(self, name, parent, pos, yaw, scale=(1.0, 1.0, 1.0)):
        i = self._next()
        return {'id': i, 'name': name, 'order': i, 'parentId': parent, 'position': [round(v, 4) for v in pos],
                'rotation': yaw_quat(yaw), 'scale': list(scale)}

    def empty(self, name, parent=-1, pos=(0, 0, 0), yaw=0.0, **components):
        e = self._common(name, parent, pos, yaw)
        e.update(components)
        self.empties.append(e)
        return e['id']

    def model(self, index, stem, parent, pos, yaw=0.0, name=None, **extra):
        m = index[stem]
        e = self._common(name or stem, parent, pos, yaw)
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

    def light(self, name, parent, pos, intensity=2.2, rng=4.5, color=(1.0, 0.86, 0.7), shadows=False, kelvin=3000.0):
        return self.empty(name, parent, pos, Light={
            'Angular Size': 0.53, 'Cast Shadows': shadows, 'Color': list(color), 'ColorTempK': kelvin, 'Culling Mask': -1,
            'Intensity': intensity, 'Range': rng, 'Shadow Bias': 1.0, 'Shadow Near Plane': 0.05, 'Shadow Normal Bias': 1.0,
            'Shadow Resolution': 'Follow global', 'Shadow Softness': 1.0, 'Shadow Update Mode': 'Dynamic',
            'Spot Angle': 30.0, 'Type': 'Point'})

    def write(self, path):
        self.data.update({'boxes': self.boxes, 'empties': self.empties, 'models': self.models})
        write_json(path, self.data)


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
