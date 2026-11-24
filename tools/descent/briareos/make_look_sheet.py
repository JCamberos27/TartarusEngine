# Jacob's look sheet (W6 Briareos, 2026-11-24): KNOWN section + OPTION before/after panels, from the 677c753d sweep and the option runs. CPU only.
# python -I make_look_sheet.py   -> house-polish\sweep_fix3\look_sheet\look_sheet.html + panels\*.png
import os, glob, html, re
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
B = r'D:\Jacob\tblocks\descent\house-polish'
FIX = B + r'\sweep_fix3'; BASE = B + r'\sweep_after'; OUT = FIX + r'\look_sheet'; PAN = OUT + r'\panels'
os.makedirs(PAN, exist_ok=True)
try: F = ImageFont.truetype('arial.ttf', 18)
except Exception: F = ImageFont.load_default()
def find(d, wid, view='full'):
    g = glob.glob(os.path.join(d, '*_%s_%s.png' % (wid, view)))
    return g[0] if g else None
def pane(items, name, tw=800):
    """items = [(label, path or None)] side by side, one panel PNG."""
    th = tw * 9 // 16; sh = Image.new('RGB', (tw * len(items), th + 26), (24, 24, 24)); d = ImageDraw.Draw(sh)
    for i, (lab, p) in enumerate(items):
        d.text((i * tw + 6, 3), lab, fill=(255, 255, 255) if p else (230, 90, 90), font=F)
        if p: sh.paste(Image.open(p).convert('RGB').resize((tw, th), Image.LANCZOS), (i * tw, 26))
        else: d.text((i * tw + 20, 60), 'no image', fill=(200, 80, 80), font=F)
    fn = os.path.join(PAN, name + '.png'); sh.save(fn); return 'panels/' + name + '.png'
def diffpane(a, b, name, tw=800):
    if not (a and b): return None
    A = np.asarray(Image.open(a).convert('L'), float); Bn = np.asarray(Image.open(b).convert('L'), float)
    d = np.clip(np.abs(A - Bn) * 6, 0, 255).astype(np.uint8)
    p = os.path.join(PAN, name + '_diffsrc.png'); Image.fromarray(d).convert('RGB').save(p)
    return p
def dstat(a, b):
    if not (a and b): return ''
    sd = np.asarray(Image.open(b).convert('L'), float) - np.asarray(Image.open(a).convert('L'), float); d = np.abs(sd)
    m = d > 6
    return 'Pixels changed by more than 6/255: %.2f %% of the frame, largest change %d/255%s.' % (100.0 * float(m.mean()), int(d.max()), (', mean of the changed pixels %+.1f/255 (after minus before)' % float(sd[m].mean())) if m.any() else '; no visible change at this pose')
def gstep(p):   # strongest blurred brightness step in the GI view (99.9th pct of the gradient), to pick the side where a seam shows most
    im = Image.open(p).convert('L').resize((960, 540)).filter(ImageFilter.GaussianBlur(2)); a = np.asarray(im, float)
    gx = np.abs(np.diff(a, axis=1))[:-1]; gy = np.abs(np.diff(a, axis=0))[:, :-1]
    return float(np.percentile(np.hypot(gx, gy), 99.9))

sections = []
# ---- KNOWN -------------------------------------------------------------------------------------------------------------
known = []
for key, title in (('thr24-living-room', 'living room 1|4 (thr24)'), ('thr07-kitchen-dining', 'kitchen-dining 3|7 (thr07)'), ('thr02-entrance-hall', 'hall 1|2 (thr02)')):
    best = max(('look-neg', 'look-pos'), key=lambda s: gstep(find(FIX + r'\open\png', '%s_%s' % (key, s), 'gi') or find(FIX + r'\open\png', '%s_%s' % (key, s))))
    wid = '%s_%s' % (key, best)
    img = pane([('677c753d (fix-3) %s' % wid, find(FIX + r'\open\png', wid)), ('99579b4e (fix-2) same id', find(BASE + r'\open\png', wid))], 'known_' + key)
    known.append((img, 'KNOWN: seam not softened yet: %s. Side chosen = %s (strongest brightness step in the GI view of the two sides). Fade A does not change this seam; a follow-up is being priced.' % (title, best)))
wid = 'door20756_a-near'
known.append((pane([('677c753d (fix-3) ' + wid, find(FIX + r'\open\png', wid)), ('99579b4e (fix-2) ' + wid, find(BASE + r'\open\png', wid))], 'known_door20756'),
              'KNOWN: the back-hall strip by door 20756 is darker than lamp-1 because the bake now treats the doorway as open (wall x -2.01..-1.81, z 29.85..30.84; kd8 strip). Lamp-1 is not in the sweeps; the right image is 99579b4e (fix-2).'))
