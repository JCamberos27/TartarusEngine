# HARD-EDGE DETECTOR for the sweep PNGs (W6 Briareos, 2026-11-24). CPU only (numpy + OpenCV + PIL).
# usage: python -I hard_edge_scan.py --run DIR [--run DIR2 ...] --out OUTDIR [--views full gi] [--baseline DIR] [--top 60]
#   DIR = a sweep png folder (NNN_<id>_full.png / _gi.png). Writes OUTDIR\hard_edges.csv (every flagged edge), HARD_EDGES.md
#   (ranked list: shot, edge location, step %, length, orientation; with --baseline: whether the same edge existed there)
#   and OUTDIR\marks\<shot>.png (the shot with the edges drawn in red) for the top N.
# WHAT IT FINDS: a straight, SHARP luminance step in a smooth area = a hard shading line (a light/shadow/probe edge, a rectangular
# patch border), not texture. Method (all in linear light, image resized to W=960):
#   1. L = linear luminance; Ls = L blurred sigma 1.5 (kills fabric / siding / wood-grain texture).
#   2. at each pixel and each of 4 axes: step = |mean(Ls over 6 px on side A at distance 3..9) - mean(side B)| / max(side A, side B, floor)
#      "sharp" = the step measured over a 2 px gap is >= SHARP_RATIO of the step measured over a 12 px gap (a soft penumbra spreads
#      across 12 px, a hard line does not).
#   3. keep pixels with step >= STEP_MIN and sharp, drop pixels where either side is darker than FLOOR (noise) or where the
#      surrounding 15x15 window is high-texture (local std of L, relative, > TEXTURE_MAX: that is geometry detail, not a smooth surface).
#   4. link them into straight runs (Hough, min length LEN_MIN px) and report each run with its median step.
# KNOWN LIMIT: a real geometric corner / door frame / depth edge is also a sharp step, so this finds CANDIDATES. Use --baseline
# (the same waypoint id in the previous run): an edge at the same place with the same step is old geometry; a NEW or STRONGER edge
# is a lighting regression. Eyes-on still decides; the report lists the top shots per run for that.
import os, re, sys, csv, math, argparse
import numpy as np
import cv2

# ---- thresholds (printed in the report header; the SM asked for them) -----------------------------------------------------
W = 960              # analysis width
STEP_MIN = 0.12      # relative luminance step across the edge (12 %)
STEP_STRONG = 0.25   # >= this = "STRONG" in the report
SHARP_RATIO = 0.60   # step over a 2 px gap / step over a 12 px gap
FLOOR = 0.003        # linear luminance below this on either side = noise, ignored (about sRGB 25/255; night facades sit at 20-35)
ABS_MIN = 3.0        # AND the step must be >= this many 8-bit display codes (sRGB gamma 2.2): rejects 1-code quantisation steps in the dark
TEXTURE_MAX = 0.45   # local relative std (15x15, after the sigma blur) above this = textured area, ignored
SIGMA = 2.5          # blur before measuring (px at 960): removes siding / fabric / grain
PER_SHOT = 6         # at most this many edges kept per shot and view (the strongest by step x length)
LEN_MIN = 90         # px at W=960 (9 % of the width)
BASELINE_TOL = 0.06  # a baseline edge counts as "same" when within 12 px and the step differs by < 6 points

ap = argparse.ArgumentParser()
ap.add_argument('--run', action='append', required=True, help='a png folder (repeatable; the shots of all runs are scanned)')
ap.add_argument('--out', required=True)
ap.add_argument('--views', nargs='+', default=['full', 'gi'])
ap.add_argument('--baseline', help='png folder of the previous look; edges are compared by waypoint id, NNN_ prefix ignored')
ap.add_argument('--top', type=int, default=60)
ap.add_argument('--only', action='append', default=[], help='only waypoint ids that start with this prefix (repeatable)')
a = ap.parse_args()

LUT = np.array([((c / 255.0) / 12.92 if c / 255.0 <= 0.04045 else ((c / 255.0 + 0.055) / 1.055) ** 2.4) for c in range(256)], np.float32)

def load_lum(path):
    im = cv2.imread(path, cv2.IMREAD_COLOR)
    if im is None:
        return None
    h, w = im.shape[:2]
    im = cv2.resize(im, (W, int(round(h * W / w))), interpolation=cv2.INTER_AREA)
    b, g, r = [LUT[im[:, :, i]] for i in range(3)]
    return 0.2126 * r + 0.7152 * g + 0.0722 * b, im

def shift(img, dx, dy):
    return np.roll(np.roll(img, -dy, axis=0), -dx, axis=1)

def box_mean_along(Ls, ux, uy, d0, d1):
    """mean of Ls sampled at distances d0..d1 (px) along unit step (ux, uy) from each pixel."""
    acc = np.zeros_like(Ls); n = 0
    for d in range(d0, d1 + 1):
        acc += shift(Ls, int(round(ux * d)), int(round(uy * d))); n += 1
    return acc / n

