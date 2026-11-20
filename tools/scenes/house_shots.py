# Review shots of the Forest house, for the asset-by-asset inspection: cameras framed on each piece's measured box (or
# set round each room), one engine run for the lot (--smoke-test --smoke-shots), then contact sheets.
#
#   python tools/scenes/house_shots.py <inventory.json> <out dir> [--rooms] [--pieces] [--room NAME]... [--group G]...
#                                      [--ids 1,2,3] [--lights 0|1] [--only 'Circuit;...'] [--walls] [--day]
#                                      [--angles eye,high,low,side] [--size 320]
#
# <inventory.json> comes from house_inventory.py. --rooms: five views of every room (four corners and the middle,
# eye height); --pieces: per piece, an eye-height view from the room's open side, a high three-quarter view, a low view
# of where it meets the floor or wall, and for wall pieces a view along the wall. --lights 1 turns every circuit on
# (TARTARUS_LIGHTS_ON), 0 leaves them as they start; --only lights just those circuits (TARTARUS_LIGHTS_ONLY) and --walls
# looks square onto each wall of the rooms picked: a dark room beside a lit one shows any light leaking through. Editor gizmos, grid and colliders are off and the view Shaded for
# the run (the prefs are put back after). Contact sheets of 20 go to <out dir>/sheet_*.png with each shot's name.
import json
import math
import os
import subprocess
import sys

import scene_kit

from PIL import Image, ImageDraw

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
EYE = 1.6


def look(cam, target):
    """Yaw, pitch (degrees, the engine camera's convention) from cam to target."""
    d = [target[i] - cam[i] for i in range(3)]
    h = math.hypot(d[0], d[2]) or 1e-6
    return math.degrees(math.atan2(d[2], d[0])), math.degrees(math.atan2(d[1], h))


def room_floor(inv, piece):
    for r in inv['rooms']:
        if r['name'] == piece['room']:
            return r
    return None


def piece_cams(inv, p, angles):
    lo, hi = p['box'][:3], p['box'][3:]
    c = [(lo[i] + hi[i]) / 2 for i in range(3)]
    size = max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 0.25)
    r = room_floor(inv, p)
    floor = r['floor'] - 0.485 if r else lo[1]      # pack floor -> world (PACK_C y)
    centre = r['centre'] if r else [c[0] + 3.0, c[1], c[2]]
    # from the piece toward the room's middle: its open side
    dx, dz = centre[0] - c[0], centre[2] - c[2]
    ln = math.hypot(dx, dz)
    if ln < 0.3:
        dx, dz, ln = 1.0, 0.0, 1.0
    dx, dz = dx / ln, dz / ln
    dist = max(1.3, 1.7 * size)
    out = []
    if 'eye' in angles:
        cam = [c[0] + dx * dist, floor + EYE if r else c[1] + 0.6, c[2] + dz * dist]
        out.append(('eye', cam, c))
    if 'high' in angles:
        a = math.radians(40)
        hx, hz = dx * math.cos(a) - dz * math.sin(a), dx * math.sin(a) + dz * math.cos(a)
        cam = [c[0] + hx * dist * 1.1, max(c[1], floor + 1.0) + 1.1, c[2] + hz * dist * 1.1]
        out.append(('high', cam, c))
    if 'low' in angles:
        tgt = [c[0], lo[1] + 0.05, c[2]] if not p['wall'] else c
        cam = [c[0] + dx * max(0.8, size), lo[1] + 0.35 if not p['wall'] else c[1] - 0.15, c[2] + dz * max(0.8, size)]
        out.append(('low', cam, tgt))
    if 'side' in angles and p['wall']:
        # along the wall: perpendicular to the way out of it
        cam = [c[0] - dz * 0.9 + dx * 0.25, c[1], c[2] + dx * 0.9 + dz * 0.25]
        out.append(('side', cam, c))
    return out


def room_cams(inv, room):
    c = room['centre']
    floor = room['floor'] - 0.485
    out = [('mid', [c[0], floor + EYE, c[2]], [c[0] + 1.0, floor + EYE - 0.3, c[2]])]
    for k, (ox, oz) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
        cam = [c[0] + ox * 1.2, floor + EYE, c[2] + oz * 1.2]
        out.append((f'v{k}', cam, [c[0] - ox * 3.0, floor + 1.1, c[2] - oz * 3.0]))
    return out


def wall_cams(inv, room):
    """From the room's middle, one view square onto each of its four walls (leak checks)."""
    c = room['centre']
    y = room['floor'] - 0.485 + 1.4
    return [(f'w{k}', [c[0], y, c[2]], [c[0] + dx * 3.0, y - 0.4, c[2] + dz * 3.0])
            for k, (dx, dz) in enumerate(((1, 0), (-1, 0), (0, 1), (0, -1)))]


def safe(s):
    return ''.join(ch if ch.isalnum() else '_' for ch in s)[:48]


# The engine runs with its user folder under build/shot_profile: a copy of the real editor prefs with gizmos, grid,
# stats and debug views off and Shaded on. The real prefs are never written; the folder's caches stay warm across runs.
SHOT_PREFS = {'viewShowGizmo': False, 'viewShowGrid': False, 'showColliders': False, 'showLightGizmos': False,
              'showProbeGizmos': False, 'showDecalGizmos': False, 'showVolumeGizmos': False, 'shadingMode': 0,
              'showBodyDebug': False, 'sceneShowStats': False}


