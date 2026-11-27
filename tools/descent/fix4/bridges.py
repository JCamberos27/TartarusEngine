"""fix4 re-apply: the 4 GI seam bridges (Kronos/W5; Jacob IN after his look).

Source: dev/look-bridges e079c65f vs its base 657f87b8 (Forest.json only, +4 "GI Volume" empties, nothing else).
Old ids 38106/38107/38108/38112 are void after #1216; everything is matched by NAME here.
New ids/orders come from Kronos's block (TODAY_6PM_PLAN: ids 36190-36199, orders 268784-268793).

usage: python -I tools/descent/fix4/bridges.py project/scenes/Forest.json [--check]

- Parent: the one empty named "House Lights" at (0,0,0) (old id 35585).
- Each bridge goes in "empties" right after its room's GI volume (as on the old branch).
- The bridges copy their neighbour volume's "Circuits" string; the script checks it still matches on the target.
- Idempotent: a bridge already present (by name) and in spec is left alone; one present but different FAILS.
- Fails loudly on a missing/ambiguous name, a parent mismatch or an id/order clash.
- TEXT insertion: every existing byte of Forest.json stays as the editor wrote it (its float printer can't be
  reproduced from Python); each new block is inserted after its neighbour's closing line, then the whole file is
  re-parsed and checked against the expected entity list.
"""
import json
import sys

PARENT = 'House Lights'
IDS = range(36190, 36200)
ORDERS = range(268784, 268794)


def _vol(circuits, fade_axis, size):
    fade = [0.0, 0.0, 0.0]
    fade[fade_axis] = 0.30000001192092896
    return {
        'Bake File': 'scenes/Forest.gi',
        'Circuits': circuits,
        'Fade': 0.10000000149011612,
        'Fade Max': list(fade),
        'Fade Min': list(fade),
        'Intensity': 2.0,
        'Outside': True,
        'Size': size,
        'Spacing': 0.5,
    }


# (name, after (neighbour GI volume), id, order, GI Volume, position): values copied from e079c65f.
BRIDGES = [
    ('GI bridge living 1-4', 'GI living room 1', 36190, 268784,
     _vol('Living room;Lamp_table_A 15;Lamp_stand 14', 0, [0.8319354057312012, 2.815000057220459, 4.349999904632568]),
     [-31.204030990600586, 2.0380001068115234, 10.183062553405762]),
    ('GI bridge kitchen-dining 3-7', 'GI kitchen-dining 3', 36191, 268785,
     _vol('Kitchen;Lamp_wall_fluorescent 18;Lamp_stand 19', 0, [0.7100018262863159, 2.815000057220459, 5.550000190734863]),
     [-35.900001525878906, 2.0380001068115234, -12.503000259399414]),
    ('GI bridge entrance hall 1-2', 'GI entrance hall 1', 36192, 268786,
     _vol('Hall', 2, [2.4000000953674316, 2.815000057220459, 0.7400000095367432]),
     [-32.481998443603516, 2.0380001068115234, -1.9499998092651367]),
    ('GI bridge kitchen-dining 1-dining bay', 'GI kitchen-dining 1', 36193, 268787,
     _vol('Kitchen;Lamp_stand 19', 0, [0.870000422000885, 2.809999942779541, 2.1500000953674316]),
     [-27.315000534057617, 2.0399999618530273, -10.174999237060547]),
]


def die(msg):
    sys.exit(f'bridges.py: FAIL: {msg}')


def all_entities(scene):
    for key, val in scene.items():
        if isinstance(val, list):
            for e in val:
                if isinstance(e, dict) and 'id' in e:
                    yield key, e


def one_named(empties, name):
    hits = [i for i, e in enumerate(empties) if e.get('name') == name]
    if len(hits) != 1:
        die(f'{len(hits)} empties named {name!r} (need exactly 1)')
    return hits[0]


