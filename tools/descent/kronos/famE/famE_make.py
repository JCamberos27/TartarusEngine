"""famE A/B scene + tour builder (W5). python -I famE_make.py <A Forest.json> <ced5d6b7 Forest.json> <scenes dir> <sweep tour> <out>
B = A with ONLY family E swapped in from ced5d6b7: added 38103/38104/38105, changed 36392/36394/35609."""
import copy
import json
import sys

a_path, e_path, scenes, sweep_path, out = sys.argv[1:6]
A = json.load(open(a_path, encoding='utf-8-sig'))
E = json.load(open(e_path, encoding='utf-8-sig'))
ADDED, CHANGED = (38103, 38104, 38105), (36392, 36394, 35609)
LISTS = ('empties', 'models', 'boxes')


def find(s, i):
    for k in LISTS:
        for n, e in enumerate(s.get(k, [])):
            if e.get('id') == i:
                return k, n
    return None, None


B = copy.deepcopy(A)
for i in CHANGED:
    ka, na = find(B, i)
    ke, ne = find(E, i)
    assert ka and ke, ('changed id missing', i)
    B[ka][na] = copy.deepcopy(E[ke][ne])
orders = {e.get('order') for k in LISTS for e in B.get(k, []) if 'order' in e}
for i in ADDED:
    assert find(B, i)[0] is None, ('id already in A', i)
    ke, ne = find(E, i)
    assert ke, ('added id missing in ced5d6b7', i)
    new = copy.deepcopy(E[ke][ne])
    # Argus's bridges collided with family E once (now ids 38106-38108, orders 160012-160014): fail loudly on any clash.
    assert new.get('order') not in orders, ('order already used in A', i, new.get('order'))
    B[ke].append(new)
json.dump(A, open(f'{scenes}/Forest_famE_A.json', 'w', encoding='utf-8'), indent=1)
json.dump(B, open(f'{scenes}/Forest_famE_B.json', 'w', encoding='utf-8'), indent=1)

sweep = json.load(open(sweep_path, encoding='utf-8'))
want = ('jlook-facade-', 'jlook-porch-front', 'jlook-jamb-35614_east', 'living-room_')
wps = [w for w in sweep['waypoints'] if w['id'].startswith(want)]
assert wps, 'no waypoints'
base = {k: v for k, v in sweep.items() if k != 'waypoints'}
for arm in ('A', 'B'):  # variant C (36370 shadows) dropped by the PM 17:5x
    json.dump(dict(base, scene=f'scenes/Forest_famE_{arm}.json', waypoints=wps),
              open(f'{out}/tour_{arm}.json', 'w', encoding='utf-8'), indent=1)
nl = lambda s: sum(1 for k in LISTS for e in s.get(k, []) if 'Light' in e)
print(f'A lights {nl(A)}, B lights {nl(B)}; {len(wps)} views: ' + ', '.join(w['id'] for w in wps))
