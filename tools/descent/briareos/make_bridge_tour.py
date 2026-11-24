# Look poses for the GI BRIDGE volumes (named "GI bridge ..."), found BY NAME in a scene (never by entity id). CPU only.
#   cd <repo worktree root>; python -I tools/descent/briareos/make_bridge_tour.py --scene <Forest.json with the bridges> --out <dir>/bridges.json
# Per bridge: two poses, one each side along the bridge's thin axis (the opening it blends across), looking at its centre, eye 1.6, pitch -8;
# the first free spot (no collider within 0.3 m in the body band) at 1.8, 1.4, 2.2, 1.0, 2.6 m back. The tour is the --extra input of make_look_tour.py.
# Run it on the scene that HAS the bridges; shoot the same json BEFORE (scene without them) and AFTER.
import sys, os, json, math, argparse
import numpy as np
sys.path.insert(0, os.path.join(os.getcwd(), 'tools', 'gi'))
import gi_audit as G

ap = argparse.ArgumentParser()
ap.add_argument('--scene', default='project/scenes/Forest.json'); ap.add_argument('--out', required=True)
ap.add_argument('--match', default='GI bridge'); ap.add_argument('--eye', type=float, default=1.6)
a = ap.parse_args()
S = G.Scene(json.load(open(a.scene, encoding='utf-8')))
GROUND, UPPER = 0.194, 3.104
bl = []
for e in S.ents:
    c = e.get('collider')
    if not c or c.get('isTrigger') or c.get('shape', 0) != 0 or not S.active(e): continue
    if 'Openable' in e or any('Openable' in p for p in S.ancestors(e)): continue
    b = S.box_collider(e)
    if b is None: continue
    lo, hi = b.aabb()
    if hi[0] - lo[0] > 40 or hi[2] - lo[2] > 40: continue
    bl.append((np.array(lo), np.array(hi)))
def free(px, py, pz, pad=0.3):
    for lo, hi in bl:
        if hi[1] > py + 0.15 and lo[1] < py + a.eye + 0.2 and lo[0] - pad < px < hi[0] + pad and lo[2] - pad < pz < hi[2] + pad: return False
    return True

rooms = [v for v in S.volumes() if v['active'] and not v['name'].startswith(a.match)]
def inside(px, py, pz):
    return any(r['box'].inside_margin([px, py + a.eye, pz]) > 0.05 for r in rooms)
wps = []
for v in S.volumes():
    if not v['name'].startswith(a.match) or not v['active']: continue
    lo, hi = v['box'].aabb(); c = [(lo[i] + hi[i]) / 2 for i in range(3)]
    ext = [hi[i] - lo[i] for i in range(3)]
    ax = 0 if ext[0] < ext[2] else 2                       # the thin horizontal axis = the way light crosses the opening
    floor = GROUND if c[1] < 3.0 else UPPER
    slug = ''.join(ch if ch.isalnum() else '-' for ch in v['name'].lower()).strip('-')
    for sgn, side in ((-1, 'a'), (1, 'b')):
        for d in (1.8, 1.4, 2.2, 1.0, 2.6):
            p = [c[0], c[2]]; p[0 if ax == 0 else 1] += sgn * d
            if not free(p[0], floor, p[1]) or not inside(p[0], floor, p[1]): continue   # free AND inside a room's GI volume (not out in the yard)
            yaw = math.degrees(math.atan2(-sgn if ax == 2 else 0, -sgn if ax == 0 else 0))
            wps.append({'id': 'bridge-%s_%s' % (slug.replace('gi-bridge-', ''), side), 'room': v['name'], 'giVolume': v['name'], 'kind': 'sweep_bridge',
                        'feet': [round(p[0], 3), floor, round(p[1], 3)], 'yawDeg': round(yaw, 1), 'pitchDeg': -8.0, 'cut': True,
                        'note': '%s centre (%.2f, %.2f), thin axis %s, %.1f m %s of it' % (v['name'], c[0], c[2], 'xz'[ax // 2], d, side)})
            break
        else:
            print('NO FREE SPOT', v['name'], side)
json.dump({'title': 'GI bridges (by name)', 'waypoints': wps}, open(a.out, 'w'), indent=1)
for w in wps: print(w['id'], w['feet'], w['yawDeg'])
print(len(wps), 'bridge poses ->', a.out)
