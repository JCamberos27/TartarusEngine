# Generates project/scenes/Arena.json: a walled 64 x 64 m combat arena with cover, for testing the
# player's weapons against the enemy squad. The player (Player Spawn with its weapons and the whole
# Player Body / outfit subtree), the Sun and the sky / post settings are copied from the Sandbox so
# the arena plays exactly like it; everything else is built here. Re-running overwrites hand edits.
# Usage: python tools/gen_arena_scene.py <repo root>
import json
import math
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..')
scenes = os.path.join(root, 'project', 'scenes')
sandbox = json.load(open(os.path.join(scenes, 'Sandbox.json'), encoding='utf-8'))

AK = 'assets/Weapons/AKS74U/AKS74U.fpsanim'
REMINGTON = 'assets/Weapons/Remington870/Remington870.fpsanim'
BODY_CONTROLLER = 'assets/Animations/Controllers/fps_body_locomotion.controller'

# Scene-wide settings (sky, post, shadows, fog...) come over as-is; the asset library lists don't.
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
    sys.exit(f'gen_arena_scene: {msg}')

# --- copy entities out of the Sandbox -------------------------------------------------------------
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

def path_of(ref):
    return ref.get('path') if isinstance(ref, dict) else ref

spawn_id = find('Player Spawn')
spawn = src[spawn_id][1]
fpc = spawn.get('First Person Controller') or fail('Player Spawn has no First Person Controller')
if path_of(fpc.get('Animation Set')) != AK: fail('Player Spawn Animation Set is not the AKS-74U')
if path_of(fpc.get('Secondary Animation Set')) != REMINGTON: fail('Player Spawn Secondary Animation Set is not the Remington 870')
if fpc.get('Gravity Gun') is not True: fail('Player Spawn has the gravity gun off')
body = [i for i in subtree(spawn_id) if src[i][1]['name'] == 'Player Body']
if len(body) != 1: fail('Player Spawn has no single Player Body child')
b = src[body[0]][1]
if 'Character Outfit' not in b or 'First Person Body' not in b: fail('Player Body lacks Character Outfit / First Person Body')
pieces = [e for i, (_, e) in src.items() if e.get('parentId') == body[0]]
if len(pieces) != 9: fail(f'Player Body has {len(pieces)} pieces, expected 9')
for p in pieces:
    ctrl = path_of(p.get('Animator Controller', {}).get('Controller'))
    if ctrl != BODY_CONTROLLER: fail(f'body piece "{p["name"]}" is not on {BODY_CONTROLLER}')

# --- helpers --------------------------------------------------------------------------------------
def quat_euler(pitch=0.0, yaw=0.0, roll=0.0):
    # Degrees, applied yaw (Y) * pitch (X) * roll (Z); returns [x, y, z, w].
    p, y, r = (math.radians(a) * 0.5 for a in (pitch, yaw, roll))
    cy, sy, cp, sp, cr, sr = math.cos(y), math.sin(y), math.cos(p), math.sin(p), math.cos(r), math.sin(r)
    # q = qy * qx * qz
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

GROUND, WALL, ACCENT = mat('light', 0.9), mat('dark'), mat('orange', 0.75)
CRATE = mat('orange', 0.8, (0.85, 0.7, 0.55))

