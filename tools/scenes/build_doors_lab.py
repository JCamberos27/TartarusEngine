"""Builds tests/scenes/DoorsLab.json: the light test scene for --npc-test roam, doors and decals (out of Apartment Hallway,
2026-11-18). A corridor with rooms off it behind the apartment's own doors (Door_Op_1 front doors and interior doors, hinged
the same way), a door out of the building, and a short stair up to a room on an upper floor with its own door - open-topped
boxes on LocomotionLab's grid floor, under its sun.

Made from LocomotionLab.json (its sky, lighting, ground, player and body: the test scenes' standard player tuning, but for
the apartment's 0.22 m capsule - see the spawn below), so the labs stay alike; the doors are copied from tests/scenes/Apartment Hallway.json (read only). Run from the repo root:
    python tools/scenes/build_doors_lab.py

Plan (metres, X east, Z south, floor y = 0). The corridor is the hallway's west end, kept at the hallway's own coordinates so
the decal scenario's fixed spots and cameras shoot the same wall and floor: 2.0 m clear (wall centre lines z = +-1.075,
0.15 m thick, so the faces are z = +-1.0), the west end wall's face at x = -10.45, the north and south walls solid from there
to x = -3.96. Off it:
    101 Entry (north, x -6..0, z 1.075..5) through 101 Front Door, and 101 Bedroom (z 5..9) through 101 Bedroom Door;
    102 Entry (south, x -6..0, z -5..-1.075) through 102 Front Door, 102 Bedroom (x 0..4) through 102 Bedroom Door, and the
    outside through 102 Back Door (x = 4);
    outside, a stair (0.25 m risers, 0.35 m treads) up to a 1.5 m deck (x 6..12, z -8..0) and 201 Room on it (z -8..-4)
    through 201 Front Door.
Every door is 1.2 m or more from the walls either side of it and has 1.2 m clear in front and behind (the doors scenario's
runs), and its leaf swings into the room named in it.
"""
import copy
import json
import os

from lab_scene import LabScene
from scene_kit import door_pose, euler_quat, first

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'tests', 'scenes', 'LocomotionLab.json')
HALLWAY = os.path.join(ROOT, 'tests', 'scenes', 'Apartment Hallway.json')
DST = os.path.join(ROOT, 'tests', 'scenes', 'DoorsLab.json')

H = 2.7                # wall height
T = 0.15               # partition thickness
TX = 0.3               # the corridor's west end wall (the hallway's outside wall)
HALL = 1.075           # corridor wall centre lines
DOOR_W, DOOR_H = 0.92, 2.06
UP = 1.5               # the upper floor

