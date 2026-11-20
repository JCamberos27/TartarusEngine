# Helpers for scene generators: the model index (path, guid and engine-measured bounds of every imported model)
# and the scene-JSON pieces (empties, models, boxes, lights) in the format SceneSerializer reads.
import argparse
import json
import math
import os
import secrets
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'assets'))
from import_model_library import model_report, write_json  # noqa: E402

ROOTS = ['assets/Architecture', 'assets/Furniture', 'assets/Props']
SCENE_FORMAT_VERSION = 4   # = kSceneFormatVersion (src/Game/SceneSerializerInternal.h): bump both together


def model_index(repo, exe=None):
    """stem -> {'path', 'guid', 'min', 'max'}; bounds are after the .meta's scale / axis / pivot. Exits if the engine
    could not read a model (a stem missing from the index would only fail later, as a KeyError)."""
    project = os.path.join(repo, 'project')
    exe = exe or find_exe(repo)
    report = model_report(exe, [os.path.join(project, *r.split('/')) for r in ROOTS])   # exits on a failed model
    index = {}
    for root in ROOTS:
        for folder, _, files in os.walk(os.path.join(project, *root.split('/'))):
            for name in files:
                if not name.lower().endswith('.fbx'):
                    continue
                full = os.path.join(folder, name)
                r = report.get(os.path.normcase(os.path.normpath(full)))   # the report's keys: its paths, normcased
                if not r:
                    sys.exit(f'scene_kit.model_index: --model-report has no row for {full}')
                with open(full + '.meta', encoding='utf-8') as f:
                    g = json.load(f)['guid']
                rel = os.path.relpath(full, project).replace(os.sep, '/')
                index[os.path.splitext(name)[0]] = {'path': rel, 'guid': g, 'min': r['min'], 'max': r['max'],
                                                    'meshes': r['meshes']}
    return index


# --- quaternions ([x, y, z, w]) ----------------------------------------------------------------------------------
def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]


def qrot(q, v):
    """v rotated by the unit quaternion q."""
    x, y, z, w = q
    vx, vy, vz = v
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return [vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx)]


def qinv(q):
    """The inverse of a unit quaternion."""
    return [-q[0], -q[1], -q[2], q[3]]


def axis_q(axis, deg):
    """deg degrees about the unit vector axis."""
    h = math.radians(deg) / 2
    s = math.sin(h)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h)]


def euler_quat(yaw=0.0, pitch=0.0, roll=0.0):
    """[x, y, z, w] for yaw about Y, then pitch about X, then roll about Z (degrees; each in the frame left by the
    one before, so yaw turns the piece and pitch / roll tip it about its own axes)."""
    hy, hp, hr = (math.radians(v) / 2 for v in (yaw, pitch, roll))
    q = qmul(qmul([0.0, math.sin(hy), 0.0, math.cos(hy)], [math.sin(hp), 0.0, 0.0, math.cos(hp)]),
             [0.0, 0.0, math.sin(hr), math.cos(hr)])
    return [round(v, 6) for v in q]


# --- asset references and scene files ---------------------------------------------------------------------------
def ref(repo, path):
    """{'path', 'pathGuid'} for a project-relative asset (the guid from its .meta); '' for no path, as the engine
    writes an empty slot."""
    if not path:
        return ''
    with open(os.path.join(repo, 'project', path + '.meta'), encoding='utf-8') as f:
        return {'path': path, 'pathGuid': json.load(f)['guid']}


def write_scene_meta(path):
    """The scene's .meta with a new guid, unless it has one: settings.json and the build list point at that guid."""
    meta = path + '.meta'
    if os.path.exists(meta):
        return
    with open(meta, 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'guid': secrets.token_hex(8), 'metaVersion': 1, 'type': 'scene'}, f, indent=2)
        f.write('\n')


def descendants(ents, root):
    """Every entity under `root` (by parentId), at any depth."""
    kids = {}
    for e in ents:
        kids.setdefault(e['parentId'], []).append(e)
    out, todo = [], [root]
    while todo:
        for e in kids.get(todo.pop(), []):
            out.append(e)
            todo.append(e['id'])
    return out


# --- Door_Op_1 in a wall ----------------------------------------------------------------------------------------
DOOR_WALL_Z = 0.032   # the frame's wall centre, door-local +Z of its origin


