# Review pictures of the apartment scenes, room by room, to check that every piece is aligned and spaced: builds the
# scene exactly as build_apartment.py does, writes a copy with flat, bright "review" lighting, has the engine shoot
# each room from its four corners (eye height, looking at the room's middle) and from overhead, and tiles each room's
# views into one labelled contact sheet. --night also shoots one view per room in the scene's real lighting.
#
#   python tools/scenes/review_shots.py <repo> --out <dir> [--floor] [--units 310,311] [--rooms "Kitchen,Hall"]
#                                       [--night] [--exe <TartarusEngine.exe>]
# <repo> is the checkout with the Drive assets and a built exe (the Work clone). Sheets: <out>/sheets/<unit>_<room>.png
import argparse
import math
import os
import re
import subprocess
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_apartment as ba  # noqa: E402
from apartment_kit import AREAS, FURNISHED, ROOMS, H, floor_units, inner  # noqa: E402

EYE = 1.6
REVIEW = {'skyHorizonColor': [0.85, 0.85, 0.85], 'skyZenithColor': [0.9, 0.9, 0.9], 'skyAmbientIntensity': 1.4,
          'exposureEV': 0.0, 'filmGrain': 0.0, 'vignetteIntensity': 0.0, 'bloomEnabled': False, 'gradeContrast': 0.0,
          'gradeSaturation': 0.0, 'ssaoIntensity': 1.0}
SHEET_W, SHEET_H = 620, 290   # each view on the sheet


def slug(text):
    return re.sub(r'[^A-Za-z0-9]+', '_', text).strip('_')


def view(x, y, z, tx, ty, tz):
    """yaw, pitch looking from (x, y, z) at (tx, ty, tz): yaw 0 = +X, -90 = -Z."""
    dx, dy, dz = tx - x, ty - y, tz - z
    return math.degrees(math.atan2(dz, dx)), math.degrees(math.atan2(dy, math.hypot(dx, dz)))


def blocked(b, unit, x, z):
    """Is plan point (x, z) inside (or within 35 cm of) a piece of `unit` reaching up past eye height?"""
    for p in b.placed:
        if p['unit'] == unit and p['y1'] > EYE - 0.2 and p['y0'] < EYE + 0.1 and \
                p['x0'] - 0.35 < x < p['x1'] + 0.35 and p['z0'] - 0.35 < z < p['z1'] + 0.35:
            return True
    return False


def room_cameras(b, unit, xf, room):
    """[(name, (x, y, z, yaw, pitch))]: four corners and overhead, in the world."""
    b.xf = xf
    x0, z0, x1, z1 = inner(room)
    cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
    world = (lambda x, z: b.at(x, z)) if room in ROOMS else (lambda x, z: (x, z))
    cams = []
    for i, (kx, kz) in enumerate(((x0, z0), (x1, z0), (x1, z1), (x0, z1))):
        # the clear point nearest the corner, stepping toward the middle
        best = None
        for step in range(12):
            f = 0.1 + step * 0.06
            x, z = kx + (cx - kx) * f, kz + (cz - kz) * f
            if not blocked(b, unit, x, z):
                best = (x, z)
                break
        if best is None:
            continue
        wx, wz = world(*best)
        tx, tz = world(cx, cz)
        cams.append((f'c{i}', (wx, EYE, wz) + view(wx, EYE, wz, tx, 0.55, tz)))
    tx, tz = world(cx, cz)
    cams.append(('top', (tx, H - 0.12, tz, -90.0, -82.0)))
    return cams


def run(exe, project, scene, cams, out):
    env = dict(os.environ)
    env['TARTARUS_SHOT_CAMERAS'] = ';'.join(f'{n}:' + ','.join(f'{v:.3f}' for v in c) for n, c in cams)
    os.makedirs(out, exist_ok=True)
    r = subprocess.run([exe, '--project', project, '--smoke-test', scene, '--smoke-shots', out], env=env,
                       capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=1800)
    if r.returncode:
        sys.exit('review_shots: the engine failed:\n' + r.stdout[-3000:] + r.stderr[-2000:])


