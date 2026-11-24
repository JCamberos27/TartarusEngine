#!/usr/bin/env python3
"""Add a Reflection Probe 'Probe Master Bedroom' to Forest.json (text insert after 'Probe Upstairs Bathroom', CRLF-safe, idempotent, fails loudly).

The master bedroom is ~10 m from House Probe Upstairs' centre (one 29 x 11 m cube for the whole floor), so its metals (armchair rail, lamp bases, poles)
reflect the box-projected, differently lit upstairs cube and do not take the room's red light. Same remedy the two bathrooms got: a small room probe,
importance 3. Capture Scene = rendered at runtime: no bake, no recapture step.
usage: add_master_probe.py SCENE.json --id N --order N
"""
import argparse, json, re, sys
ap = argparse.ArgumentParser(); ap.add_argument('scene'); ap.add_argument('--id', type=int, required=True); ap.add_argument('--order', type=int, required=True)
a = ap.parse_args()
s = open(a.scene, 'rb').read().decode('utf-8'); nl = '\r\n' if '\r\n' in s else '\n'
if '"name": "Probe Master Bedroom"' in s: print('already present: no change'); sys.exit(0)
anchor = '"name": "Probe Upstairs Bathroom"'
if s.count(anchor) != 1: sys.exit('FAIL: anchor "Probe Upstairs Bathroom" count = %d' % s.count(anchor))
if re.search(r'"id": %d,' % a.id, s): sys.exit('FAIL: id %d already used' % a.id)
i = s.index(anchor); j = s.index('static": true' + nl + '    },', i) + len('static": true' + nl + '    },')
ent = {"Reflection Probe": {"Capture Scene": True, "Importance": 3.0, "Size": [6.4, 2.8, 5.9]}, "id": a.id, "name": "Probe Master Bedroom", "order": a.order,
       "parentId": 22363, "position": [-9.95, 4.6, 23.1], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0], "static": True}
txt = json.dumps(ent, indent=2)
txt = nl.join('    ' + l for l in txt.split('\n'))
s = s[:j] + nl + txt + ',' + s[j:]
open(a.scene, 'wb').write(s.encode('utf-8'))
json.loads(s)  # must still parse
print('added Probe Master Bedroom id', a.id, 'order', a.order)
