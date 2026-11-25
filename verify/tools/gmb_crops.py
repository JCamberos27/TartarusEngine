"""G-MB stage A footage: per shot, the 640x360 window where OFF and ON differ most (px > 8), cropped from both,
scaled 2x, labelled "BEFORE - <name>" / "AFTER - <name>". One file per image."""
import sys, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont

root, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
shots = [('forest', 'Forest_border1.png', 'forest border 1'), ('forest', 'Forest_border2.png', 'forest border 2'),
         ('forest', 'Forest_house.png', 'forest house'), ('hallway', 'Apartment Hallway_hall2.png', 'hallway')]
W, H = 640, 360
try:
    font = ImageFont.truetype('arialbd.ttf', 40)
except OSError:
    font = ImageFont.load_default()
n = 0
for scene, name, label in shots:
    a = Image.open(os.path.join(root, f'shots_off1_{scene}', name)).convert('RGB')
    b = Image.open(os.path.join(root, f'shots_on1_{scene}', name)).convert('RGB')
    d = (np.abs(np.asarray(a, np.int16) - np.asarray(b, np.int16)).max(axis=2) > 8).astype(np.int32)
    ii = d.cumsum(0).cumsum(1)
    best, at = -1, (0, 0)
    for y in range(0, d.shape[0] - H + 1, 30):
        for x in range(0, d.shape[1] - W + 1, 40):
            s = ii[y + H - 1, x + W - 1] - (ii[y - 1, x + W - 1] if y else 0) - (ii[y + H - 1, x - 1] if x else 0) + \
                (ii[y - 1, x - 1] if x and y else 0)
            if s > best:
                best, at = s, (x, y)
    x, y = at
    for tag, img in (('BEFORE', a), ('AFTER', b)):
        n += 1
        c = img.crop((x, y, x + W, y + H)).resize((W * 2, H * 2), Image.LANCZOS)
        dr = ImageDraw.Draw(c)
        text = f'{tag} - {label}'
        dr.rectangle((0, 0, dr.textlength(text, font=font) + 24, 60), fill=(0, 0, 0))
        dr.text((12, 8), text, fill=(255, 255, 255), font=font)
        fn = f'{n:02d}_{tag}_{label.replace(" ", "_")}.png'
        c.save(os.path.join(out, fn))
        print(fn, f'crop at {x},{y}, px>8 in window {best}')
