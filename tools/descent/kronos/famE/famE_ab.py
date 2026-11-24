"""famE A/B analysis (W5). python -I famE_ab.py <famE dir> <repeats>
A = 677c753d scene; B = A + family E (3 unshadowed strip fills 38103-38105, 36392/36394/35609 trimmed).
(Variant C, spot 36370 shadows, was dropped by the PM.)
Frame time: median gpu_total / gpu_opaque over each view's still-hold ('settle') frames, first 30 after the cut dropped,
pooled over the interleaved repeats; the A1-vs-A2 spread is the noise floor. NOT perf numbers for the record.
Lights reaching a view (tour_lights.csv): switched on, fade > 0, camera within Range.
Spot shadow slots (budget 8), A: per view, which shadowed spots hold a slot, and how many shadowed street lamps
('Lamp Light') reach the view with NO slot -> data for the street-lamp FOUND_ISSUES item (indoor AND outdoor views)."""
import csv
import os
import statistics
import sys
from collections import defaultdict

d, reps = sys.argv[1], int(sys.argv[2])
ARMS = 'AB'


def med(v):
    return statistics.median(v) if v else float('nan')


def perf(run):
    rows, seen = defaultdict(lambda: defaultdict(list)), defaultdict(int)
    p = os.path.join(d, run, 'tour_perf.csv')
    if not os.path.exists(p):
        return rows
    for r in csv.DictReader(open(p, encoding='utf-8')):
        kind, _, seg = r['segment'].partition(':')
        if kind != 'settle':  # the still holds; 'shot' / 'walk' rows are captures and camera moves
            continue
        seen[seg] += 1
        if seen[seg] <= 30:  # skip the first 30 frames after each camera cut (shadow/LOD settle)
            continue
        for k in ('gpu_total_ms', 'gpu_opaque_ms'):
            v = float(r[k])
            if v >= 0:
                rows[seg][k].append(v)
    return rows


def lights(run):
    """per view: lights reaching it, strip fills among them, slot holders (shadowed spots with slot >= 0), and shadowed
    street lamps that reach the view with no slot. 'entity' in tour_lights is the runtime handle, not the scene id."""
    out = defaultdict(lambda: {'reach': 0, 'fills': 0, 'slotted': [], 'lamps_noslot': 0, 'lamps_slot': 0})
    p = os.path.join(d, run, 'tour_lights.csv')
    if not os.path.exists(p):
        return out
    for r in csv.DictReader(open(p, encoding='utf-8')):
        v = out[r['waypoint']]
        on = r.get('on', '1') == '1'
        reach = on and float(r.get('fade') or 0) > 0 and float(r['distance']) <= float(r['range'])
        if reach:
            v['reach'] += 1
            if '(strip)' in r['light']:
                v['fills'] += 1
        if r['kind'] == 'spot' and r.get('shadow') == '1' and on:
            slot = int(r.get('slot') or -1)
            if slot >= 0:
                v['slotted'].append(f"{r['light']}@{slot}")
            if r['light'] == 'Lamp Light':
                if slot >= 0:
                    v['lamps_slot'] += 1
                elif reach:
                    v['lamps_noslot'] += 1
    return out


P = {f'{a}{i}': perf(f'{a}{i}') for a in ARMS for i in range(1, reps + 1)}
Lt = {a: lights(f'{a}1') for a in ARMS}
views = sorted({s for p in P.values() for s in p} | set(Lt['A']))


def pooled(arm, v, k):
    return [x for i in range(1, reps + 1) for x in P[f'{arm}{i}'][v][k]]


print('# famE A/B (A = 677c753d scene; B = A + family E: 3 unshadowed strip fills, 3 trimmed originals)\n')
print('NOT perf numbers for the record: read B-A against the A1-vs-A2 spread (noise floor).\n')
print('| view | lights A/B | strip fills (B) | gpu_total A (ms) | B (ms) | B-A | A1/A2 spread | opaque A | opaque B |')
print('|---|---|---|---|---|---|---|---|---|')
agg, spreads = {a: [] for a in ARMS}, []
for v in views:
    m = {a: med(pooled(a, v, 'gpu_total_ms')) for a in ARMS}
    o = {a: med(pooled(a, v, 'gpu_opaque_ms')) for a in ARMS}
    sp = abs(med(P['A1'][v]['gpu_total_ms']) - med(P[f'A{reps}'][v]['gpu_total_ms'])) if reps > 1 else float('nan')
    if all(m[a] == m[a] for a in ARMS):
        for a in ARMS:
            agg[a].append(m[a])
        spreads.append(sp)
    print(f"| {v} | {Lt['A'][v]['reach']}/{Lt['B'][v]['reach']} | {Lt['B'][v]['fills']} | {m['A']:.2f} | {m['B']:.2f} | "
          f"{m['B'] - m['A']:+.2f} | {sp:.2f} | {o['A']:.2f} | {o['B']:.2f} |")
if agg['A']:
    print(f"\nAll views (median of per-view medians): gpu_total A {med(agg['A']):.2f} ms, B {med(agg['B']):.2f} ms, "
          f"B-A {med(agg['B']) - med(agg['A']):+.2f} ms; median A1/A2 spread {med(spreads):.2f} ms")

print('\n## Spot shadow slots in A (budget 8): who holds them, and street lamps left unshadowed\n')
print('| view | slots used | holders (light@slot) | street lamps with a slot | street lamps reaching the view, NO slot |')
print('|---|---|---|---|---|')
for v in views:
    a = Lt['A'][v]
    print(f"| {v} | {len(a['slotted'])} | {', '.join(sorted(a['slotted']))} | {a['lamps_slot']} | {a['lamps_noslot']} |")
