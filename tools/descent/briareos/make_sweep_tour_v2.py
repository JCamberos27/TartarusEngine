# SEAM / DARK-PATCH SWEEP tour generator v2, EVERY SURFACE (W6 Briareos). CPU only.
# v2: each room spot faces all 4 walls + ceiling + floor; every door both sides at two distances (open run A, shut run B);
# stairs up/down; outside near the exterior doors (facade, porch ceiling, fittings). Writes sweep_tour_open.json (doorState
# allOpen: rooms, thresholds, doors open, outside, stairs) and sweep_tour_shut.json (authored = doors shut: doors, outside,
# each room's first spot ceiling + floor).
# For every room (GI volumes grouped by name, storey by storey): the best free spot (not in a wall or a collider,
# clear of furniture, inside the room's volumes), then a ceiling-facing, a floor-facing and a level shot at eye 1.6 m;
# for every doorway / volume seam between two volumes: shots from the threshold looking each way.
# usage (from a worktree root, any checkout of the scene to sweep):
#   python -I make_sweep_tour.py [--scene project/scenes/Forest.json] [--out DIR] [--door-state allOpen|authored]
# writes DIR/sweep_tour.json (the --gi-tour file), DIR/sweep_plan.json (rooms, reasons) and DIR/SWEEP_DRYCHECK.md
import sys, os, json, math, re, argparse, collections
import numpy as np
sys.path.insert(0, os.path.join(os.getcwd(), 'tools', 'gi'))
import gi_audit as G

ap = argparse.ArgumentParser()
ap.add_argument('--scene', default='project/scenes/Forest.json')
ap.add_argument('--out', default=os.path.dirname(os.path.abspath(__file__)))
ap.add_argument('--door-state', default='allOpen')
ap.add_argument('--eye', type=float, default=1.6)
ap.add_argument('--bounds', default=r'D:\Jacob\tblocks\descent\house-polish\atlas_fixtures\bounds_audit_final.json')
a = ap.parse_args()

EYE = a.eye
PAD = 0.25            # the player's radius: nothing blocking within this of the camera (xz)
CELL = 0.1
GROUND_FLOOR, UPPER_FLOOR = 0.194, 3.104     # Floors_1st / Floors_2nd collider tops
COVER = 3.5          # every free floor cell of a room within this of one of its spots
MAXSPOTS = 6
CEIL_GAP = 0.3        # eye must be at least this far under the ceiling

doc = json.load(open(a.scene, encoding='utf-8'))
S = G.Scene(doc)
vols = [v for v in S.volumes() if v['active'] and 'sealed' not in v['name']]   # sealed voids are not rooms

# ---- blockers: every solid box collider (not a trigger, not a door leaf) as a world AABB -----------------------------
def under_openable(e):
    return 'Openable' in e or any('Openable' in p for p in S.ancestors(e))
bl_min, bl_max = [], []
for e in S.ents:
    c = e.get('collider')
    if not c or c.get('isTrigger') or c.get('shape', 0) != 0 or not S.active(e) or under_openable(e):
        continue
    b = S.box_collider(e)
    if b is None:
        continue
    lo, hi = b.aabb()
    if hi[0] - lo[0] > 40 or hi[2] - lo[2] > 40:
        continue            # terrain-sized
    bl_min.append(lo); bl_max.append(hi)
# 2026-11-24 (W6): props the player can't walk through but that have no collider (the Christmas tree: living-room-spot4 stood inside it,
# shots 84-86/89 were branches). Treat them as a 0.9 m radius, floor-to-2 m blocker so no spot or wall run is placed in them.
SOFT_RADIUS = 0.9
for e in S.ents:
    if not S.active(e) or e.get('collider'):
        continue
    if not re.search(r'christ|xmas', e.get('path', '') + ' ' + e.get('name', ''), re.I):
        continue
    p = S.position(e)
    if not (-20 < p[0] < 20 and 15 < p[2] < 40):
        continue
    bl_min.append(np.array([p[0] - SOFT_RADIUS, p[1] - 0.05, p[2] - SOFT_RADIUS])); bl_max.append(np.array([p[0] + SOFT_RADIUS, p[1] + 2.0, p[2] + SOFT_RADIUS]))
    print('soft blocker', e.get('name'), [round(x, 2) for x in p])
BMIN, BMAX = np.array(bl_min), np.array(bl_max)
print('solid colliders', len(BMIN))

def blockers_for(floor):
    """The colliders in the eye's body band (knee to head) of a storey."""
    m = (BMAX[:, 1] > floor + 0.15) & (BMIN[:, 1] < floor + EYE + 0.2)
    return BMIN[m], BMAX[m]

def clearance(px, pz, bmin, bmax):
    """Horizontal distance from (px, pz) to the nearest blocker (0 = inside one). Arrays over points."""
    dx = np.maximum(np.maximum(bmin[None, :, 0] - px[:, None], px[:, None] - bmax[None, :, 0]), 0.0)
    dz = np.maximum(np.maximum(bmin[None, :, 2] - pz[:, None], pz[:, None] - bmax[None, :, 2]), 0.0)
    return np.hypot(dx, dz).min(axis=1) if bmin.shape[0] else np.full(len(px), 9.0)

def vols_at_floor(floor):
    out = []
    for v in vols:
        lo, hi = v['box'].aabb()
        if lo[1] - 0.05 <= floor + 0.8 <= hi[1] + 0.05:
            out.append(v)
    return out

def volume_holding(px, py, pz, vlist, margin=0.05):
    best, bm = None, -1e9
    for v in vlist:
        m = v['box'].inside_margin([px, py, pz])
        if m > bm: best, bm = v, m
    return (best, bm)

def slug(s):
    return re.sub(r'[^a-z0-9]+', '-', s.lower()).strip('-')

plan = {'rooms': [], 'thresholds': [], 'doors': [], 'outside': [], 'stairs': [], 'unresolved': []}
RUNS = {'open': [], 'shut': []}
def add(run, key, col, room, vol, feet, yaw, pitch, note=None, outside=False):
    """One waypoint in tour `run` ('open' / 'shut'); its id is <key>_<col> (the sheets group by key, columns by col)."""
    wid = '%s_%s' % (slug(key), col)
    w = {'id': wid, 'room': room, 'giVolume': vol['name'] if vol else '', 'kind': 'sweep_' + col.split('-')[0],
         'feet': [round(feet[0], 3), round(feet[1], 3), round(feet[2], 3)], 'yawDeg': round(((yaw + 180) % 360) - 180, 1),
         'pitchDeg': round(pitch, 1), 'cut': True}
    if outside: w['outside'] = True
    if note: w['note'] = note
    if any(x['id'] == wid for x in RUNS[run]): raise SystemExit('duplicate id ' + wid)
    RUNS[run].append(w)
    return w

def yaw_of(dx, dz):
    return math.degrees(math.atan2(dz, dx))

# ---- rooms ------------------------------------------------------------------------------------------------------------
by_room = collections.defaultdict(list)
for v in vols:
    by_room[G.room_of(v['name'])].append(v)

