# Before / after LOOK SHEET for Jacob (W6 Briareos). CPU only (Pillow + numpy).
#   python -I make_look_sheet4.py --tour <sweep_tour_look4.json> --before <png dir> --after <png dir> --out <dir> [--before-label ..] [--after-label ..] [--thumb 640]
# --before / --after = the --gi-tour output folders (NN_<id>_full.png and NN_<id>_gi.png). The groups come from <tour>_groups.json (make_look_tour.py).
# Per shot: BEFORE | AFTER | difference x6 (white = changed), full view, plus the GI-only view when --gi is given. Writes <out>\look_sheet.html + panels\*.png.
import os, glob, html, json, argparse
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ap = argparse.ArgumentParser()
ap.add_argument('--tour', required=True); ap.add_argument('--before', required=True); ap.add_argument('--after', required=True); ap.add_argument('--out', required=True)
ap.add_argument('--before-label', default='BEFORE'); ap.add_argument('--after-label', default='AFTER'); ap.add_argument('--thumb', type=int, default=640)
ap.add_argument('--gi', action='store_true', help='also a GI-only row per shot'); ap.add_argument('--note', action='append', default=[], help='GROUP=text, a caption line for a group')
a = ap.parse_args()
PAN = os.path.join(a.out, 'panels'); os.makedirs(PAN, exist_ok=True)
try: F = ImageFont.truetype('arial.ttf', 18)
except Exception: F = ImageFont.load_default()
notes = dict(n.split('=', 1) for n in a.note)

def find(d, wid, view='full'):
    g = glob.glob(os.path.join(d, '*_%s_%s.png' % (wid, view)))
    return g[0] if g else None
def gray(p): return np.asarray(Image.open(p).convert('L'), float)
def dstat(b, c):
    if not (b and c): return 'missing image'
    d = np.abs(gray(b) - gray(c))
    return 'max diff %d/255, %.2f %% of pixels over 6/255' % (d.max(), 100.0 * (d > 6).mean())
def panel(items, name, tw):
    th = tw * 9 // 16; sh = Image.new('RGB', (tw * len(items), th + 26), (24, 24, 24)); dr = ImageDraw.Draw(sh)
    for i, (lab, im) in enumerate(items):
        dr.text((i * tw + 6, 3), lab, fill=(255, 255, 255) if im is not None else (230, 90, 90), font=F)
        if im is not None: sh.paste(im.resize((tw, th), Image.LANCZOS), (i * tw, 26))
        else: dr.text((i * tw + 20, 60), 'no image', fill=(200, 80, 80), font=F)
    fn = os.path.join(PAN, name + '.png'); sh.save(fn); return 'panels/' + name + '.png'
def load(p): return Image.open(p).convert('RGB') if p else None
def diff(b, c):
    if not (b and c): return None
    d = np.clip(np.abs(gray(b) - gray(c)) * 6, 0, 255).astype(np.uint8)
    return Image.fromarray(d).convert('RGB')

groups = json.load(open(os.path.splitext(a.tour)[0] + '_groups.json', encoding='utf-8'))
h = ['<html><meta charset="utf-8"><body style="background:#181818;color:#ddd;font-family:sans-serif;max-width:2100px"><h1>Look sheet: %s vs %s</h1>' % (html.escape(a.before_label), html.escape(a.after_label)),
     '<p>Every shot is the SAME camera pose before and after. Third image = absolute difference x6 (white = changed, black = identical).</p>']
for g in groups:
    h.append('<h2>%s</h2>' % html.escape(g['title']))
    if g['group'] in notes: h.append('<p><b>%s</b></p>' % html.escape(notes[g['group']]))
    for wid in g['ids']:
        b, c = find(a.before, wid), find(a.after, wid)
        img = panel([('%s  %s' % (a.before_label, wid), load(b)), ('%s' % a.after_label, load(c)), ('difference x6', diff(b, c))], wid, a.thumb)
        h.append('<p>%s: %s</p><img src="%s" style="max-width:100%%">' % (html.escape(wid), dstat(b, c), html.escape(img)))
        if a.gi:
            bg, cg = find(a.before, wid, 'gi'), find(a.after, wid, 'gi')
            img = panel([('GI only, %s' % a.before_label, load(bg)), ('GI only, %s' % a.after_label, load(cg)), ('difference x6', diff(bg, cg))], wid + '_gi', a.thumb)
            h.append('<img src="%s" style="max-width:100%%">' % html.escape(img))
h.append('</body></html>')
open(os.path.join(a.out, 'look_sheet.html'), 'w', encoding='utf-8').write('\n'.join(h))
print('look sheet ->', os.path.join(a.out, 'look_sheet.html'), 'panels', len(os.listdir(PAN)))
