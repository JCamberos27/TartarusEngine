# Generates the default testing scene, project/scenes/Sandbox.json, and its prototype grid
# textures (project/textures/proto_grid_*.png). Usage: python tools/gen_sandbox_scene.py <repo root>
# Hand edits made in the editor are lost if this is re-run - it's the scene's starting point.
# The scene's other assets (court, basketball, materials, animator controller) come from
# tools/gen_sandbox_assets.py; run that first on a fresh checkout.
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

def mat(color, rough=0.6, metal=0.0, tex=None, tri=None, emis=None, emisStrength=1.0, **extra):
    m = {'baseColor': list(color), 'metallic': metal, 'roughness': rough,
         'emissiveColor': list(emis or (0, 0, 0)), 'emissiveStrength': emisStrength,
         'triplanar': tri is not None, 'triplanarScale': tri or 1.0}
    if tex: m['albedoMap'] = 'textures/' + tex
    m.update(extra)
    return [{'embedded': m}]

def matref(path):
    # A .mat file (project-relative) - for materials that need engine://Standard.shader's lobes.
    return [{'path': path}]

def rigidbody(mass, lin=0.05, ang=0.05, ccd=False, kinematic=False):
    return {'Mass': mass, 'Use Gravity': True, 'Is Kinematic': kinematic, 'Initial Velocity': [0, 0, 0],
            'Linear Damping': lin, 'Angular Damping': ang, 'Continuous Collision': ccd,
            **{f'Freeze {k} {a}': False for k in ('Position', 'Rotation') for a in 'XYZ'},
            'Interpolate': 'Interpolate'}

SHAPES = {'box': 0, 'sphere': 1, 'capsule': 2, 'convex': 3, 'mesh': 4}
COMBINE = {'average': 0, 'minimum': 1, 'multiply': 2, 'maximum': 3}

def col(shape='box', half=None, friction=0.6, bounce=0.0, static=None, fcomb='average', bcomb='average',
        trigger=False):
    c = {'isTrigger': trigger}
    if SHAPES[shape]: c['shape'] = SHAPES[shape]
    if half: c['halfExtents'] = list(half)
    c['friction'] = friction
    c['bounciness'] = bounce
    if static is not None: c['staticFriction'] = static
    if COMBINE[fcomb]: c['frictionCombine'] = COMBINE[fcomb]
    if COMBINE[bcomb]: c['bounceCombine'] = COMBINE[bcomb]
    return c

def particles(**f):
    d = {'Emitting': True, 'Rate': 20.0, 'Max Particles': 500, 'Lifetime': 2.0, 'Start Speed': 3.0, 'Spread': 25.0,
         'Start Size': 0.1, 'End Size': 0.1, 'Start Color': [1, 1, 1], 'End Color': [1, 1, 1], 'Start Alpha': 1.0,
         'End Alpha': 0.0, 'Intensity': 1.0, 'Gravity Modifier': 0.0, 'Blend Mode': 'Alpha Blended'}
    d.update(f)
    return {'Particle System': d}

def r4(v):
    return [round(x, 4) for x in v]

def group(name, pos=(0, 0, 0), parent=-1):
    i = nid()
    ents['empties'].append({'name': name, 'id': i, 'parentId': parent, 'order': i,
                            'position': list(pos), 'rotation': [0, 0, 0], 'scale': [1, 1, 1]})
    return i

def box(name, center, size, color=(1, 1, 1), rot=(0, 0, 0), m=None, body=None,
        friction=0.6, bounce=0.0, parent=-1, collider=None, extra=None):
    i = nid()
    b = {'name': name, 'id': i, 'parentId': parent, 'order': i, 'center': [round(v, 4) for v in center],
         'size': [round(v, 4) for v in size], 'color': list(color), 'rotation': [round(v, 4) for v in rot],
         'collider': collider or {'isTrigger': False, 'friction': friction, 'bounciness': bounce}}
    if m: b['materials'] = m
    if body: b['Rigidbody'] = body
    if extra: b.update(extra)
    ents['boxes'].append(b)
    return i

def model(name, path, pos, rot=(0, 0, 0), scale=(1, 1, 1), m=None, collider=None, body=None, parent=-1,
          extra=None):
    i = nid()
    e = {'path': path, 'name': name, 'id': i, 'parentId': parent, 'order': i, 'position': r4(pos),
         'rotation': r4(rot), 'scale': r4(scale)}
    if m: e['materials'] = m
    if collider: e['collider'] = collider
    if body: e['Rigidbody'] = body
    if extra: e.update(extra)
    ents['models'].append(e)
    return i

def prim(kind, name, pos, scale, m, **kw):
    # A primitive mesh (cube / sphere / cylinder / capsule / donut / cone / plane). No collider
    # unless one is passed - these are mostly visual details.
    return model(name, f'primitive://{kind}#p{_id[0] + 1}', pos, scale=scale, m=m, **kw)

def empty(name, pos=(0, 0, 0), rot=(0, 0, 0), parent=-1, extra=None):
    i = nid()
    e = {'name': name, 'id': i, 'parentId': parent, 'order': i, 'position': r4(pos), 'rotation': r4(rot),
         'scale': [1, 1, 1]}
    if extra: e.update(extra)
    ents['empties'].append(e)
    return i

