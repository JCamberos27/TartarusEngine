# Compact LOOK tour for a before/after sheet (W6 Briareos). CPU only. Picks shots from the full open tour by NAME / POSITION (never by entity id:
# ids were renumbered by #1216) and adds the fixed look poses, so the SAME poses are shot BEFORE and AFTER a bake.
#   cd <repo worktree root>; python -I tools/descent/briareos/make_sweep_tour_v2.py --out <gen dir>     (full tours + sweep_plan.json)
#   python -I tools/descent/briareos/make_look_tour.py --gen <gen dir> --out <dir>/sweep_tour_look4.json [--extra a.json --extra b.json]
# --extra = more tours (same format) whose waypoints are appended, e.g. the bridges' seam poses; ids must not repeat.
import json, os, argparse, math

ap = argparse.ArgumentParser()
ap.add_argument('--gen', required=True, help='folder with sweep_tour_open.json + sweep_plan.json from make_sweep_tour_v2.py')
ap.add_argument('--out', required=True)
ap.add_argument('--extra', action='append', default=[])
a = ap.parse_args()
here = os.path.dirname(os.path.abspath(__file__))
tour = json.load(open(os.path.join(a.gen, 'sweep_tour_open.json'), encoding='utf-8'))
plan = json.load(open(os.path.join(a.gen, 'sweep_plan.json'), encoding='utf-8'))
W = {w['id']: w for w in tour['waypoints']}

# group -> list of waypoint ids (exact). A missing id is an error: a look pose must never silently vanish.
GROUPS = [
    ('master-ceiling', 'Master bedroom ceiling line (the diagonal across the red room ceiling) and the Bounds +z edge z 25.938',
     ['master-bedroom_ceil', 'master-bedroom-spot2_ceil', 'master-bedroom-spot3_ceil', 'master-bedroom-spot4_ceil', 'master-ceil-edge_pz',
      'jlook-red-zface_across', 'jlook-red-zface_along']),
    ('south-fade', 'Kitchen south window wall z 20.3 (the inward fade of the south lamp; floor 0.194 + ceiling 2.871 junctions)',
     ['jlook-south-wall-z20-3_view', 'jlook-chandelier-graze_face']),
]
# poses not in the full tour (Atlas, computed from collider geometry; yaw 0 = +x, 90 = +z, -90 = -z)
EXTRA_POSES = [
    ('hall-line', 'Stairwell hall line: the entrance-hall ceiling cut (3.05) and the landing seen from above',
     [('jlook-hallline_view', 'entrance hall', (3.40, 0.194, 28.30), 90.0, 20.0), ('jlook-hallline_alt', 'upstairs landing', (7.50, 3.104, 29.20), 150.0, -20.0)]),
]
# thresholds by POSITION (xz): the seams Jacob has seen (living room 1|4, kitchen-dining 3|7, hall 1|2)
SEAMS = [('seam-living', 'Seam living room 1|4 (z 24.2, x 11.16)', (11.16, 24.2)),
         ('seam-kitchen', 'Seam kitchen-dining 3|7 (z 28.9, x -12.43)', (-12.43, 28.9)),
         ('seam-hall', 'Seam entrance hall 1|2 (x -0.95, z 25.53)', (-0.95, 25.53))]

sel, groups = [], []
def take(wp, group):
    if any(w['id'] == wp['id'] for w in sel): raise SystemExit('duplicate id ' + wp['id'])
    w = dict(wp); w['group'] = group; sel.append(w)
for g, title, ids in GROUPS:
    groups.append({'group': g, 'title': title, 'ids': ids})
    for i in ids:
        if i not in W: raise SystemExit('look pose %s is not in the full tour (scene changed?)' % i)
        take(W[i], g)
for g, title, shots in EXTRA_POSES:
    groups.append({'group': g, 'title': title, 'ids': [s[0] for s in shots]})
    for i, room, f, y, p in shots:
        take({'id': i, 'room': room, 'giVolume': '', 'kind': 'sweep_option', 'feet': list(f), 'yawDeg': y, 'pitchDeg': p, 'cut': True}, g)
for g, title, (x, z) in SEAMS:
    best = min((t for t in plan['thresholds'] if t['storey'] == 'ground'), key=lambda t: math.hypot(t['at'][0] - x, t['at'][1] - z))
    d = math.hypot(best['at'][0] - x, best['at'][1] - z)
    if d > 1.0: raise SystemExit('no threshold within 1 m of %s (nearest %s at %.2f m)' % (title, best['id'], d))
    ids = [best['id'] + '_look-neg', best['id'] + '_look-pos']
    groups.append({'group': g, 'title': title + ' = %s' % best['id'], 'ids': ids})
    for i in ids: take(W[i], g)
for fn in a.extra:
    ex = json.load(open(fn, encoding='utf-8'))
    g = os.path.splitext(os.path.basename(fn))[0]
    groups.append({'group': g, 'title': ex.get('title', g), 'ids': [w['id'] for w in ex['waypoints']]})
    for w in ex['waypoints']: take(w, g)
# the stills wait for the previous shot's light to settle only when 'cut' is set; the first shot of the tour carries none
sel[0] = dict(sel[0]); sel[0].pop('cut', None)
out = dict(tour); out['waypoints'] = [{k: v for k, v in w.items() if k != 'group'} for w in sel]
json.dump(out, open(a.out, 'w'), indent=1)
json.dump(groups, open(os.path.splitext(a.out)[0] + '_groups.json', 'w'), indent=1)
print('look tour', len(sel), 'shots,', len(groups), 'groups ->', a.out)
