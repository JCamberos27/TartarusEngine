# The common corridor of the apartment floor: everything here is in world coordinates (Builder.common).
import json
import os

import scene_kit as k
from apartment_kit import CORRIDOR, DOOR, DOOR_W, H, MODES, T, WINDOW_FRAME, WINDOW_SIZE, inner

WINDOW = ('Window_Fake', 1.05)   # at the far (south) end: model, sill
CHAIR_RAIL = 0.95


def spans(a, b, gaps):
    """[a, b] with each (centre, width) gap cut out."""
    out, pos = [], a
    for c, w in sorted(gaps):
        if c - w / 2 > pos:
            out.append((pos, c - w / 2))
        pos = max(pos, c + w / 2)
    if b > pos:
        out.append((pos, b))
    return out


def corridor(b):
    """The common corridor: bare concrete, two-tone landlord paint with a chair rail, the apartments' front doors
    down both sides with their numbers beside them, a window at the far end, the stairwell door at the near end.
    Four ceiling lights - one dead, one failing - and wall lamps between the doors; the households' overflow on the
    floor by their doors."""
    r = 'Corridor'
    x0, z0, x1, z1 = CORRIDOR
    mid = (x0 + x1) / 2
    shell = b.group('Shell')
    b.s.box('Floor - Corridor', shell, (mid, -0.05, (z0 + z1) / 2), (x1 - x0, 0.1, z1 - z0), b.mats['concrete'])
    b.s.box('Ceiling', shell, (mid, H + 0.05, (z0 + z1) / 2), (x1 - x0 - T, 0.1, z1 - z0 + T), b.mats['ceiling'],
            castShadows=0)
    walls, trim = b.s.empty('Walls', shell), b.s.empty('Trim', shell)
    stem, sill = WINDOW
    w, h = WINDOW_SIZE[stem]
    window = (mid, w - 2 * WINDOW_FRAME, sill + h - WINDOW_FRAME, sill + WINDOW_FRAME)
    b.walls([('x', z0, x0, x1, [(mid,) + DOOR]), ('x', z1, x0, x1, [window])], walls, trim)
    b.window(r, 'x', z1, mid, stem, sill)
    # the sides are the apartments' own walls: trim them here in one run each, broken at the front doors
    doors = {side: [z for _, s_, z, _ in b.front_doors if s_ == side] for side in 'WE'}
    for side, c, sign in (('W', x0, 1), ('E', x1, -1)):
        b.trim_run(trim, c, sign, z0 + T / 2, z1 - T / 2, [(z, DOOR_W) for z in doors[side]])
    # two-tone paint: a darker skin up to a chair rail, on all four walls, around the door frames (~1 m wide)
    low, rail = b.mats['wall_corridor_low'], b.mats['trim']
    faces = [('z', x0 + T / 2, 1, z0 + T / 2, z1 - T / 2, [(z, 1.0) for z in doors['W']]),
             ('z', x1 - T / 2, -1, z0 + T / 2, z1 - T / 2, [(z, 1.0) for z in doors['E']]),
             ('x', z0 + T / 2, 1, x0 + T / 2, x1 - T / 2, [(mid, 1.0)]),
             ('x', z1 - T / 2, -1, x0 + T / 2, x1 - T / 2, [])]
    for axis, face, sign, a, bnd, gaps in faces:
        for s0, s1 in spans(a, bnd, gaps):
            for name, y0, y1, proud, mat in (('Wainscot', 0.0, CHAIR_RAIL, 0.006, low),
                                             ('Chair Rail', CHAIR_RAIL - 0.01, CHAIR_RAIL + 0.04, 0.02, rail)):
                d = face + sign * proud / 2
                if axis == 'z':
                    center, size = (d, (y0 + y1) / 2, (s0 + s1) / 2), (proud, y1 - y0, s1 - s0)
                else:
                    center, size = ((s0 + s1) / 2, (y0 + y1) / 2, d), (s1 - s0, y1 - y0, proud)
                b.s.box(name, trim, center, size, mat, collider=False, static=True)
    b.door(b.group('Doors'), 'Stairwell Door', 'x', z0, mid, -1, True, 0.0, fixed=True)
    for number, side, _, t in b.front_doors:
        number_plate(b, r, side, t, number)
    # ceiling lights every ~5 m: on, failing, dead, on
    n = max(1, round((z1 - z0) / 5.0))
    for i in range(n):
        z = z0 + (i + 0.5) * (z1 - z0) / n
        state = ('on', 'failing', 'dead', 'on')[i % 4]
        if state == 'dead':
            b.ceiling_fixture(r, 'Lamp_Ceiling_A', mid, z)
            continue
        flicker = {'Mode': MODES['Fluorescent'], 'MinInterval': 4.0, 'MaxInterval': 14.0, 'Depth': 0.85,
                   'Seed': 17 + i} if state == 'failing' else None
        b.ceiling_fixture(r, 'Lamp_Ceiling_A', mid, z, on=True, glow=0.5, name=f'Ceiling {i + 1}', intensity=0.7,
                          rng=4.5, color=(1.0, 0.84, 0.62), spot=70.0, fill=0, flicker=flicker)
    # wall lamps between the doors, alternating sides: a small warm pool each, no shadows
    for side, z in (('W', 2.5), ('E', 7.5), ('W', 12.5), ('E', 17.5)):
        lamp = b.wall(r, side, z, 'Lamp_Wall', y=1.62, gap=0.0, extra_yaw=90.0, castShadows=0,
                      **b.lamp_glow('Lamp_Wall', 0.7))
        lx = (lamp['x1'] + 0.12) if side == 'W' else (lamp['x0'] - 0.12)
        b.light(r, f'Wall Lamp {z:g}', (lx, lamp['y1'] - 0.12, z), 0.3, 2.6, (1.0, 0.72, 0.45), shadows=False,
                fill=0)
    # the far window's plant, a painting each side, the breaker cupboard by the stairs
    ix0, iz0, ix1, iz1 = inner(r)
    b.put(r, 'Plant_A', ix0 + 0.32, iz1 - 0.34, 30.0, collide=True)
    b.hang(r, 'W', 9.9, 'Painting_Medium_J', 1.65)
    b.hang(r, 'E', 10.4, 'Painting_Medium_F', 1.65)
    b.wall(r, 'E', 1.0, 'Electricbox_Main', y=1.25)
    # door mats outside the two lived-in-tidy flats, the roommates' boxes, the recluse's bin bags
    door_z = {n: z for n, _, z, _ in b.front_doors}
    b.put(r, 'Carpet_D', ix0 + 0.34, door_z[310], 90.0, scale=0.45)
    b.put(r, 'Carpet_D', ix1 - 0.34, door_z[311], 90.0, scale=0.45)
    b.put(r, 'Plant_B', ix1 - 0.16, door_z[311] - 0.75, 0.0)
    box = b.wall(r, 'W', door_z[312] - 1.55, 'Cardboard_Box_D', collide=True, extra_yaw=90.0)
    b.on(r, box, 'Cardboard_Box_A', yaw=10.0)
    b.wall(r, 'W', door_z[312] - 2.15, 'Cardboard_Box_E', collide=True, extra_yaw=95.0)
    b.put(r, 'Garbage_Bag_A', ix1 - 0.26, door_z[313] + 1.25, 0.0)
    b.put(r, 'Garbage_Bag_B', ix1 - 0.26, door_z[313] + 1.75, 0.0)


def number_plate(b, room, side, t, number, y=1.4, scale=1.6):
    """The apartment number in brass plates (Door_Number_<d>, tools/assets/door_numbers.py) on a corridor wall,
    centred at t along it, read left to right from the corridor."""
    lo, hi = b.idx['Door_Number_0']['min'], b.idx['Door_Number_0']['max']
    pitch = (hi[0] - lo[0]) * scale + 0.012
    step = -pitch if side == 'W' else pitch  # facing a west wall, your right is north (-Z)
    # mostly un-metal the brass: fully metallic it only mirrors the dark corridor and the digits disappear
    meta = json.load(open(os.path.join(b.repo, 'project', b.idx['Door_Number_0']['path'] + '.meta'),
                          encoding='utf-8'))
    brass = k.from_mat(b.repo, meta['materialRemap']['Numbers_Mat'], metallicMap='', metallic=0.3)
    digits = str(number)
    for i, d in enumerate(digits):
        b.wall(room, side, t + (i - (len(digits) - 1) / 2) * step, f'Door_Number_{d}', y=y, gap=0.006,
               scale=scale, name=f'Number {d}',
               materials=[{'embedded': brass}] * b.idx[f'Door_Number_{d}']['meshes'])