def door_pose(axis, line, along, sign):
    """(yaw, x, z) of a Door_Op_1 frame in a wall running along `axis` ('x': at z = line, 'z': at x = line), centred at
    `along`; sign +1 when the room it swings into is on the line's + side. The door is modelled with its inside (chain,
    catch, hinge knuckles) on its back, local -Z: that faces the room it swings into, and it swings that way (Swing
    Back). The frame sits on the wall's centre line."""
    if axis == 'x':
        yaw, wx, wz = (180.0 if sign > 0 else 0.0), along, line
    else:
        yaw, wx, wz = (-90.0 if sign > 0 else 90.0), line, along
    return yaw, wx - math.sin(math.radians(yaw)) * DOOR_WALL_Z, wz - math.cos(math.radians(yaw)) * DOOR_WALL_Z


def first(it, what):
    """The first item of `it`; exits naming `what` when there is none (instead of a bare StopIteration)."""
    v = next(iter(it), None)
    if v is None:
        sys.exit(f'{os.path.basename(sys.argv[0])}: no {what} found')
    return v


class Scene:
    def __init__(self, base):
        skip = {'boxes', 'empties', 'models', 'libraryMaterials', 'libraryModels', 'libraryPrefabs', 'librarySounds',
                'libraryTextures'}
        self.data = {k: v for k, v in base.items() if k not in skip}
        self.data['formatVersion'] = SCENE_FORMAT_VERSION
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
            'Shadow Softness': softness, 
            'Spot Angle': spot or 30.0, 'Type': 'Spot' if spot else 'Point'}, **extra)

    def write(self, path):
        self.data.update({'boxes': self.boxes, 'empties': self.empties, 'models': self.models})
        write_json(path, self.data)

    # --- the player ------------------------------------------------------------------------------------------------
    def copy_player(self, arena, pos, yaw=0.0, controller=None):
        """Arena's player at pos: its spawn (First Person Controller, `controller` keys overriding Arena's), body and
        the body's Torso / Head / Arms pieces. Returns the body's id, for the caller's outfit_piece()s."""
        spawn = first((e for e in arena['empties'] if 'First Person Controller' in e), "Arena's player spawn")
        body = first((e for e in arena['empties'] if e.get('parentId') == spawn['id'] and 'First Person Body' in e),
                     "Arena's player body")
        ctrl = {k: v for k, v in spawn.items() if k[0].isupper()}
        ctrl['First Person Controller'] = {**ctrl['First Person Controller'], **(controller or {})}
        sid = self.empty('Player Spawn', -1, pos, yaw=yaw, **ctrl)
        bid = self.empty('Player Body', sid, tuple(body['position']), **{k: v for k, v in body.items() if k[0].isupper()})
        for p in arena['models']:
            if p.get('parentId') == body['id'] and p.get('Outfit Piece', {}).get('Slot') in ('Torso', 'Head', 'Arms'):
                e = self._common(p['name'], bid, tuple(p['position']), 0.0)
                e['rotation'] = p['rotation']
                e.update({k: v for k, v in p.items() if k in ('path', 'pathGuid', 'materials', 'Outfit Piece')})
                self.models.append(e)
        return bid

    def outfit_piece(self, repo, body, slot, path, mat, flags):
        """A worn piece under the player's body: the model at path in the .mat mat (Outfit Piece Flags: 1 body
        part, 0 clothing, 2 a rigid attachment)."""
        m = ref(repo, path)
        e = self._common(slot, body, (0.0, 0.0, 0.0), 0.0)
        e.update({**m, 'materials': [{'guid': ref(repo, mat)['pathGuid'], 'path': mat}],
                  'Outfit Piece': {'Flags': flags, 'Item': m, 'Slot': slot}})
        self.models.append(e)


def light_flicker(Mode=0, MinInterval=4.0, MaxInterval=15.0, Depth=1.0, Seed=1):
    """The native "Light Flicker" component (Mode: 0 Fluorescent, 1 Tv, 2 Candle)."""
    return {'Light Flicker': {'Mode': ['Fluorescent', 'Tv', 'Candle'][Mode], 'Min Interval': MinInterval,
                              'Max Interval': MaxInterval, 'Depth': Depth, 'Seed': Seed}}


