# Generates the default testing scene, project/scenes/Sandbox.json, and its prototype grid
# textures (project/textures/proto_grid_*.png). Usage: python tools/gen_sandbox_scene.py <repo root>
# Hand edits made in the editor are lost if this is re-run - it's the scene's starting point.
import json, math, random, sys, os
from PIL import Image, ImageDraw

ROOT = sys.argv[1]
random.seed(7)

# ---------------------------------------------------------------- textures
def grid_tex(path, base, line, major, size=512, cells=4):
    img = Image.new('RGB', (size, size), base)
    d = ImageDraw.Draw(img)
    step = size // cells
    for i in range(cells + 1):
        p = min(i * step, size - 1)
        w = 1
        d.line([(p, 0), (p, size)], fill=line, width=w)
        d.line([(0, p), (size, p)], fill=line, width=w)
    # thick border = the 4 m major line (tiles seamlessly: half on each edge)
    for o in (0, 1, size - 2, size - 1):
        d.line([(o, 0), (o, size)], fill=major)
        d.line([(0, o), (size, o)], fill=major)
    img.save(path)

tex_dir = os.path.join(ROOT, 'project', 'textures')
os.makedirs(tex_dir, exist_ok=True)
grid_tex(os.path.join(tex_dir, 'proto_grid_light.png'), (122, 124, 128), (104, 106, 111), (84, 86, 92))
grid_tex(os.path.join(tex_dir, 'proto_grid_dark.png'), (62, 64, 70), (52, 54, 60), (40, 42, 48))
grid_tex(os.path.join(tex_dir, 'proto_grid_orange.png'), (200, 104, 40), (176, 90, 34), (148, 74, 28))

# ---------------------------------------------------------------- helpers
ents = {'boxes': [], 'models': [], 'empties': []}
_id = [0]
def nid():
    _id[0] += 1
    return _id[0]

def mat(color, rough=0.6, metal=0.0, tex=None, tri=None, emis=None, emisStrength=1.0):
    m = {'baseColor': list(color), 'metallic': metal, 'roughness': rough,
         'emissiveColor': list(emis or (0, 0, 0)), 'emissiveStrength': emisStrength,
         'triplanar': tri is not None, 'triplanarScale': tri or 1.0}
    if tex: m['albedoMap'] = 'textures/' + tex
    return [{'embedded': m}]

def rigidbody(mass, lin=0.05, ang=0.05, ccd=False):
    return {'Mass': mass, 'Use Gravity': True, 'Is Kinematic': False, 'Initial Velocity': [0, 0, 0],
            'Linear Damping': lin, 'Angular Damping': ang, 'Continuous Collision': ccd,
            **{f'Freeze {k} {a}': False for k in ('Position', 'Rotation') for a in 'XYZ'}}

def group(name, pos=(0, 0, 0), parent=-1):
    i = nid()
    ents['empties'].append({'name': name, 'id': i, 'parentId': parent, 'order': i,
                            'position': list(pos), 'rotation': [0, 0, 0], 'scale': [1, 1, 1]})
    return i

def box(name, center, size, color=(1, 1, 1), rot=(0, 0, 0), m=None, body=None,
        friction=0.6, bounce=0.0, parent=-1):
    i = nid()
    b = {'name': name, 'id': i, 'parentId': parent, 'order': i, 'center': [round(v, 4) for v in center],
         'size': list(size), 'color': list(color), 'rotation': list(rot),
         'collider': {'isTrigger': False, 'friction': friction, 'bounciness': bounce}}
    if m: b['materials'] = m
    if body: b['Rigidbody'] = body
    ents['boxes'].append(b)
    return i

def sphere(name, pos, radius, m, body=None, friction=0.4, bounce=0.3, parent=-1, collide=True):
    i = nid()
    s = {'path': f'primitive://sphere#s{i}', 'name': name, 'id': i, 'parentId': parent, 'order': i,
         'position': [round(v, 4) for v in pos], 'rotation': [0, 0, 0], 'scale': [radius * 2] * 3,
         'materials': m}
    if collide:
        s['collider'] = {'isTrigger': False, 'shape': 1, 'halfExtents': [0.5, 0, 0],
                         'friction': friction, 'bounciness': bounce}
    if body: s['Rigidbody'] = body
    ents['models'].append(s)
    return i

