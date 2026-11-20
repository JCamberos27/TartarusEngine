# Generates tests/scenes/BloodTest.json: a small range for the volumetric blood.
# A line of four dummies stands with a wall close behind them (exit sprays and wall spatter), one under a
# low ceiling (spray clipped against it, ceiling drips), one beside a staircase (blood down the steps)
# and one by a stack of loose physics crates (blood on moving props). The player (Player Spawn and its
# whole Player Body subtree), the Sun and the sky / post settings are copied from the Sandbox, as
# gen_arena_scene.py does. Re-running overwrites hand edits.
# Usage: python tools/gen_blood_scene.py <repo root>
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'scenes'))
from proto_scene import ProtoScene, Sandbox, euler_quat, grid_mat, write_sorted  # noqa: E402

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..')
scenes = os.path.join(root, 'tests', 'scenes')

# Light surfaces, so the blood reads against them.
GROUND, WALL, ACCENT = grid_mat('light', 0.9), grid_mat('light', 0.8, (0.8, 0.8, 0.8)), grid_mat('orange', 0.75)
CRATE = grid_mat('orange', 0.8, (0.85, 0.7, 0.55))

S = ProtoScene('gen_blood_scene', WALL)
sandbox = Sandbox(S)
scene = sandbox.settings()   # sky, post, shadows, fog... as-is; not the entities or the asset library lists
group, box, block = S.group, S.box, S.block

def rigidbody(mass):
    return {'Angular Damping': 0.05, 'Continuous Collision': False, 'Freeze Position X': False,
            'Freeze Position Y': False, 'Freeze Position Z': False, 'Freeze Rotation X': False,
            'Freeze Rotation Y': False, 'Freeze Rotation Z': False, 'Initial Velocity': [0.0, 0.0, 0.0],
            'Interpolate': 'Interpolate', 'Is Kinematic': False, 'Linear Damping': 0.05, 'Mass': mass,
            'Use Gravity': True}

# --- the range ------------------------------------------------------------------------------------
spawn_id = sandbox.find('Player Spawn')
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
    box(name, (-7.95, 1.1, 2.6 - k * 1.1), (0.1, 1.4, 1.0), surfaces, grid_mat('light', 0.8, tint))
props = group('Props')
for k, (x, y, z) in enumerate([(2.6, 0.3, -4.9), (3.3, 0.3, -4.9), (2.95, 0.9, -4.9)]):
    box(f'Loose Crate {k + 1}', (x, y, z), (0.6, 0.6, 0.6), props, CRATE, Rigidbody=rigidbody(6.0))

lighting = group('Lighting')
sandbox.copy_tree(sandbox.find('Sun'), lighting)
S.node('Range Reflection Probe', lighting, (0, 2.5, -1.0), **{'Reflection Probe': {'Importance': 0.5, 'Size': [16.0, 6.0, 14.0]}})

# --- player and the soldiers ----------------------------------------------------------------------
sandbox.copy_tree(spawn_id, -1, position=[-1.0, 0.1, 1.0], rotation=euler_quat(180))
enemies = group('Enemies')
S.node('Dummy Settings', enemies, (0.0, 0.0, -5.0), **{'Dummy Settings': {'Respawn': False, 'Respawn Delay': 8.0}})
for k, x in enumerate([-4.5, -1.5, 1.5, 4.5]):
    S.node(f'Dummy Spawn {k + 1}', enemies, (float(x), 0.1, -4.5), euler_quat(0), **{'Dummy Spawn': {'Roam': False}})

write_sorted(os.path.join(scenes, 'BloodTest.json'), scene, S.ents)