def main():
    lab = LabScene(SRC)
    j = lab.data
    with open(HALLWAY, encoding='utf-8') as f:
        hall = json.load(f)


    def door_template(stem):
        """The hallway's Frame and Leaf entries of a door whose frame is `stem` (Door_Op_1_Frame or Door_Op_1_Frame_Interior)."""
        frame = first((x for x in hall['models'] if x['name'] == 'Frame' and x['path'].endswith('/' + stem + '.fbx')),
                      f'{stem} Frame in Apartment Hallway.json')
        leaf = first((x for x in hall['models'] if x['name'] == 'Leaf' and x.get('parentId') == frame['parentId']),
                     f'{stem} Leaf in Apartment Hallway.json')
        return frame, leaf


    FRONT = door_template('Door_Op_1_Frame')
    INTERIOR = door_template('Door_Op_1_Frame_Interior')

    # LocomotionLab's lab pieces (the ledge, its stairs and platform) and its dummy go; the ground, sun, player and body stay.
    j['boxes'] = [b for b in j['boxes'] if b['name'] == 'Ground']
    j['empties'] = [e for e in j['empties'] if e['name'] not in ('Dummy', 'Lab Dummy Settings')]
    ground = first(j['boxes'], "'Ground' box in LocomotionLab.json")

    WALL_MAT = {'albedoMap': '', 'aoMap': '', 'baseColor': [0.84, 0.83, 0.8], 'emissiveColor': [0.0, 0.0, 0.0], 'emissiveMap': '',
                'emissiveStrength': 0.0, 'factorsScaleMaps': True, 'metallic': 0.0, 'metallicMap': '', 'metallicRoughnessMap': '',
                'normalMap': '', 'roughness': 0.9, 'roughnessMap': '', 'triplanar': False, 'triplanarScale': 1.0}
    DECK_MAT = copy.deepcopy(ground['materials'][0]['embedded'])


    take_id = lab.take_id


    def empty(name, parent=-1, pos=(0.0, 0.0, 0.0), yaw=0.0, **components):
        return lab.empty(name, parent, pos, euler_quat(yaw), **components)


    def box(name, parent, centre, size, mat=WALL_MAT):
        i, o = take_id()
        j['boxes'].append({'collider': {'friction': 0.8, 'isTrigger': False}, 'color': [1.0, 1.0, 1.0], 'id': i,
                           'materials': [{'embedded': copy.deepcopy(mat)}], 'name': name, 'order': o, 'parentId': parent,
                           'position': [round(float(v), 4) for v in centre], 'rotation': [0.0, 0.0, 0.0, 1.0],
                           'scale': [round(float(v), 4) for v in size], 'static': True})


    # Walls: (axis, line, a, b, thickness, floor, [door centres along it]). axis 'x' runs along X at z = line, 'z' along Z at x = line.
    WALLS = [
        # the corridor
        ('z', -10.6, -HALL - T / 2, HALL + T / 2, TX, 0.0, []),          # west end (its face at x = -10.45)
        ('x', HALL, -10.6, 0.0, T, 0.0, [-3.5]),                          # north: 101 Front Door
        ('x', -HALL, -10.6, 0.0, T, 0.0, [-3.0]),                         # south: 102 Front Door
        ('z', 0.0, -HALL, HALL, T, 0.0, []),                              # east end
        # 101 (north)
        ('z', -6.0, HALL, 9.0, T, 0.0, []),
        ('z', 0.0, HALL, 9.0, T, 0.0, []),
        ('x', 5.0, -6.0, 0.0, T, 0.0, [-2.0]),                            # 101 Bedroom Door
        ('x', 9.0, -6.0, 0.0, T, 0.0, []),
        # 102 (south)
        ('z', -6.0, -5.0, -HALL, T, 0.0, []),
        ('z', 0.0, -5.0, -HALL, T, 0.0, [-3.0]),                          # 102 Bedroom Door
        ('z', 4.0, -5.0, -HALL, T, 0.0, [-3.5]),                          # 102 Back Door (outside)
        ('x', -5.0, -6.0, 4.0, T, 0.0, []),
        ('x', -HALL, 0.0, 4.0, T, 0.0, []),
        # 201 (upstairs)
        ('x', -4.0, 6.0, 12.0, T, UP, [9.0]),                             # 201 Front Door
        ('x', -8.0, 6.0, 12.0, T, UP, []),
        ('z', 6.0, -8.0, -4.0, T, UP, []),
        ('z', 12.0, -8.0, -4.0, T, UP, []),
    ]
    # Doors: (name, frame kind, axis, line, along, +1 when the room it swings into is on the line's + side).
    DOORS = [
        ('101 Front Door', FRONT, 'x', HALL, -3.5, 1),
        ('101 Bedroom Door', INTERIOR, 'x', 5.0, -2.0, 1),
        ('102 Front Door', FRONT, 'x', -HALL, -3.0, -1),
        ('102 Bedroom Door', INTERIOR, 'z', 0.0, -3.0, 1),
        ('102 Back Door', FRONT, 'z', 4.0, -3.5, -1),
        ('201 Front Door', FRONT, 'x', -4.0, 9.0, -1),
    ]

    root = empty('Doors Lab')
    walls = empty('Walls', root)
    for axis, line, a, b, t, y0, doors in WALLS:
        a, b = min(a, b) - t / 2, max(a, b) + t / 2   # overlapping the crossing walls
        pieces, pos = [], a
        for c in sorted(doors):                        # cut round each door: the stretch before it, the lintel over it
            pieces.append((pos, c - DOOR_W / 2, y0, y0 + H))
            pieces.append((c - DOOR_W / 2, c + DOOR_W / 2, y0 + DOOR_H, y0 + H))
            pos = c + DOOR_W / 2
        pieces.append((pos, b, y0, y0 + H))
        for p0, p1, q0, q1 in pieces:
            if p1 - p0 < 1e-3:
                continue
            mid, hy = (p0 + p1) / 2, (q0 + q1) / 2
            if axis == 'x':
                box('Wall', walls, (mid, hy, line), (p1 - p0, q1 - q0, t))
            else:
                box('Wall', walls, (line, hy, mid), (t, q1 - q0, p1 - p0))

    # The upper floor: a deck and the stair up to its north edge, climbing toward -Z.
    upper = empty('Upper Floor', root)
    box('Deck 1.5m', upper, (9.0, UP / 2, -4.0), (6.0, UP, 8.0), DECK_MAT)
    steps = int(round(UP / 0.25))
    for i in range(steps):
        h = 0.25 * (i + 1)
        box('Stair %02d' % (i + 1), upper, (9.0, h / 2, 0.175 + 0.35 * (steps - 1 - i)), (2.0, h, 0.35), DECK_MAT)

    grp = empty('Doors', root)
    for name, (frame, leaf), axis, line, along, sign in DOORS:
        yaw, x, z = door_pose(axis, line, along, sign)
        pos = [round(x, 4), UP if name.startswith('2') else 0.0, round(z, 4)]
        parent = empty(name, grp)
        for src in (frame, leaf):
            e = copy.deepcopy(src)
            e['id'], e['order'] = take_id()
            e['parentId'] = parent
            e['position'] = pos
            e['rotation'] = euler_quat(yaw)
            j['models'].append(e)

    # A fill light over the corridor's decal wall (the walls shade it from the sun).
    lighting = first((e for e in j['empties'] if e['name'] == 'Lighting'), "'Lighting' in LocomotionLab.json")['id']
    empty('Corridor Fill', lighting, (-8.4, 2.4, 0.0), Light={
        'Angular Size': 0.53, 'Cast Shadows': False, 'Color': [1.0, 0.95, 0.88], 'ColorTempK': 0.0, 'Culling Mask': -1,
        'Intensity': 1.5, 'Range': 6.0, 'Shadow Bias': 1.0, 'Shadow Near Plane': 0.05, 'Shadow Normal Bias': 1.0,
        'Shadow Softness': 1.0, 'Spot Angle': 30.0,
        'Type': 'Point'})

    # The player at the corridor's west end, facing east down it (the hallway's spawn).
    spawn = first((e for e in j['empties'] if e['name'] == 'Player Spawn'), "'Player Spawn' in LocomotionLab.json")
    spawn['position'] = [-9.6, 0.05, 0.0]
    spawn['rotation'] = euler_quat(-90.0)
    # LocomotionLab's player but for one thing: the apartment's capsule, 0.22 m (a person's shoulders seen from above). The
    # standard 0.30 m (0.7 m wide with PhysX's contact offset) scrapes the 0.87 m an open Door_Op_1 leaves: measured here, every
    # door's hinge-jamb graze kept 0.14 of its speed at 0.30 (the doors scenario asks 0.5) and 0.59 at 0.22, the rest unchanged.
    spawn['First Person Controller']['Capsule Radius'] = 0.22

    lab.write(DST)
    print('wrote', os.path.relpath(DST, ROOT), len(j['boxes']), 'boxes,', len(j['models']), 'models,', len(j['empties']), 'empties')


if __name__ == '__main__':
    main()
