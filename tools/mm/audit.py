r"""The full-pack motion matching audit: what the mocap pack holds, measured, before choosing what to bake.

    set TARTARUS_MM_SOURCE=D:\...\Manny_Mocap_Export
    python tools/mm/gen_takes.py --audit
    build\Release\TartarusEngine.exe --bake-motion tools/mm/audit.mmconfig
    python tools/mm/audit.py [--dir work/mm_audit] [--md docs/mm_audit.md]

Reads the audit bake (work/mm_audit/MannyAudit.mmanim / .mmdb, the formats in MotionClipStream.h and MotionFeatures.h),
its takes list and the bake report (per-take finger motion), and writes docs/mm_audit.md (the numbers, for people) and
work/mm_audit/audit.json (every segment and event, for gen_takes' curation).

Measured per take (style = what the actor was asked for: the take's group - stroll / walk / jog / run / crouch / stand;
mixed takes - transitions, acceleration - are 'mixed'):
- steady segments: moving (> 0.5 m/s), speed steady (|accel| < 0.5 m/s^2), heading and facing steady (< 20 deg/s), one travel
  direction off the facing (8 sectors), at least 0.5 s - their speed, length and steps (contact touchdowns);
- starts and stops (from / to under 0.3 m/s), with the travel direction off the facing;
- turns on the move (the travel heading turning faster than 45 deg/s while over 0.5 m/s), the angle turned, left / right;
- capture glitches (a bone turning over 35 deg in a frame): spikes that come back within 6 frames (repairable by
  interpolation), twist-bone flips, and jumps;
- airborne frames outside the jump takes (detector false positives);
- finger animation (the bake report);
- redundancy: each steady 0.5 s window's distance to the nearest window of another take, same style, direction and speed
  band, in the normalized feature space.
Speeds, headings and directions are judged on the velocity averaged over a stride (1 s): the root keeps the stride's sway.
"""
import argparse
import csv
import json
import math
import struct
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
FPS = 60.0
SQRT_HALF = math.sqrt(0.5)
DIRS = ['fwd', 'fwd-left', 'left', 'back-left', 'back', 'back-right', 'right', 'fwd-right']  # left = the rig's +X side
GLITCH_DEG = 35.0
STYLES = ['stroll', 'walk', 'jog', 'run', 'crouch', 'stand', 'mixed']


# --- readers -----------------------------------------------------------------------------------------------------------

class Reader:
    def __init__(self, path):
        self.b = Path(path).read_bytes()
        self.o = 0

    def get(self, fmt):
        v = struct.unpack_from('<' + fmt, self.b, self.o)
        self.o += struct.calcsize('<' + fmt)
        return v if len(v) > 1 else v[0]

    def str(self):
        n = self.get('H')
        s = self.b[self.o:self.o + n].decode('utf-8', 'replace')
        self.o += n
        return s

    def array(self, dtype, count):
        a = np.frombuffer(self.b, dtype=dtype, count=count, offset=self.o)
        self.o += a.nbytes
        return a


def read_mmanim(path):
    r = Reader(path)
    assert r.b[:4] == b'TMMA', path
    r.o = 4
    version, fps, frames, bones, statics, clips = r.get('IfIIII')
    r.get('ff')  # source pelvis height, rest yaw
    names = [r.str() for _ in range(bones)]
    for _ in range(statics):
        r.str()
        r.get('ffff')
    clip = []
    for _ in range(clips):
        c = dict(name=r.str(), tags=r.str(), first=r.get('I'), count=r.get('I'), start=r.get('f'))
        if version >= 2:
            c['fingerFirst'] = r.get('I')  # 0xFFFFFFFF: no finger track
        clip.append(c)
    root = r.array('<f4', frames * 3).reshape(frames, 3)
    pelvis = r.array('<f4', frames * 3).reshape(frames, 3)
    rot = r.array('<u2', frames * bones * 3).reshape(frames, bones, 3)
    return dict(fps=fps, bones=names, clips=clip, root=root, pelvis=pelvis, rot=rot)