AXES = ((1, 0, 'vertical line'), (0, 1, 'horizontal line'), (1, 1, 'diagonal \\'), (1, -1, 'diagonal /'))

def scan(path):
    r = load_lum(path)
    if r is None:
        return [], None
    L, bgr = r
    Ls = cv2.GaussianBlur(L, (0, 0), SIGMA)
    m = cv2.blur(Ls, (15, 15)); m2 = cv2.blur(Ls * Ls, (15, 15))
    texture = np.sqrt(np.maximum(m2 - m * m, 0)) / np.maximum(m, FLOOR)
    smooth = texture < TEXTURE_MAX
    edges = []
    H, Wd = L.shape
    for ux, uy, name in AXES:
        n = math.hypot(ux, uy); vx, vy = ux / n, uy / n
        a_near = box_mean_along(Ls, vx, vy, 3, 9)
        b_near = box_mean_along(Ls, -vx, -vy, 3, 9)
        mid = np.maximum(np.maximum(a_near, b_near), FLOOR)
        step = np.abs(a_near - b_near) / mid
        a_far = box_mean_along(Ls, vx, vy, 10, 14); b_far = box_mean_along(Ls, -vx, -vy, 10, 14)
        # sharpness: gap of 1..2 px against the wider footprint
        a_n = box_mean_along(Ls, vx, vy, 2, 3); b_n = box_mean_along(Ls, -vx, -vy, 2, 3)
        step2 = np.abs(a_n - b_n) / np.maximum(np.maximum(a_n, b_n), FLOOR)
        step12 = np.abs(a_far - b_far) / np.maximum(np.maximum(a_far, b_far), FLOOR)
        sharp = step2 >= SHARP_RATIO * np.maximum(step12, 1e-6)
        g = lambda x: 255.0 * np.power(np.maximum(x, 0), 1 / 2.2)
        ok = (step >= STEP_MIN) & sharp & (np.minimum(a_near, b_near) >= FLOOR) & smooth & (np.abs(g(a_near) - g(b_near)) >= ABS_MIN)
        ok[:12, :] = False; ok[-12:, :] = False; ok[:, :12] = False; ok[:, -12:] = False
        # thin to local maxima across the line normal (keep the strongest pixel of each cross-section)
        mask = (ok.astype(np.uint8)) * 255
        mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8))
        lines = cv2.HoughLinesP(mask, 1, np.pi / 180, 40, minLineLength=LEN_MIN, maxLineGap=6)
        if lines is None:
            continue
        for x1, y1, x2, y2 in np.asarray(lines).reshape(-1, 4):
            ang = math.degrees(math.atan2(y2 - y1, x2 - x1)) % 180
            # keep only lines along this axis (the step is perpendicular to the axis vector)
            lang = (math.degrees(math.atan2(vy, vx)) + 90) % 180
            if min(abs(ang - lang), 180 - abs(ang - lang)) > 20:
                continue
            ln = math.hypot(x2 - x1, y2 - y1)
            xs = np.linspace(x1, x2, int(ln)).astype(int); ys = np.linspace(y1, y2, int(ln)).astype(int)
            st = float(np.median(step[ys, xs]))
            edges.append({'x1': int(x1), 'y1': int(y1), 'x2': int(x2), 'y2': int(y2), 'len': round(ln), 'step': st,
                          'orient': ('vertical' if abs(x2 - x1) < abs(y2 - y1) * 0.3 else 'horizontal' if abs(y2 - y1) < abs(x2 - x1) * 0.3 else 'diagonal'),
                          'meanL': float(np.mean((a_near[ys, xs] + b_near[ys, xs]) / 2))})
    # merge near-duplicates (same place found on several axes / overlapping segments)
    edges.sort(key=lambda e: -e['step'] * e['len'])
    kept = []
    for e in edges:
        cx, cy = (e['x1'] + e['x2']) / 2, (e['y1'] + e['y2']) / 2
        if any(math.hypot(cx - (k['x1'] + k['x2']) / 2, cy - (k['y1'] + k['y2']) / 2) < 25 and k['orient'] == e['orient'] for k in kept):
            continue
        kept.append(e)
    return kept[:PER_SHOT], bgr

def place(e, Wd, H):
    cx, cy = (e['x1'] + e['x2']) / 2 / Wd, (e['y1'] + e['y2']) / 2 / H
    col = 'left' if cx < 0.33 else 'right' if cx > 0.67 else 'centre'
    row = 'top' if cy < 0.33 else 'bottom' if cy > 0.67 else 'middle'
    return '%s-%s (x %.0f%%, y %.0f%%)' % (row, col, cx * 100, cy * 100)

def index(d):
    out = {}
    for f in os.listdir(d):
        m = re.match(r'^\d+_(.+)_(full|gi)\.png$', f)
        if m: out[(m.group(1), m.group(2))] = os.path.join(d, f)
    return out