def group(name, parent=-1, pos=(0, 0, 0)):
    i = nid()
    ents['empties'].append({'id': i, 'name': name, 'order': i, 'parentId': parent, 'position': list(pos),
                            'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]})
    return i

def box(name, center, size, parent, m=WALL, yaw=0.0, pitch=0.0, friction=0.8):
    # A cube primitive: mesh + Box collider + level geometry. center is the box's middle.
    i = nid()
    ents['boxes'].append({'collider': {'friction': friction, 'isTrigger': False}, 'color': [1.0, 1.0, 1.0],
                          'id': i, 'materials': m, 'name': name, 'order': i, 'parentId': parent,
                          'position': [round(v, 4) for v in center], 'rotation': quat_euler(pitch, yaw),
                          'scale': [round(v, 4) for v in size]})
    return i

def block(name, x, z, w, h, d, parent, m=WALL, yaw=0.0, y0=0.0):
    # Box standing on the ground (or on y0): x/z is the footprint centre.
    return box(name, (x, y0 + h * 0.5, z), (w, h, d), parent, m, yaw)

# --- arena ----------------------------------------------------------------------------------------
HALF = 32.0                   # playable area is 64 x 64 m
env = group('Environment')
box('Ground', (0, -0.1, 0), (2 * HALF + 6, 0.2, 2 * HALF + 6), env, GROUND)
for n, (x, z, w, d) in {'North Wall': (0, -HALF - 0.25, 2 * HALF + 1, 0.5),
                        'South Wall': (0, HALF + 0.25, 2 * HALF + 1, 0.5),
                        'West Wall': (-HALF - 0.25, 0, 0.5, 2 * HALF),
                        'East Wall': (HALF + 0.25, 0, 0.5, 2 * HALF)}.items():
    block(n, x, z, w, 4.0, d, env)

lighting = group('Lighting')
copy_tree(find('Sun'), lighting)
i = nid()
ents['empties'].append({'Reflection Probe': {'Importance': 0.5, 'Size': [2 * HALF, 8.0, 2 * HALF]}, 'id': i,
                        'name': 'Arena Reflection Probe', 'order': i, 'parentId': lighting, 'position': [0, 4.0, 0],
                        'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]})

cover = group('Cover')

# West lane: two roofless block buildings joined by a corridor - the flank route. Doors face north /
# south along the lane, windows face the centre.
def building(name, cx, cz, w=7.0, d=8.0, h=3.5, t=0.3, door=1.6, win=1.2, sill=1.0, lintel=2.2):
    g = group(name, cover)
    seg = (w - door) * 0.5
    for side, zf in (('North', cz - d * 0.5), ('South', cz + d * 0.5)):
        block(f'{side} Wall L', cx - w * 0.5 + seg * 0.5, zf, seg, h, t, g)
        block(f'{side} Wall R', cx + w * 0.5 - seg * 0.5, zf, seg, h, t, g)
        block(f'{side} Door Lintel', cx, zf, door, h - lintel, t, g, y0=lintel)
    inner = d - t                 # side walls run between the end walls
    block('West Wall', cx - w * 0.5, cz, t, h, inner, g)
    part = (inner - win) * 0.5
    ex = cx + w * 0.5
    block('East Wall N', ex, cz - win * 0.5 - part * 0.5, t, h, part, g)
    block('East Wall S', ex, cz + win * 0.5 + part * 0.5, t, h, part, g)
    block('East Window Sill', ex, cz, t, sill, win, g)
    block('East Window Lintel', ex, cz, t, h - lintel, win, g, y0=lintel)

building('Building North', -18, -8)
building('Building South', -18, 8)
corridor = group('Corridor', cover)
block('Corridor West', -21.5, 0, 0.3, 2.0, 8.0, corridor)
block('Corridor East N', -14.5, -2.5, 0.3, 2.0, 3.0, corridor)   # gap at z = 0 crosses to the centre
block('Corridor East S', -14.5, 2.5, 0.3, 2.0, 3.0, corridor)
block('West Lane Low Wall N', -18, -20, 4.0, 1.0, 0.4, cover)
block('West Lane Low Wall S', -18, 20, 4.0, 1.0, 0.4, cover)

# Centre: a sightline-breaking wall, a plinth with a ramp, crates, low walls and pillars.
centre = group('Centre', cover)
block('Centre Wall', 0, 0, 12.0, 2.5, 0.4, centre)
block('Plinth', 0, -12, 6.0, 1.0, 6.0, centre, ACCENT)
ramp_len = math.hypot(4.0, 1.0)
box('Plinth Ramp', (0, 0.4, -7.0), (3.0, 0.2, ramp_len), centre, ACCENT, pitch=math.degrees(math.atan2(1.0, 4.0)))

def crates(name, x, z, layout, parent, yaw=0.0):
    # layout: (dx, dz, level) offsets in crate units (1.2 m).
    g = group(name, parent)
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    for k, (dx, dz, lv) in enumerate(layout):
        ox, oz = dx * 1.2, dz * 1.2
        box(f'Crate {k + 1}', (x + ox * c + oz * s, 0.6 + lv * 1.2, z - ox * s + oz * c), (1.2, 1.2, 1.2), g, CRATE, yaw)

crates('Crates West', -6, 8, [(0, 0, 0), (1, 0, 0), (0, 0, 1)], centre)
crates('Crates East', 5, -4, [(0, 0, 0), (0, 1, 0), (0, 1, 1)], centre, 15)
crates('Crates South', 7, 13, [(0, 0, 0), (1, 0, 0)], centre)
crates('Crates North', -5, -19, [(0, 0, 0), (1, 0, 0), (1, 0, 1)], centre, -10)
for k, (x, z, yaw) in enumerate([(3, 6, 0), (-3, 15, 30), (8, -11, 45), (-8, -6, -30), (0, 21, 0), (0, -23, 0)]):
    block(f'Low Wall {k + 1}', x, z, 3.0, 1.0, 0.4, centre, yaw=yaw)
for k, (x, z) in enumerate([(-9, 4), (9, -4), (-8, -14), (9, 17)]):
    block(f'Pillar {k + 1}', x, z, 0.8, 4.0, 0.8, centre)

# East lane: a trench of staggered low walls with two crate towers.
trench = group('Trench', cover)
for k, z in enumerate(range(-20, 21, 5)):
    block(f'Trench Wall {k + 1}', 15.0 if k % 2 == 0 else 21.0, float(z), 4.0, 1.0, 0.4, trench)
crates('Tower North', 18, -2.5, [(0, 0, 0), (0, 0, 1)], trench)
crates('Tower South', 18, 7.5, [(0, 0, 0), (0, 0, 1)], trench)

# --- player and enemy spawns ----------------------------------------------------------------------
# Same facing as the Sandbox spawn (yaw 180 looks down -Z, toward the enemy side).
copy_tree(spawn_id, -1, position=[0.0, 0.1, HALF - 4.0], rotation=quat_euler(yaw=180))
enemies = group('Enemies')
for k, x in enumerate([-20, -10, 0, 10, 20]):
    i = nid()
    ents['empties'].append({'id': i, 'name': f'Enemy Spawn {k + 1}', 'order': i, 'parentId': enemies,
                            'position': [float(x), 0.1, -HALF + 5.0], 'rotation': quat_euler(yaw=0),
                            'scale': [1, 1, 1]})

for k in ('boxes', 'empties', 'models'):
    scene[k] = ents[k]
out = os.path.join(scenes, 'Arena.json')
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    json.dump(scene, f, indent=2, sort_keys=True)
    f.write('\n')
print(f'wrote {out}: {len(ents["boxes"])} boxes, {len(ents["empties"])} empties, {len(ents["models"])} models')
