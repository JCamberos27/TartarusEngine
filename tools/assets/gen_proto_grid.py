# The Master Scene's prototype grid: a neutral mid-grey with lighter lines every 10 cm (barely there), 1 m and 5 m,
# one 5 x 5 m tile per texture, for world-space triplanar boxes (triplanarScale 0.2 = one tile per 5 m). Neutral so a
# material's base colour tints it: one texture for the slate ground and every module's colour.
# Lines are antialiased analytically (coverage of each texel by the line's width), centred on the tile's edges so tiles join seamlessly.
#
#   python tools/assets/gen_proto_grid.py [--size 2048]
#
# Writes project/assets/Materials/Prototype/proto_grid_neutral.png (albedo, sRGB) and proto_grid_neutral_emissive.png
# (the 1 m and 5 m lines alone, for a faint glow where a material wants the grid to read in the dark). Needs numpy and
# Pillow.
import argparse
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '..', '..', 'project', 'assets', 'Materials', 'Prototype'))

TILE_M = 5.0
BASE = 0.62           # sRGB of the field: mid-grey, so a base colour sets the hue and value
# (spacing m, width m, how far toward white the line lifts the field: low contrast, read as panel seams)
LINES = [(0.1, 0.005, 0.06), (1.0, 0.014, 0.30), (5.0, 0.030, 0.55)]
GLOW_FROM = 1.0       # only lines at least this far apart glow (no 10 cm shimmer)


def coverage(n, spacing, width):
    """Per texel, the fraction of it a line of `width` (metres) every `spacing` covers, along one axis."""
    px = TILE_M / n
    # Texel i spans [i, i+1) * px; the nearest line centre is the nearest multiple of `spacing`.
    lo = np.arange(n) * px
    hi = lo + px
    centre = np.round((lo + hi) * 0.5 / spacing) * spacing
    a = np.clip(np.minimum(hi, centre + width / 2) - np.maximum(lo, centre - width / 2), 0.0, None)
    return np.clip(a / px, 0.0, 1.0)


def build(n):
    level = np.zeros((n, n))
    glow = np.zeros((n, n))
    for spacing, width, white in LINES:
        c = coverage(n, spacing, width)
        both = 1.0 - (1.0 - c[None, :]) * (1.0 - c[:, None])  # either axis's line
        level = np.maximum(level, both * white)
        if spacing >= GLOW_FROM:
            glow = np.maximum(glow, both * white / LINES[-1][2])
    lin_base = ((BASE + 0.055) / 1.055) ** 2.4
    lin = lin_base + (1.0 - lin_base) * level                      # mix in linear light, then encode
    srgb = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1 / 2.4) - 0.055)
    albedo = np.round(np.clip(srgb, 0, 1) * 255).astype(np.uint8)
    return albedo, np.round(np.clip(glow, 0, 1) * 255).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--size', type=int, default=2048)
    a = ap.parse_args()
    albedo, glow = build(a.size)
    os.makedirs(OUT, exist_ok=True)
    for name, img in (('proto_grid_neutral.png', albedo), ('proto_grid_neutral_emissive.png', glow)):
        Image.fromarray(np.dstack([img] * 3), 'RGB').save(os.path.join(OUT, name), optimize=True)
        print(os.path.join(OUT, name), os.path.getsize(os.path.join(OUT, name)), 'bytes')


if __name__ == '__main__':
    main()
