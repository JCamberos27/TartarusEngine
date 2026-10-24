import bpy, sys, numpy as np

# Resize a texture whose alpha is only a UV-island mask: the pack left the padding black, which bleeds
# dark fringes along every UV seam once mipmapped. Push-pull fill the padding from the island edges,
# then drop the alpha. Each map is a (source, destination, size) triple:
#   blender -b --factory-startup --python fill_padding_resize.py -- <src.png> <dst.png> 2048 [...]
def push_pull(rgb, w):
    if min(rgb.shape[:2]) <= 1:
        return rgb / np.maximum(w, 1e-6)[..., None]
    h, wd = rgb.shape[0] // 2 * 2, rgb.shape[1] // 2 * 2
    pr = (rgb[0:h:2, 0:wd:2] + rgb[1:h:2, 0:wd:2] + rgb[0:h:2, 1:wd:2] + rgb[1:h:2, 1:wd:2])
    pw = (w[0:h:2, 0:wd:2] + w[1:h:2, 0:wd:2] + w[0:h:2, 1:wd:2] + w[1:h:2, 1:wd:2])
    coarse = push_pull(pr, pw)
    up = np.repeat(np.repeat(coarse, 2, 0), 2, 1)
    up = np.pad(up, ((0, rgb.shape[0] - up.shape[0]), (0, rgb.shape[1] - up.shape[1]), (0, 0)), mode="edge")
    filled = rgb / np.maximum(w, 1e-6)[..., None]
    t = np.clip(w, 0.0, 1.0)[..., None]
    return filled * t + up * (1.0 - t)

args = sys.argv[sys.argv.index("--") + 1:]
for src, dst, size in zip(args[0::3], args[1::3], args[2::3]):
    img = bpy.data.images.load(src)
    img.alpha_mode = "STRAIGHT"
    W, H = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(H, W, 4)
    mask = (px[..., 3] > 0.5).astype(np.float32)
    rgb = push_pull(px[..., :3] * mask[..., None], mask)
    px[..., :3] = np.where(mask[..., None] > 0, px[..., :3], rgb)
    px[..., 3] = 1.0
    img.pixels[:] = px.ravel()
    img.scale(int(size), int(size))
    img.filepath_raw = dst
    img.file_format = "PNG"
    img.save()
    print("saved", dst)

