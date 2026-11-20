"""Builds tests/scenes/LocomotionLab.json: the player's body and weapon on an open grid floor, for watching and recording
locomotion - a long flat run, a 1 m ledge, stairs up to a 2.5 m platform to drop off, and a bare training-dummy soldier.

Made from WeaponLab.json (its sky, lighting, player and body), so the two stay alike. Run from the repo root:
    python tools/scenes/build_locomotion_lab.py
"""
import os

from lab_scene import LabScene

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'tests', 'scenes', 'WeaponLab.json')
DST = os.path.join(ROOT, 'tests', 'scenes', 'LocomotionLab.json')

lab = LabScene(SRC)
j = lab.data
box = lab.ground_box


# The run: the player starts at the origin facing +Z (WeaponLab's spawn); everything off to the side of a 6 m wide lane.
box('Ledge 1m', (-6.0, 0.5, 8.0), (4.0, 1.0, 4.0))
# Stairs (0.25 m risers, 0.35 m treads) up to a 2.5 m platform.
for i in range(10):
    h = 0.25 * (i + 1)
    box('Stair %02d' % (i + 1), (8.0, h / 2.0, 4.0 + 0.35 * i), (2.0, h, 0.35))
box('Platform 2.5m', (8.0, 1.25, 9.5), (4.0, 2.5, 4.0))

# A bare training dummy (stands, takes hits) - --npc-test drives it.
lab.empty('Dummy', pos=(-3.0, 0.0, 14.0), rotation=(0.0, 1.0, 0.0, 0.0), **{'Dummy Spawn': {'Roam': True}})
# One dummy, never respawned: the lab is for watching it move, not a fight.
lab.empty('Lab Dummy Settings', **{'Dummy Settings': {'Respawn': False, 'Respawn Delay': 3.0}})

lab.write(DST)
print('wrote', os.path.relpath(DST, ROOT), len(j['boxes']), 'boxes')
