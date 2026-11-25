"""G-MB stage A pixel A/B: per shot, pixels differing (any channel > 0 / > 8) between modes; plus a diff heat image."""
import sys, os, glob
import numpy as np
from PIL import Image

root = sys.argv[1]
pairs = [('off1', 'off2', 'noise OFF'), ('on1', 'on2', 'noise ON'), ('off1', 'lead1', 'OFF vs LEAD'),
         ('off2', 'lead2', 'OFF vs LEAD #2'), ('off1', 'on1', 'OFF vs ON'), ('off2', 'on2', 'OFF vs ON #2'),
         ('lead1', 'on1', 'LEAD vs ON')]
for scene in ('forest', 'hallway'):
    for a, b, label in pairs:
        da, db = os.path.join(root, f'shots_{a}_{scene}'), os.path.join(root, f'shots_{b}_{scene}')
        if not (os.path.isdir(da) and os.path.isdir(db)):
            continue
        for pa in sorted(glob.glob(os.path.join(da, '*.png'))):
            pb = os.path.join(db, os.path.basename(pa))
            if not os.path.exists(pb):
                print(f'{scene} {label} {os.path.basename(pa)}: missing in {b}')
                continue
            x = np.asarray(Image.open(pa).convert('RGB')).astype(np.int16)
            y = np.asarray(Image.open(pb).convert('RGB')).astype(np.int16)
            d = np.abs(x - y).max(axis=2)
            n0, n8 = int((d > 0).sum()), int((d > 8).sum())
            print(f'{scene:8s} {label:15s} {os.path.basename(pa):28s} px>0 {n0:8d}  px>8 {n8:7d}  max {int(d.max())}')
            if n8 and a.startswith('off') and b.startswith('on'):
                heat = Image.fromarray(np.clip(d * 16, 0, 255).astype(np.uint8))
                heat.save(os.path.join(root, f'diff_{scene}_{a}_{b}_{os.path.basename(pa)}'))