base_ix = index(a.baseline) if a.baseline else {}
os.makedirs(os.path.join(a.out, 'marks'), exist_ok=True)
rows = []
for d in a.run:
    for (wid, view), path in sorted(index(d).items()):
        if view not in a.views or (a.only and not any(wid.startswith(p) for p in a.only)):
            continue
        edges, bgr = scan(path)
        if bgr is None:
            continue
        H, Wd = bgr.shape[:2]
        bedges = None
        if a.baseline and (wid, view) in base_ix:
            bedges, _ = scan(base_ix[(wid, view)])
        for e in edges:
            tag = ''
            if a.baseline:
                if (wid, view) not in base_ix: tag = 'NO BASELINE'
                else:
                    cx, cy = (e['x1'] + e['x2']) / 2, (e['y1'] + e['y2']) / 2
                    same = [b for b in bedges if math.hypot(cx - (b['x1'] + b['x2']) / 2, cy - (b['y1'] + b['y2']) / 2) < 12 and b['orient'] == e['orient']]
                    if not same: tag = 'NEW'
                    elif e['step'] - max(b['step'] for b in same) > BASELINE_TOL: tag = 'STRONGER (+%.0f pts)' % (100 * (e['step'] - max(b['step'] for b in same)))
                    else: tag = 'same as baseline'
            rows.append({'shot': os.path.basename(path), 'id': wid, 'view': view, 'where': place(e, Wd, H), 'step_pct': round(100 * e['step'], 1),
                         'len_px': e['len'], 'orient': e['orient'], 'strong': e['step'] >= STEP_STRONG, 'vs_baseline': tag, 'seg': (e['x1'], e['y1'], e['x2'], e['y2']),
                         'path': path})
rank = lambda r: (0 if r['view'] == 'full' else 1, -(r['step_pct'] * min(r['len_px'], 400) / 100.0))
rows.sort(key=lambda r: (-(r['step_pct'] * min(r['len_px'], 400)), r['shot']))
with open(os.path.join(a.out, 'hard_edges.csv'), 'w', newline='', encoding='utf-8') as f:
    w = csv.writer(f); w.writerow(['shot', 'view', 'where', 'step_pct', 'len_px_at_960', 'orientation', 'strong', 'vs_baseline'])
    for r in rows: w.writerow([r['shot'], r['view'], r['where'], r['step_pct'], r['len_px'], r['orient'], 'STRONG' if r['strong'] else '', r['vs_baseline']])
# marks for the top shots (distinct shots)
shots_done = []
for r in rows:
    if r['shot'] in shots_done: continue
    if len(shots_done) >= a.top: break
    shots_done.append(r['shot'])
    _, bgr = load_lum(r['path'])
    for q in rows:
        if q['shot'] == r['shot']:
            cv2.line(bgr, (q['seg'][0], q['seg'][1]), (q['seg'][2], q['seg'][3]), (0, 0, 255) if q['strong'] else (0, 165, 255), 2)
    cv2.imwrite(os.path.join(a.out, 'marks', r['shot']), bgr)
nshots = len({(r['shot']) for r in rows})
with open(os.path.join(a.out, 'HARD_EDGES.md'), 'w', encoding='utf-8') as f:
    f.write('# Hard-edge scan (W6 Briareos)\n\n')
    f.write('Runs: %s. Views: %s. Baseline: %s.\n\n' % (', '.join(a.run), ', '.join(a.views), a.baseline or 'none'))
    f.write('**Thresholds** (linear light, analysis width %d px): flag a straight sharp luminance step >= **%d %%** (STRONG >= %d %%), '
            'sharpness (2 px-gap step / 12 px-gap step) >= %.2f, both sides brighter than %.3f linear and the step >= %.0f display codes, local texture (15x15 relative std) < %.2f, '
            'line length >= %d px (%d %% of the width); at most %d edges per shot and view. Candidates only: a real corner or door frame is also a sharp step; "same as baseline" '
            'separates old geometry from a NEW/STRONGER lighting edge.\n\n' % (W, STEP_MIN * 100, STEP_STRONG * 100, SHARP_RATIO, FLOOR, ABS_MIN, TEXTURE_MAX, LEN_MIN, LEN_MIN * 100 // W, PER_SHOT))
    f.write('%d edges flagged in %d shots. Marked PNGs (top %d shots): `marks\\`.\n\n' % (len(rows), nshots, a.top))
    f.write('| # | shot | view | edge location | step % | len px | orient | strong | vs baseline |\n|---|---|---|---|---|---|---|---|---|\n')
    for i, r in enumerate(rows[:200], 1):
        f.write('| %d | %s | %s | %s | %.1f | %d | %s | %s | %s |\n' % (i, r['shot'], r['view'], r['where'], r['step_pct'], r['len_px'], r['orient'], 'STRONG' if r['strong'] else '', r['vs_baseline']))
print('flagged', len(rows), 'edges in', nshots, 'shots ->', a.out)