def sphere(name, pos, radius, m, body=None, friction=0.4, bounce=0.3, parent=-1, collide=True, extra=None):
    i = nid()
    s = {'path': f'primitive://sphere#s{i}', 'name': name, 'id': i, 'parentId': parent, 'order': i,
         'position': [round(v, 4) for v in pos], 'rotation': [0, 0, 0], 'scale': [radius * 2] * 3,
         'materials': m}
    if collide:
        s['collider'] = {'isTrigger': False, 'shape': 1, 'halfExtents': [0.5, 0, 0],
                         'friction': friction, 'bounciness': bounce}
    if body: s['Rigidbody'] = body
    if extra: s.update(extra)
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
# Sun from the south-west at 47.9 deg - the elevation of the sun in the HDRI sky below, which is
# rotated (skyRotationDegrees) so its sun sits where this light comes from.
light('Sun', (0, 30, 0), (-47.9, -35, 0), 'directional', (1.0, 0.93, 0.82), 2.4, rng=200, shadows=True,
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
    x = -7 + k * 2.8
    i = nid()
    ents['models'].append({'path': YBOT, 'name': name, 'id': i, 'parentId': g_plaza, 'order': i,
                           'position': [x, 0.2, 0.5], 'rotation': [0, 0, 0], 'scale': [1, 1, 1],
                           'Animation': {'Clip': 'assets/characters/ybot/' + clip, 'Play Automatically': True,
                                         'Wrap Mode': 'Loop', 'Speed': 1.0, 'Cross Fade': 0.25}})
    box(name + ' Plinth', (x, 0.21, 0.5), (1.4, 0.02, 1.4), m=mat((0.85, 0.47, 0.18), 0.5), parent=g_plaza)
# The sixth runs animations/ybot_showcase.controller: Idle -> Walk -> Run -> Jump -> Idle on exit
# times, crossfading between them (#175 Animator Controller).
model('Y Bot - Animator Controller', YBOT, (7, 0.2, 0.5), parent=g_plaza,
      extra={'Animator Controller': {'Controller': 'animations/ybot_showcase.controller', 'Speed': 1.0}})
box('Y Bot - Animator Controller Plinth', (7, 0.21, 0.5), (1.4, 0.02, 1.4), m=mat((0.16, 0.58, 0.60), 0.5),
    parent=g_plaza)

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

# ---------------------------------------------------------------- basketball arena (south)
# An NBA court scaled up by COURT_S (markings, floor and hall) with hoops and balls scaled by
# HOOP_S - a regulation 24 cm ball in a 46 cm ring is too fiddly to shoot with the gravity gun. The
# rim stays at the real 3.05 m. A glass hall with a roof keeps a thrown ball from rolling away. Play basketball with the gravity
# gun: right mouse picks the ball up, hold left mouse to charge a shot, release to throw.
# Scoring: a Goal Trigger under each rim counts baskets on the Scoreboard (+3 from 6.9 m out),
# bursts sparks and confetti, flashes a light and plays the swish.
g_arena = group('Basketball Arena')
COURT_S = 1.5                               # court + hall scale (the floor texture stretches with it)
HOOP_S = 2.0                                # ring, net, backboard and ball scale
COURT_L, COURT_W, APRON = 28.65 * COURT_S, 15.24 * COURT_S, 2.0 * COURT_S
FLOOR_L, FLOOR_W = COURT_L + 2 * APRON, COURT_W + 2 * APRON
FLOOR_TOP = 0.12
HALL_X = FLOOR_L / 2 + 1.0                  # glass walls
HALL_Z = FLOOR_W / 2 + 1.0
HALL_H = 15.0
CZ = 21.38 + HALL_Z                         # court centre z: the north (door) wall stays at z 21.4
RIM_Y = FLOOR_TOP + 3.05
STEEL = mat((0.16, 0.17, 0.19), 0.4, 0.85)
NAVY_PAD = mat((0.10, 0.17, 0.38), 0.75, sheen=[0.2, 0.25, 0.4])
GLASS = matref('materials/sandbox/arena_glass.mat')
glass_col = col('box', friction=0.3, bounce=0.6)

box('Court Floor', (0, FLOOR_TOP / 2, CZ), (FLOOR_L, FLOOR_TOP, FLOOR_W), m=matref('materials/sandbox/court_floor.mat'),
    collider=col('box', friction=0.6, static=0.75, bounce=0.84), parent=g_arena)

# Glass walls: east / west full, north with a doorway, the roof on top. Steel mullions every
# ~4.3 m, a ring beam at the top, and a concrete kerb outside the base.
g_hall = group('Glass Hall', parent=g_arena)
for sx in (-1, 1):
    box('Glass Wall ' + ('East' if sx > 0 else 'West'), (sx * HALL_X, HALL_H / 2, CZ), (0.06, HALL_H, 2 * HALL_Z),
        m=GLASS, collider=glass_col, parent=g_hall)
box('Glass Wall South', (0, HALL_H / 2, CZ + HALL_Z), (2 * HALL_X, HALL_H, 0.06), m=GLASS, collider=glass_col,
    parent=g_hall)
DOOR = 1.3
for sx in (-1, 1):
    w = HALL_X - DOOR
    box('Glass Wall North ' + ('East' if sx > 0 else 'West'), (sx * (DOOR + w / 2), HALL_H / 2, CZ - HALL_Z),
        (w, HALL_H, 0.06), m=GLASS, collider=glass_col, parent=g_hall)
box('Glass Door Header', (0, (3.0 + HALL_H) / 2, CZ - HALL_Z), (2 * DOOR, HALL_H - 3.0, 0.06), m=GLASS,
    collider=glass_col, parent=g_hall)
box('Glass Roof', (0, HALL_H + 0.03, CZ), (2 * HALL_X + 0.06, 0.06, 2 * HALL_Z + 0.06), m=GLASS, collider=glass_col,
    parent=g_hall)
# Inside the door, a glass baffle: walk round either end; a loose ball can't roll straight out.
box('Glass Door Baffle', (0, 1.75, CZ - HALL_Z + 1.55), (5.4, 3.5, 0.06), m=GLASS, collider=glass_col, parent=g_hall)
for sx in (-1, 1):
    box('Baffle Post ' + ('East' if sx > 0 else 'West'), (sx * 2.7, 1.8, CZ - HALL_Z + 1.55), (0.1, 3.6, 0.1),
        m=STEEL, parent=g_hall)
    box('Door Post ' + ('East' if sx > 0 else 'West'), (sx * DOOR, HALL_H / 2, CZ - HALL_Z), (0.14, HALL_H, 0.14),
        m=STEEL, parent=g_hall)
box('Door Lintel', (0, 3.0, CZ - HALL_Z), (2 * DOOR + 0.14, 0.14, 0.14), m=STEEL, parent=g_hall)
n_long, n_short = 10, 6
for k in range(n_long + 1):
    x = -HALL_X + k * (2 * HALL_X / n_long)
    for sz in (-1, 1):
        if sz < 0 and abs(x) < DOOR + 0.2: continue
        box(f'Mullion {"N" if sz < 0 else "S"}{k}', (x, HALL_H / 2, CZ + sz * HALL_Z), (0.12, HALL_H, 0.12), m=STEEL,
            parent=g_hall)
for k in range(1, n_short):
    z = CZ - HALL_Z + k * (2 * HALL_Z / n_short)
    for sx in (-1, 1):
        box(f'Mullion {"E" if sx > 0 else "W"}{k}', (sx * HALL_X, HALL_H / 2, z), (0.12, HALL_H, 0.12), m=STEEL,
            parent=g_hall)
for sx in (-1, 1):
    box('Ring Beam ' + ('East' if sx > 0 else 'West'), (sx * HALL_X, HALL_H - 0.15, CZ), (0.3, 0.3, 2 * HALL_Z + 0.3),
        m=STEEL, parent=g_hall)
for sz in (-1, 1):
    box('Ring Beam ' + ('South' if sz > 0 else 'North'), (0, HALL_H - 0.15, CZ + sz * HALL_Z), (2 * HALL_X + 0.3, 0.3, 0.3),
        m=STEEL, parent=g_hall)
# Roof trusses across the hall: their shadows stripe the court in the afternoon sun.
for k in range(1, n_long):
    x = -HALL_X + k * (2 * HALL_X / n_long)
    box(f'Roof Truss {k}', (x, HALL_H - 0.35, CZ), (0.22, 0.5, 2 * HALL_Z), m=STEEL, parent=g_hall)
KERB = mat((0.42, 0.42, 0.44), 0.85)
for sx in (-1, 1):
    box('Kerb ' + ('East' if sx > 0 else 'West'), (sx * (HALL_X + 0.2), 0.15, CZ), (0.4, 0.3, 2 * HALL_Z + 0.8),
        m=KERB, parent=g_hall)
box('Kerb South', (0, 0.15, CZ + HALL_Z + 0.2), (2 * HALL_X, 0.3, 0.4), m=KERB, parent=g_hall)
for sx in (-1, 1):
    w = HALL_X - DOOR
    box('Kerb North ' + ('East' if sx > 0 else 'West'), (sx * (DOOR + w / 2), 0.15, CZ - HALL_Z - 0.2), (w, 0.3, 0.4),
        m=KERB, parent=g_hall)

# --- the two baskets
BALL_TAG = 'Basketball'
for side, team, label in ((-1, 'Home', 'West'), (1, 'Away', 'East')):
    g_hoop = group(f'Hoop {label}', parent=g_arena)
    bx = side * (COURT_L / 2 - 1.60 * COURT_S)     # basket centre, on the painted markings
    face = side * (COURT_L / 2 - 1.22 * COURT_S)   # backboard front face
    board_x = face + side * 0.015
    # Backboard: tempered glass with painted lines, a steel frame and a padded bottom edge. Its
    # bottom edge sits 0.15 m (x HOOP_S) below the rim, as on a real board.
    BW, BH = 1.83 * HOOP_S, 1.07 * HOOP_S
    board_bot = RIM_Y - 0.15 * HOOP_S
    board_y = board_bot + BH / 2
    box(f'Backboard {label}', (board_x, board_y, CZ), (0.03, BH, BW),
        m=matref('materials/sandbox/backboard_glass.mat'), collider=col('box', friction=0.4, bounce=0.45),
        parent=g_hoop)
    fx = board_x + side * 0.03
    for n, c, s in (('Top', (fx, board_bot + BH + 0.02, CZ), (0.05, 0.05, BW + 0.05)),
                    ('Left', (fx, board_y, CZ - BW / 2 - 0.02), (0.05, BH + 0.04, 0.05)),
                    ('Right', (fx, board_y, CZ + BW / 2 + 0.02), (0.05, BH + 0.04, 0.05))):
        box(f'Backboard Frame {n} {label}', c, s, m=STEEL, parent=g_hoop)
    box(f'Backboard Pad {label}', (board_x, board_bot - 0.04, CZ), (0.08, 0.12, BW + 0.05), m=NAVY_PAD, parent=g_hoop)
    # Rim + bracket + net (the OBJs are regulation size; scaled by HOOP_S).
    RING_R = 0.2376 * HOOP_S                       # outer radius of the ring tube
    model(f'Rim {label}', 'models/basketball/rim.obj', (bx, RIM_Y, CZ), scale=(HOOP_S,) * 3,
          m=matref('materials/sandbox/rim.mat'), collider=col('mesh', friction=0.5, bounce=0.35), parent=g_hoop)
    box(f'Rim Bracket {label}', (bx + side * (RING_R + (abs(face - bx) - RING_R) / 2), RIM_Y - 0.03, CZ),
        (abs(face - bx) - RING_R + 0.02, 0.08, 0.3), m=matref('materials/sandbox/rim.mat'), parent=g_hoop)
    model(f'Net {label}', 'models/basketball/net.obj', (bx, RIM_Y, CZ), scale=(HOOP_S,) * 3,
          m=matref('materials/sandbox/net.mat'), parent=g_hoop)
    # Stanchion behind the baseline: padded base, post, arm to the board, diagonal brace.
    post_x = side * (COURT_L / 2 + 1.35)
    box(f'Stanchion Base {label}', (side * (COURT_L / 2 + 1.55), FLOOR_TOP + 0.5, CZ), (1.1, 1.0, 1.6), m=NAVY_PAD,
        parent=g_hoop)
    post_h = board_y + 0.1 - FLOOR_TOP
    box(f'Stanchion Post {label}', (post_x, FLOOR_TOP + post_h / 2, CZ), (0.22, post_h, 0.22), m=STEEL, parent=g_hoop)
    box(f'Stanchion Post Pad {label}', (post_x, FLOOR_TOP + 1.6, CZ), (0.34, 1.2, 0.34), m=NAVY_PAD, parent=g_hoop)
    arm_x0, arm_x1 = board_x + side * 0.03, post_x
    box(f'Stanchion Arm {label}', ((arm_x0 + arm_x1) / 2, board_y, CZ), (abs(arm_x1 - arm_x0), 0.14, 0.14),
        m=STEEL, parent=g_hoop)
    brace_len = math.hypot(abs(arm_x1 - arm_x0) * 0.6, 1.0)
    brace_ang = math.degrees(math.atan2(1.0, abs(arm_x1 - arm_x0) * 0.6))
    box(f'Stanchion Brace {label}', (arm_x1 - side * abs(arm_x1 - arm_x0) * 0.3, board_y - 0.5, CZ),
        (brace_len, 0.1, 0.1), rot=(0, 0, -side * brace_ang), m=STEEL, parent=g_hoop)
    # Goal trigger just under the ring, with its celebration as children.
    goal = empty(f'Goal {label}', (bx, RIM_Y - 0.28 * HOOP_S, CZ), parent=g_hoop, extra={
        'collider': col('box', half=(0.12 * HOOP_S, 0.06 * HOOP_S, 0.12 * HOOP_S), trigger=True),
        'Goal Trigger': {'Tag': BALL_TAG, 'Team': team, 'Points': 2, 'Three Points': 3,
                         'Three Point Distance': round(6.9 * COURT_S, 3),
                         'Require Downward': True, 'Flash Intensity': 45.0}})
    empty(f'Goal Sparks {label}', (0, 0.28 * HOOP_S + 0.05, 0), parent=goal, extra=particles(
        Emitting=False, Rate=900.0, **{'Max Particles': 450, 'Lifetime': 1.1, 'Start Speed': 4.5, 'Spread': 55.0,
        'Start Size': 0.07, 'End Size': 0.01, 'Start Color': [1.0, 0.82, 0.35], 'End Color': [1.0, 0.35, 0.05],
        'Intensity': 7.0, 'Gravity Modifier': 0.7, 'Blend Mode': 'Additive'}))
    empty(f'Goal Confetti {label}', (0, 0.28 * HOOP_S + 0.1, 0), parent=goal, extra=particles(
        Emitting=False, Rate=650.0, **{'Max Particles': 400, 'Lifetime': 2.4, 'Start Speed': 5.5, 'Spread': 35.0,
        'Start Size': 0.1, 'End Size': 0.08, 'Start Color': [0.35, 0.65, 1.0] if team == 'Away' else [1.0, 0.55, 0.15],
        'End Color': [1.0, 0.95, 0.7], 'Start Alpha': 1.0, 'End Alpha': 0.0, 'Intensity': 1.4,
        'Gravity Modifier': 0.35}))
    light(f'Goal Flash {label}', (0, 0.28 * HOOP_S + 0.8, 0), (0, 0, 0), 'point', (1.0, 0.8, 0.45), 0.0, rng=14, parent=goal)

# --- scoreboard: a centre-hung box with seven-segment digits on its north and south faces.
g_score = group('Scoreboard', parent=g_arena)
SB_Y = HALL_H - 4.4
empty('Score', (0, SB_Y, CZ), parent=g_score, extra={'Scoreboard': {'Home': 0, 'Away': 0}})
box('Scoreboard Body', (0, SB_Y, CZ), (3.4, 2.0, 3.4), m=mat((0.05, 0.05, 0.06), 0.5, 0.3), parent=g_score)
box('Scoreboard Rim Top', (0, SB_Y + 1.05, CZ), (3.5, 0.1, 3.5), m=STEEL, parent=g_score)
box('Scoreboard Rim Bottom', (0, SB_Y - 1.05, CZ), (3.5, 0.1, 3.5), m=STEEL, parent=g_score)
for sx in (-1, 1):
    for sz in (-1, 1):
        box(f'Scoreboard Cable {sx}{sz}', (sx * 1.5, (SB_Y + 1.1 + HALL_H - 0.6) / 2, CZ + sz * 1.5),
            (0.03, HALL_H - 0.6 - SB_Y - 1.1, 0.03), m=STEEL, parent=g_score)
SEGMENTS = {  # letter: (x, y, horizontal)
    'A': (0.0, 0.40, True), 'B': (0.21, 0.20, False), 'C': (0.21, -0.20, False), 'D': (0.0, -0.40, True),
    'E': (-0.21, -0.20, False), 'F': (-0.21, 0.20, False), 'G': (0.0, 0.0, True)}
TEAM_GLOW = {'Home': (1.0, 0.45, 0.1), 'Away': (0.2, 0.75, 1.0)}
for face in (-1, 1):                         # -1 north face (seen from the north), +1 south
    zf = CZ + face * 1.72
    right = -face                            # the viewer's right, along x: north viewers face +z
    for team, gx in (('Home', -0.9), ('Away', 0.9)):   # home = the west hoop's side
        for place, off in (('Tens', -0.3), ('Ones', 0.3)):
            cx = gx + right * off
            digit = empty(f'Score Digit {team} {place} {"N" if face < 0 else "S"}', (cx, SB_Y + 0.15, zf), parent=g_score,
                          extra={'Score Digit': {'Team': team, 'Place': place, 'On Strength': 7.0, 'Off Strength': 0.05}})
            for letter, (lx, ly, horiz) in SEGMENTS.items():
                size = (0.34, 0.075, 0.03) if horiz else (0.075, 0.34, 0.03)
                lit = place == 'Ones' and letter != 'G'  # shows "0" while editing; Play drives it
                prim('cube', f'Seg {letter}', (right * lx, ly, 0), size,
                     mat((0.08, 0.05, 0.04), 0.4, emis=TEAM_GLOW[team], emisStrength=7.0 if lit else 0.05),
                     parent=digit)
        box(f'Scoreboard Team Bar {team} {"N" if face < 0 else "S"}', (gx, SB_Y - 0.62, zf), (1.2, 0.12, 0.04),
            m=mat((0.1, 0.1, 0.1), 0.5, emis=TEAM_GLOW[team], emisStrength=2.5), parent=g_score)

# --- basketballs: a rack on the north apron, one at centre court, one on each free-throw line.
BALL_R = 0.1193 * HOOP_S
def basketball(name, pos, parent):
    return model(name, 'models/basketball/basketball.obj', pos, rot=(random.uniform(0, 360), random.uniform(0, 360), 0),
                 scale=(2 * BALL_R,) * 3, m=matref('materials/sandbox/basketball.mat'),
                 collider=col('sphere', half=(0.5, 0, 0), friction=0.55, static=0.7, bounce=0.84),
                 body=rigidbody(0.62, 0.1, 0.25, ccd=True), parent=parent,
                 extra={'tag': BALL_TAG})
g_balls = group('Basketballs', parent=g_arena)
RACK_Z = CZ - COURT_W / 2 - APRON / 2
RACK_X = 6.0 * COURT_S
PITCH = 2 * BALL_R + 0.08                   # ball spacing on the rack
RACK_W = 5 * PITCH + 0.1
RACK_D = 2 * BALL_R + 0.1
TIERS = (0.42, 0.42 + 2 * BALL_R + 0.2)
box('Ball Rack Frame L', (RACK_X - RACK_W / 2, TIERS[1] / 2 + 0.1, RACK_Z), (0.05, TIERS[1] + 0.2, RACK_D), m=STEEL,
    parent=g_balls)
box('Ball Rack Frame R', (RACK_X + RACK_W / 2, TIERS[1] / 2 + 0.1, RACK_Z), (0.05, TIERS[1] + 0.2, RACK_D), m=STEEL,
    parent=g_balls)
for tier, y in enumerate(TIERS):
    box(f'Ball Rack Shelf {tier + 1}', (RACK_X, y, RACK_Z), (RACK_W, 0.04, RACK_D), m=STEEL, parent=g_balls, friction=0.9)
    box(f'Ball Rack Lip Front {tier + 1}', (RACK_X, y + 0.05, RACK_Z - RACK_D / 2), (RACK_W, 0.06, 0.02), m=STEEL,
        parent=g_balls)
    box(f'Ball Rack Lip Back {tier + 1}', (RACK_X, y + 0.05, RACK_Z + RACK_D / 2), (RACK_W, 0.06, 0.02), m=STEEL,
        parent=g_balls)
    for k in range(5):
        basketball(f'Basketball {tier * 5 + k + 1}', (RACK_X + (k - 2) * PITCH, y + 0.02 + BALL_R + 0.001, RACK_Z),
                   g_balls)
basketball('Basketball Centre Court', (0, FLOOR_TOP + BALL_R + 0.001, CZ), g_balls)
for side in (-1, 1):
    basketball(f'Basketball Free Throw {"East" if side > 0 else "West"}',
               (side * (COURT_L / 2 - 5.79 * COURT_S - 0.3), FLOOR_TOP + BALL_R + 0.001, CZ + 0.6), g_balls)

# --- arena lighting: four shadowed spots from the roof, fixtures that glow, and a reflection
# probe so the varnished floor reflects the hall.
g_alight = group('Arena Lighting', parent=g_arena)
for k, (sx, sz) in enumerate(((-1, -1), (1, -1), (-1, 1), (1, 1))):
    x, z = sx * 7.2 * COURT_S, CZ + sz * 3.6 * COURT_S
    light(f'Arena Spot {k + 1}', (x, HALL_H - 0.9, z), (-90, 0, 0), 'spot', (1.0, 0.96, 0.9), 45.0, rng=HALL_H + 5, spot=42,
          shadows=True, softness=1.2, parent=g_alight)
    box(f'Arena Fixture {k + 1}', (x, HALL_H - 0.75, z), (0.9, 0.12, 0.9),
        m=mat((0.9, 0.9, 0.88), 0.4, emis=(1.0, 0.96, 0.9), emisStrength=9.0), parent=g_alight,
        collider=col('box', friction=0.3, bounce=0.3))
empty('Arena Reflection Probe', (0, 4.0, CZ), parent=g_alight,
      extra={'Reflection Probe': {'Size': [2 * HALL_X, 8.0, 2 * HALL_Z], 'Importance': 2.0}})

# --- the player spawns at the arena door, looking in; a camera frames the court for the Game view.
empty('Player Spawn', (0, 0.02, CZ - HALL_Z - 4.5), (0, 180, 0), extra={'First Person Controller': {
    'Move Speed': 4.0, 'Sprint Multiplier': 1.6, 'Jump Speed': 5.5, 'Eye Height': 1.7, 'Capsule Radius': 0.3,
    'Capsule Height': 1.85, 'Mouse Sensitivity': 0.1, 'Invert Y': False, 'Field of View': 75.0, 'Kill Height': -20.0,
    'Gravity': 18.0, 'Gravity Gun': True, 'Min Throw Speed': 3.5, 'Max Throw Speed': 16.0, 'Throw Charge Time': 1.1,
    'Throw Backspin': 2.0,
    # The AKS-74U in hand; the gravity gun is the unarmed slot (2 / Holster puts the AK away).
    'Animation Set': {'path': 'assets/fps/AKS74U/AKS74U.fpsanim', 'pathGuid': 'c0ff631aac421f76'},
    'Camera Bone': 'head', 'View Model FOV': 50.0, 'View Model Offset': [0.0562, -0.032, 0.0],
    'View Model Rotation': [-0.24, 0.39, 0.0], 'View Model Scale': 1.0}})
cam_pos = (-15.0 * COURT_S, 7.2 * COURT_S, CZ - 8.2 * COURT_S)
tgt = (0.0, 2.0, CZ)
dx, dy, dz = tgt[0] - cam_pos[0], tgt[1] - cam_pos[1], tgt[2] - cam_pos[2]
yaw = math.degrees(math.atan2(-dx, -dz))
pitch = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
empty('Arena Camera', cam_pos, (pitch, yaw, 0), extra={'Camera': {'Field of View': 55.0, 'Near': 0.1, 'Far': 600.0}})

# ---------------------------------------------------------------- fountain (between plaza and arena)
g_fount = group('Fountain')
FZ = 11.5
STONE = mat((0.62, 0.6, 0.56), 0.8)
prim('cylinder', 'Fountain Basin Floor', (0, 0.06, FZ), (4.6, 0.12, 4.6), mat((0.12, 0.28, 0.36), 0.6), parent=g_fount,
     collider=col('convex'))
for k in range(16):  # the basin wall: a 16-gon of stone blocks
    a = 2 * math.pi * k / 16
    x, z = 2.3 * math.cos(a), FZ + 2.3 * math.sin(a)
    box(f'Fountain Wall {k + 1}', (x, 0.3, z), (0.93, 0.6, 0.3), rot=(0, -math.degrees(a) + 90, 0), m=STONE,
        parent=g_fount)
prim('cylinder', 'Fountain Water', (0, 0.42, FZ), (4.3, 0.02, 4.3), matref('materials/sandbox/water.mat'),
     parent=g_fount)
prim('cylinder', 'Fountain Column', (0, 0.75, FZ), (0.5, 1.5, 0.5), STONE, parent=g_fount, collider=col('convex'))
prim('donut', 'Fountain Bowl', (0, 1.5, FZ), (2.0, 1.2, 2.0), STONE, parent=g_fount)
prim('cylinder', 'Fountain Spout', (0, 1.6, FZ), (0.16, 0.3, 0.16), mat((0.72, 0.55, 0.3), 0.3, 1.0), parent=g_fount)
empty('Fountain Jet', (0, 1.75, FZ), parent=g_fount, extra=particles(
    Rate=280.0, **{'Max Particles': 700, 'Lifetime': 1.35, 'Start Speed': 4.6, 'Spread': 7.0, 'Start Size': 0.07,
    'End Size': 0.17, 'Start Color': [0.85, 0.94, 1.0], 'End Color': [0.7, 0.85, 1.0], 'Start Alpha': 0.75,
    'End Alpha': 0.0, 'Intensity': 1.3, 'Gravity Modifier': 1.0}))
empty('Fountain Mist', (0, 0.5, FZ), parent=g_fount, extra=particles(
    Rate=45.0, **{'Max Particles': 200, 'Lifetime': 1.8, 'Start Speed': 0.7, 'Spread': 80.0, 'Start Size': 0.3,
    'End Size': 0.9, 'Start Color': [0.9, 0.95, 1.0], 'End Color': [0.9, 0.95, 1.0], 'Start Alpha': 0.22,
    'End Alpha': 0.0, 'Intensity': 1.0, 'Gravity Modifier': -0.05}))
light('Fountain Light', (0, 0.6, FZ), (0, 0, 0), 'point', (0.45, 0.75, 1.0), 3.0, rng=6, parent=g_fount)

# ---------------------------------------------------------------- material gallery (north of the plaza)
# Each sphere uses a .mat on engine://Standard.shader, which renders the advanced lobes.
g_gal = group('Material Gallery')
GALLERY = [('Chrome', 'gallery_chrome'), ('Gold', 'gallery_gold'), ('Brushed Steel', 'gallery_brushed'),
           ('Car Paint', 'gallery_carpaint'), ('Velvet', 'gallery_velvet'), ('Jade', 'gallery_jade'),
           ('Glass', 'gallery_glass'), ('Neon', 'gallery_neon')]
for k, (label, file) in enumerate(GALLERY):
    x = -7 + k * 2.0
    prim('cylinder', f'Pedestal {label}', (x, 0.5, -9), (0.7, 1.0, 0.7), mat((0.2, 0.21, 0.23), 0.5, 0.2),
         parent=g_gal, collider=col('convex'))
    prim('cylinder', f'Pedestal Cap {label}', (x, 1.02, -9), (0.8, 0.04, 0.8), mat((0.72, 0.55, 0.3), 0.3, 1.0),
         parent=g_gal)
    sphere(f'Gallery {label}', (x, 1.5, -9), 0.45, matref(f'materials/sandbox/{file}.mat'), parent=g_gal,
           friction=0.4, bounce=0.2)
box('Gallery Plinth', (0, 0.05, -9), (17, 0.1, 2.2), m=mat((0.26, 0.28, 0.31), 0.6), parent=g_gal)

# ---------------------------------------------------------------- wrecking ball (physics playground)
# A 120 kg ball on a seven-link chain of Ball joints, starting 55 degrees up; on Play it swings
# into the brick wall. The links are heavy (25 kg) and damped: a light chain on a heavy ball (the
# old 6 kg : 150 kg) is a mass ratio the solver can't hold, so the chain stretched and whipped.
g_wreck = group('Wrecking Ball', parent=g_phys)
PIV = (27.2, 8.6, -6.6)
for sx in (-1, 1):
    box(f'Gantry Post {"East" if sx > 0 else "West"}', (PIV[0] + sx * 1.7, 4.4, PIV[2]), (0.3, 8.6, 0.3), m=STEEL,
        parent=g_wreck)
box('Gantry Beam', (PIV[0], PIV[1] + 0.15, PIV[2]), (3.7, 0.3, 0.3), m=STEEL, parent=g_wreck)
ANG = math.radians(55)
d = (0.0, -math.cos(ANG), -math.sin(ANG))
LINK = 0.8
prev = -1
for k in range(7):
    c = tuple(PIV[i] + d[i] * (LINK / 2 + LINK * k) for i in range(3))
    link = prim('capsule', f'Chain Link {k + 1}', c, (0.24, LINK, 0.24), mat((0.3, 0.3, 0.32), 0.35, 1.0),
                parent=g_wreck, collider=col('capsule', half=(0.25, 0.25, 0)), body=rigidbody(25, 0.15, 0.8),
                extra={'rotation': [-(180 - 55), 0, 0]})
    ents['models'][-1]['joint'] = {'type': 2, 'connectedOrder': prev, 'anchor': [0, -LINK / 2, 0], 'axis': [1, 0, 0],
                                   'breakForce': 0.0, 'breakTorque': 0.0, 'useLimit': False, 'limitLower': 0.0,
                                   'limitUpper': 0.0}
    prev = link
BR = 0.6
bc = tuple(PIV[i] + d[i] * (LINK * 7 + BR) for i in range(3))
sphere('Wrecking Ball', bc, BR, mat((0.14, 0.14, 0.15), 0.35, 0.95), body=rigidbody(120, 0.05, 0.3, True),
       friction=0.5, bounce=0.1, parent=g_wreck)
ents['models'][-1]['joint'] = {'type': 2, 'connectedOrder': prev, 'anchor': [-d[0] * BR, -d[1] * BR, -d[2] * BR],
                               'axis': [1, 0, 0], 'breakForce': 0.0, 'breakTorque': 0.0, 'useLimit': False,
                               'limitLower': 0.0, 'limitUpper': 0.0}

# ---------------------------------------------------------------- moving platforms (movement course)
# Kinematic rigidbodies driven by the Animator: a lift up to the deck and a turntable to ride.
box('Lift Platform', (-19.8, 1.2, -2.45), (3.0, 0.3, 3.0), m=ACCENT, parent=g_course,
    body=rigidbody(50, kinematic=True),
    extra={'Animator': {'Spin Deg/Sec': [0, 0, 0], 'Orbit Axis': [0, 1, 0], 'Orbit Speed': 0.0, 'Orbit Radius': 0.0,
                        'Bob Amplitude': 1.0, 'Bob Frequency Hz': 0.1, 'Color Cycle Hz': 0.0}})
prim('cylinder', 'Turntable', (-19.0, 0.25, 10.4), (3.6, 0.3, 3.6), ACCENT, parent=g_course,
     collider=col('convex'), body=rigidbody(50, kinematic=True),
     extra={'Animator': {'Spin Deg/Sec': [0, 30, 0], 'Orbit Axis': [0, 1, 0], 'Orbit Speed': 0.0, 'Orbit Radius': 0.0,
                         'Bob Amplitude': 0.0, 'Bob Frequency Hz': 0.0, 'Color Cycle Hz': 0.0}})

# ---------------------------------------------------------------- scene file
scene = {
    'formatVersion': 3,
    '_comment': 'Tartarus Sandbox - the default testing scene. Centre: animated Mixamo Y Bots (idle/walk/run/'
                'strafe/jump + one on an Animator Controller; assets in project/assets/characters/ybot, not in '
                'git). South: glass-walled basketball arena (an NBA court at 1.5x with 2x hoops and balls, goal '
                'triggers, scoreboard) - Play spawns you at its door with the AKS-74U; press 2 (or Holster) for '
                'the gravity gun - right mouse grabs a ball, hold left mouse to charge a shot - and 1 for the AK '
                'again. Between: fountain (particles). North: material gallery, then the ball '
                'pit. East: physics playground (crate pyramid, brick wall + wrecking ball on a chain of joints, '
                'ball ramp, domino run). West: movement course (stairs, ramps, step blocks, lift, turntable). '
                'Late-afternoon sun, sky ambient, light fog, colour grading. Regenerate with '
                'tools/gen_sandbox_assets.py then tools/gen_sandbox_scene.py.',
    # Poly Haven 'Kloofendal 48d Partly Cloudy (Pure Sky)' (CC0), 4k .hdr - local, not in git (size);
    # get it from polyhaven.com into project/assets/sky/. Missing -> the procedural colours below.
    'skySource': 1, 'skyHdriPath': 'assets/sky/kloofendal_48d_partly_cloudy_puresky_4k.hdr',
    'skyRotationDegrees': 90.7,
    'skyHorizonColor': [0.70, 0.76, 0.84], 'skyZenithColor': [0.22, 0.42, 0.74], 'skyAmbientIntensity': 0.8,
    'exposureEV': 0.0, 'tonemapOperator': 1,
    'bloomEnabled': True, 'bloomIntensity': 0.12, 'bloomThreshold': 1.2, 'bloomKnee': 0.5,
    'ssaoEnabled': True, 'ssaoRadius': 0.5, 'ssaoIntensity': 1.0, 'ssaoBias': 0.025,
    'shadowsEnabled': True, 'shadowCascades': 4, 'shadowDistance': 150.0, 'shadowResolution': 4096,
    'msaaSamples': 4,
    # #162 - a touch of warmth and contrast, a soft vignette, and thin height fog for depth.
    'fxaa': False, 'gradeTemperature': 6.0, 'gradeTint': 0.0, 'gradeContrast': 8.0, 'gradeSaturation': 6.0,
    'gradeColorFilter': [1.0, 1.0, 1.0], 'vignetteIntensity': 0.18, 'vignetteSmoothness': 0.45,
    'fogEnabled': True, 'fogMode': 2, 'fogColor': [0.66, 0.73, 0.82], 'fogDensity': 0.0045,
    'fogStart': 10.0, 'fogEnd': 300.0, 'fogHeightFalloff': 0.06, 'fogBaseHeight': 0.0,
    'assetFolders': ['Scenes'], 'assetMeta': [],
    **ents,
    'libraryModels': [], 'libraryTextures': [], 'librarySounds': [], 'libraryPrefabs': [], 'libraryMaterials': [],
}
out = os.path.join(ROOT, 'project', 'scenes', 'Sandbox.json')
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    json.dump(scene, f, indent=1)
print('entities', sum(len(v) for v in ents.values()), '->', out)