def shot_profile():
    base = os.path.join(REPO, 'build', 'shot_profile')
    real = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'TartarusEngine', 'editor_prefs.json')
    d = {}
    if os.path.exists(real):
        with open(real, encoding='utf-8') as f:
            d = json.load(f)
    d.update(SHOT_PREFS)
    os.makedirs(os.path.join(base, 'TartarusEngine'), exist_ok=True)
    with open(os.path.join(base, 'TartarusEngine', 'editor_prefs.json'), 'w', encoding='utf-8') as f:
        json.dump(d, f, indent=4)
    return base


def run_engine(cams, out_dir, lights, scene, only=None, check=True):
    """--smoke-shots of <scene> from each camera. lights: True every circuit on, False as the switches start, None
    untouched; only: just these ';'-separated circuits on (';' alone = every circuit off). A failed run exits the
    script with the engine's output unless check=False; returns the exit code."""
    exe = scene_kit.find_exe(REPO)
    os.makedirs(out_dir, exist_ok=True)
    env = dict(os.environ, LOCALAPPDATA=shot_profile(), TARTARUS_HIDDEN='1', TARTARUS_MUTE='1',
               TARTARUS_SHOT_CAMERAS=';'.join(f'{n}:{p[0]:.3f},{p[1]:.3f},{p[2]:.3f},{yaw:.2f},{pitch:.2f}'
                                              for n, p, yaw, pitch in cams))
    if lights is not None:
        env['TARTARUS_LIGHTS_ON'] = '1' if lights else '0'
    if only:
        env['TARTARUS_LIGHTS_ONLY'] = only
    r = subprocess.run([exe, '--smoke-test', os.path.join(REPO, 'project', 'scenes', scene), '--smoke-shots', out_dir],
                       cwd=REPO, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=3600)
    if r.returncode and check:
        sys.exit(f'engine exit {r.returncode} ({scene}):\n{r.stdout[-3000:]}{r.stderr[-2000:]}')
    return r.returncode


def sheets(out_dir, names, stem, size, per=30, cols=6):
    w, h = size, size * 9 // 16
    made = []
    for s0 in range(0, len(names), per):
        chunk = names[s0:s0 + per]
        rows = (len(chunk) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * w, rows * (h + 14)), (20, 20, 20))
        d = ImageDraw.Draw(sheet)
        for k, n in enumerate(chunk):
            p = os.path.join(out_dir, f'{stem}_{n}.png')
            x, y = (k % cols) * w, (k // cols) * (h + 14)
            if os.path.exists(p):
                im = Image.open(p).convert('RGB')
                im.thumbnail((w, h))
                sheet.paste(im, (x, y + 14))
            d.text((x + 2, y), n[:46], fill=(255, 220, 120))
        path = os.path.join(out_dir, f'sheet_{s0 // per:03d}.png')
        sheet.save(path)
        made.append(path)
    return made


def sheets_to(src, dst, names, stem, size):
    os.makedirs(dst, exist_ok=True)
    made = sheets(src, names, stem, size)
    out = []
    for m in made:
        t = os.path.join(dst, os.path.basename(m))
        os.replace(m, t)
        out.append(t)
    return out


def main():
    a = sys.argv[1:]
    inv = json.load(open(a[0], encoding='utf-8'))
    out = a[1]

    def opt(name, default=None, many=False):
        vals = [a[i + 1] for i in range(len(a) - 1) if a[i] == name]
        return vals if many else (vals[-1] if vals else default)
    rooms = opt('--room', many=True)
    groups = opt('--group', many=True)
    ids = {int(v) for v in opt('--ids', '').split(',') if v}
    angles = opt('--angles', 'eye,high,low,side').split(',')
    lights = opt('--lights')
    size = int(opt('--size', '320'))
    cams = []
    if '--rooms' in a:
        for r in inv['rooms']:
            if rooms and r['name'] not in rooms:
                continue
            for tag, cam, tgt in room_cams(inv, r):
                cams.append((f'{safe(r["name"])}_{tag}', cam, *look(cam, tgt)))
    if '--walls' in a:
        for r in inv['rooms']:
            if rooms and r['name'] not in rooms:
                continue
            for tag, cam, tgt in wall_cams(inv, r):
                cams.append((f'{safe(r["name"])}_{tag}', cam, *look(cam, tgt)))
    if '--pieces' in a or ids:
        for p in inv['pieces']:
            if ids and p['id'] not in ids:
                continue
            if not ids and (p['grouped'] or (rooms and p['room'] not in rooms) or
                            (groups and not any(p['group'].startswith(g) for g in groups))):
                continue
            for tag, cam, tgt in piece_cams(inv, p, angles):
                cams.append((f'{p["id"]}_{safe(p["name"])}_{tag}', cam, *look(cam, tgt)))
    if not cams:
        print('no cameras')
        return
    scene = 'ForestDay.json' if '--day' in a else 'Forest.json'
    print(f'{len(cams)} shots -> {out}')
    rc = run_engine(cams, out, None if lights is None else lights == '1', scene, opt('--only'))
    made = []
    for ang in sorted({c[0].rsplit('_', 1)[1] for c in cams}):     # one set of sheets per angle
        sub = os.path.join(out, ang)
        names = [c[0] for c in cams if c[0].rsplit('_', 1)[1] == ang]
        made += sheets_to(out, sub, names, os.path.splitext(scene)[0], size)
    print(f'engine exit {rc}; {len(made)} sheets')
    for m in made:
        print(m)


if __name__ == '__main__':
    main()
