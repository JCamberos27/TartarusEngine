# Every piece of the Forest house and its yard, for the asset-by-asset review: builds the Forest in memory (as
# forest_house_into.py does), names the rooms by what's in them, and writes each top-level piece with its group, room,
# world position and world box.
#
#   python tools/scenes/house_inventory.py <out.json> [<repo>]
# The JSON feeds house_shots.py (cameras framed on each piece) and the review checklist.
import json
import math
import os
import sys

import build_forest as bf
import forest_house as fh
import house_lights as hl

# Groups reviewed as a whole per room (the plan's section G): rubbish, leaves, decals, the yard's overgrowth.
GROUPED = ('Rubbish', 'Damage', 'Overgrowth')


def main():
    out = sys.argv[1]
    repo = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', '..'))
    got = {}
    add_house = fh.add_house

    def capture(*a, **kw):
        r = add_house(*a, **kw)
        got['pk'] = r[0]
        return r
    fh.add_house = capture
    s, _ = bf.build(repo)
    pk = got['pk']
    ents = s.empties + s.models + s.boxes
    by = {e['id']: e for e in ents}
    kids = {}
    for e in ents:
        kids.setdefault(e['parentId'], []).append(e)

    rooms = hl.named_rooms(pk.fit, fh.floor_rects(pk), fh)

    def room_at(p):
        return hl.room_of(rooms, p)

    # world transforms of the scene entities (position + yaw is enough for framing)
    def local(e):
        return e.get('position', [0, 0, 0]), e.get('rotation', [0, 0, 0, 1]), e.get('scale', [1, 1, 1])

    cache = {}

    def world(i):
        if i in cache:
            return cache[i]
        e = by[i]
        p, r, sc = local(e)
        if e['parentId'] in by:
            pp, pr, ps = world(e['parentId'])
            v = fh.qrot(pr, [p[0] * ps[0], p[1] * ps[1], p[2] * ps[2]])
            res = ([pp[0] + v[0], pp[1] + v[1], pp[2] + v[2]], fh.qmul(pr, r), [ps[a] * sc[a] for a in range(3)])
        else:
            res = (list(p), list(r), list(sc))
        cache[i] = res
        return res

    house = next(e for e in s.empties if e['name'] == 'Abandoned House')
    lay = next(e for e in kids[house['id']] if e['name'] == 'Pack Layout')
    yard = next(e for e in s.empties if e['name'] == 'Yard')
    inv_lay = {v: k_ for k_, v in pk.remap.items()}       # scene id -> pack id

    def pack_point(w):
        """A world point back in pack space (to_world inverted)."""
        a = math.radians(fh.HOUSE_YAW)
        dx, dz = w[0] - fh.HOUSE_AT[0], w[2] - fh.HOUSE_AT[2]
        return [fh.PACK_C[0] + math.cos(a) * dx - math.sin(a) * dz, w[1] - fh.HOUSE_AT[1] + fh.PACK_C[1],
                fh.PACK_C[2] + math.sin(a) * dx + math.cos(a) * dz]

    def wbox(e):
        """The piece's world box from the fit's measured bounds (pack pieces), else a small box at its origin."""
        pid = inv_lay.get(e['id'])
        boxes = pk.fit.boxes.get(pid) if pid is not None else None
        if not boxes and pid is not None:
            boxes = pk.fit.prop_boxes(pid)
        if boxes:
            pts = [fh.to_world(p) for b in boxes for p in b.corners()]
            return [round(min(p[a] for p in pts), 3) for a in range(3)] + \
                   [round(max(p[a] for p in pts), 3) for a in range(3)]
        p = world(e['id'])[0]
        return [round(p[a] - 0.15, 3) for a in range(3)] + [round(p[a] + 0.15, 3) for a in range(3)]

    rows = []

    def add(e, group, grouped=False):
        p = world(e['id'])[0]
        bx = wbox(e)
        r = hl.room_of(rooms, pack_point([(bx[k] + bx[k + 3]) / 2 for k in range(3)]), 0.6)   # by its middle
        rows.append({'id': e['id'], 'name': e['name'], 'group': group, 'grouped': grouped,
                     'room': r['name'] if r else ('porch/outside' if group != 'Yard' else 'yard'),
                     'pos': [round(v, 3) for v in p], 'box': bx, 'path': e.get('path', ''),
                     'kind': 'model' if 'path' in e else ('light' if 'Light' in e else 'empty'),
                     'wall': bool(e.get('Light Switch') and e.get('Light Switch', {}).get('Rocker')) or
                     e['name'].startswith(('Painting', 'Mirror', 'Lamp_wall', 'Light Switch', 'Ventilationgrid',
                                           'Shelf_', 'Curtains_'))})

    for g in kids[lay['id']]:
        if g['name'] == 'House_01':
            for sub in kids[g['id']]:
                for e in kids.get(sub['id'], []):
                    add(e, f"House_01/{sub['name']}", grouped=sub['name'] in ('Walls', 'Roofs', 'Floors_1st',
                                                                             'Floors_2nd'))
        else:
            for e in kids.get(g['id'], []):
                add(e, g['name'], grouped=g['name'] in GROUPED)
    for g in kids[yard['id']]:
        for e in kids.get(g['id'], []):
            add(e, f"Yard/{g['name']}", grouped=g['name'] in GROUPED)
            rows[-1]['room'] = 'yard'
    porch = next((e for e in s.empties if e['name'] == 'Porch Light'), None)
    if porch and porch['parentId'] != lay['id']:
        for e in kids.get(porch['id'], []):
            add(e, 'Porch Light')
    data = {'rooms': [{'name': r['name'], 'storey': r['storey'], 'floor': r['floor'], 'ceil': r['ceil'],
                       'area': round(len(r['cells']) * hl.ROOM_CELL ** 2, 1),
                       'centre': fh.to_world((sum(c[0] for c in r['cells']) / len(r['cells']), r['floor'],
                                              sum(c[1] for c in r['cells']) / len(r['cells'])))} for r in rooms],
            'pieces': rows}
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=1)
    from collections import Counter
    print(f'{len(rows)} pieces ({sum(not r["grouped"] for r in rows)} on their own), {len(rooms)} rooms -> {out}')
    print(Counter(r['room'] for r in rows))


if __name__ == '__main__':
    main()
