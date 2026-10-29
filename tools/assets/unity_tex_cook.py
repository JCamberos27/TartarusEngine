# Texture cook for unity_import.py, run inside Blender (its bundled Python has numpy and reads TGA / TIF / EXR / PSD):
#   blender -b --factory-startup --python tools/assets/unity_tex_cook.py -- <jobs.json>
# jobs.json is a list of {"src": path, "max": 2048, "outs": [{"path": out.png, "expr": ..., "srgb": bool, "alpha": bool}]}.
# Each out is one PNG built from the source's channels:
#   "rgba" / "rgb"          the image as-is (rgb drops alpha)
#   ["ch", c, scale, bias]  grey from channel c (0-3): v * scale + bias, clamped (1 - smoothness: scale -1, bias 1)
#   ["ao", c, strength]     grey 1 - strength * (1 - v): Unity's Occlusion Strength baked in
#   ["normal_y"]            rgb with green flipped (DirectX -> OpenGL)
# Sources over "max" are halved (box filter, in linear light for sRGB colour maps) until they fit.
import json
import os
import sys

import bpy
import numpy as np


def to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.clip(c, 0, None), 1 / 2.4) - 0.055)


def load(src):
    img = bpy.data.images.load(src, check_existing=False)
    img.colorspace_settings.name = 'Non-Color'  # raw values, no view transform on read or write
    w, h = img.size
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(h, w, 4), w, h


def halve(px, srgb):
    h, w = px.shape[:2]
    px = px[:h - h % 2, :w - w % 2]
    if srgb:
        px = px.copy()
        px[..., :3] = to_linear(px[..., :3])
    px = px.reshape(h // 2, 2, w // 2, 2, 4).mean(axis=(1, 3))
    if srgb:
        px[..., :3] = to_srgb(px[..., :3])
    return px


def save(px, path, alpha):
    h, w = px.shape[:2]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img = bpy.data.images.new(os.path.basename(path), w, h, alpha=alpha)
    img.colorspace_settings.name = 'Non-Color'
    img.pixels.foreach_set(np.clip(px, 0, 1).astype(np.float32).ravel())
    img.filepath_raw = path
    img.file_format = 'PNG'
    scene = bpy.context.scene
    scene.render.image_settings.color_mode = 'RGBA' if alpha else 'RGB'
    scene.render.image_settings.color_depth = '8'
    scene.render.image_settings.compression = 60
    img.save_render(path, scene=scene)
    bpy.data.images.remove(img)


def build(px, expr):
    if expr in ('rgba', 'rgb'):
        return px
    if expr[0] == 'ch':
        v = np.clip(px[..., expr[1]] * expr[2] + expr[3], 0, 1)
    elif expr[0] == 'ao':
        v = 1.0 - expr[2] * (1.0 - px[..., expr[1]])
    elif expr[0] == 'normal_y':
        out = px.copy()
        out[..., 1] = 1.0 - out[..., 1]
        return out
    else:
        raise ValueError(expr)
    out = np.ones(px.shape, dtype=np.float32)
    out[..., 0] = out[..., 1] = out[..., 2] = v
    return out


def main():
    jobs = json.load(open(sys.argv[sys.argv.index('--') + 1], encoding='utf-8'))
    done = failed = 0
    for job in jobs:
        try:
            px, w, h = load(job['src'])
            # Blender's pixel rows run bottom-up; so do the rows it saves, so nothing to flip.
            for out in job['outs']:
                p = px
                while max(p.shape[0], p.shape[1]) > job.get('max', 2048):
                    p = halve(p, out.get('srgb', False))
                save(build(p, out['expr']), out['path'], out.get('alpha', False))
                done += 1
        except Exception as e:  # report and carry on: unity_import.py lists what didn't cook
            failed += 1
            print(f'[TexCook] FAILED {job["src"]}: {e}')
    print(f'[TexCook] {done} written, {failed} failed')


main()
