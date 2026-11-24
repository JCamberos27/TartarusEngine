# Mini tours for the OPTION before/after shots (W6 Briareos): a filtered copy of sweep_tour_open.json, same poses as the sweep BEFORE.
# usage: python -I make_option_tours.py   -> sweep_tour_opt_<name>.json next to this script
import json, os
here = os.path.dirname(os.path.abspath(__file__))
t = json.load(open(os.path.join(here, 'sweep_tour_open.json')))
OPT = {'tableb': ('jlook-tableb-line_floor', 'jlook-tableb-line_ceil'),            # 6c4b9d89 dev/fix3-fam3-tableB-fade
       'south': ('jlook-south-wall-z20-3_view',)}                                    # 525e3a75 dev/fix3-fam4-south
# poses from Atlas (2026-11-24, computed from collider geometry; yaw 0 = +x, 90 = +z): not in the sweep tour, option-only shots.
EXTRA = {'halfwall': [('jlook-halfwall_view', (-8.20, 0.194, 22.80), 56.0, 0.0, 'kitchen')],        # as-built only, no branch
         'hallline': [('jlook-hallline_view', (3.40, 0.194, 28.30), 90.0, 20.0, 'entrance hall'),   # BEFORE as-built 3.05 cut; AFTER d26b5d3c (runtime preview, no re-bake)
                      ('jlook-hallline_alt', (7.50, 3.104, 29.20), 150.0, -20.0, 'upstairs landing')]}
for name, shots in EXTRA.items():
    wps = [{'id': i, 'room': room, 'giVolume': '', 'kind': 'sweep_option', 'feet': list(f), 'yawDeg': y, 'pitchDeg': pt, 'cut': True} for i, f, y, pt, room in shots]
    wps[0].pop('cut', None)
    out = dict(t); out['waypoints'] = wps
    json.dump(out, open(os.path.join(here, 'sweep_tour_opt_%s.json' % name), 'w'), indent=1)
    print(name, [w['id'] for w in wps])
_all = [x for v in EXTRA.values() for x in v]
wps = [{'id': i, 'room': room, 'giVolume': '', 'kind': 'sweep_option', 'feet': list(f), 'yawDeg': y, 'pitchDeg': pt, 'cut': True} for i, f, y, pt, room in _all]
wps[0].pop('cut', None); out = dict(t); out['waypoints'] = wps
json.dump(out, open(os.path.join(here, 'sweep_tour_opt_asbuilt.json'), 'w'), indent=1); print('asbuilt', [w['id'] for w in wps])
for name, ids in OPT.items():
    wps = [w for w in t['waypoints'] if w['id'] in ids]
    assert len(wps) == len(ids), (name, [w['id'] for w in wps])
    wps[0] = dict(wps[0]); wps[0].pop('cut', None)
    out = dict(t); out['waypoints'] = wps
    json.dump(out, open(os.path.join(here, 'sweep_tour_opt_%s.json' % name), 'w'), indent=1)
    print(name, [w['id'] for w in wps])