def light(name, pos, rot, kind, color, intensity, rng=10.0, spot=30.0, shadows=False, softness=1.0,
          angular=1.0, parent=-1):
    i = nid()
    L = {'kind': kind, 'color': list(color), 'intensity': intensity, 'range': rng, 'castShadows': shadows}
    if kind == 'spot': L['spotAngle'] = spot
    if kind == 'directional': L['angularSize'] = angular
    if shadows:
        L['shadow'] = {'enabled': True, 'bias': 1.0, 'normalBias': 1.0, 'softness': softness,
                       'nearPlane': 0.1 if kind != 'directional' else 0.05, 'resolution': 0, 'updateMode': 0}
    ents['empties'].append({'name': name, 'id': i, 'parentId': parent, 'order': i, 'position': list(pos),
                            'rotation': list(rot), 'scale': [1, 1, 1], 'light': L})
    return i

# palette (muted, readable under a warm sun)
ORANGE = (0.86, 0.45, 0.18); TEAL = (0.16, 0.58, 0.60); CREAM = (0.90, 0.86, 0.76)
SLATE = (0.30, 0.34, 0.40); RED = (0.78, 0.20, 0.18); BLUE = (0.20, 0.40, 0.78)
YELLOW = (0.92, 0.74, 0.22); GREEN = (0.30, 0.62, 0.30); PURPLE = (0.50, 0.32, 0.66)
PALETTE = [ORANGE, TEAL, CREAM, RED, BLUE, YELLOW, GREEN, PURPLE]

GROUND = mat((1, 1, 1), 0.9, tex='proto_grid_light.png', tri=0.25)
WALL = mat((1, 1, 1), 0.85, tex='proto_grid_dark.png', tri=0.25)
ACCENT = mat((1, 1, 1), 0.7, tex='proto_grid_orange.png', tri=0.25)
CONCRETE = mat((0.40, 0.40, 0.42), 0.85)

# ---------------------------------------------------------------- environment
g_env = group('Environment')
box('Ground', (0, -0.5, 0), (120, 1, 120), m=GROUND, parent=g_env, friction=0.8)
H = 1.6
for n, c, s in (('Wall North', (0, H / 2, -60.5), (122, H, 1)), ('Wall South', (0, H / 2, 60.5), (122, H, 1)),
                ('Wall East', (60.5, H / 2, 0), (1, H, 120)), ('Wall West', (-60.5, H / 2, 0), (1, H, 120))):
    box(n, c, s, m=WALL, parent=g_env)

# ---------------------------------------------------------------- lighting
g_light = group('Lighting')
# Late-afternoon sun from the south-west: long readable shadows across the plaza.
light('Sun', (0, 30, 0), (-42, -35, 0), 'directional', (1.0, 0.93, 0.82), 2.4, rng=200, shadows=True,
      softness=0.35, angular=1.2, parent=g_light)
i = nid()
ents['empties'].append({'name': 'Plaza Reflection Probe', 'id': i, 'parentId': g_light, 'order': i,
                        'position': [0, 3, 0], 'rotation': [0, 0, 0], 'scale': [1, 1, 1],
                        'Reflection Probe': {'Size': [30, 8, 30], 'Importance': 1.0}})
i = nid()
ents['empties'].append({'name': 'Sandbox Reflection Probe', 'id': i, 'parentId': g_light, 'order': i,
                        'position': [0, 6, 0], 'rotation': [0, 0, 0], 'scale': [1, 1, 1],
                        'Reflection Probe': {'Size': [120, 14, 120], 'Importance': 0.5}})

# ---------------------------------------------------------------- character plaza (centre)
g_plaza = group('Character Plaza')
box('Plaza Floor', (0, 0.1, 0), (16, 0.2, 10), m=mat((0.26, 0.28, 0.31), 0.6), parent=g_plaza)
box('Plaza Trim North', (0, 0.15, -5.15), (16.3, 0.3, 0.3), m=ACCENT, parent=g_plaza)
box('Plaza Trim South', (0, 0.15, 5.15), (16.3, 0.3, 0.3), m=ACCENT, parent=g_plaza)
box('Plaza Trim East', (8.15, 0.15, 0), (0.3, 0.3, 10.6), m=ACCENT, parent=g_plaza)
box('Plaza Trim West', (-8.15, 0.15, 0), (0.3, 0.3, 10.6), m=ACCENT, parent=g_plaza)

YBOT = 'assets/characters/ybot/Y Bot.fbx'
clips = [('Y Bot - Idle', 'idle.fbx'), ('Y Bot - Walk', 'walking.fbx'), ('Y Bot - Run', 'standard run.fbx'),
         ('Y Bot - Strafe', 'left strafe walking.fbx'), ('Y Bot - Jump', 'jump.fbx')]
