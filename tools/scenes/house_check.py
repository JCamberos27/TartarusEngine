# Check the Forest house as built into a scene: every piece by its measured bounds (house_fit.py) against the walls,
# the other pieces, the floors and the doorways. Prints a count per problem and the worst offenders.
#
#   python tools/scenes/house_check.py project/scenes/Forest.json [--list]
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import forest_house as fh  # noqa: E402
import house_fit as hf  # noqa: E402

GROUPS = ('House_01_Props', 'Rubbish', 'House Lights', 'Porch Light')
SKIP = ('Carpet_', 'Curtains_')


def main():
    path = sys.argv[1]
    show = '--list' in sys.argv
    with open(path, encoding='utf-8') as f:
        d = json.load(f)
    ents = {e['id']: e for key in ('empties', 'boxes', 'models') for e in d.get(key, [])}
    lay = next(e['id'] for e in ents.values() if e['name'] == 'Pack Layout')
    B = hf.bounds()
    memo = {}

    def world(i):
        """Pack-space position, rotation, scale (relative to Pack Layout)."""
        if i == lay:
            return [0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 1.0], [1.0, 1.0, 1.0]
        if i in memo:
            return memo[i]
        e = ents[i]
        p, r, s = e.get('position', [0, 0, 0]), e.get('rotation', [0, 0, 0, 1]), e.get('scale', [1, 1, 1])
        if e['parentId'] not in ents:
            memo[i] = None
            return None
        par = world(e['parentId'])
        if par is None:
            memo[i] = None
            return None
        pp, pr, ps = par
        v = hf.qrot(pr, [p[k] * ps[k] for k in range(3)])
        memo[i] = ([pp[k] + v[k] for k in range(3)], hf.qmul(pr, r), [ps[k] * s[k] for k in range(3)])
        return memo[i]

    def piece(i):
        """The top piece an entity belongs to (a prop, a stamped prefab), or None for the building itself."""
        while i in ents:
            p = ents[i]['parentId']
            if p in ents and ents[p]['name'] in GROUPS:
                return i
            i = p
        return None

    structure, pieces = [], {}
    for i, e in ents.items():
        w = world(i)
        if w is None:
            continue
        pc = piece(i)
        c = e.get('collider')
        if pc is None and c and 'halfExtents' in c and not c.get('isTrigger') and not e.get('Openable') and \
                not e['name'].startswith(fh.STRUCTURE_SKIP):
            he, ctr = c.get('halfExtents', [0.3, 0.3, 0.3]), c.get('center', [0, 0, 0])
            structure.append(hf.Box.of(w[0], w[1], w[2], [ctr[k] - he[k] for k in range(3)],
                                       [ctr[k] + he[k] for k in range(3)], tag=e['name']))
        if pc is not None and e.get('path') in B and e.get('active', True):
            b = B[e['path']]
            pieces.setdefault(pc, []).append(hf.Box.of(w[0], w[1], w[2], b[0], b[1], tag=e['name']))
    name = {i: re.sub(r' \(\d+\)$', '', ents[i]['name']) for i in pieces}
    fixed = {i for i in pieces if name[i].startswith(fh.FIXED)}

    # doorways, from the leaves
    zones = []
    for i, e in ents.items():
        o, c = e.get('Openable'), e.get('collider')
        w = world(i)
        if not o or not c or w is None or c['halfExtents'][1] * 2 < 1.8:
            continue
        he, ctr = c['halfExtents'], c.get('center', [0, 0, 0])
        mid = hf.qrot(w[1], ctr)
        cx, cz = w[0][0] + mid[0], w[0][2] + mid[2]
        al, th = hf.qrot(w[1], (1, 0, 0)), hf.qrot(w[1], (0, 0, 1))
        wd = he[0] * 2
        hw, hd = wd / 2 + 0.05, 0.6
        pts = [(cx + al[0] * sa * hw + th[0] * sd * hd, cz + al[2] * sa * hw + th[2] * sd * hd)
               for sa in (-1, 1) for sd in (-1, 1)]
        y0 = w[0][1] + mid[1] - he[1]
        zones.append(hf.Zone(hf.hull(pts), y0 + 0.05, y0 + 1.9, ents[e['parentId']]['name']))

    probs = {'into wall': [], 'overlapping': [], 'floating': [], 'sunk': [], 'in doorway': []}
    ids = sorted(pieces)
    for i in ids:
        if i in fixed or name[i].startswith(SKIP):
            continue
        bs = pieces[i]
        depth = max((hf.penetration(b, s) for b in bs for s in structure), default=0.0)
        if depth > 0.02:
            probs['into wall'].append((depth, name[i], i))
        bottom = min(b.ylo() for b in bs)
        floor = fh.FLOOR_1 if bottom < 3.0 else fh.FLOOR_2
        if bottom < floor - 0.03:
            probs['sunk'].append((floor - bottom, name[i], i))
        elif bottom > floor + 0.04:
            under = False
            for j in ids:
                if j == i:
                    continue
                for c in pieces[j]:
                    if bottom - 0.06 <= c.yhi() <= bottom + 0.04 and any(hf.poly_overlap(c.xz(), b.xz()) for b in bs):
                        under = True
                        break
                if under:
                    break
            if not under and not any(bottom - 0.06 <= s.yhi() <= bottom + 0.04 for s in structure
                                     if any(hf.poly_overlap(s.xz(), b.xz()) for b in bs)):
                probs['floating'].append((bottom - floor, name[i], i))
        for z in zones:
            if any(z.hits(b.shrunk(0.01)) for b in bs) and bs[0].ylo() < z.y0 + 1.5:
                probs['in doorway'].append((0, name[i] + ' @ ' + z.name, i))
                break
    for n_, i in enumerate(ids):
        if i in fixed or name[i].startswith(SKIP):
            continue
        for j in ids[n_ + 1:]:
            if j in fixed or name[j].startswith(SKIP):
                continue
            dep = max((hf.penetration(a, b) for a in pieces[i] for b in pieces[j]), default=0.0)
            if dep > 0.02:
                probs['overlapping'].append((dep, f'{name[i]} x {name[j]}', i))
    print(f'house_check {os.path.basename(path)}: {len(pieces)} pieces, {len(structure)} wall boxes, {len(zones)} doorways')
    for k_, v in probs.items():
        v.sort(reverse=True)
        print(f'  {k_}: {len(v)}' + ('  e.g. ' + '; '.join(f'{t} ({x:.2f})' for x, t, _ in v[:6]) if v else ''))
        if show:
            for x, t, i in v:
                p = world(i)[0]
                print(f'      {x:.3f} {t} #{i} at ({p[0]:.2f}, {p[1]:.2f}, {p[2]:.2f})')


if __name__ == '__main__':
    main()
