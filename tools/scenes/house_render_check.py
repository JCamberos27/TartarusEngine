# Render-level check of the Forest house's light circuits: every lit fitting (a model on a Light Circuit that glows) is
# shot head-on twice, once as the switches start and once with every circuit on (TARTARUS_LIGHTS_ON 0 / 1), and the
# pixels where the fitting sits are compared. A circuit that is off must leave its fitting dark - the shades kept
# glowing once while the CPU-side material swap said they were off, which a check of the materials alone missed.
#
#   python tools/scenes/house_render_check.py [<out dir>] [--max N]
# Exit 0 when every fitting off is dark and on is clearly brighter; the failures are listed with their shots.
import json
import math
import os
import sys

from PIL import Image, ImageStat

import house_shots as hs

REPO = hs.REPO
OFF_MAX = 0.55       # the brightest pixels (99th percentile, 0..1) at a fitting whose circuit is off: no glow there
ON_SHARE = 0.75      # sanity: at least this share of the fittings are brighter on than off (the shots see them)


def qrot(q, v):
    x, y, z, w = q
    ix = w * v[0] + y * v[2] - z * v[1]
    iy = w * v[1] + z * v[0] - x * v[2]
    iz = w * v[2] + x * v[1] - y * v[0]
    iw = -x * v[0] - y * v[1] - z * v[2]
    return [ix * w + iw * -x + iy * -z - iz * -y, iy * w + iw * -y + iz * -x - ix * -z,
            iz * w + iw * -z + ix * -y - iy * -x]


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else os.path.join(REPO, 'b', 'house_render_check')
    cap = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 999
    with open(os.path.join(REPO, 'project', 'scenes', 'Forest.json'), encoding='utf-8') as f:
        d = json.load(f)
    ents = d['empties'] + d['models'] + d['boxes']
    by = {e['id']: e for e in ents}
    memo = {}

    def world(i):
        if i in memo:
            return memo[i]
        e = by[i]
        p, r, s = e.get('position', [0, 0, 0]), e.get('rotation', [0, 0, 0, 1]), e.get('scale', [1, 1, 1])
        if e['parentId'] in by:
            pp, pr, ps = world(e['parentId'])
            v = qrot(pr, [p[0] * ps[0], p[1] * ps[1], p[2] * ps[2]])
            res = ([pp[0] + v[0], pp[1] + v[1], pp[2] + v[2]], qmul(pr, r), [ps[k] * s[k] for k in range(3)])
        else:
            res = (list(p), list(r), list(s))
        memo[i] = res
        return res
    # one model per fitting: the topmost glowing piece under each circuit's fitting
    lit = [e for e in d['models'] if e.get('Light Circuit', {}).get('Glow', 0) > 0]
    tops = {}
    for e in lit:
        top = e
        while top['parentId'] in by and by[top['parentId']].get('Light Circuit'):
            top = by[top['parentId']]
        tops[top['id']] = top
    lights = [(world(e['id'])[0], e['Light Circuit']['Circuit']) for e in d['empties'] if 'Light' in e and e.get('Light Circuit')]
    starts_on = {e['Light Switch']['Circuit'] for e in ents if e.get('Light Switch', {}).get('Start On')}
    circuit_of = {}
    cams = []
    for i, e in list(tops.items())[:cap]:
        p = world(i)[0]
        circuit = e['Light Circuit']['Circuit']
        # the fitting's own light tells which way its room is: the camera stands back from the fitting, away from walls
        near = [q for q, c in lights if c == circuit and math.dist(q, p) > 0.6]
        q = min(near, key=lambda q: math.dist(q, p)) if near else [0.0, p[1], 26.0]   # else toward the house's middle
        dx, dz = q[0] - p[0], q[2] - p[2]
        if math.hypot(dx, dz) < 0.05:
            dx, dz = 1.0, 0.0
        ln = math.hypot(dx, dz)
        dist = 1.1
        cam = [p[0] + dx / ln * dist, p[1] - 0.45 if p[1] > 1.8 else p[1] + 0.35, p[2] + dz / ln * dist]
        cams.append((f'{i}_{hs.safe(e["name"])}', cam, *hs.look(cam, p)))
        circuit_of[cams[-1][0]] = circuit
    print(f'{len(cams)} fittings')
    for state in ('0', '1'):
        hs.run_engine(cams, os.path.join(out, state), state == '1', 'Forest.json')
    fails, brighter = [], 0

    def p99(path):
        im = Image.open(path).convert('L')
        w, h = im.size
        hist = im.crop((w // 2 - w // 24, h // 2 - h // 24, w // 2 + w // 24, h // 2 + h // 24)).histogram()
        n, acc = sum(hist), 0
        for v, c in enumerate(hist):
            acc += c
            if acc >= 0.99 * n:
                return v / 255.0
        return 1.0
    for name, *_ in cams:
        paths = [os.path.join(out, st, f'Forest_{name}.png') for st in ('0', '1')]
        if not all(os.path.exists(q) for q in paths):
            print(f'FAIL {name}: missing shot')
            fails.append(name)
            continue
        off, on = p99(paths[0]), p99(paths[1])
        start_on = circuit_of[name] in starts_on      # on from the start: lit in both shots
        brighter += on > off + 0.05 or start_on
        ok = start_on or off <= OFF_MAX
        print(f'{"ok  " if ok else "FAIL"} {name} [{circuit_of[name]}]: off {off:.2f} on {on:.2f}')
        if not ok:
            fails.append(name)
    print(f'{len(cams) - len(fails)}/{len(cams)} fittings dark when off; {brighter}/{len(cams)} brighter when on')
    sys.exit(1 if fails or brighter < ON_SHARE * len(cams) else 0)


if __name__ == '__main__':
    main()