for k, (name, clip) in enumerate(clips):
    x = -6 + k * 3
    i = nid()
    ents['models'].append({'path': YBOT, 'name': name, 'id': i, 'parentId': g_plaza, 'order': i,
                           'position': [x, 0.2, 0.5], 'rotation': [0, 0, 0], 'scale': [1, 1, 1],
                           'Animation': {'Clip': 'assets/characters/ybot/' + clip, 'Play Automatically': True,
                                         'Wrap Mode': 'Loop', 'Speed': 1.0, 'Cross Fade': 0.25}})
    box(name + ' Plinth', (x, 0.21, 0.5), (1.4, 0.02, 1.4), m=mat((0.85, 0.47, 0.18), 0.5), parent=g_plaza)

# Lamp posts at the plaza corners: warm, unshadowed fills that read at dusk and don't fight the sun.
for k, (x, z) in enumerate(((-8.8, -5.8), (8.8, -5.8), (-8.8, 5.8), (8.8, 5.8))):
    box(f'Lamp Post {k + 1}', (x, 1.6, z), (0.16, 3.2, 0.16), m=mat((0.12, 0.12, 0.13), 0.4, 0.9), parent=g_plaza)
    sphere(f'Lamp Globe {k + 1}', (x, 3.35, z), 0.2, mat((1, 0.85, 0.6), 0.3, emis=(1.0, 0.72, 0.42),
                                                          emisStrength=6.0), parent=g_plaza, collide=False)
    light(f'Lamp Light {k + 1}', (x, 3.35, z), (0, 0, 0), 'point', (1.0, 0.72, 0.42), 6.0, rng=9, parent=g_plaza)

# ---------------------------------------------------------------- physics playground (east)
g_phys = group('Physics Playground')
box('Physics Pad', (22, 0.05, 0), (24, 0.1, 24), m=mat((0.30, 0.32, 0.35), 0.85), parent=g_phys)

# Pyramid of 1 m crates (rests exactly; knocks down nicely)
base = 6
for row in range(base):
    for c in range(base - row):
        x = 14 + c * 1.02 + row * 0.51
        box(f'Pyramid Crate {row}-{c}', (x, 0.1 + 0.5 + row * 1.0 + 0.001 * row, -8), (1, 1, 1),
            m=mat(ORANGE if (row + c) % 2 == 0 else TEAL, 0.7), body=rigidbody(8), friction=0.7, parent=g_phys)

# Brick wall of small crates (staggered)
for row in range(6):
    n = 8 if row % 2 == 0 else 7
    off = 0 if row % 2 == 0 else 0.4
    for c in range(n):
        box(f'Wall Brick {row}-{c}', (24 + off + c * 0.81, 0.1 + 0.3 + row * 0.6 + 0.001 * row, -3),
            (0.8, 0.6, 0.4), m=mat((0.55, 0.12, 0.09) if row % 2 == 0 else (0.45, 0.10, 0.08), 0.8), body=rigidbody(3),
            friction=0.8, parent=g_phys)

# Ramp with balls parked at the top; they roll off as soon as Play starts
ang = 18.0
L = 10.0
top_y = math.sin(math.radians(ang)) * L
box('Ball Ramp', (16, top_y / 2 + 0.1, 5), (4, 0.3, L), rot=(ang, 0, 0), m=ACCENT, parent=g_phys, friction=0.5)
box('Ramp Back Stop', (16, top_y + 0.6, 5 - L / 2 * math.cos(math.radians(ang)) - 0.25), (4, 1.2, 0.3),
    m=WALL, parent=g_phys)
for k in range(5):
    x = 14.6 + k * 0.7
    sphere(f'Ramp Ball {k + 1}', (x, top_y + 0.9, 5 - L / 2 * math.cos(math.radians(ang)) + 0.6), 0.3,
           mat(PALETTE[k + 3], 0.25, 0.1 if k % 2 else 0.8), body=rigidbody(2, 0.02, 0.05, True), parent=g_phys)

# Domino run: the first one is tipped, so the whole line cascades on Play
for k in range(18):
    tilt = 20 if k == 0 else 0
    box(f'Domino {k + 1}', (26, 0.1 + 0.6 + (0.1 if k == 0 else 0), 4 + k * 0.55), (0.6, 1.2, 0.15),
        rot=(tilt, 0, 0), m=mat((0.95, 0.95, 0.93) if k % 2 == 0 else (0.1, 0.1, 0.11), 0.35),
        body=rigidbody(1.5), friction=0.5, parent=g_phys)

