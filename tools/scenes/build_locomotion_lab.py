"""Builds tests/scenes/LocomotionLab.json: the player's body and weapon on an open grid floor, for watching and recording
locomotion - a long flat run, a 1 m ledge, stairs up to a 2.5 m platform to drop off, and a bare training-dummy soldier.

Made from WeaponLab.json (its sky, lighting, player and body), so the two stay alike. Run from the repo root:
    python tools/scenes/build_locomotion_lab.py
"""
import copy
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'tests', 'scenes', 'WeaponLab.json')
DST = os.path.join(ROOT, 'tests', 'scenes', 'LocomotionLab.json')

j = json.load(open(SRC, encoding='utf-8'))
next_id = max(x['id'] for k in ('boxes', 'empties', 'models') for x in j[k]) + 1
next_order = max(x.get('order', 0) for k in ('boxes', 'empties', 'models') for x in j[k]) + 1
ground = next(b for b in j['boxes'] if b['name'] == 'Ground')


def box(name, pos, size):
    global next_id, next_order
    b = copy.deepcopy(ground)
    b['name'] = name
    b['id'] = next_id
    b['order'] = next_order
    b['position'] = [float(v) for v in pos]
    b['scale'] = [float(v) for v in size]
    b['rotation'] = [0.0, 0.0, 0.0, 1.0]
    next_id += 1
    next_order += 1
    j['boxes'].append(b)


# The run: the player starts at the origin facing +Z (WeaponLab's spawn); everything off to the side of a 6 m wide lane.
box('Ledge 1m', (-6.0, 0.5, 8.0), (4.0, 1.0, 4.0))
# Stairs (0.25 m risers, 0.35 m treads) up to a 2.5 m platform.
for i in range(10):
    h = 0.25 * (i + 1)
    box('Stair %02d' % (i + 1), (8.0, h / 2.0, 4.0 + 0.35 * i), (2.0, h, 0.35))
box('Platform 2.5m', (8.0, 1.25, 9.5), (4.0, 2.5, 4.0))

# A bare soldier, a training dummy (stands, takes hits) - --npc-test drives it.
spawn = {'id': next_id, 'name': 'Dummy Soldier', 'order': next_order, 'parentId': -1, 'position': [-3.0, 0.0, 14.0],
         'rotation': [0.0, 1.0, 0.0, 0.0], 'scale': [1.0, 1.0, 1.0],
         'NPC Spawn': {'Bare': True, 'Brain': 'Training Dummy', 'Outfit Seed': 1, 'Skill': 0.5, 'Squad': 0, 'Weapon': 'AKS-74U'}}
next_id += 1
next_order += 1
j['empties'].append(spawn)

# One soldier, never respawned: the lab is for watching it move, not a fight.
squad = {'id': next_id, 'name': 'Lab Squad Settings', 'order': next_order, 'parentId': -1, 'position': [0.0, 0.0, 0.0],
         'rotation': [0.0, 0.0, 0.0, 1.0], 'scale': [1.0, 1.0, 1.0],
         'Squad Settings': {'Squad Size': 1, 'Respawn': False, 'Respawn Delay': 3.0, 'Difficulty': 1.0}}
next_id += 1
next_order += 1
j['empties'].append(squad)

with open(DST, 'w', encoding='utf-8', newline='\n') as f:
    json.dump(j, f, indent=2)
    f.write('\n')
print('wrote', os.path.relpath(DST, ROOT), len(j['boxes']), 'boxes')