def from_mat(repo, mat_path, grunge=None, grunge_strength=0.0, grunge_scale=0.5, receive_decals=True, **overrides):
    """An embedded material copied from a .mat asset's properties (e.g. a lamp shade's, with its glow switched
    off: emissive_strength=0). grunge* / receive_decals: see _decal_keys."""
    with open(os.path.join(repo, 'project', mat_path), encoding='utf-8') as f:
        p = json.load(f)['properties']

    m = {'albedoMap': ref(repo, p.get('_AlbedoMap')), 'aoMap': ref(repo, p.get('_AOMap')), 'baseColor': p.get('_BaseColor', [1, 1, 1]),
         'emissiveColor': p.get('_EmissiveColor', [0, 0, 0]), 'emissiveMap': ref(repo, p.get('_EmissiveMap')),
         'emissiveStrength': p.get('_EmissiveStrength', 0.0), 'factorsScaleMaps': True, 'metallic': p.get('_Metallic', 0.0),
         'metallicMap': ref(repo, p.get('_MetallicMap')), 'metallicRoughnessMap': ref(repo, p.get('_MetallicRoughnessMap')),
         'normalMap': ref(repo, p.get('_NormalMap')), 'roughness': p.get('_Roughness', 0.6),
         'roughnessMap': ref(repo, p.get('_RoughnessMap')), 'triplanar': False, 'triplanarScale': 1.0}
    m.update(_decal_keys(repo, grunge, grunge_strength, grunge_scale, receive_decals))
    for key, value in overrides.items():
        m[{'emissive_strength': 'emissiveStrength'}.get(key, key)] = value
    return m


def _decal_keys(repo, grunge, grunge_strength, grunge_scale, receive_decals):
    """The embedded material's grunge map (a project-relative texture laid over the surface triplanar in world
    space, docs/DECALS.md), its strength (0 = off) and tiles per metre, and whether clustered decals project onto
    it. Only the non-default ones, as the engine writes them, so materials without them don't change."""
    m = {}
    if grunge:
        m['grungeMap'] = ref(repo, grunge)
    if grunge_strength != 0.0:
        m['grungeStrength'] = grunge_strength
    if grunge_scale != 0.5:
        m['grungeScale'] = grunge_scale
    if not receive_decals:
        m['receiveDecals'] = False
    return m


def textured(repo, albedo=None, normal=None, rough=None, ao=None, base=(1.0, 1.0, 1.0), roughness=0.8, metallic=0.0,
             triplanar_scale=1.0, emissive=(0.0, 0.0, 0.0), strength=0.0, grunge=None, grunge_strength=0.0,
             grunge_scale=0.5, receive_decals=True):
    """An embedded box material over imported texture paths (project-relative), triplanar so boxes of any size tile."""
    m = {'albedoMap': ref(repo, albedo), 'aoMap': ref(repo, ao), 'baseColor': list(base), 'emissiveColor': list(emissive),
         'emissiveMap': '', 'emissiveStrength': strength, 'factorsScaleMaps': True, 'metallic': metallic,
         'metallicMap': '', 'metallicRoughnessMap': '', 'normalMap': ref(repo, normal), 'roughness': roughness,
         'roughnessMap': ref(repo, rough), 'triplanar': bool(albedo or normal), 'triplanarScale': triplanar_scale}
    m.update(_decal_keys(repo, grunge, grunge_strength, grunge_scale, receive_decals))
    return m


def builder_args(scene, what):
    """<repo> (default: this checkout) and --force, for a builder that writes a whole committed scene."""
    ap = argparse.ArgumentParser(description=f'Rebuild the {scene} scene from scratch.')
    ap.add_argument('repo', nargs='?', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    ap.add_argument('--force', action='store_true', help=f'overwrite the existing scene and lose {what}')
    args = ap.parse_args()
    args.repo = os.path.abspath(args.repo)
    return args


def refuse_overwrite(path, args, what):
    if os.path.exists(path) and not args.force:
        sys.exit(f'{path} exists and has {what}, which a rebuild would lose. Pass --force to overwrite it.')


def find_exe(repo):
    """TARTARUS_EXE (another build, e.g. a worktree's), else the repo's build/Release, else b/Release."""
    exe = os.environ.get('TARTARUS_EXE')
    if exe:
        if not os.path.isfile(exe):
            sys.exit(f'TARTARUS_EXE={exe} does not exist')
        return exe
    for d in ('build', 'b'):
        exe = os.path.join(repo, d, 'Release', 'TartarusEngine.exe')
        if os.path.isfile(exe):
            return exe
    sys.exit(f'No TartarusEngine.exe under {repo}\\build\\Release or b\\Release: build it, or set TARTARUS_EXE')