def read_mmdb(path):
    r = Reader(path)
    assert r.b[:4] == b'TMMD', path
    r.o = 4
    version, frames, dims, clips = r.get('IIII')
    r.get('III')
    mean, std = np.zeros(dims, np.float32), np.ones(dims, np.float32)
    qs, qo = np.ones(dims, np.float32), np.zeros(dims, np.float32)
    for d in range(dims):
        r.str()
        mean[d], std[d] = r.get('ff')
        if version >= 2:
            qs[d], qo[d] = r.get('ff')
    for _ in range(clips):
        r.str()
        r.get('II')
    if version >= 2:  # 16-bit features, x = q * qScale + qOffset (MotionFeatures.h), and a style per frame
        feat = r.array('<i2', frames * dims).reshape(frames, dims).astype(np.float32) * qs + qo
    else:
        feat = r.array('<f4', frames * dims).reshape(frames, dims)
    gait = r.array('u1', frames)
    flags = r.array('u1', frames)
    style = r.array('u1', frames) if version >= 2 else None
    return dict(feat=feat, gait=gait, flags=flags, style=style, mean=mean, std=np.maximum(std, 1e-6))


def unpack_quats(p):
    """MotionMath::UnpackQuat, vectorized: (..., 3) u16 -> (..., 4) x y z w."""
    w0, w1, w2 = p[..., 0].astype(np.int64), p[..., 1].astype(np.int64), p[..., 2].astype(np.int64)
    big = (w0 & 1) | ((w1 & 1) << 1)
    r0 = (w0 >> 1) / 32767.0 * (2 * SQRT_HALF) - SQRT_HALF
    r1 = (w1 >> 1) / 32767.0 * (2 * SQRT_HALF) - SQRT_HALF
    r2 = w2 / 65535.0 * (2 * SQRT_HALF) - SQRT_HALF
    rb = np.sqrt(np.maximum(0.0, 1.0 - r0 * r0 - r1 * r1 - r2 * r2))
    q = np.zeros(p.shape[:-1] + (4,))
    rest = [r0, r1, r2]
    for b in range(4):
        m = big == b
        k = 0
        for i in range(4):
            if i == b:
                q[..., i][m] = rb[m]
            else:
                q[..., i][m] = rest[k][m]
                k += 1
    return q


def quat_angle_deg(a, b):
    d = np.abs(np.sum(a * b, axis=-1)).clip(0.0, 1.0)
    return np.degrees(2.0 * np.arccos(d))


# --- per take ----------------------------------------------------------------------------------------------------------

def style_of(group):
    g = group.lower()
    if g in ('stroll', 'walk', 'jog', 'run', 'crouch', 'stand'):
        return g
    return 'mixed'


def wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def smooth(x, frames):
    if frames <= 1 or len(x) < frames:
        return x
    k = np.ones(frames) / frames
    pad = frames // 2
    xp = np.pad(x, (pad, frames - 1 - pad), mode='edge')
    return np.convolve(xp, k, mode='valid')


def runs(mask):
    """[start, end) runs of True."""
    out, start = [], None
    for i, m in enumerate(mask):
        if m and start is None:
            start = i
        elif not m and start is not None:
            out.append((start, i))
            start = None
    if start is not None:
        out.append((start, len(mask)))
    return out


