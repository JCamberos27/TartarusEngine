# Generates project/scenes/BloodTest.json: a small range for the volumetric blood.
# A line of four soldiers stands with a wall close behind them (exit sprays and wall spatter), one under a
# low ceiling (spray clipped against it, ceiling drips), one beside a staircase (blood down the steps)
# and one by a stack of loose physics crates (blood on moving props). The player (Player Spawn and its
# whole Player Body subtree), the Sun and the sky / post settings are copied from the Sandbox, as
# gen_arena_scene.py does. Re-running overwrites hand edits.
# Usage: python tools/gen_blood_scene.py <repo root>
import json
import math
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..')
scenes = os.path.join(root, 'project', 'scenes')
sandbox = json.load(open(os.path.join(scenes, 'Sandbox.json'), encoding='utf-8'))

SKIP_KEYS = {'boxes', 'empties', 'models', 'libraryMaterials', 'libraryModels', 'libraryPrefabs',
             'librarySounds', 'libraryTextures'}
scene = {k: v for k, v in sandbox.items() if k not in SKIP_KEYS}
scene['formatVersion'] = 4
ents = {'boxes': [], 'empties': [], 'models': []}

_next = [0]
def nid():
    _next[0] += 1
    return _next[0] - 1

def fail(msg):
    sys.exit(f'gen_blood_scene: {msg}')

src = {}
for kind in ('boxes', 'empties', 'models'):
    for e in sandbox[kind]:
        src[e['id']] = (kind, e)

def find(name):
    hits = [i for i, (_, e) in src.items() if e['name'] == name]
    if len(hits) != 1: fail(f'expected one "{name}" in Sandbox.json, found {len(hits)}')
    return hits[0]

def subtree(top):
    ids, frontier = [top], [top]
    while frontier:
        p = frontier.pop()
        kids = sorted(i for i, (_, e) in src.items() if e.get('parentId') == p)
        ids += kids
        frontier += kids
    return ids

def copy_tree(top, parent=-1, **overrides):
    remap = {}
    for old in subtree(top):
        remap[old] = nid()
    for old in subtree(top):
        kind, e = src[old]
        c = json.loads(json.dumps(e))
        c['id'] = remap[old]
        c['order'] = remap[old]
        c['parentId'] = parent if old == top else remap[e['parentId']]
        if old == top: c.update(overrides)
        ents[kind].append(c)
    return remap[top]

def quat_euler(pitch=0.0, yaw=0.0, roll=0.0):
    # Degrees, applied yaw (Y) * pitch (X) * roll (Z); returns [x, y, z, w].
    p, y, r = (math.radians(a) * 0.5 for a in (pitch, yaw, roll))
    cy, sy, cp, sp, cr, sr = math.cos(y), math.sin(y), math.cos(p), math.sin(p), math.cos(r), math.sin(r)
    w = cy * cp * cr + sy * sp * sr
    x = cy * sp * cr + sy * cp * sr
    yy = sy * cp * cr - cy * sp * sr
    z = cy * cp * sr - sy * sp * cr
    return [round(x, 6), round(yy, 6), round(z, 6), round(w, 6)]

TEX = {'light': ('assets/Materials/Prototype/proto_grid_light.png', 'c90849c8fc6c88b0'),
       'dark': ('assets/Materials/Prototype/proto_grid_dark.png', '2b60657ce9aa4555'),
       'orange': ('assets/Materials/Prototype/proto_grid_orange.png', 'c389fb6bc574bb89')}

def mat(tex, rough=0.85, color=(1, 1, 1)):
    path, guid = TEX[tex]
    return [{'embedded': {'albedoMap': {'path': path, 'pathGuid': guid}, 'aoMap': '', 'baseColor': list(color),
                          'emissiveColor': [0, 0, 0], 'emissiveMap': '', 'emissiveStrength': 1.0,
                          'factorsScaleMaps': True, 'metallic': 0.0, 'metallicMap': '', 'metallicRoughnessMap': '',
                          'normalMap': '', 'roughness': rough, 'roughnessMap': '', 'triplanar': True,
                          'triplanarScale': 0.25}}]

# Light surfaces, so the blood reads against them.
GROUND, WALL, ACCENT = mat('light', 0.9), mat('light', 0.8, (0.8, 0.8, 0.8)), mat('orange', 0.75)
CRATE = mat('orange', 0.8, (0.85, 0.7, 0.55))