STOREYS = (('ground', GROUND_FLOOR), ('upstairs', UPPER_FLOOR))
room_spots = {}     # (room, storey) -> (x, z, floor, vol, long_dir)
for room in sorted(by_room):
    for sname, floor in STOREYS:
        vl = [v for v in by_room[room] if v in vols_at_floor(floor)]
        if not vl:
            continue
        lo = np.min([v['box'].aabb()[0] for v in vl], axis=0); hi = np.max([v['box'].aabb()[1] for v in vl], axis=0)
        xs = np.arange(lo[0] + CELL / 2, hi[0], CELL); zs = np.arange(lo[2] + CELL / 2, hi[2], CELL)
        X, Z = np.meshgrid(xs, zs); X = X.ravel(); Z = Z.ravel()
        eyey = floor + EYE
        inside = np.zeros(len(X), bool)
        for v in vl:
            b = v['box']; a0, a1 = b.aabb()
            inside |= (X > a0[0] + 0.05) & (X < a1[0] - 0.05) & (Z > a0[2] + 0.05) & (Z < a1[2] - 0.05)
        X, Z = X[inside], Z[inside]
        if not len(X):
            continue
        bmin, bmax = blockers_for(floor)
        clr = np.concatenate([clearance(X[i:i + 400], Z[i:i + 400], bmin, bmax) for i in range(0, len(X), 400)])
        free = clr >= PAD
        if not free.any():
            plan['unresolved'].append({'room': room, 'storey': sname, 'why': 'no free cell clear of colliders'}); continue
        cx, cz = X[free].mean(), Z[free].mean()
        score = np.minimum(clr, 1.5) - 0.12 * np.hypot(X - cx, Z - cz)
        score[~free] = -1e9
        fx = X[free]; fz = Z[free]; fc = clr[free]
        extent = float(max(fx.max() - fx.min(), fz.max() - fz.min()))
        eyey = floor + EYE
        # spots: the best centre first, then (k-centre) the free cell farthest from every spot so far, until every free
        # cell is within COVER m of a spot (long rooms like the kitchen-dining get several), at most MAXSPOTS
        k = int(score.argmax()); spots = [(float(X[k]), float(Z[k]), float(clr[k]))]
        dmin = np.hypot(fx - spots[0][0], fz - spots[0][1])
        while len(spots) < MAXSPOTS and (dmin > COVER).any():
            # extra spots need >= 0.6 m clear (2026-11-24: living-room spot6 at 0.38 m sat in a closet nook, shots 92/93 were clutter/blank wall)
            sc = np.where((dmin > COVER) & (fc >= 0.6), np.minimum(fc, 1.5) + 0.2 * np.minimum(dmin, 6.0), -1e9)
            if sc.max() < -1e8:
                break
            j = int(sc.argmax()); spots.append((float(fx[j]), float(fz[j]), float(fc[j])))
            dmin = np.minimum(dmin, np.hypot(fx - fx[j], fz - fz[j]))
        def run(x, z, dx, dz):
            d = 0.0
            while d < 12.0:
                px, pz = x + dx * (d + 0.1), z + dz * (d + 0.1)
                if clearance(np.array([px]), np.array([pz]), bmin, bmax)[0] < 0.05: break
                if not any(v['box'].contains([px, eyey, pz]) for v in vl): break
                d += 0.1
            return d
        out = []
        for (x, z, c) in spots:
            runs = [(run(x, z, dx, dz), (dx, dz)) for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1))]
            rl, dirn = max(runs)
            vol, marg = volume_holding(x, eyey, z, vl)
            out.append((x, z, floor, vol, dirn, extent, c, marg, rl))
        room_spots[(room, sname)] = out


# ---- the room shots (v2)

def free_at(x, z, floor, need=0.25):
    bmin, bmax = blockers_for(floor)
    return float(clearance(np.array([x]), np.array([z]), bmin, bmax)[0]) >= need

def in_volume(x, z, floor):
    return any(v['box'].contains([x, floor + EYE, z]) for v in vols_at_floor(floor))

CARD = ((0, (1, 0), 'xp'), (180, (-1, 0), 'xn'), (90, (0, 1), 'zp'), (-90, (0, -1), 'zn'))

# ---- A. room spots: 4 walls + ceiling + floor --------------------------------------------------------------------------
first_spot = []
for (room, sname), spots in sorted(room_spots.items(), key=lambda kv: (kv[0][1], kv[0][0])):
    base = room if room != 'stairwell' else 'stairwell-' + ('low' if sname == 'ground' else 'top')
    for si, (x, z, floor, vol, (dx, dz), extent, clr, marg, runlen) in enumerate(spots):
        label = base if si == 0 else '%s-spot%d' % (base, si + 1)
        yaw = yaw_of(dx, dz); feet = (x, floor, z)
        add('open', label, 'ceil', room, vol, feet, yaw, 60.0)
        add('open', label, 'floor', room, vol, feet, yaw, -60.0)
        bmin, bmax = blockers_for(floor)
        for ywd, (cx, cz), tag in CARD:
            # skip a wall that is under 0.4 m away (the camera would be nose to the plaster)
            px, pz = x + cx * 0.4, z + cz * 0.4
            if clearance(np.array([px]), np.array([pz]), bmin, bmax)[0] < 0.01:
                continue
            add('open', label, 'wall-' + tag, room, vol, feet, ywd, 0.0)
        if si == 0:
            first_spot.append((label, room, vol, feet, yaw))
        plan['rooms'].append({'room': label, 'storey': sname, 'spot': [round(x, 2), round(z, 2)], 'floor': floor, 'volume': vol['name'],
                              'clear_m': round(clr, 2), 'volume_margin_m': round(marg, 2), 'long_extent_m': round(extent, 1)})
# run B (doors shut as Play starts them): each room's first spot, ceiling + floor
for label, room, vol, feet, yaw in first_spot:
    add('shut', label, 'ceil', room, vol, feet, yaw, 60.0)
    add('shut', label, 'floor', room, vol, feet, yaw, -60.0)

# ---- doors: both sides, two distances, open (run A) and shut (run B) --------------------------------------------------
doors = []
for e in S.ents:
    if 'Openable' not in e or not S.active(e):
        continue
    b = S.box_collider(e)
    if b is None: continue
    lo, hi = b.aabb()
    nax = 0 if (hi[0] - lo[0]) < (hi[2] - lo[2]) else 2          # the thin axis = the door's normal
    c = [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2]
    floor = GROUND_FLOOR if lo[1] < 1.5 else UPPER_FLOOR
    doors.append({'id': e['id'], 'name': e.get('name', ''), 'ext': 'exterior' in e.get('name', ''), 'c': c, 'nax': nax, 'floor': floor, 'aabb': (lo, hi)})
