# Contact sheets for the seam / dark-patch sweep (W6 Briareos). CPU only.
# usage: python -I make_sweep_sheets.py --run LABEL=DIR [--run LABEL2=DIR2 ...] --out OUTDIR [--thumb 640]
#   DIR = a --gi-tour output folder (NN_<id>_full.png and NN_<id>_gi.png per waypoint of sweep_tour.json).
# One sheet per camera group (room spot: ceil | floor | wall-xp | wall-xn | wall-zp | wall-zn; door: a/b near/far;
# threshold: look-neg | look-pos; outside, stairs likewise), columns from the ids (<key>_<col>),
# rows per run: full, then gi. Several --run = baseline vs after on one sheet. Writes OUTDIR\sheets\*.png + index.html.
import os, re, sys, argparse, html
from PIL import Image, ImageDraw, ImageFont

ap = argparse.ArgumentParser()
ap.add_argument('--run', action='append', required=True, help='LABEL=DIR (repeatable)')
ap.add_argument('--out', required=True)
ap.add_argument('--thumb', type=int, default=480)
ap.add_argument('--nobase', action='append', default=[], help='KEY=reason: baseline poses of this camera group are not comparable (repeatable)')
ap.add_argument('--baseline-label', default='baseline', help='the run label whose rows get the no-valid-baseline mark')
ap.add_argument('--only', action='append', default=[], help='only camera groups whose key starts with this prefix (repeatable)')
a = ap.parse_args()
# 2026-11-24 (SM): fix-3 tours were regenerated (tree blocker): these camera groups have a different pose than the 99579b4e baseline.
# Pairing is by id (the NNN_ prefix is stripped), never by index.
NOBASE = {'den-lamp-column': 'new (no baseline)', 'master-ceil-edge': 'new (no baseline)',
          'living-room-spot4': 'old pose was inside the Christmas tree',
          'living-room': 'pose moved (tree blocker)', 'living-room-spot2': 'pose moved (tree blocker)', 'living-room-spot5': 'pose moved (tree blocker)'}
for n in a.nobase:
    k, _, why = n.partition('='); NOBASE[k] = why or 'pose changed'
runs = []
for r in a.run:
    lab, _, d = r.partition('=')
    runs.append((lab, d))
TW = a.thumb; TH = TW * 9 // 16; LAB = 22
try:
    font = ImageFont.truetype('arial.ttf', 15)
except Exception:
    font = ImageFont.load_default()

def index(d):
    """{(id, view): path}"""
    out = {}
    for f in os.listdir(d):
        m = re.match(r'^\d+_(.+)_(full|gi)\.png$', f)
        if m: out[(m.group(1), m.group(2))] = os.path.join(d, f)
    return out
idx = [(lab, index(d)) for lab, d in runs]

ORDER = ['ceil', 'floor', 'wall-xp', 'wall-xn', 'wall-zp', 'wall-zn', 'look-neg', 'look-pos', 'a-near', 'a-far', 'b-near', 'b-far',
         'near', 'ceil', 'left', 'right', 'far', 'along-up', 'along-down', 'back-up', 'back-down']
groups = {}      # key -> set of columns; an id is <key>_<col>, the key has no underscore
for lab, ix in idx:
    for (wid, view) in ix:
        key, _, col = wid.rpartition('_')
        if key: groups.setdefault(key, set()).add(col)
os.makedirs(os.path.join(a.out, 'sheets'), exist_ok=True)
made = []
for key in sorted(groups):
    if a.only and not any(key.startswith(p) for p in a.only): continue
    cols = sorted(groups[key], key=lambda c: (ORDER.index(c) if c in ORDER else 99 + (int(re.sub(r'\D', '', c) or 0)), c))
    rows = [(lab, ix, view) for lab, ix in idx for view in ('full', 'gi')]
    W = TW * len(cols); H = (TH + LAB) * len(rows) + LAB
    sheet = Image.new('RGB', (W, H), (24, 24, 24)); dr = ImageDraw.Draw(sheet)
    dr.text((6, 3), key + (('   [baseline not comparable: %s]' % NOBASE.get(key, 'new in fix-3: Jacob look position')) if (key in NOBASE or key.startswith(('jlook-', 'walkwp-', 'walkpop-'))) else ''), fill=(255, 255, 255), font=font)
    for ri, (lab, ix, view) in enumerate(rows):
        y0 = LAB + ri * (TH + LAB)
        for ci, col in enumerate(cols):
            wid = '%s_%s' % (key, col)
            p = ix.get((wid, view))
            x0 = ci * TW
            nb = lab == a.baseline_label and (key in NOBASE or key.startswith(('jlook-', 'walkwp-', 'walkpop-')))
            dr.text((x0 + 6, y0 + 3), ('NO VALID BASELINE | ' if nb else '%s | ' % lab) + (os.path.basename(p) if p else '%s_%s (missing)' % (wid, view)),
                    fill=(230, 90, 90) if nb else (210, 210, 120), font=font)
            if p:
                im = Image.open(p).convert('RGB').resize((TW, TH), Image.LANCZOS)
                sheet.paste(im, (x0, y0 + LAB))
            else:
                dr.rectangle([x0, y0 + LAB, x0 + TW - 1, y0 + LAB + TH - 1], outline=(120, 40, 40))
                dr.text((x0 + 10, y0 + LAB + 10), 'missing', fill=(200, 80, 80), font=font)
    fn = os.path.join(a.out, 'sheets', key + '.png')
    sheet.save(fn); made.append((key, fn))
with open(os.path.join(a.out, 'index.html'), 'w', encoding='utf-8') as f:
    f.write('<html><meta charset="utf-8"><body style="background:#181818;color:#ddd;font-family:sans-serif"><h2>Sweep sheets: %s</h2>' %
            html.escape(', '.join(l for l, _ in runs)))
    for key, fn in made:
        f.write('<h3>%s</h3><img src="sheets/%s" style="max-width:100%%">' % (html.escape(key), html.escape(os.path.basename(fn))))
    f.write('</body></html>')
print('sheets', len(made), '->', a.out)