def analyse_take(c, anim, db, quats, bone_names):
    a, n = c['first'], c['count']
    root = anim['root'][a:a + n].astype(np.float64)
    x, z, yaw = root[:, 0], root[:, 1], np.unwrap(root[:, 2])
    # The generated root keeps most of the stride's sway (its position is the pelvis, smoothed over 0.1 s), so its
    # instantaneous speed and heading swing every step: everything below is judged on the velocity averaged over a stride
    # (1 s), not frame by frame.
    vx, vz = smooth(np.gradient(x) * FPS, int(FPS)), smooth(np.gradient(z) * FPS, int(FPS))
    speed = np.hypot(vx, vz)
    accel = smooth(np.gradient(speed) * FPS, 15)
    yawrate = np.degrees(smooth(np.gradient(yaw), int(FPS)) * FPS)
    heading = np.unwrap(np.arctan2(vx, vz))
    headrate = np.degrees(np.gradient(heading) * FPS)
    travel = np.array([wrap(h - y) for h, y in zip(np.arctan2(vx, vz), yaw)])
    sector = ((np.round(travel / (math.pi / 4)).astype(int)) % 8)
    flags = db['flags'][a:a + n]
    gait = db['gait'][a:a + n]
    contact = [(flags & 32) != 0, (flags & 64) != 0]
    touch = np.zeros(n, bool)
    for cl in contact:
        touch[1:] |= cl[1:] & ~cl[:-1]

    out = dict(frames=int(n), seconds=n / FPS)
    # Steady segments.
    steady = (speed > 0.5) & (np.abs(accel) < 0.5) & (np.abs(headrate) < 20.0) & (np.abs(yawrate) < 20.0)
    segs = []
    for s, e in runs(steady):
        # split where the travel sector changes
        cut = s
        for i in range(s + 1, e + 1):
            if i == e or sector[i] != sector[cut]:
                if i - cut >= 0.5 * FPS:
                    segs.append(dict(start=int(cut), end=int(i), dir=DIRS[int(sector[cut])], speed=float(speed[cut:i].mean()),
                                     seconds=(i - cut) / FPS, steps=int(touch[cut:i].sum()),
                                     crouch=bool(((flags[cut:i] & 1) != 0).mean() > 0.5)))
                cut = i
    out['segments'] = segs
    # Starts and stops.
    moving = speed > 0.3
    starts, stops = [], []
    for s, e in runs(moving):
        if s > 0 and e - s >= 0.5 * FPS:
            k = min(e - 1, s + int(0.6 * FPS))
            starts.append(dict(frame=int(s), dir=DIRS[int(sector[k])], speed=float(speed[k])))
        if e < n and e - s >= 0.5 * FPS:
            k = max(s, e - int(0.4 * FPS))
            stops.append(dict(frame=int(e), dir=DIRS[int(sector[k])], speed=float(speed[k])))
    out['starts'], out['stops'] = starts, stops
    # Turns on the move.
    turning = (np.abs(headrate) > 45.0) & (speed > 0.5)
    turns = []
    for s, e in runs(turning):
        ang = math.degrees(heading[min(e, n - 1)] - heading[s])
        if abs(ang) >= 30.0:
            turns.append(dict(frame=int(s), angle=float(ang), side='left' if ang > 0 else 'right',
                              speed=float(speed[s:e].mean())))
    out['turns'] = turns
    # Glitches.
    q = quats[a:a + n]
    step = quat_angle_deg(q[1:], q[:-1])  # (n-1, bones)
    events = []
    bad = step > GLITCH_DEG
    for b in range(step.shape[1]):
        for s, e in runs(bad[:, b]):
            before, after = s, min(n - 1, e + 1)
            back = float(quat_angle_deg(q[before, b], q[after, b])) if after - before <= 7 else 999.0
            name = bone_names[b]
            kind = 'spike' if back < 10.0 else ('twist-flip' if 'twist' in name and step[s:e, b].max() > 120.0 else 'jump')
            events.append(dict(frame=int(s), bone=name, maxDeg=float(step[s:e, b].max()), frames=int(e - s), kind=kind))
    out['glitches'] = events
    out['airborne'] = int(((flags & 4) != 0).sum())
    # The longest stretch at 3.5 m/s or more (the run gate's context).
    fast = dict(seconds=0.0, steps=0, start=0.0, peak=0.0, end=0.0)
    for s, e in runs(speed >= 3.5):
        if (e - s) / FPS > fast['seconds']:
            fast = dict(seconds=(e - s) / FPS, steps=int(touch[s:e].sum()), start=float(speed[s]), peak=float(speed[s:e].max()),
                        end=float(speed[e - 1]))
    out['fast'] = fast
    out['speedGait'] = [int((gait == g).sum()) for g in range(4)]
    out['_speed'] = speed
    out['_sector'] = sector
    out['_steady'] = steady
    return out


# --- redundancy --------------------------------------------------------------------------------------------------------

def redundancy(takes, db):
    """Each steady 0.5 s window's normalized-feature distance to the nearest window of another take in the same
    (style, direction, 0.25 m/s band)."""
    feat = (db['feat'] - db['mean']) / db['std']
    groups = defaultdict(list)  # key -> [(take index, global frame)]
    for ti, t in enumerate(takes):
        for s in t['segments']:
            band = round(s['speed'] / 0.25) * 0.25
            for f in range(s['start'], s['end'], int(0.5 * FPS)):
                groups[(t['style'], s['dir'], band)].append((ti, t['first'] + f))
    per_take = defaultdict(list)
    for key, items in groups.items():
        if len(items) < 2:
            for ti, _ in items:
                per_take[ti].append(float('inf'))
            continue
        tis = np.array([i[0] for i in items])
        X = feat[np.array([i[1] for i in items])].astype(np.float64)
        for i in range(len(items)):
            d = np.sqrt(((X - X[i]) ** 2).sum(axis=1))
            d[tis == tis[i]] = np.inf
            per_take[tis[i]].append(float(d.min()))
    return per_take