def block_span(lines, name):
    """(first, last) line index of the entity object whose entity-level "name" line is exactly this name."""
    key = f'      "name": {json.dumps(name, ensure_ascii=False)},'
    hits = [i for i, l in enumerate(lines) if l == key]
    if len(hits) != 1:
        die(f'{len(hits)} entity-level name lines for {name!r} (need exactly 1)')
    i = hits[0]
    a = next(j for j in range(i, -1, -1) if lines[j] == '    {')
    b = next(j for j in range(i, len(lines)) if lines[j] in ('    },', '    }'))
    return a, b


def block_text(ent):
    return ['    ' + l for l in json.dumps(ent, indent=2, ensure_ascii=False).split('\n')]


def main():
    path = sys.argv[1]
    check = '--check' in sys.argv[2:]
    raw = open(path, 'rb').read().decode('utf-8')
    nl = '\r\n' if '\r\n' in raw else '\n'  # keep the checkout's line endings (autocrlf gives CRLF on Windows)
    if nl == '\r\n' and raw.count('\n') != raw.count('\r\n'):
        die('mixed line endings')
    scene = json.loads(raw)
    empties = scene['empties']
    parent = empties[one_named(empties, PARENT)]
    if any(abs(v) > 1e-6 for v in parent.get('position', [0, 0, 0])):
        die(f'{PARENT!r} is not at the origin: {parent.get("position")}')
    pid = parent['id']
    names = {b[0] for b in BRIDGES}
    for _, e in all_entities(scene):
        if e.get('name') in names:
            continue
        if e['id'] in IDS:
            die(f'id {e["id"]} (Kronos block) already used by {e.get("name")!r}')
        if e.get('order') in ORDERS:
            die(f'order {e["order"]} (Kronos block) already used by {e.get("name")!r}')

    lines = raw.split(nl)
    expect = list(empties)
    changed = []
    for name, after, nid, order, vol, pos in BRIDGES:
        nb = empties[one_named(empties, after)]
        if nb.get('parentId') != pid:
            die(f'{after!r} is not a child of {PARENT!r}')
        if nb['GI Volume'].get('Circuits') != vol['Circuits']:
            die(f'{after!r} Circuits changed: {nb["GI Volume"].get("Circuits")!r} != {vol["Circuits"]!r}')
        want = {'GI Volume': vol, 'id': nid, 'name': name, 'order': order, 'parentId': pid, 'position': pos,
                'rotation': [0.0, 0.0, 0.0, 1.0], 'scale': [1.0, 1.0, 1.0]}
        hits = [e for e in empties if e.get('name') == name]
        if hits:
            if len(hits) > 1 or hits[0] != want:
                die(f'{name!r} is already in the scene but NOT in spec ({len(hits)} found): fix by hand')
            print(f'= {name}: already in spec (id {nid})')
            continue
        _, b = block_span(lines, after)
        new = block_text(want)
        if lines[b] == '    }':  # neighbour was the last element: it gets the comma, the new block ends the list
            lines[b] = '    },'
        else:
            new[-1] += ','
        lines[b + 1:b + 1] = new
        expect.insert(next(i for i, e in enumerate(expect) if e.get('name') == after) + 1, want)
        changed.append(f'+ {name}: id {nid}, order {order}, parent {PARENT} ({pid}), after {after!r}, '
                       f'pos {pos}, size {vol["Size"]}')
    out = nl.join(lines)
    got = json.loads(out)
    rest = lambda s: {k: v for k, v in s.items() if k != 'empties'}
    if got['empties'] != expect or rest(got) != rest(scene):
        die('re-parse check failed: the edited file is not the expected scene')
    for c in changed:
        print(c)
    if changed and not check:
        open(path, 'wb').write(out.encode('utf-8'))
        print(f'bridges.py: wrote {path} ({len(changed)} change(s))')
    elif changed:
        print(f'bridges.py: --check: {len(changed)} change(s) NOT written')
        sys.exit(2)
    else:
        print('bridges.py: no change')


if __name__ == '__main__':
    main()
