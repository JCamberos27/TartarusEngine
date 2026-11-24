# Before/after sheet for Jacob: rows = shots, columns = variants (labelled). Usage: python make_sheet.py OUT.png ROOTDIR:variant:label ... --shots a,b,c
# e.g. python make_sheet.py sheet.png red_ab:base:BEFORE red_ab:face1:"roomFace=1" --shots jacob-redroom-metal,armchair-rail
import sys, glob, os
from PIL import Image, ImageDraw
args = sys.argv[1:]; out = args[0]; shots = []; cols = []
i = 1
while i < len(args):
    if args[i] == '--shots': shots = args[i + 1].split(','); i += 2
    else: root, var, label = args[i].split(':', 2); cols.append((root, var, label)); i += 1
W, H, LBL = 640, 360, 22
sheet = Image.new('RGB', (W * len(cols), (H + LBL) * len(shots)), (20, 20, 20))
d = ImageDraw.Draw(sheet)
for r, s in enumerate(shots):
    for c, (root, var, label) in enumerate(cols):
        m = glob.glob(os.path.join(root, var, 'png', f'*_{s}_full.png'))
        x, y = c * W, r * (H + LBL)
        d.text((x + 6, y + 5), f'{s}: {label}', fill=(255, 255, 255))
        if m: sheet.paste(Image.open(m[0]).convert('RGB').resize((W, H), Image.LANCZOS), (x, y + LBL))
        else: d.text((x + 6, y + LBL + 6), 'missing', fill=(255, 80, 80))
sheet.save(out, optimize=True)
print(out, sheet.size, os.path.getsize(out) // 1024, 'KB')
