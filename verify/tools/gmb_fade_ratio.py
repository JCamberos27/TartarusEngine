"""G-MB fade check: per run folder, the forest-border-2 window's 1px/2px detail ratio for each border2 capture
(> 0.73 = LOD cross-fade stipple), plus the run's smoke fps."""
import glob, os, re, sys
import numpy as np
from PIL import Image

root = sys.argv[1]
for d in sorted(glob.glob(os.path.join(root, '*', ''))):
    row = []
    for p in sorted(glob.glob(os.path.join(d, 'Forest_border2*.png'))):
        a = np.asarray(Image.open(p).convert('L'), np.float32)[210:570, 0:640]
        r = np.abs(a[:, 1:] - a[:, :-1]).mean() / np.abs(a[:, 2:] - a[:, :-2]).mean()
        row.append(f"{os.path.basename(p)[7:-4]} {r:.3f}{' STIPPLE' if r > 0.73 else ''}")
    log = open(os.path.join(d, 'run.log'), encoding='utf-8', errors='replace').read()
    m = re.search(r'fps=([0-9.]+)', log)
    print(os.path.basename(os.path.dirname(d)), ' | '.join(row), '| fps', m.group(1) if m else '?')