def group(name, parent=-1, pos=(0, 0, 0)):
    i = nid()
    ents['empties'].append({'id': i, 'name': name, 'order': i, 'parentId': parent, 'position': list(pos),
                            'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]})
    return i

def box(name, center, size, parent, m=WALL, yaw=0.0, pitch=0.0, friction=0.8, rigidbody=None):
    i = nid()
    e = {'collider': {'friction': friction, 'isTrigger': False}, 'color': [1.0, 1.0, 1.0],
         'id': i, 'materials': m, 'name': name, 'order': i, 'parentId': parent,
         'position': [round(v, 4) for v in center], 'rotation': quat_euler(pitch, yaw),
         'scale': [round(v, 4) for v in size]}
    if rigidbody: e['Rigidbody'] = rigidbody
    ents['boxes'].append(e)
    return i

def block(name, x, z, w, h, d, parent, m=WALL, yaw=0.0, y0=0.0):
    return box(name, (x, y0 + h * 0.5, z), (w, h, d), parent, m, yaw)

def rigidbody(mass):
    return {'Angular Damping': 0.05, 'Continuous Collision': False, 'Freeze Position X': False,
            'Freeze Position Y': False, 'Freeze Position Z': False, 'Freeze Rotation X': False,
            'Freeze Rotation Y': False, 'Freeze Rotation Z': False, 'Initial Velocity': [0.0, 0.0, 0.0],
            'Interpolate': 'Interpolate', 'Is Kinematic': False, 'Linear Damping': 0.05, 'Mass': mass,
            'Use Gravity': True}

# --- the range ------------------------------------------------------------------------------------
spawn_id = find('Player Spawn')
env = group('Environment')
box('Ground', (0, -0.1, 0), (30, 0.2, 30), env, GROUND)
block('Back Wall', 0, -6.0, 16.0, 4.0, 0.4, env)                 # 1.5 m behind the soldiers
block('West Wall', -8.2, -1.0, 0.4, 4.0, 10.0, env)
block('East Wall', 8.2, -1.0, 0.4, 4.0, 10.0, env)
box('Low Ceiling', (-4.5, 2.7, -4.0), (3.4, 0.2, 4.4), env, WALL)  # over the westmost soldier
for k in range(6):                                                # stairs up to the east, beside soldier 4
    block(f'Step {k + 1}', 6.6, -4.0 + k * 0.45, 2.4, 0.18 * (k + 1), 0.45, env, ACCENT)
# Surface panels along the west wall's inside: the impacts and holes by what they strike
# (ImpactFx reads the surface from the name). Tinted so each reads as its stuff.
surfaces = group('Surface Panels')
for k, (name, tint) in enumerate([('Metal Plate', (0.55, 0.57, 0.6)), ('Wood Panel', (0.7, 0.5, 0.3)),
                                  ('Brick Panel', (0.65, 0.3, 0.22)), ('Glass Pane', (0.75, 0.85, 0.9)),
                                  ('Mud Bank', (0.35, 0.27, 0.18)), ('Tile Panel', (0.85, 0.85, 0.82)),
                                  ('Concrete Block', (0.6, 0.6, 0.58))]):
    box(name, (-7.95, 1.1, 2.6 - k * 1.1), (0.1, 1.4, 1.0), surfaces, mat('light', 0.8, tint))
props = group('Props')
for k, (x, y, z) in enumerate([(2.6, 0.3, -4.9), (3.3, 0.3, -4.9), (2.95, 0.9, -4.9)]):
    box(f'Loose Crate {k + 1}', (x, y, z), (0.6, 0.6, 0.6), props, CRATE, rigidbody=rigidbody(6.0))

lighting = group('Lighting')
copy_tree(find('Sun'), lighting)
i = nid()
ents['empties'].append({'Reflection Probe': {'Importance': 0.5, 'Size': [16.0, 6.0, 14.0]}, 'id': i,
                        'name': 'Range Reflection Probe', 'order': i, 'parentId': lighting, 'position': [0, 2.5, -1.0],
                        'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]})

# --- player and the soldiers ----------------------------------------------------------------------
copy_tree(spawn_id, -1, position=[-1.0, 0.1, 1.0], rotation=quat_euler(yaw=180))
enemies = group('Enemies')
i = nid()
ents['empties'].append({'id': i, 'name': 'AI Director', 'order': i, 'parentId': enemies, 'position': [0.0, 0.0, -5.0],
                        'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1],
                        'Squad Settings': {'Squad Size': 4, 'Respawn': False, 'Respawn Delay': 8.0, 'Difficulty': 1.0,
                                           'NPC Damage Scale': 0.45}})
for k, (x, weapon) in enumerate([(-4.5, 'AKS-74U'), (-1.5, 'AKS-74U'), (1.5, 'Remington 870'), (4.5, 'AKS-74U')]):
    i = nid()
    ents['empties'].append({'id': i, 'name': f'Enemy Spawn {k + 1}', 'order': i, 'parentId': enemies,
                            'position': [float(x), 0.1, -4.5], 'rotation': quat_euler(yaw=0), 'scale': [1, 1, 1],
                            'NPC Spawn': {'Weapon': weapon, 'Squad': 0, 'Skill': 0.6, 'Outfit Seed': k, 'Brain': 'Squad AI'}})

for k in ('boxes', 'empties', 'models'):
    scene[k] = ents[k]
out = os.path.join(scenes, 'BloodTest.json')
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    json.dump(scene, f, indent=2, sort_keys=True)
    f.write('\n')
print(f'wrote {out}: {len(ents["boxes"])} boxes, {len(ents["empties"])} empties, {len(ents["models"])} models')