known.append((None, 'KNOWN: table lamp line (Lamp_table_B, z 26.07): not visible with all lights on; no action. Room: upstairs master bedroom (GI master bedroom 1, floor y 3.104, x -14.4..-11.1). Tested: 10 poses, floor / wall / ceiling, both sides of z 26.07, before vs option 6c4b9d89, all <= 19/255 and 0.00 % of pixels over 6/255; CPU, all lights on: floor <= 7 %, the only spot is a dim 0.3 m ceiling corner at x -11.0, z 25.9. Option 6c4b9d89 dropped from the options (PM rule).'))
known.append((pane([('677c753d master-bedroom_ceil', find(FIX + r'\open\png', 'master-bedroom_ceil'))], 'known_master_ceil', 960), 'KNOWN: master bedroom ceiling diagonal: still present, cause under investigation (Atlas + Talos). 5802c9f0 and 5564577b are already in this build.'))
sections.append(('KNOWN: seam not softened yet / known darker spots', known))
# ---- OPTIONS -----------------------------------------------------------------------------------------------------------
O = FIX + r'\options'
opts = []
for wid, aft, sha, extra in (('jlook-south-wall-z20-3_view', 'after_south', '525e3a75', ''),):
    b = find(FIX + r'\open\png', wid); a = find(O + r'\%s\png' % aft, wid)
    opts.append((pane([('BEFORE: 677c753d as-built (sweep) ' + wid, b), ('AFTER: option %s on the 677c753d exe (scene swap)' % sha, a), ('difference x6 (white = changed)', diffpane(b, a, 'opt_' + wid))], 'opt_' + wid, 640),
                 'OPTION %s: %s. %s%s' % (sha, wid, dstat(b, a), (' tableB: no visible change at the tested poses (retake: 8 poses on both sides of z 26.07, floor / wall / ceiling, wide and oblique, all <= 19/255 and 0.00 %% of pixels over 6/255; option stays in the follow-up set. Frames: options\retake_before / retake_after).' if sha == '6c4b9d89' else ''))))
for wid in ('jlook-hallline_view', 'jlook-hallline_alt'):
    b = find(O + r'\asbuilt\png', wid); a = find(O + r'\after_hallline\png', wid)
    opts.append((pane([('BEFORE: as-built (3.05 cut) ' + wid, b), ('AFTER: d26b5d3c ramp 2.87->3.10 (runtime preview, no re-bake)', a), ('difference x6', diffpane(b, a, 'opt_' + wid))], 'opt_' + wid, 640),
                 'OPTION d26b5d3c (hall line, light 35610 top): AFTER is a RUNTIME PREVIEW, NO RE-BAKE (Bounds changed). %s' % dstat(b, a)))
wid = 'jlook-halfwall_view'
opts.append((pane([('as-built 677c753d: kitchen half wall ' + wid, find(O + r'\asbuilt\png', wid))], 'opt_halfwall', 960), 'OPTION (c): KITCHEN HALF WALL, as-built only (no branch). The line Atlas left is on the half wall end (x -6.78), sill (y 1.00) and lintel underside (y 2.46).'))
sections.append(('OPTIONS: before / after', opts))
# ---- html --------------------------------------------------------------------------------------------------------------
h = ['<html><meta charset="utf-8"><body style="background:#181818;color:#ddd;font-family:sans-serif;max-width:1700px"><h1>Look sheet: build 677c753d</h1>',
     '<p>Sweep pairs (fix-3 vs 99579b4e by waypoint id): <a style="color:#8cf" href="../sheets_open/index.html">open</a> | <a style="color:#8cf" href="../sheets_shut/index.html">shut</a>. Walk steps: JACOB_WALK_STEPS.md and WALK_POPS.md (house-polish\briareos\sweep).</p>']
for title, items in sections:
    h.append('<h2>%s</h2>' % html.escape(title))
    for img, cap in items: h.append('<p>%s</p>' % html.escape(cap) + ('<img src="%s" style="max-width:100%%">' % html.escape(img) if img else ''))
h.append('<h2>Sweep read (W6, build 677c753d)</h2><p>Eyes-on: house-polish\briareos\EYES_ON_SWEEP_677c753d.md. KNOWN: master bedroom ceiling diagonal: still present, cause under investigation (Atlas + Talos); 5802c9f0 and 5564577b are already in this build. PASS on the named shots read: siding at night on all facades, red floor spill, hall dome, runner rug, chandelier graze, den lamp, the 3 jamb thresholds, bay-east siding.  Jacob #2 (hall-bathroom doorway wall): NOT VERIFIED at the named pose (jlook-bathdoor-wall_face framed another wall); the sweep shots door20762_b-near / b-far, which show that doorway from the hall, read smooth on both sides with no blotch. AS EXPECTED (known): sink (still dark, known: black basin bowls dark; counter and splash wall lit), door 20755 (GI 0.59 vs the 0.60 bar), child bedroom 2 (45/125 probe outliers, none visible).</p>')
h.append('</body></html>')
open(OUT + r'\look_sheet.html', 'w', encoding='utf-8').write('\n'.join(h))
print('look sheet ->', OUT + r'\look_sheet.html', 'panels', len(os.listdir(PAN)))