# --- report ------------------------------------------------------------------------------------------------------------

def pct(v, p):
    return float(np.percentile(v, p)) if len(v) else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dir', default=str(ROOT / 'work' / 'mm_audit'))
    ap.add_argument('--md', default=str(ROOT / 'docs' / 'mm_audit.md'))
    a = ap.parse_args()
    d = Path(a.dir)
    anim = read_mmanim(d / 'MannyAudit.mmanim')
    db = read_mmdb(d / 'MannyAudit.mmdb')
    rows = {r['name']: r for r in csv.DictReader(l for l in (d / 'takes_audit.csv').open(encoding='utf-8') if not l.startswith('#'))}
    report = json.loads((ROOT / 'work' / 'mm_bake' / 'MannyAudit.report.json').read_text(encoding='utf-8'))
    fingers = {t['name']: t for t in report['takes'] if 'fingerBones' in t}
    print(f"audit: {len(anim['clips'])} takes, {len(db['gait'])} frames - unpacking rotations")
    quats = unpack_quats(anim['rot'])

    takes = []
    for c in anim['clips']:
        row = rows.get(c['name'], {})
        tags = c['tags'].split(';')
        t = analyse_take(c, anim, db, quats, anim['bones'])
        t.update(name=c['name'], first=c['first'], tags=tags, group=row.get('group', ''), exaggerated='exaggerated' in tags,
                 style=style_of(row.get('group', '')), baked=False)
        fr = fingers.get(c['name'], {})
        t['fingerBonesMoving'], t['fingerRangeDeg'] = fr.get('fingerBonesMoving', 0), fr.get('fingerRangeDeg', 0.0)
        takes.append(t)
    current = {r['name'] for r in csv.DictReader(l for l in (ROOT / 'tools' / 'mm' / 'takes.csv').open(encoding='utf-8')
                                                   if not l.startswith('#')) if r['include'] == '1'}
    for t in takes:
        t['baked'] = t['name'] in current
    print('audit: redundancy')
    red = redundancy(takes, db)
    for ti, t in enumerate(takes):
        v = [x for x in red.get(ti, []) if math.isfinite(x)]
        t['nnDist'] = v
        t['uniqueWindows'] = sum(1 for x in red.get(ti, []) if not math.isfinite(x))

    md = []
    w = md.append
    w('# Motion matching library audit')
    w('')
    w('Generated by `tools/mm/audit.py` from the full-pack audit bake (every take, exaggerated included, the same 1 s trims, '
      'no budgets). Numbers, not opinions; `work/mm_audit/audit.json` holds every segment and event.')
    w('')
    tot = sum(t['seconds'] for t in takes)
    w(f"**{len(takes)} takes, {tot / 3600:.2f} h after trims.** Baked today (tools/mm/takes.csv): "
      f"{sum(1 for t in takes if t['baked'])} takes (budget-cut, so less than their length).")
    w('')

    # Steady speeds by style and direction.
    w('## Steady locomotion by style and direction')
    w('')
    w('Steady = moving, |accel| < 0.5 m/s^2, heading < 20 deg/s, one direction, >= 0.5 s. Speeds are the segments\' means '
      '(p25 / p50 / p75, m/s); "longest" is the longest steady segment (s, steps).')
    w('')
    w('| style | direction | segments | seconds | speed p25 / p50 / p75 | longest |')
    w('|---|---|---|---|---|---|')
    for style in STYLES:
        for ex in (False, True):
            for di in DIRS:
                segs = [(s, t) for t in takes if t['style'] == style and t['exaggerated'] == ex for s in t['segments'] if s['dir'] == di]
                if not segs:
                    continue
                sp = [s['speed'] for s, _ in segs]
                lo = max(segs, key=lambda st: st[0]['seconds'])
                w(f"| {style}{' (_S)' if ex else ''} | {di} | {len(segs)} | {sum(s['seconds'] for s, _ in segs):.1f} | "
                  f"{pct(sp, 25):.2f} / {pct(sp, 50):.2f} / {pct(sp, 75):.2f} | {lo[0]['seconds']:.1f} s, {lo[0]['steps']} steps ({lo[1]['name']}) |")
    w('')

    # The run gate.
    w('## The run gate: steady running')
    w('')
    w('Gate (plan, Step 0): at least 6 consecutive steady strides (12 steps) at 3.5 m/s or more, forward. Every steady '
      'segment over 3.0 m/s:')
    w('')
    fast = sorted(((s, t) for t in takes for s in t['segments'] if s['speed'] >= 3.0), key=lambda st: -st[0]['steps'])
    w('| take | baked | dir | speed | seconds | steps |')
    w('|---|---|---|---|---|---|')
    for s, t in fast[:40]:
        w(f"| {t['name']} | {'yes' if t['baked'] else 'no'} | {s['dir']} | {s['speed']:.2f} | {s['seconds']:.2f} | {s['steps']} |")
    best = max((s['steps'] for s, t in fast if s['speed'] >= 3.5 and s['dir'] == 'fwd'), default=0)
    w('')
    w(f"**Verdict:** the longest steady forward segment at >= 3.5 m/s has {best} steps - the gate "
      f"{'PASSES' if best >= 12 else 'FAILS'} ({best} of 12).")
    w('')
    w('Steady or not, the longest time each take spends at 3.5 m/s or more (stride-averaged speed), with the steps in it '
      'and its speed at the stretch\'s start, peak and end - a cruise holds its speed; a dash rises to its peak and brakes:')
    w('')
    w('| take | baked | seconds >= 3.5 m/s | steps | start / peak / end m/s |')
    w('|---|---|---|---|---|')
    for t in sorted(takes, key=lambda t: -t['fast']['seconds'])[:15]:
        f = t['fast']
        if f['seconds'] <= 0.0:
            break
        w(f"| {t['name']} | {'yes' if t['baked'] else 'no'} | {f['seconds']:.2f} | {f['steps']} | "
          f"{f['start']:.2f} / {f['peak']:.2f} / {f['end']:.2f} |")
    w('')

    # Coverage matrix: starts, stops, turns, left / right.
    w('## Coverage: starts, stops and turns on the move')
    w('')
    w('| style | starts by direction | stops by direction | turns left (30-60 / 60-120 / 120-180+) | turns right |')
    w('|---|---|---|---|---|')
    for style in STYLES:
        ts = [t for t in takes if t['style'] == style and not t['exaggerated']]
        if not ts:
            continue
        st = defaultdict(int)
        sp = defaultdict(int)
        for t in ts:
            for e in t['starts']:
                st[e['dir']] += 1
            for e in t['stops']:
                sp[e['dir']] += 1
        tl, tr = [0, 0, 0], [0, 0, 0]
        for t in ts:
            for e in t['turns']:
                b = 0 if abs(e['angle']) < 60 else 1 if abs(e['angle']) < 120 else 2
                (tl if e['side'] == 'left' else tr)[b] += 1
        fmt = lambda dct: ', '.join(f"{k} {dct[k]}" for k in DIRS if dct[k])
        w(f"| {style} | {fmt(st)} | {fmt(sp)} | {tl[0]} / {tl[1]} / {tl[2]} | {tr[0]} / {tr[1]} / {tr[2]} |")
    w('')
    w('### Left / right balance (steady seconds; the mirroring decision)')
    w('')
    w('| style | left side (fwd-left + left + back-left) | right side | ratio |')
    w('|---|---|---|---|')
    mirror = []
    for style in STYLES:
        L = sum(s['seconds'] for t in takes if t['style'] == style and not t['exaggerated'] for s in t['segments']
                if s['dir'] in ('fwd-left', 'left', 'back-left'))
        R = sum(s['seconds'] for t in takes if t['style'] == style and not t['exaggerated'] for s in t['segments']
                if s['dir'] in ('fwd-right', 'right', 'back-right'))
        if L + R < 1.0:
            continue
        ratio = max(L, R) / max(min(L, R), 0.1)
        if ratio > 1.25:
            mirror.append(style)
        w(f"| {style} | {L:.1f} | {R:.1f} | {ratio:.2f}{' - mirror' if ratio > 1.25 else ''} |")
    w('')
    w(f"**Mirror (left/right more than 1.25 apart):** {', '.join(mirror) if mirror else 'none'}.")
    w('')

    # Glitches.
    w('## Capture glitches (a bone over 35 deg in a frame)')
    w('')
    kinds = defaultdict(int)
    by_bone = defaultdict(int)
    for t in takes:
        for g in t['glitches']:
            kinds[g['kind']] += 1
            by_bone[g['bone']] += 1
    w(f"Events: {', '.join(f'{k} {v}' for k, v in sorted(kinds.items()))}. Spikes come back within 6 frames (repairable by "
      "interpolation); twist flips are twist bones turning over 120 deg; jumps are neither.")
    w('')
    w('| bone | events |')
    w('|---|---|')
    for b, v in sorted(by_bone.items(), key=lambda kv: -kv[1])[:15]:
        w(f"| {b} | {v} |")
    w('')
    w('| take | events (kind: count) |')
    w('|---|---|')
    for t in sorted(takes, key=lambda t: -len(t['glitches'])):
        if not t['glitches']:
            break
        k = defaultdict(int)
        for g in t['glitches']:
            k[g['kind']] += 1
        w(f"| {t['name']} | {', '.join(f'{a}: {b}' for a, b in sorted(k.items()))} |")
    w('')

    # Airborne false positives.
    w('## Airborne frames outside the jump takes')
    w('')
    w('The bake\'s Airborne label (both ankles over 0.18 m with the pelvis 2 cm up, 4+ frames) also catches the jog\'s and '
      'the run\'s own flight phases. Those are locomotion, not jumps: the runtime treats Airborne as a jump segment, so they '
      'are mislabelled (Step 2 tightens the detector).')
    w('')
    fp = [(t['name'], t['airborne']) for t in takes if t['airborne'] and 'jump' not in t['tags']]
    w(f"{sum(n for _, n in fp)} frames in {len(fp)} takes: " + ', '.join(f'{nm} {n}' for nm, n in sorted(fp, key=lambda x: -x[1])) + '.')
    w('')

    # Fingers.
    w('## Finger animation (the bake report)')
    w('')
    moving = [t for t in takes if t['fingerBonesMoving'] > 0]
    w(f"{len(moving)} of {len(takes)} takes move a finger bone over 5 deg. Takes with still fingers: "
      + ', '.join(t['name'] for t in takes if t['fingerBonesMoving'] == 0) + '.')
    w('')

    # Speed labels vs styles.
    w('## Speed-labelled gait inside each style (why a style label is needed)')
    w('')
    w('| style | idle | walk | jog | run (frames, by the speed thresholds 0.3 / 1.6 / 2.9 m/s) |')
    w('|---|---|---|---|---|')
    for style in STYLES:
        g = [sum(t['speedGait'][k] for t in takes if t['style'] == style and not t['exaggerated']) for k in range(4)]
        if sum(g):
            w(f"| {style} | {g[0]} | {g[1]} | {g[2]} | {g[3]} |")
    w('')

    # Redundancy.
    w('## Redundancy of steady locomotion (curation input)')
    w('')
    w('Per take: its steady 0.5 s windows\' distance to the nearest window of another take in the same style, direction and '
      '0.25 m/s band (normalized features); "alone" = no other take has that style/direction/band.')
    w('')
    w('| take | baked | steady windows | alone | NN distance p25 / p50 |')
    w('|---|---|---|---|---|')
    for t in sorted(takes, key=lambda t: (t['style'], t['name'])):
        nwin = len(t['nnDist']) + t['uniqueWindows']
        if nwin == 0:
            continue
        w(f"| {t['name']} | {'yes' if t['baked'] else 'no'} | {nwin} | {t['uniqueWindows']} | "
          f"{pct(t['nnDist'], 25):.2f} / {pct(t['nnDist'], 50):.2f} |")
    w('')

    Path(a.md).write_text('\n'.join(md) + '\n', encoding='utf-8')
    for t in takes:
        for k in [k for k in t if k.startswith('_')]:
            del t[k]
    (d / 'audit.json').write_text(json.dumps(dict(takes=takes, mirror=mirror, runGateSteps=best), indent=0), encoding='utf-8')
    print(f'audit: wrote {a.md} and {d / "audit.json"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