# Loose spheres of different sizes / materials for pushing around
for k in range(8):
    r = 0.25 + 0.1 * (k % 4)
    sphere(f'Physics Ball {k + 1}', (18 + (k % 4) * 1.6, 0.1 + r + 0.01, -13 + (k // 4) * 1.6), r,
           mat(PALETTE[k], 0.3 if k % 2 else 0.6, 1.0 if k in (2, 5) else 0.0), body=rigidbody(1 + r * 4),
           parent=g_phys)

# ---------------------------------------------------------------- ball pit (north)
g_pit = group('Ball Pit')
px, pz, half = 0, -18, 4
box('Pit Floor', (px, 0.05, pz), (half * 2, 0.1, half * 2), m=ACCENT, parent=g_pit)
for n, c, s in (('Pit Wall N', (px, 0.6, pz - half - 0.15), (half * 2 + 0.6, 1.2, 0.3)),
                ('Pit Wall S', (px, 0.6, pz + half + 0.15), (half * 2 + 0.6, 1.2, 0.3)),
                ('Pit Wall E', (px + half + 0.15, 0.6, pz), (0.3, 1.2, half * 2)),
                ('Pit Wall W', (px - half - 0.15, 0.6, pz), (0.3, 1.2, half * 2))):
    box(n, c, s, m=WALL, parent=g_pit)
k = 0
for layer in range(3):
    for gx in range(6):
        for gz in range(6):
            x = px - 3.1 + gx * 1.24 + random.uniform(-0.15, 0.15)
            z = pz - 3.1 + gz * 1.24 + random.uniform(-0.15, 0.15)
            k += 1
            sphere(f'Pit Ball {k}', (x, 1.5 + layer * 1.3, z), 0.35, mat(random.choice(PALETTE), 0.35),
                   body=rigidbody(0.6, 0.05, 0.1), bounce=0.4, parent=g_pit)

# ---------------------------------------------------------------- movement course (west)
g_course = group('Movement Course')
box('Course Pad', (-22, 0.05, 0), (24, 0.1, 24), m=mat((0.30, 0.32, 0.35), 0.85), parent=g_course)
# Stairs: 0.2 m rise, 0.4 m run, up to a 2 m deck
for s in range(10):
    box(f'Stair {s + 1}', (-14 - s * 0.4, 0.1 + (s + 1) * 0.1, -6), (0.4, (s + 1) * 0.2, 4),
        m=CONCRETE, parent=g_course)
box('Deck', (-19.8, 0.1 + 1.0, -6), (4, 2.0, 4), m=WALL, parent=g_course)
# Ramps at 10, 20, 30 degrees
for k, a in enumerate((10, 20, 30)):
    Lr = 6.0
    hgt = math.sin(math.radians(a)) * Lr
    box(f'Ramp {a} deg', (-14 - k * 4.5, hgt / 2 + 0.1, 5), (3.5, 0.3, Lr), rot=(-a, 0, 0), m=ACCENT,
        parent=g_course)
# Step-height test blocks (0.1 .. 0.6 m)
for k in range(6):
    h = 0.1 * (k + 1)
    box(f'Step Test {h:.1f} m', (-28, 0.1 + h / 2, -8 + k * 2), (1.5, h, 1.5), m=CONCRETE, parent=g_course)
# Pillars / cover
for k in range(4):
    box(f'Pillar {k + 1}', (-30 + (k % 2) * 4, 0.1 + 1.5, 6 + (k // 2) * 4), (1, 3, 1), m=WALL, parent=g_course)

# ---------------------------------------------------------------- scene file
scene = {
    'formatVersion': 3,
    '_comment': 'Tartarus Sandbox - the default testing scene. Centre: animated Mixamo Y Bots (idle/walk/run/'
                'strafe/jump, assets in project/assets/characters/ybot, not in git). East: physics '
                'playground (crate pyramid, brick wall, ball ramp, domino run). North: ball pit. West: '
                'movement course (stairs, 10/20/30 degree ramps, step-height blocks). Late-afternoon sun '
                '+ sky ambient, warm plaza lamps, reflection probes.',
    'skyHorizonColor': [0.70, 0.76, 0.84], 'skyZenithColor': [0.22, 0.42, 0.74], 'skyAmbientIntensity': 0.8,
    'exposureEV': 0.0, 'tonemapOperator': 1,
    'bloomEnabled': True, 'bloomIntensity': 0.12, 'bloomThreshold': 1.2, 'bloomKnee': 0.5,
    'ssaoEnabled': True, 'ssaoRadius': 0.5, 'ssaoIntensity': 1.0, 'ssaoBias': 0.025,
    'shadowsEnabled': True, 'shadowCascades': 4, 'shadowDistance': 150.0, 'shadowResolution': 4096,
    'msaaSamples': 4,
    'assetFolders': ['Scenes'], 'assetMeta': [],
    **ents,
    'libraryModels': [], 'libraryTextures': [], 'librarySounds': [], 'libraryPrefabs': [], 'libraryMaterials': [],
}
out = os.path.join(ROOT, 'project', 'scenes', 'Sandbox.json')
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    json.dump(scene, f, indent=1)
print('entities', sum(len(v) for v in ents.values()), '->', out)