def sheet(title, paths, out):
    cols = 3
    rows = (len(paths) + cols - 1) // cols
    im = Image.new('RGB', (cols * SHEET_W, rows * (SHEET_H + 18) + 26), (24, 24, 24))
    g = ImageDraw.Draw(im)
    g.text((6, 6), title, fill=(255, 255, 255))
    for i, (label, p) in enumerate(paths):
        x, y = (i % cols) * SHEET_W, 26 + (i // cols) * (SHEET_H + 18)
        if os.path.exists(p):
            im.paste(Image.open(p).convert('RGB').resize((SHEET_W, SHEET_H)), (x, y + 18))
        g.text((x + 4, y + 3), label, fill=(220, 220, 120))
    im.save(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('repo')
    ap.add_argument('--out', required=True)
    ap.add_argument('--floor', action='store_true')
    ap.add_argument('--units', default='')
    ap.add_argument('--rooms', default='')
    ap.add_argument('--night', action='store_true')
    ap.add_argument('--exe')
    args = ap.parse_args()
    repo, out = os.path.abspath(args.repo), os.path.abspath(args.out)
    exe = args.exe or os.path.join(repo, 'build', 'Release', 'TartarusEngine.exe')
    project = os.path.join(repo, 'project')
    b = ba.Builder(repo, floor=args.floor)
    ba.build(b)
    for line in b.validate():
        print('review_shots: check:', line)
    units = floor_units() if args.floor else [(FURNISHED, (0.0, 0.0, 1, 1))]
    want_units = {u.strip() for u in args.units.split(',') if u.strip()}
    want_rooms = {r.strip() for r in args.rooms.split(',') if r.strip()}
    jobs = []  # (unit, room, cams)
    for number, xf in units:
        if want_units and str(number) not in want_units:
            continue
        for room in ROOMS:
            if not want_rooms or room in want_rooms:
                jobs.append((number, room, room_cameras(b, number, xf, room)))
    if args.floor and (not want_units or 'Corridor' in want_units):
        for room in AREAS:
            if not want_rooms or room in want_rooms:
                x0, z0, x1, z1 = inner(room)
                cx = (x0 + x1) / 2
                cams = []
                for i, z in enumerate((z0 + 0.4, (z0 + z1) / 2, z1 - 0.4)):
                    for j, tz in enumerate((z0, z1)):
                        if abs(tz - z) > 1.0:
                            cams.append((f'c{i}{j}', (cx, EYE, z) + view(cx, EYE, z, cx, 0.9, tz)))
                jobs.append(('Corridor', room, cams))

    # copies of the scene beside the real one (so asset paths resolve the same way): as built, and review-lit
    which = 'Floor3' if args.floor else '3Bed'
    real, scene = (os.path.join(project, 'scenes', f'_{kind}_{which}.json') for kind in ('Night', 'Review'))
    b.s.write(real)
    b.d.update(REVIEW)
    for e in b.s.empties:  # the player's body would stand in the hall's pictures
        if e['name'] == 'Player Spawn':
            e['position'] = [0.0, -100.0, 0.0]
    b.s.write(scene)
    names = []
    for unit, room, cams in jobs:
        for n, c in cams:
            names.append((f'{unit}_{slug(room)}_{n}', c))
    raw = os.path.join(out, 'raw')
    night = {}
    try:
        run(exe, project, scene, names, raw)
        if args.night:
            first = [(f'{unit}_{slug(room)}_night', cams[0][1]) for unit, room, cams in jobs if cams]
            run(exe, project, real, first, os.path.join(out, 'night'))
            nstem = os.path.splitext(os.path.basename(real))[0]
            night = {n: os.path.join(out, 'night', f'{nstem}_{n}.png') for n, _ in first}
    finally:
        os.remove(scene)
        os.remove(real)
    stem = os.path.splitext(os.path.basename(scene))[0]
    os.makedirs(os.path.join(out, 'sheets'), exist_ok=True)
    for unit, room, cams in jobs:
        paths = [(n, os.path.join(raw, f'{stem}_{unit}_{slug(room)}_{n}.png')) for n, _ in cams]
        if f'{unit}_{slug(room)}_night' in night:
            paths.append(('night', night[f'{unit}_{slug(room)}_night']))
        dst = os.path.join(out, 'sheets', f'{unit}_{slug(room)}.png')
        sheet(f'{unit} {room}', paths, dst)
        print('review_shots:', dst)


if __name__ == '__main__':
    main()
