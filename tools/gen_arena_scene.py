# Generates tests/scenes/Arena.json: a walled 64 x 64 m combat arena with cover, for testing the
# player's weapons against the training dummies. The player (Player Spawn with its weapons and the whole
# Player Body / outfit subtree), the Sun and the sky / post settings are copied from the Sandbox so
# the arena plays exactly like it; everything else is built here. Re-running overwrites hand edits.
# Usage: python tools/gen_arena_scene.py <repo root>
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'scenes'))
from proto_scene import ProtoScene, Sandbox, euler_quat, grid_mat, write_sorted  # noqa: E402

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..')
scenes = os.path.join(root, 'tests', 'scenes')

AK = 'assets/Weapons/AKS74U/AKS74U.fpsanim'
REMINGTON = 'assets/Weapons/Remington870/Remington870.fpsanim'

GROUND, WALL, ACCENT = grid_mat('light', 0.9), grid_mat('dark'), grid_mat('orange', 0.75)
CRATE = grid_mat('orange', 0.8, (0.85, 0.7, 0.55))

S = ProtoScene('gen_arena_scene', WALL)
sandbox = Sandbox(S)
scene = sandbox.settings()   # sky, post, shadows, fog... as-is; not the entities or the asset library lists
fail, group, box, block = S.fail, S.group, S.box, S.block

# --- copy entities out of the Sandbox -------------------------------------------------------------
def path_of(ref):
    return ref.get('path') if isinstance(ref, dict) else ref

spawn_id = sandbox.find('Player Spawn')
spawn = sandbox.entity(spawn_id)
fpc = spawn.get('First Person Controller') or fail('Player Spawn has no First Person Controller')
if path_of(fpc.get('Animation Set')) != AK: fail('Player Spawn Animation Set is not the AKS-74U')
if path_of(fpc.get('Secondary Animation Set')) != REMINGTON: fail('Player Spawn Secondary Animation Set is not the Remington 870')
if fpc.get('Gravity Gun') is not True: fail('Player Spawn has the gravity gun off')
body = [i for i in sandbox.subtree(spawn_id) if sandbox.entity(i)['name'] == 'Player Body']
if len(body) != 1: fail('Player Spawn has no single Player Body child')
b = sandbox.entity(body[0])
if 'Character Outfit' not in b or 'First Person Body' not in b: fail('Player Body lacks Character Outfit / First Person Body')
pieces = sandbox.children(body[0])
if len(pieces) != 9: fail(f'Player Body has {len(pieces)} pieces, expected 9')
# (Motion matching poses the body, Phase 6: a piece carries no Animator Controller.)
for p in pieces:
    if 'Animator Controller' in p: fail(f'body piece "{p["name"]}" still has an Animator Controller (the legacy locomotion is gone)')

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
sandbox.copy_tree(sandbox.find('Sun'), lighting)
S.node('Arena Reflection Probe', lighting, (0, 4.0, 0),
       **{'Reflection Probe': {'Importance': 0.5, 'Size': [2 * HALF, 8.0, 2 * HALF]}})

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
sandbox.copy_tree(spawn_id, -1, position=[0.0, 0.1, HALF - 4.0], rotation=euler_quat(180))
enemies = group('Enemies')
S.node('Dummy Settings', enemies, (0.0, 0.0, -HALF + 2.0), **{'Dummy Settings': {'Respawn': True, 'Respawn Delay': 8.0}})
# Facing +Z (yaw 0), toward the player's end.
for k, x in enumerate([-20, -10, 0, 10, 20]):
    S.node(f'Dummy Spawn {k + 1}', enemies, (float(x), 0.1, -HALF + 5.0), euler_quat(0), **{'Dummy Spawn': {'Roam': True}})

write_sorted(os.path.join(scenes, 'Arena.json'), scene, S.ents)
