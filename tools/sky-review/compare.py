"""Side-by-side comparison of review renders.

    python tools/sky-review/compare.py out.png <run-dir-A> <run-dir-B> [...] [--shots a_0,b_2,...]

Columns are runs (render.sh output folders), rows are shots. Without --shots every PNG in the
first run is used. Needs Pillow.
"""
import os
import sys
from PIL import Image

args = sys.argv[1:]
shots = None
if '--shots' in args:
    i = args.index('--shots')
    shots = args[i + 1].split(',')
    del args[i:i + 2]
out, runs = args[0], args[1:]
if not shots:
    shots = sorted(f[:-4] for f in os.listdir(runs[0]) if f.endswith('.png'))
w, h = 640, 323
sheet = Image.new('RGB', (w * len(runs), h * len(shots)))
for c, run in enumerate(runs):
    for r, shot in enumerate(shots):
        path = os.path.join(run, shot + '.png')
        if os.path.exists(path):
            sheet.paste(Image.open(path).convert('RGB').resize((w, h), Image.LANCZOS), (c * w, r * h))
sheet.save(out)
print('wrote', out)