door_pts = [(d['c'][0], d['c'][2], d['floor']) for d in doors]
for d in doors:
    n = (1, 0) if d['nax'] == 0 else (0, 1)
    sides = {}
    for sgn, sname in ((-1, 'a'), (1, 'b')):
        for dist, dn in ((1.5, 'near'), (3.0, 'far')):
            ok = None
            for dd in (dist, dist - 0.3, dist - 0.6, dist + 0.3, dist + 0.6, dist - 0.9):
                if dd < 0.7: continue
                px, pz = d['c'][0] + sgn * n[0] * dd, d['c'][2] + sgn * n[1] * dd
                if not free_at(px, pz, d['floor']): continue
                # 2026-11-24 (W6): an interior door's spot must be inside the house (door20762 a-far stood outside = black shot);
                # exterior doors keep their outside side (the out-door shots cover it on purpose).
                if not d['ext'] and volume_holding(px, d['floor'] + EYE, pz, vols_at_floor(d['floor']))[1] < 0.0: continue
                ok = (px, pz, dd); break
            if ok is None:
                plan['unresolved'].append({'door': d['id'], 'side': sname, 'dist': dn, 'why': 'no free spot at 0.7-3.6 m'}); continue
            px, pz, dd = ok
            vol, m = volume_holding(px, d['floor'] + EYE, pz, vols_at_floor(d['floor']))
            outside = m < 0.0
            yaw = yaw_of(-sgn * n[0], -sgn * n[1]); pitch = -math.degrees(math.atan2(EYE - 1.05, dd))
            for run in ('open', 'shut'):
                add(run, 'door%d' % d['id'], '%s-%s' % (sname, dn), d['name'], None if outside else vol, (px, d['floor'], pz), yaw, pitch,
                    'door %d %s side %s %.1f m (%s)' % (d['id'], 'EXTERIOR' if d['ext'] else 'interior', sname, dd, 'outside' if outside else vol['name']), outside)
            sides.setdefault(sname, []).append({'dist': round(dd, 2), 'outside': outside})
    plan['doors'].append({'id': d['id'], 'name': d['name'], 'at': [round(d['c'][0], 2), round(d['c'][2], 2)], 'normal': 'xz'[d['nax'] // 2], 'sides': sides})

# ---- outside: the exterior doors, porch ceiling, fittings --------------------------------------------------------------
lights = []     # exterior fixtures: Props_exterior lamps and any Light named Porch..., one per spot
for e in S.ents:
    nm = e.get('name', '')
    if not S.active(e) or not (('Props_exterior' in e.get('path', '') and 'amp' in e.get('path', '')) or ('Light' in e and 'orch' in nm)):
        continue
    p = S.position(e)
    if not (-20 < p[0] < 20 and 15 < p[2] < 40): continue
    if any(math.dist(p, q['pos']) < 0.4 for q in lights): continue
    lights.append({'name': nm, 'pos': p})
for d in [d for d in doors if d['ext']]:
    n = (1, 0) if d['nax'] == 0 else (0, 1)
    out_sgn = None
    for sgn in (-1, 1):
        px, pz = d['c'][0] + sgn * n[0] * 1.5, d['c'][2] + sgn * n[1] * 1.5
        if not in_volume(px, pz, d['floor']): out_sgn = sgn
    if out_sgn is None:
        plan['unresolved'].append({'door': d['id'], 'why': 'cannot tell the outside side'}); continue
    on = (out_sgn * n[0], out_sgn * n[1]); perp = (-on[1], on[0])
    key = 'out-door%d' % d['id']
    face = yaw_of(-on[0], -on[1])
    def place(col, dist, lat, yawoff, pitch, note):
        # try the pose, then pull in toward the door, until the spot is free (outside is not GI-covered)
        for t in (1.0, 0.85, 0.7, 0.55, 0.4):
            px = d['c'][0] + on[0] * dist * t + perp[0] * lat * t; pz = d['c'][2] + on[1] * dist * t + perp[1] * lat * t
            if free_at(px, pz, d['floor']):
                yaw = face + yawoff
                for run in ('open', 'shut'):
                    add(run, key, col, d['name'] + ' (outside)', None, (px, d['floor'], pz), yaw, pitch, note, True)
                plan['outside'].append({'id': key + '_' + col, 'at': [round(px, 2), round(pz, 2)], 'scale': round(t, 2)}); return
        plan['unresolved'].append({'outside': key + '_' + col, 'why': 'no free spot'})
    place('near', 2.5, 0.0, 0.0, 0.0, 'facade: the door')
    place('ceil', 1.4, 0.0, 0.0, 55.0, 'porch / lintel ceiling')
    place('left', 5.0, 2.0, -25.0, 5.0, 'oblique facade')
    place('right', 5.0, -2.0, 25.0, 5.0, 'oblique facade')
    place('far', 9.0, 0.0, 0.0, 3.0, 'whole facade')
    near_l = [l for l in lights if math.hypot(l['pos'][0] - d['c'][0], l['pos'][2] - d['c'][2]) < 6 and l['pos'][1] < d['floor'] + 3.6
              and (l['pos'][0] - d['c'][0]) * on[0] + (l['pos'][2] - d['c'][2]) * on[1] > -0.4]
    for li, l in enumerate(near_l):
        lx, ly, lz = l['pos']
        done = False
        for t in (3.0, 2.2, 1.6):
            px = lx + on[0] * t; pz = lz + on[1] * t
            if in_volume(px, pz, d['floor']) or not free_at(px, pz, d['floor']): continue
            yaw = yaw_of(lx - px, lz - pz); pitch = math.degrees(math.atan2(ly - (d['floor'] + EYE), t))
            for run in ('open', 'shut'):
                add(run, key, 'fitting%d' % (li + 1), l['name'], None, (px, d['floor'], pz), yaw, pitch, 'fixture %s at %.1f, %.1f, %.1f' % (l['name'], lx, ly, lz), True)
            plan['outside'].append({'id': key + '_fitting%d' % (li + 1), 'light': l['name'], 'at': [round(px, 2), round(pz, 2)]}); done = True; break
        if not done:
            plan['unresolved'].append({'outside_fitting': l['name'], 'door': d['id'], 'why': 'no free spot to view it from'})

# ---- stairs: Phase 0 tour's stairs points, looking along / back, up / down ---------------------------------------------
try:
    p0 = json.load(open('tools/gi/forest_house_tour.json', encoding='utf-8'))
    for w in p0['waypoints']:
        if w.get('kind') != 'stairs': continue
        fx, fy, fz = w['feet']; yaw = w['yawDeg']; key = 'stairs-' + w['id'].replace('stairs_', '')
        vol, _ = volume_holding(fx, fy + EYE, fz, vols_at_floor(GROUND_FLOOR) + vols_at_floor(UPPER_FLOOR))
        for col, yy, pp in (('along-up', yaw, 35.0), ('along-down', yaw, -35.0), ('back-up', yaw + 180, 35.0), ('back-down', yaw + 180, -35.0)):
            add('open', key, col, 'stairwell', vol, (fx, fy, fz), yy, pp, 'Phase 0 stairs point %s' % w['id'])
        plan['stairs'].append({'id': key, 'feet': w['feet']})
except Exception as ex:
    plan['unresolved'].append({'stairs': str(ex)})

# ---- thresholds (open archways / volume seams away from a door): look each way, run A only -------------------------------
pairs = G.adjacent_pairs(vols)
seen = set(); thr = []
for A, B, axis, gap in pairs:
    if axis == 1: continue
    (amin, amax), (bmin_, bmax_) = A['box'].aabb(), B['box'].aabb()
    o = 2 if axis == 0 else 0
    c = (max(amin[axis], bmin_[axis]) + min(amax[axis], bmax_[axis])) / 2.0
    lo_o, hi_o = max(amin[o], bmin_[o]), min(amax[o], bmax_[o])
    for sname, floor in STOREYS:
        if not (A in vols_at_floor(floor) and B in vols_at_floor(floor)): continue
        ra, rb = G.room_of(A['name']), G.room_of(B['name'])
        bmin, bmax = blockers_for(floor)
        ts = np.arange(lo_o + 0.05, hi_o - 0.049, 0.1)
        if not len(ts): continue
        P = np.stack([np.full(len(ts), c) if axis == 0 else ts, ts if axis == 0 else np.full(len(ts), c)], axis=1)
        clr = clearance(P[:, 0], P[:, 1], bmin, bmax)
        ok = np.where(clr >= 0.25)[0]
        if not len(ok):
            plan['unresolved'].append({'seam': '%s | %s' % (A['name'], B['name']), 'storey': sname, 'gap_m': round(gap, 2), 'why': 'wall-only seam: no walkable point'})
            continue
        mid = (lo_o + hi_o) / 2.0
        k = ok[np.argmin(np.abs(ts[ok] - mid) - 0.3 * np.minimum(clr[ok], 1.0))]
        px, pz = float(P[k, 0]), float(P[k, 1])
        if any(abs(px - dx_) < 1.3 and abs(pz - dz_) < 1.3 and fl == floor for dx_, dz_, fl in door_pts):
            continue                      # a door's own shots cover it
        key = (frozenset((ra, rb)), sname, round(px / 1.5), round(pz / 1.5), axis)
        if key in seen: continue
        seen.add(key); thr.append((A, B, axis, gap, sname, floor, px, pz, float(clr[k])))
for n_, (A, B, axis, gap, sname, floor, px, pz, clr) in enumerate(sorted(thr, key=lambda t: (t[4], G.room_of(t[0]['name']), t[6], t[7]))):
    ra, rb = G.room_of(A['name']), G.room_of(B['name'])
    (amin, amax) = A['box'].aabb()
    a_is_neg = (amin[axis] + amax[axis]) / 2 < (px if axis == 0 else pz)
    neg, pos = (A, B) if a_is_neg else (B, A)
    d = (1, 0) if axis == 0 else (0, 1)
    tag = 'thr%02d-%s' % (n_ + 1, slug(ra if ra == rb else ra + '__' + rb))
    vol, _ = volume_holding(px, floor + EYE, pz, vols_at_floor(floor))
    seam = 'seam' if gap > 0.05 else ('overlap' if gap < -0.05 else 'touch')
    note = '%s | %s, %s %.2f m, %s' % (neg['name'], pos['name'], seam, gap, sname)
    add('open', tag, 'look-neg', ra, vol, (px, floor, pz), yaw_of(-d[0], -d[1]), 0.0, note)
    add('open', tag, 'look-pos', ra, vol, (px, floor, pz), yaw_of(d[0], d[1]), 0.0, note)
    plan['thresholds'].append({'id': tag, 'volumes': [neg['name'], pos['name']], 'axis': 'xz'[axis // 2], 'gap_m': round(gap, 2), 'storey': sname,
                               'at': [round(px, 2), round(pz, 2)], 'clear_m': round(clr, 2)})

# ---- light-bounds faces (PM condition 2026-11-24): one shot per not-OK face of Atlas's bounds audit, looking across the face --
# Stand on the lit side of the plane (face value v on axis x or z), at the light's own lateral position, d m back from the plane,
# look toward the plane (and beyond) pitched down 20 deg, so a hard light edge shows on the floor and walls. Named after the light id.
if os.path.exists(a.bounds):
    lpos = {e['id']: S.position(e) for e in S.ents if 'Light' in e and S.active(e)}
    for r in json.load(open(a.bounds, encoding='utf-8')):
        if r['status'] == 'OK' or r['id'] not in lpos:
            continue
        lx, ly, lz = lpos[r['id']]
        ax = 0 if r['face'][1] == 'x' else 2
        sgn = 1 if r['face'][0] == '+' else -1
        floor = UPPER_FLOOR if ly > 3.0 else GROUND_FLOOR
        lat0 = lz if ax == 0 else lx
        done = False
        for d in (2.5, 2.0, 1.5, 3.0, 1.0, 3.5):
            for lat in (0.0, 0.6, -0.6, 1.2, -1.2, 2.0, -2.0):
                pa = r['value'] - sgn * d
                px, pz = (pa, lat0 + lat) if ax == 0 else (lat0 + lat, pa)
                if not (in_volume(px, pz, floor) and free_at(px, pz, floor, 0.3)):
                    continue
                dirx, dirz = (sgn, 0) if ax == 0 else (0, sgn)
                vol, _ = volume_holding(px, floor + EYE, pz, vols_at_floor(floor))
                add('open', 'bounds_%d' % r['id'], r['face'].replace('+', 'p').replace('-', 'n'), G.room_of(vol['name']) if vol else '', vol,
                    (px, floor, pz), yaw_of(dirx, dirz), -20.0,
                    'light %d %s: bounds face %s at %.3f (%s), %.1f m back, light at %.2f, %.2f, %.2f' % (r['id'], r['name'], r['face'], r['value'], r['status'], d, lx, ly, lz))
                plan.setdefault('bounds', []).append({'id': 'bounds-%d_%s' % (r['id'], r['face'].replace('+', 'p').replace('-', 'n')), 'light': r['id'], 'face': r['face'],
                                                      'value': r['value'], 'status': r['status'], 'at': [round(px, 2), round(pz, 2)], 'back_m': d})
                done = True; break
            if done: break
        if not done:
            plan['unresolved'].append({'bounds_light': r['id'], 'face': r['face'], 'value': r['value'], 'why': 'no free in-volume spot 1-3.5 m back from the face'})
else:
    print('no bounds audit at', a.bounds, '-> no bounds shots')

# ---- JACOB LOOK POSITIONS (SM 2026-11-24, hard shading lines): reconstructed from house-polish\jacob_look_fix2..6 ------------------
# All are new = NO VALID BASELINE. Ids 'jlook-<what>_<col>'. Exterior ones also run in the shut tour (doors shut = Play start state);
# interior ones in the open tour only. Each pose is checked free (collider clearance) and, inside, in a GI volume; else it is dropped to
# plan['unresolved'] rather than placed blind.
JL = []
def jl(runs, key, col, x, z, floor, yaw, pitch, note, outside=False, tries=(), scan=0.0):
    """place (x, z) or the first of `tries` (dx, dz) offsets that is free (and inside a volume for interior shots)."""
    _extra = []
    if scan > 0:   # fallback: every 0.2 m offset within `scan`, nearest first
        _n = int(round(scan / 0.2))
        _extra = sorted(((i * 0.2, j * 0.2) for i in range(-_n, _n + 1) for j in range(-_n, _n + 1) if i or j), key=lambda t: t[0] ** 2 + t[1] ** 2)
    for dx, dz in ((0, 0),) + tuple(tries) + tuple(_extra):
        px, pz = x + dx, z + dz
        if not free_at(px, pz, floor, 0.3): continue
        if not outside and not in_volume(px, pz, floor): continue
        vol = None if outside else volume_holding(px, floor + EYE, pz, vols_at_floor(floor))[0]
        for run in runs:
            add(run, key, col, 'jacob look' if outside else G.room_of(vol['name']), vol, (px, floor, pz), yaw, pitch, note, outside)
        JL.append(key + '_' + col); return True
    plan['unresolved'].append({'jlook': key + '_' + col, 'why': 'no free spot near (%.1f, %.1f)' % (x, z)}); return False

# 1. night exterior siding, all 4 facades (image 1: square patches on the siding), 8 m back, eye 1.6, pitch +12 to catch the 2nd storey
OFF = ((0, -1), (0, 1), (-1, 0), (1, 0), (0, -2), (0, 2), (-2, 0), (2, 0))
for tag, xs in (('S', (-9, 0, 9)), ('N', (-9, 0, 9))):
    for xx in xs:
        z = 12.0 if tag == 'S' else 40.0
        jl(('open', 'shut'), 'jlook-facade-%s' % tag, 'x%+d' % xx, xx, z, GROUND_FLOOR, 90.0 if tag == 'S' else -90.0, 12.0,
           'Jacob 1: %s facade siding at night, 8 m back' % tag, True, tuple((dx, dz * 1.0) for dx, dz in OFF))
for tag, zs in (('W', (23.0, 28.5)), ('E', (23.0, 28.5))):
    for zz in zs:
        x = -22.5 if tag == 'W' else 22.5
        jl(('open', 'shut'), 'jlook-facade-%s' % tag, 'z%d' % int(zz), x, zz, GROUND_FLOOR, 0.0 if tag == 'W' else 180.0, 12.0,
           'Jacob 1: %s facade siding at night, 8 m back' % tag, True, tuple((dx, dz) for dx, dz in OFF))
# facade corners (oblique, 45 deg)
for tag, (cx, cz, yw) in (('SW', (-21.0, 11.0, 45.0)), ('SE', (21.0, 11.0, 135.0)), ('NW', (-21.0, 41.0, -45.0)), ('NE', (21.0, 41.0, -135.0))):
    jl(('open', 'shut'), 'jlook-corner-%s' % tag, 'obl', cx, cz, GROUND_FLOOR, yw, 12.0, 'Jacob 1: oblique corner, siding at night', True,
       tuple((dx, dz) for dx, dz in OFF))
# the front porch as Jacob stood (image 1): 7 m out from the front door 20760 (x 1.0, z 24.04, outside = -z)
jl(('open', 'shut'), 'jlook-porch-front', 'wide', 1.0, 17.0, GROUND_FLOOR, 90.0, 8.0, 'Jacob 1: front porch + upper siding from 7 m (his look)', True, OFF)

# 2. red room (master bedroom, red bulbs 35598/35606/35608/35611) -> its doors, floor spill (image 3): from inside the red room through each
# east door (20758 z 26.47, 20759 z 29.37; wall x -6.76), and back from the far side, floor-ward pitch -25
for did, dz in ((20758, 26.47), (20759, 29.37)):
    jl(('open',), 'jlook-red-door%d' % did, 'in-near', -8.3, dz, UPPER_FLOOR, 0.0, -25.0, 'Jacob 3: red room floor spill through door %d, 1.5 m inside' % did, False, ((0, 0.3), (0, -0.3), (-0.4, 0), (0.3, 0.6)))
    jl(('open',), 'jlook-red-door%d' % did, 'in-far', -10.3, dz, UPPER_FLOOR, 0.0, -20.0, 'Jacob 3: red room floor spill through door %d, 3.5 m inside' % did, False, ((0, 0.4), (0, -0.4), (-0.4, 0)))
    jl(('open',), 'jlook-red-door%d' % did, 'out-near', -5.0, dz, UPPER_FLOOR, 180.0, -25.0, 'Jacob 3: the spill edge seen from the far side of door %d' % did, False, ((0, 0.3), (0, -0.3), (0.4, 0)))
# the upper hall seen from the red room's door line down the hall (image 3 camera looks along the hall)
jl(('open',), 'jlook-red-hall', 'along', -7.4, 27.0, UPPER_FLOOR, 0.0, -22.0, 'Jacob 3: from the master bedroom door down the upper hall', False, ((0, 0.5), (0, -0.5), (-0.5, 0)))

# 2b. Atlas pinned image 3 (atlas_seams/look_fix2 #3): NOT a doorway; the line is the master-bedroom lights' shared Bounds +z face at z 25.938
# (box-room partition line) across the open part of the room (x -11.1..-6.9, open to z 31.6). Look across it, and along it.
jl(('open',), 'jlook-red-zface', 'across', -9.0, 28.5, UPPER_FLOOR, -90.0, -30.0, 'Atlas #3: across the Bounds +z face z 25.938, from z 28.5 facing -z (CPU step at x -9.27: 99579b4e -40%, fix-3 5564577b none)', False, ((0, 0.3), (0, -0.3), (0.4, 0), (-0.4, 0)))
jl(('open',), 'jlook-red-zface', 'along', -7.2, 26.3, UPPER_FLOOR, 180.0, -25.0, 'Atlas #3: along the z 25.94 line from x -7.2 facing -x', False, ((0, 0.2), (0, 0.4), (-0.3, 0), (0, -0.2)))

# 3. upper-hall ceiling by the stairs, dome fixture (image 4): looking up at the dome fixtures at (3.66, 5.82, 25.26 / 26.66), (1.99, 5.82, 25.26 / 26.66)
for k, (px, pz, yw) in enumerate(((1.0, 26.2, 0.0), (5.3, 26.2, 180.0), (2.8, 26.0, 90.0))):
    jl(('open',), 'jlook-hall-dome', 'view%d' % (k + 1), px, pz, UPPER_FLOOR, yw, 48.0, 'Jacob 4: upper hall ceiling by the stairs, dome fixture', False, ((0.4, 0), (-0.4, 0), (0, 0.4), (0, -0.4)))

# 4. entrance-hall runner rug (Carpet_hallway 20889 at -1.81, 25.4) + stairs on the right (image 5/6): along the runner each way, floor-ward
jl(('open',), 'jlook-entrance-rug', 'along-e', -5.5, 25.4, GROUND_FLOOR, 0.0, -25.0, 'Jacob 6: runner rug + stairs, looking +x toward the living room', False, ((0, 0.4), (0, -0.4), (0.4, 0)))
jl(('open',), 'jlook-entrance-rug', 'along-w', 3.6, 25.4, GROUND_FLOOR, 180.0, -25.0, 'Jacob 6: runner rug, looking -x', False, ((0, 0.4), (0, -0.4), (-0.4, 0)))
jl(('open',), 'jlook-entrance-rug', 'stairs', -0.6, 25.4, GROUND_FLOOR, 90.0, -15.0, 'Jacob 6: runner rug + stairs side', False, ((0, -0.4), (0.4, 0)))

# 5. kitchen sink (Kitcabinet sink 20827 at -14.33, 29.38, west wall) to verify the probe recapture (image 4 = black sink basin)
jl(('open',), 'jlook-kitchen-sink', 'basin', -13.0, 29.38, GROUND_FLOOR, 180.0, -38.0, 'Jacob 4(b): kitchen sink basin, verify the probe recapture', False, ((0.3, 0), (0.6, 0), (0.9, 0), (0, 0.3), (0, -0.3)))
jl(('open',), 'jlook-kitchen-sink', 'wide', -11.5, 29.38, GROUND_FLOOR, 180.0, -12.0, 'Jacob 4(b): kitchen sink and counter wide', False, ((0.3, 0), (0.6, 0), (0, 0.4), (0, -0.4)))
# 5. Atlas look-sheet options (SM 2026-11-24). Atlas gave yaw 0 = +z, -180 = -z; THIS tour is yaw 0 = +x, 90 = +z, -90 = -z, so converted.
#    BEFORE = af9eb895 sweep; AFTER = same poses, option sha detached in wt-gpu on the SM's GO.
jl(('open',), 'jlook-tableB-line', 'floor', -12.8, 23.8, UPPER_FLOOR, 90.0, -20.0, 'Atlas: Lamp_table_B 35606/35608 +z edge z 26.07 along Wall_Interior_3x(4), x -14.4..-11.1, floor line + wall base. AFTER 6c4b9d89 (fam3-tableB-fade)', False, ((0, -0.3), (0.3, 0), (-0.3, 0), (0, 0.3)))
jl(('open',), 'jlook-tableB-line', 'ceil', -12.8, 23.8, UPPER_FLOOR, 90.0, 25.0, 'Atlas: same pose, ceiling line at z 26.07 (ceiling y 5.781). AFTER 6c4b9d89 (fam3-tableB-fade)', False, ((0, -0.3), (0.3, 0), (-0.3, 0), (0, 0.3)))
jl(('open',), 'jlook-south-wall-z20-3', 'view', -10.5, 23.0, GROUND_FLOOR, -90.0, -5.0, 'Atlas: Lamp_stand 19 (35613) -z edge z 20.3 = south window wall x -14.38..-6.78, floor 0.194 + ceiling 2.871 junctions + windows. AFTER 525e3a75 (fam4-south)', False, ((0, 0.3), (0.3, 0), (-0.3, 0), (0, -0.3)))
# 6. FOUND_ISSUES row 111 (SM 2026-11-24 PM): doorway beside light 35614 Lamp_floor 20 (8.874, 30.39), doorway z 27.93..28.88 in wall x 9.148..9.342,
#    upstairs. Is the 0.19 m threshold strip lit through the jamb? BEFORE af9eb895; AFTER dev/fix3-jambs 34ec488e (follow-up set). Door 20761 a/b shots already exist (baseline).
jl(('open',), 'jlook-jamb-35614', 'west', 7.4, 28.4, UPPER_FLOOR, 0.0, -40.0, 'row 111: threshold strip x 9.148..9.342 of the doorway beside 35614, from the lamp side (west). PASS / lit strip', False, ((-0.3, 0), (-0.6, 0), (0, 0.3), (0, -0.3)))
jl(('open',), 'jlook-jamb-35614', 'east', 11.1, 28.4, UPPER_FLOOR, 180.0, -40.0, 'row 111: same threshold strip from the far side (east)', False, ((0.3, 0), (0.6, 0), (0, 0.3), (0, -0.3)))
jl(('open',), 'jlook-jamb-35601', 'threshold', -5.50, 26.15, UPPER_FLOOR, 180.0, -52.0, 'row 111 (Atlas): 35601/36406 threshold strip x -6.857..-6.663 at z 25.99..~26.3 next to the jamb end z 25.99, opening z 25.99..26.94. Even light up to the jamb = PASS, jamb-shadow wedge = lit strip', False, ((0, 0.2), (0.2, 0), (0, -0.2), (-0.2, 0)))
jl(('open',), 'jlook-jamb-36400', 'threshold', -8.00, 29.05, UPPER_FLOOR, 0.0, -52.0, 'row 111 (Atlas): 36400 threshold strip x -6.857..-6.663 at z 28.90..~29.1 next to the jamb end z 28.90, opening z 28.90..29.85. Even light up to the jamb = PASS', False, ((0, 0.2), (-0.2, 0), (0, -0.2), (0.2, 0)))
# 7. SM PM add: night still of the main-facade siding east of the living-room bay, x 13.6..14.6, z 21.1..24.0 (outdoor pocket by the bay's east wall), from outside.
#    EYES_ON: "lit strip on the siding? (family E)". BEFORE = this sweep; AFTER = follow-up set (Atlas family E trim + fills).
jl(('open',), 'jlook-facade-bay-east', 'night', 16.5, 18.0, GROUND_FLOOR, 118.0, 3.0, 'SM PM: siding x 13.6..14.6, z 21.1..24.0 east of the living-room bay, from outside at night: lit strip on the siding? (family E)', True, ((0.5, 0), (-0.5, 0), (0, -0.5), (1.0, -0.5), (-1.0, -0.5), (0, -1.0), (1.0, 0), (-1.0, 0)))
# 8. SM PM T1 (sweep build 677c753d): three named eyes-on shots.
#    (1) Jacob #2 hall -> bathroom doorway WALL blotch (jacob_look_fix2\2.png: dark splotches on the wall beside the door-head, shadow-map edge of hall spots 35594/35596 at grazing incidence).
#        Camera FACES the hall +z wall around door 20762 (5.85, z 26.95), not looking into the bath.
jl(('open',), 'jlook-bathdoor-wall', 'face', 5.2, 24.5, GROUND_FLOOR, 90.0, 12.0, 'Jacob #2: faces the hall wall beside the bathroom doorway (door 20762), blotch/stair-step shadow edge from hall spots 35594/35596 (Shadow Softness 2.5)', False, ((0, -0.3), (0.3, 0), (-0.3, 0), (0, 0.3), (0, -0.6)), 1.6)
#    (2) point light grazing a wall near a cube-face corner: kitchen chandelier 35586 (-9.25, 2.06, 21.88), south wall z 20.3 is 1.58 m away -> cube seams on the wall at x -10.83 / -7.67 and y 0.48.
jl(('open',), 'jlook-chandelier-graze', 'face', -9.25, 24.2, GROUND_FLOOR, -90.0, 12.0, 'kitchen chandelier 35586 vs south wall z 20.3: look for vertical cube-face seams at x -10.83 / -7.67 and a horizontal one at y 0.48', False, ((0.3, 0), (-0.3, 0), (0, 0.3), (0, -0.3), (0.6, 0), (-0.6, 0)))
jl(('open',), 'jlook-chandelier-graze', 'oblique', -6.8, 22.4, GROUND_FLOOR, -160.0, 8.0, 'kitchen chandelier 35586: oblique along the south wall z 20.3, grazing incidence toward x -12', False, ((0.3, 0), (0, 0.3), (0.3, 0.3), (-0.3, 0), (0, -0.3)), 1.6)
#    (3) thin caster: the living-room chandelier 35592 (10.95, 2.06, 22.76) arms/chain (thin geometry round a shadow-casting point light), close-up of the ceiling and its shadow from 1.8 m.
jl(('open',), 'jlook-thin-caster', 'chandelier', 10.95, 24.6, GROUND_FLOOR, -90.0, 30.0, 'thin caster: living-room chandelier 35592 arms/chain, close-up of its shadow on the ceiling (y 2.87) from 1.8 m', False, ((0.3, 0), (-0.3, 0), (0, 0.3), (0, -0.3), (0.6, 0), (-0.6, 0)))
print('jacob-look shots placed:', len(JL))

# ---- fix-3 extras (SM 2026-11-24): Atlas dev/fix3-master-ceil 5802c9f0, master bedroom ceiling edge, no baseline exists --------
for lid in (35598, 35611):
    fx, fz = -9.27, 23.44
    vol, _ = volume_holding(fx, UPPER_FLOOR + EYE, fz, vols_at_floor(UPPER_FLOOR))
    add('open', 'bounds_%d' % lid, 'pz', G.room_of(vol['name']) if vol else 'master-bedroom', vol, (fx, UPPER_FLOOR, fz), 90.0, 30.0,
        'fix-3 master bedroom ceiling edge (light %d), W6 extra for dev/fix3-master-ceil 5802c9f0' % lid)
    plan.setdefault('bounds', []).append({'id': 'bounds-%d_pz' % lid, 'light': lid, 'face': '+z', 'status': 'fix3-extra', 'at': [fx, fz]})

# ---- fix-3 extra (SM 2026-11-24): den lamp 36409/36410 (dev/fix3-den-lamp b6ac15a8) on the den face of column 20599 (z 31.538) ----
_fx, _fz = 0.30, 29.40
_vol, _ = volume_holding(_fx, UPPER_FLOOR + EYE, _fz, vols_at_floor(UPPER_FLOOR))
add('open', 'den-lamp-column20599', 'view', G.room_of(_vol['name']) if _vol else 'upstairs den', _vol, (_fx, UPPER_FLOOR, _fz), 90.0, 5.0,
    'fix-3 den lamp Lamp_stand 36409 / light 36410: bright patch on column 20599 at bulb height (CPU probe 0.386, house norm 0.35-0.54); flag if hot')

# ---- Kronos walk-pop locations (SM 2026-11-24 PM): the COUNTED pops (full route 23, GI route 3) + the 2 fast ramps, by name -------------
#      source gi-p2\kronos\doors\fix-3\seam_pops_tour_{full,gi}_route.md + routes_only.json (his walk waypoints). Frame index within a segment is
#      not mapped to a position (his walk is interpolated), so each segment gets its two end poses + a midpoint pose. Open run only.
_KR = r'D:\Jacob\tblocks\descent\gi-p2\kronos\doors\fix-3'
_KW = r'D:\Jacob\tblocks\descent\gi-p1\argus\routes_only.json'
def _kslug(x): return x.replace('_', '-')
if os.path.exists(_KW) and os.path.isdir(_KR):
    _kw = {w['id']: w for w in json.load(open(_KW))['waypoints']}
    _segs = collections.OrderedDict()      # (a, b) -> {'tags': set}
    def _addseg(a_, b_, tag): _segs.setdefault((a_, b_), set()).add(tag)
    for _view, _fn in (('full', 'seam_pops_tour_full_route.md'), ('gi', 'seam_pops_tour_gi_route.md')):
        for _ln in open(os.path.join(_KR, _fn), encoding='utf-8-sig'):
            _m = re.match(r'^\| route:[^:]+:(\w+)->(\w+) \| (\d+) \| \S+ \| (\d+) \|', _ln)
            if _m and int(_m.group(4)) > 0: _addseg(_m.group(1), _m.group(2), 'pop-%s:%s' % (_view, _m.group(4)))
    _addseg('hall_center', 'hall_west_corner', 'ramp R1 -74%'); _addseg('kitchen_door_before', 'hall_center', 'ramp R2 +65%')
    _wps_done = set(); _nseg = 0
    def _wp_shot(wid, why):
        if wid in _wps_done or wid not in _kw: return
        _wps_done.add(wid); w = _kw[wid]; fx, fy, fz = w['feet']
        v, _d = volume_holding(fx, fy + EYE, fz, vols_at_floor(fy))
        if v is not None and _d < 0.0: v = None
        add('open', 'walkwp-' + _kslug(wid), 'pose', G.room_of(v['name']) if v else (w.get('room') or 'walk'), v, (fx, fy, fz), float(w['yawDeg']), float(w['pitchDeg']), 'Kronos walk waypoint ' + wid + ' (' + why + ')', v is None)
    def _dyaw(y0, y1):
        d = (y1 - y0 + 180.0) % 360.0 - 180.0
        return d
    for (_a, _b), _tags in _segs.items():
        if _a not in _kw or _b not in _kw: continue
        _why = '; '.join(sorted(_tags)); _wa, _wb = _kw[_a], _kw[_b]
        _wp_shot(_a, 'start of %s->%s: %s' % (_a, _b, _why)); _wp_shot(_b, 'end of %s->%s: %s' % (_a, _b, _why))
        _mx = [(_wa['feet'][i] + _wb['feet'][i]) / 2.0 for i in range(3)]
        _my = _wa['yawDeg'] + _dyaw(_wa['yawDeg'], _wb['yawDeg']) / 2.0
        _v, _dd = volume_holding(_mx[0], _mx[1] + EYE, _mx[2], vols_at_floor(_mx[1]))
        if _v is not None and _dd < 0.0: _v = None
        add('open', 'walkpop-%s-to-%s' % (_kslug(_a), _kslug(_b)), 'mid', G.room_of(_v['name']) if _v else (_wa.get('room') or 'walk'), _v, tuple(_mx), _my, 0.0, 'Kronos walk segment midpoint ' + _a + '->' + _b + ': ' + _why, _v is None)
        _nseg += 1
    plan['walkpops'] = [{'segment': '%s->%s' % k, 'why': sorted(v)} for k, v in _segs.items()]
    print('walk-pop segments', _nseg, 'waypoint shots', len(_wps_done))

# ---- write the two tours --------------------------------------------------------------------------------------------------
os.makedirs(a.out, exist_ok=True)
for run, ds, name in (('open', 'allOpen', 'sweep_tour_open.json'), ('shut', 'authored', 'sweep_tour_shut.json')):
    wps = RUNS[run]
    wps[0].pop('cut', None)
    tour = {'version': 1, 'scene': 'scenes/Forest.json', 'stillFrames': 0,
            'player': {'eyeHeight': EYE, 'horizontalFovDeg': 90.0, 'verticalFovDeg': 58.72, 'walkSpeed': 4.0,
                       'source': 'sweep v2: eye 1.6 m; Forest.json Player Spawn FOV'},
            'doorState': ds, 'waypoints': wps}
    json.dump(tour, open(os.path.join(a.out, name), 'w'), indent=1)
json.dump(plan, open(os.path.join(a.out, 'sweep_plan.json'), 'w'), indent=1)

# ---- dry check (CPU only) -----------------------------------------------------------------------------------------------
fails = []
for run in RUNS:
    ids = [w['id'] for w in RUNS[run]]
    if len(set(ids)) != len(ids): fails.append('%s: duplicate ids' % run)
    for w in RUNS[run]:
        x, y, z = w['feet']; eye = [x, y + EYE, z]
        bmin, bmax = blockers_for(y)
        clr = float(clearance(np.array([x]), np.array([z]), bmin, bmax)[0])
        holder = [v for v in vols if v['box'].contains(eye)]
        top = max((v['box'].aabb()[1][1] for v in holder), default=None)
        why = []
        if clr < 0.24: why.append('clearance %.2f m' % clr)
        if not holder and not w.get('outside'): why.append('eye in no GI volume')
        if holder and w.get('outside'): why.append('flagged outside but inside a GI volume')
        if top is not None and top - eye[1] < CEIL_GAP: why.append('only %.2f m under the volume top' % (top - eye[1]))
        if any(not math.isfinite(t) for t in (*w['feet'], w['yawDeg'], w['pitchDeg'])): why.append('non-finite')
        if why: fails.append('%s %s: %s' % (run, w['id'], '; '.join(why)))
nA, nB = len(RUNS['open']), len(RUNS['shut'])
nout = sum(1 for w in RUNS['open'] if w.get('outside'))
# closets / small rooms and the doors that reach them
small = []
for room, vl in sorted(by_room.items()):
    boxes = [v['box'].aabb() for v in vl]
    lo = np.min([b[0] for b in boxes], axis=0); hi = np.max([b[1] for b in boxes], axis=0)
    area = float((hi[0] - lo[0]) * (hi[2] - lo[2]))
    if area <= 16.0 or any(k in room for k in ('closet', 'box', 'storeroom')):
        near = [d['id'] for d in doors if any(
            (b[0][0] - 0.8 <= d['c'][0] <= b[1][0] + 0.8) and (b[0][2] - 0.8 <= d['c'][2] <= b[1][2] + 0.8) and (b[0][1] - 0.3 <= d['c'][1] <= b[1][1] + 0.3) for b in boxes)]
        small.append((room, round(area, 1), near, [r['room'] for r in plan['rooms'] if r['room'].startswith(room)]))
est_open = 30 + 1.9 * nA; est_shut = 30 + 1.9 * nB      # stillFrames 0: 152 frames/shot at ~12.4 ms
est_open_still = 30 + 3.4 * nA; est_shut_still = 30 + 3.4 * nB
md = ['# Sweep v2 dry check (CPU only): every surface', '',
      'Scene `%s`; eye %.1f m. Two tours: **open** (`doorState allOpen`: rooms, thresholds, doors open, outside, stairs) and **shut** (`authored` = every door shut as Play starts them: doors, outside, each room first spot ceiling + floor).' % (a.scene, EYE), '',
      '* Run A: **%d** shots (%d outside cameras); run B: **%d** shots; total **%d** (each gives a _full and a _gi PNG)' % (nA, nout, nB, nA + nB),
      '* GPU [E, per-shot rate extrapolated from a measured 12.4 ms/frame]: with `stillFrames 0` (needs the dev/tour-stillframes PR) run A ~%.0f min + run B ~%.0f min = **~%.0f min**; without it (272 frames/shot) %.0f + %.0f = **~%.0f min**' % (
          est_open / 60, est_shut / 60, (est_open + est_shut) / 60, est_open_still / 60, est_shut_still / 60, (est_open_still + est_shut_still) / 60),
      '* Cameras with a problem: **%d**' % len(fails), '',
      '* Outside cameras (not inside a GI volume by design, flagged `outside:true`): %d; their floor is assumed = the door sill (%.3f), not a terrain sample.' % (nout, GROUND_FLOOR), '']
if fails: md += ['## Problems'] + ['* ' + f for f in fails[:60]] + ['']
if plan['unresolved']:
    md += ['## Unresolved / skipped', ''] + ['* ' + json.dumps(u) for u in plan['unresolved']] + ['']
md += ['## Closets / small rooms (<=16 m2 footprint or named closet/box/storeroom) and the doors reaching them', '', '| room | area m2 | door ids | spots |', '|---|---|---|---|']
for room, area, near, spots in small:
    via = [t['id'] for t in plan['thresholds'] if any(G.room_of(v) == room for v in t['volumes'])]
    md.append('| %s | %s | %s | %s |' % (room, area, ', '.join(map(str, near)) or ('no door: open archway, seen via thresholds ' + (', '.join(via) or '**NONE: CHECK**')), ', '.join(spots)))
md += ['', '## Doors (each: both sides x near 1.5 m / far 3 m, open + shut)', '', '| id | name | at (x, z) | normal | sides |', '|---|---|---|---|---|']
for d in plan['doors']:
    md.append('| %d | %s | %.2f, %.2f | %s | %s |' % (d['id'], d['name'], d['at'][0], d['at'][1], d['normal'], json.dumps(d['sides'])))
md += ['', '## Room spots', '', '| room | storey | spot (x, z) | volume | clear m | long m |', '|---|---|---|---|---|---|']
for r in plan['rooms']:
    md.append('| %s | %s | %.2f, %.2f | %s | %.2f | %.1f |' % (r['room'], r['storey'], r['spot'][0], r['spot'][1], r['volume'], r['clear_m'], r['long_extent_m']))
md += ['', '## Thresholds (away from doors)', '', '| id | volumes | axis | gap m | storey | at | clear m |', '|---|---|---|---|---|---|---|']
for t in plan['thresholds']:
    md.append('| %s | %s / %s | %s | %.2f | %s | %.2f, %.2f | %.2f |' % (t['id'], t['volumes'][0], t['volumes'][1], t['axis'], t['gap_m'], t['storey'], t['at'][0], t['at'][1], t['clear_m']))
open(os.path.join(a.out, 'SWEEP_DRYCHECK.md'), 'w').write('\n'.join(md) + '\n')
print('run A', nA, 'run B', nB, 'outside', nout, 'problems', len(fails), 'unresolved', len(plan['unresolved']), 'GPU min [E] (stillFrames 0)', round((est_open + est_shut) / 60), 'without', round((est_open_still + est_shut_still) / 60))
for f in fails[:40]: print(' !', f)
