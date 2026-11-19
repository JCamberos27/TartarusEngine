"""Builds project/scenes/AnimTestbed.json: a clean scene for the animation/IK overhaul only.

Made from tests/scenes/WeaponLab.json (its sky, lighting, player and body) so the player rig matches the other labs.
Everything is laid out around one lane the player starts on (origin, facing +Z):
  - the lane itself: flat, 40 m, distance posts every 5 m (foot slide, starts, stops, pivots, turns, bob)
  - stairs: a normal flight (0.18 m risers) and a steep one (0.25 m) up to platforms (foot IK, hip drop, gait phase)
  - ramps at 15 and 30 degrees (foot tilt, knee bend)
  - a 1 m ledge to drop off (landings)
  - a cover wall with a hard edge (lean / corner peek)
  - a standing dummy (turns, flinch, kills/ragdoll) and a roaming one (NPC locomotion), respawning quickly
Run from the repo root:
    python tools/scenes/build_anim_testbed.py
"""
import copy
import math
import os

from lab_scene import LabScene

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'tests', 'scenes', 'WeaponLab.json')
DST = os.path.join(ROOT, 'project', 'scenes', 'AnimTestbed.json')

lab = LabScene(SRC)
j = lab.data
take_id = lab.take_id
ground = next(b for b in j['boxes'] if b['name'] == 'Ground')


def box(name, pos, size, rot=(0.0, 0.0, 0.0, 1.0)):
    b = copy.deepcopy(ground)
    b['id'], b['order'] = take_id()
    b['name'] = name
    b['position'] = [float(v) for v in pos]
    b['scale'] = [float(v) for v in size]
    b['rotation'] = [float(v) for v in rot]
    j['boxes'].append(b)


def empty(name, pos, rot, comps):
    i, o = take_id()
    e = {'id': i, 'name': name, 'order': o, 'parentId': -1, 'position': [float(v) for v in pos],
         'rotation': [float(v) for v in rot], 'scale': [1.0, 1.0, 1.0]}
    e.update(comps)
    j['empties'].append(e)


# The lane: posts beside it every 5 m, so slide and stopping distance read against something fixed.
for z in range(5, 45, 5):
    box('Post L %02dm' % z, (-3.0, 0.5, float(z)), (0.1, 1.0, 0.1))
    box('Post R %02dm' % z, (3.0, 0.5, float(z)), (0.1, 1.0, 0.1))


def stairs(label, x, z0, riser, tread, steps, width=2.0):
    for i in range(steps):
        h = riser * (i + 1)
        box('%s Step %02d' % (label, i + 1), (x, h / 2.0, z0 + tread * i), (width, h, tread))
    top = riser * steps
    box('%s Platform' % label, (x, top / 2.0, z0 + tread * steps + 2.0 - tread / 2.0), (4.0, top, 4.0))


stairs('Stairs Normal', 7.0, 4.0, 0.18, 0.28, 8)   # 1.44 m
stairs('Stairs Steep', 12.0, 4.0, 0.25, 0.35, 10)  # 2.5 m


def ramp(label, x, z0, degrees, length=6.0, width=3.0, thick=0.2):
    a = math.radians(degrees)
    rot = (-math.sin(a / 2.0), 0.0, 0.0, math.cos(a / 2.0))  # about +X, rising toward +Z
    cy = (length / 2.0) * math.sin(a) - (thick / 2.0) * math.cos(a)
    cz = z0 + (length / 2.0) * math.cos(a)
    box('%s' % label, (x, cy, cz), (width, thick, length), rot)
    top = length * math.sin(a)
    box('%s Top' % label, (x, top / 2.0, z0 + length * math.cos(a) + 1.5), (width, top, 3.0))


ramp('Ramp 15deg', -7.0, 4.0, 15.0)
ramp('Ramp 30deg', -11.0, 4.0, 30.0)

box('Ledge 1m', (-7.0, 0.5, 20.0), (4.0, 1.0, 4.0))
box('Cover Wall', (5.0, 1.0, 24.0), (3.0, 2.0, 0.3))

# Dummies: one standing still beside the lane (turns, flinch, kills), one roaming beyond it.
empty('Dummy Standing', (-5.0, 0.0, 14.0), (0.0, 1.0, 0.0, 0.0), {'Dummy Spawn': {'Roam': False}})
empty('Dummy Roaming', (-2.0, 0.0, 32.0), (0.0, 1.0, 0.0, 0.0), {'Dummy Spawn': {'Roam': True}})
empty('Testbed Dummy Settings', (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0), {'Dummy Settings': {'Respawn': True, 'Respawn Delay': 2.0}})

lab.write(DST)
print('wrote', os.path.relpath(DST, ROOT), len(j['boxes']), 'boxes', len(j['empties']), 'empties')
