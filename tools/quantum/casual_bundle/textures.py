"""Texture cooking for the Quantum Modular Casual Bundle import (see docs/QUANTUM_SWAP.md, Phase 1 step 4).

Pillow only (no numpy): per-pixel maths runs on Pillow "F" (32-bit float) bands through ImageMath, and the sRGB
curves are lookup tables, so a 4K map cooks in a second or two.

API
  cook_texture(src, dst, cap, kind, force=False) -> CookResult
      Copy one exported texture into the repo: Lanczos downscale so the largest side <= cap (never upscales),
      any source format -> PNG, alpha dropped when fully opaque (UE exports RGBA always).
  bake_adjusted_albedo(src, dst, cap, saturation, contrast, brightness, cc_color, cc_amount, alpha_src=None, ...)
      M_Quantum_Master_Material's BaseColor chain baked into a PNG (exact graph in its docstring).
  bake_hair_albedo(alpha_tex, root_tex, dst, cap, root_color, tip_color, brightness, id_tex, value_variation, ...)
      The hair masters' root->tip colour baked into an RGBA PNG (RGB = colour, A = coverage mask).
  write_texture_meta(dst_png, kind, cap, project_root=PROJECT) -> guid
      The .png.meta next to a cooked texture, keeping an existing GUID.

kind: "color" (sRGB albedo, baked albedo), "normal" (tangent-space normal, linear), "data" (ORM, masks: linear).

Colour space (evidence)
- The engine decodes a texture as sRGB only when its .meta says isSRGB (src/Renderer/Texture.h:28-31,
  AssetLibrary ResolveTextureSettings) and does all shading in linear light (ModelFragment.glsl). Its own Max Size
  downsample is "in linear space for sRGB textures" (Texture.h:34). cook_texture matches that: "color" maps are
  resized in linear light; "normal"/"data" maps are resized on the raw values.
- UE evaluates material graphs in linear light. A UE 5.8 dump of the 669 textures the exported MIs reference: all
  BaseColor maps and nearly all hair masks/gradients/IDs are sRGB; every ORM and normal map is linear (so ORM -> the
  engine's linear _MetallicRoughnessMap/_AOMap is 1:1). The bakes decode sRGB -> linear, apply UE's maths, clamp to
  [0, 1] (UE saturates BaseColor into the GBuffer), and encode back to sRGB.

Radiance .hdr files: 16 of the exported ".bmp" files are really Radiance RGBE (header "#?RADIANCE"; the UE
exporter's HDR fallback). Pillow cannot read them, so _read_rgbe() decodes them (flat and new-style RLE) to linear
floats; "color" encodes them to sRGB, other kinds clamp the raw values. .exr is not supported (1 file in the export,
a 1x1 MetaHuman groom placeholder, not needed).
"""
from __future__ import annotations

import math
import os
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageMath

from .common import PROJECT, existing_guid, meta_path, new_guid, write_json

KINDS = ("color", "normal", "data")


@dataclass
class CookResult:
    has_alpha: bool               # the written PNG keeps an alpha channel (some texel < 255)
    size: tuple[int, int]         # (w, h) of the written PNG
    skipped: bool = False         # True when the cache found dst up to date and nothing was written


# ------------------------------------------------------------------------------------------ colour helpers ---
def srgb_to_linear(c: float) -> float:
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb(c: float) -> float:
    c = min(max(c, 0.0), 1.0)
    return c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


_DECODE_SRGB = [srgb_to_linear(i / 255.0) for i in range(256)]     # 8-bit sRGB -> linear float
_DECODE_RAW = [i / 255.0 for i in range(256)]                       # 8-bit raw -> float
_Q = 65535                                                          # float -> 16-bit index for the encode LUTs
_ENCODE_SRGB = [round(linear_to_srgb(i / _Q) * 255) for i in range(_Q + 1)]
_ENCODE_RAW = [round(i / _Q * 255) for i in range(_Q + 1)]


def _to_float(band: Image.Image, srgb: bool) -> Image.Image:
    """8-bit "L" band -> "F" band in 0..1 (linear light when srgb)."""
    return band.point(_DECODE_SRGB if srgb else _DECODE_RAW, "F")


def _to_8bit(band: Image.Image, srgb: bool) -> Image.Image:
    """"F" band (any range) -> 8-bit "L", clamped to 0..1 and sRGB-encoded when srgb."""
    q = ImageMath.lambda_eval(lambda a: a["min"](a["max"](a["x"], 0.0), 1.0) * _Q + 0.5, x=band)
    return q.convert("I").point(_ENCODE_SRGB if srgb else _ENCODE_RAW, "L")


# ----------------------------------------------------------------------------------------------- loading ---
def _read_rgbe(path: Path) -> tuple[list[Image.Image], tuple[int, int]]:
    """Radiance .hdr (RGBE) -> three linear "F" bands. Supports flat and new-style run-length scanlines."""
    data = path.read_bytes()
    pos = 0
    header = []
    while True:                                   # header lines up to the blank line
        end = data.index(b"\n", pos)
        line = data[pos:end]
        pos = end + 1
        if not line:
            break
        header.append(line)
    if not header or not header[0].startswith(b"#?"):
        raise ValueError(f"{path}: not a Radiance file")
    if not any(h == b"FORMAT=32-bit_rle_rgbe" for h in header if h.startswith(b"FORMAT=")):
        raise ValueError(f"{path}: unsupported Radiance FORMAT (only 32-bit_rle_rgbe)")
    end = data.index(b"\n", pos)
    res = data[pos:end].split()
    pos = end + 1
    if len(res) != 4 or res[0] != b"-Y" or res[2] != b"+X":
        raise ValueError(f"{path}: unsupported Radiance orientation {res!r}")
    h, w = int(res[1]), int(res[3])

    planes = [bytearray(w * h) for _ in range(4)]          # R, G, B, E planes, row-major top to bottom
    for y in range(h):
        row = y * w
        if 8 <= w < 32768 and data[pos] == 2 and data[pos + 1] == 2 and (data[pos + 2] << 8 | data[pos + 3]) == w:
            pos += 4                                       # new-style RLE: each component run-length coded
            for c in range(4):
                plane, x = planes[c], 0
                while x < w:
                    n = data[pos]
                    pos += 1
                    if n > 128:                            # run of (n - 128) copies of one byte
                        n -= 128
                        plane[row + x:row + x + n] = bytes([data[pos]]) * n
                        pos += 1
                    else:                                  # n literal bytes
                        plane[row + x:row + x + n] = data[pos:pos + n]
                        pos += n
                    x += n
        else:                                              # flat scanline: w RGBE quads
            chunk = data[pos:pos + 4 * w]
            if len(chunk) != 4 * w:
                raise ValueError(f"{path}: truncated or old-style RLE Radiance data (unsupported)")
            for c in range(4):
                planes[c][row:row + w] = chunk[c::4]
            pos += 4 * w
    size = (w, h)
    e = Image.frombytes("L", size, bytes(planes[3]))
    # value = mantissa * 2^(E - 136) (mantissa 0..255, E 0 = black), as in Greg Ward's reference reader
    scale = e.point([0.0] + [2.0 ** (i - 136) for i in range(1, 256)], "F")
    bands = []
    for c in range(3):
        m = Image.frombytes("L", size, bytes(planes[c])).point([float(i) for i in range(256)], "F")
        bands.append(ImageMath.lambda_eval(lambda a: (a["m"] + 0.5) * a["s"], m=m, s=scale))
    return bands, size


def _is_rgbe(path: Path) -> bool:
    with open(path, "rb") as f:
        return f.read(2) == b"#?"


def source_size(src: Path) -> tuple[int, int]:
    """(w, h) of a source texture from its header only."""
    if _is_rgbe(src):
        with open(src, "rb") as f:
            for line in f:
                parts = line.split()
                if len(parts) == 4 and parts[0] in (b"-Y", b"+Y"):
                    return int(parts[3]), int(parts[1])
        raise ValueError(f"{src}: no Radiance resolution line")
    with Image.open(src) as im:
        return im.size


def fit_size(size: tuple[int, int], cap: int) -> tuple[int, int]:
    """Largest-side <= cap, aspect kept, never upscaled, never below 1 px."""
    w, h = size
    if cap <= 0 or max(w, h) <= cap:
        return w, h
    s = cap / max(w, h)
    return max(1, round(w * s)), max(1, round(h * s))


def _load_8bit(src: Path) -> Image.Image:
    """A PNG/BMP/TGA... as 8-bit "L", "LA", "RGB" or "RGBA" (16-bit grey scaled down, palette expanded)."""
    im = Image.open(src)
    im.load()
    if im.mode in ("I;16", "I;16B", "I;16L", "I"):
        im = im.convert("I").point(lambda v: v * (1 / 257)).convert("L")
    elif im.mode == "P":
        im = im.convert("RGBA")
    elif im.mode == "PA":
        im = im.convert("RGBA")
    elif im.mode not in ("L", "LA", "RGB", "RGBA"):
        im = im.convert("RGBA")
    return im


def _resize_8bit(im: Image.Image, size: tuple[int, int], srgb: bool) -> Image.Image:
    """Lanczos resize; colour bands in linear light when srgb (alpha / data bands on raw values)."""
    if im.size == size:
        return im
    bands = list(im.split())
    ncolour = 1 if im.mode in ("L", "LA") else 3
    out = []
    for i, b in enumerate(bands):
        if srgb and i < ncolour:
            out.append(_to_8bit(_to_float(b, True).resize(size, Image.Resampling.LANCZOS), True))
        else:
            out.append(b.resize(size, Image.Resampling.LANCZOS))
    return Image.merge(im.mode, out)


def _drop_opaque_alpha(im: Image.Image) -> Image.Image:
    if im.mode in ("RGBA", "LA") and im.getchannel("A").getextrema()[0] == 255:
        return im.convert("RGB" if im.mode == "RGBA" else "L")
    return im


def _save_png(im: Image.Image, dst: Path) -> None:
    """Atomic PNG write (a killed run never leaves a truncated PNG the cache would trust). No metadata chunks,
    so the bytes depend only on the pixels: deterministic across runs."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(dst.name + ".tmp")
    im.save(tmp, format="PNG", compress_level=6)
    os.replace(tmp, dst)


def _up_to_date(dst: Path, sources: list[Path], size: tuple[int, int]) -> CookResult | None:
    """Cache check: dst exists, is newer than every source and already has the expected (capped) size."""
    if not dst.exists():
        return None
    t = dst.stat().st_mtime
    if any(s.stat().st_mtime > t for s in sources):
        return None
    try:
        with Image.open(dst) as im:
            if im.size != size:
                return None
            return CookResult(has_alpha=im.mode in ("RGBA", "LA"), size=im.size, skipped=True)
    except OSError:
        return None


# ---------------------------------------------------------------------------------------------- cooking ---
def cook_texture(src: Path, dst: Path, cap: int, kind: str, force: bool = False) -> CookResult:
    """Copy one exported texture to dst (PNG), downscaled to cap. See the module doc for colour handling.

    Skips the work (CookResult.skipped) when dst is newer than src and already at the capped size, unless force.
    """
    if kind not in KINDS:
        raise ValueError(f"kind must be one of {KINDS}, not {kind!r}")
    src, dst = Path(src), Path(dst)
    target = fit_size(source_size(src), cap)
    if not force and (hit := _up_to_date(dst, [src], target)):
        return hit

    if _is_rgbe(src):
        bands, size = _read_rgbe(src)
        srgb = kind == "color"
        if size != target:
            bands = [b.resize(target, Image.Resampling.LANCZOS) for b in bands]
        im = Image.merge("RGB", [_to_8bit(b, srgb) for b in bands])
    else:
        im = _resize_8bit(_load_8bit(src), target, srgb=(kind == "color"))
    im = _drop_opaque_alpha(im)
    _save_png(im, dst)
    return CookResult(has_alpha=im.mode in ("RGBA", "LA"), size=im.size)


def bake_adjusted_albedo(src: Path, dst: Path, cap: int, saturation: float = 1.0, contrast: float = 1.0,
                         brightness: float = 1.0, cc_color: tuple[float, float, float] = (0.0, 0.0, 0.0),
                         cc_amount: float = 0.0, alpha_src: Path | None = None, alpha_src_srgb: bool = True,
                         force: bool = False) -> CookResult:
    """M_Quantum_Master_Material's BaseColor output, baked. The UE graph (dumped from the .uasset with
    MaterialEditingLibrary), all in linear light, x = BaseColor texture RGB:

        d = lerp(x, dot(x, (0.2126, 0.7152, 0.0722)), 1 - Saturation)     Desaturation node, Fraction = OneMinus
        p = PositiveClampedPow(d * Brightness, Contrast)                   Multiply, then Power (max(base, 0)^exp)
        BaseColor = lerp(p, ColorCorrection.rgb, ColorCorrectionValue)     LinearInterpolate

    then clamped to [0, 1] like UE's GBuffer. Saturation > 1 (Fraction < 0) over-saturates, as in UE. Contrast is
    therefore a gamma-like exponent, NOT a 0.5-midpoint contrast: values > 1 darken mid-tones and deepen colour.

    Alpha: the master's OpacityMask is BaseColor.a (kept as is; alpha is never sRGB-decoded) unless the MI turns
    Custom_Alpha_Map off, when it is Alpha_Map.r: pass that texture as alpha_src (decoded when alpha_src_srgb).
    """
    src, dst = Path(src), Path(dst)
    target = fit_size(source_size(src), cap)
    sources = [src] + ([Path(alpha_src)] if alpha_src is not None else [])
    if not force and (hit := _up_to_date(dst, sources, target)):
        return hit
    im = _resize_8bit(_load_8bit(src), target, srgb=True)
    if im.mode in ("L", "LA"):
        im = im.convert("RGBA" if im.mode == "LA" else "RGB")
    bands = im.split()
    r, g, b = (_to_float(x, True) for x in bands[:3])
    frac = 1.0 - saturation
    lum = ImageMath.lambda_eval(lambda a: a["r"] * 0.2126 + a["g"] * 0.7152 + a["b"] * 0.0722, r=r, g=g, b=b)
    out = []
    for ch, cc in zip((r, g, b), cc_color):
        def f(a, cc=cc):
            d = a["c"] + (a["l"] - a["c"]) * frac                       # lerp(c, lum, frac)
            p = a["max"](d * brightness, 0.0) ** contrast if contrast != 1.0 else d * brightness
            return p + (cc - p) * cc_amount if cc_amount else p        # lerp(p, cc, amount)
        out.append(_to_8bit(ImageMath.lambda_eval(f, c=ch, l=lum), True))
    if alpha_src is not None:
        out.append(_to_8bit(_grey_float(Path(alpha_src), target, alpha_src_srgb), False))
    elif len(bands) == 4:
        out.append(bands[3])
    res = _drop_opaque_alpha(Image.merge("RGBA" if len(out) == 4 else "RGB", out))
    _save_png(res, dst)
    return CookResult(has_alpha=res.mode == "RGBA", size=res.size)


# Mean of UE's /Engine/EngineMaterials/Good64x64TilingNoiseHighFreq red channel (exported from UE 5.8: 64x64,
# sRGB off, TC_Grayscale; mean 128/255). The hair masters multiply their colour by (1 + that noise) tiled 24x over
# UV0, i.e. per-texel grain at 2K; the bake uses its mean, which is what the mip chain shows past arm's length.
HAIR_NOISE_MEAN = 0.502


def _grey_float(path: Path, size: tuple[int, int], srgb: bool) -> Image.Image:
    """Red channel of a texture as an "F" band at `size` (decoded from sRGB when UE imported it with sRGB on)."""
    band = _to_float(_load_8bit(path).getchannel(0), srgb)
    return band if band.size == size else band.resize(size, Image.Resampling.LANCZOS)


def _hue_tints(id_tex: Path, size: tuple[int, int], srgb: bool, h: float) -> list[Image.Image]:
    """Per-texel RGB tint = HueShift(v0, -id), v0 = lerp((1,1,1), (1,0,0), h) = (1, 1-h, 1-h), as three "F" bands.
    Rodrigues about k = (1,1,1)/sqrt(3) by angle a = -id * 2pi:
        v = v0 cos a + (k x v0) sin a + k (k . v0)(1 - cos a),  k x v0 = (0, h, -h)/sqrt(3),  k (k . v0) = (3-2h)/3
    cos/sin come from 256-entry lookup tables on the (resized) 8-bit ID band, decoded like UE when id is sRGB."""
    band = _load_8bit(id_tex).getchannel(0)
    if band.size != size:
        band = band.resize(size, Image.Resampling.LANCZOS)
    dec = _DECODE_SRGB if srgb else _DECODE_RAW
    cos_a = band.point([math.cos(-2 * math.pi * d) for d in dec], "F")
    sin_a = band.point([math.sin(-2 * math.pi * d) for d in dec], "F")
    v0 = (1.0, 1.0 - h, 1.0 - h)
    cross = (0.0, h / math.sqrt(3), -h / math.sqrt(3))
    axial = (3.0 - 2.0 * h) / 3.0
    return [ImageMath.lambda_eval(lambda m, v=v, x=x: m["c"] * (v - axial) + m["s"] * x + axial, c=cos_a, s=sin_a)
            for v, x in zip(v0, cross)]


def bake_hair_albedo(alpha_tex: Path, root_tex: Path, dst: Path, cap: int,
                     root_color: tuple[float, float, float], tip_color: tuple[float, float, float],
                     brightness: float = 1.0, id_tex: Path | None = None, value_variation: float = 0.0,
                     hue_variation: float = 0.0, noise_mean: float = HAIR_NOISE_MEAN, alpha_srgb: bool = True,
                     root_srgb: bool = True, id_srgb: bool = True, force: bool = False) -> CookResult:
    """Hair-card albedo (RGB) + coverage (A) from M_Hair_Master / M_F_Hair_Master / M_[F_]FaceHair_Master.

    The masters' BaseColor chain (UE graph dump; Variation on, isDyed / UseDiffuseBaseColor / UV2 colour off,
    which is every exported MI), all linear light:

        c  = lerp(RootColor, TipColor, T_Root.r)                 t = 0 is the root
        c *= Brightness * (1 + noise)                            noise -> its mean, see HAIR_NOISE_MEAN
        c *= lerp(1, lerp(0.5, 1.5, Unique_Hair_Value.r), RandomValueVariation)     per-strand value variation
        c *= HueShift(lerp((1,1,1), (1,0,0), RandomHueVariation), -Unique_Hair_Value.r)  per-strand hue tint
        BaseColor = clamp(c, 0, 1)

    HueShift (/Engine/.../HueShift) rotates its colour about the grey axis (1,1,1) by percentage * 360 degrees
    (RotateAboutAxis); baked per texel with Rodrigues' formula. The facial-hair vertex-colour term is not baked
    (materials.py reports it). OpacityMask = Alpha.r * a view-dependent edge fade that is 1 for a card facing the
    camera, so A = Alpha.r as UE samples it.

    - root_color / tip_color are UE LinearColor values (linear), the PNG is sRGB-encoded (its meta: isSRGB).
    - The exported masks/gradients are grey in RGB with A = 255; the red channel is read.
    - *_srgb: whether UE imported that texture with sRGB on (most Quantum hair textures are; T_Beard_root and
      T_Beard_Opacity are not). An sRGB texture is decoded before use, as UE's sampler does. That applies to the
      alpha too, so the engine's raw-alpha test at UE's clip value cuts where UE cut.
    - id_tex None, or value_variation and hue_variation both 0, skip the per-strand terms.
    Output size = the alpha texture's size, capped; the other maps are resampled to it.
    """
    alpha_tex, root_tex, dst = Path(alpha_tex), Path(root_tex), Path(dst)
    use_value = id_tex is not None and value_variation != 0.0
    use_hue = id_tex is not None and hue_variation != 0.0
    sources = [alpha_tex, root_tex] + ([Path(id_tex)] if use_value or use_hue else [])
    target = fit_size(source_size(alpha_tex), cap)
    if not force and (hit := _up_to_date(dst, sources, target)):
        return hit
    t = _grey_float(root_tex, target, root_srgb)
    k = brightness * (1.0 + noise_mean)
    if use_value:
        idv = _grey_float(Path(id_tex), target, id_srgb)
        # lerp(1, lerp(0.5, 1.5, id), v) = 1 + v * (id - 0.5), times the constant factors
        scale = ImageMath.lambda_eval(lambda m: (m["i"] * value_variation + (1.0 - 0.5 * value_variation)) * k, i=idv)
    else:
        scale = Image.new("F", target, k)
    tints = _hue_tints(Path(id_tex), target, id_srgb, hue_variation) if use_hue else None
    out = []
    for ch, (rc, tc) in enumerate(zip(root_color, tip_color)):
        c = ImageMath.lambda_eval(lambda m, rc=rc, tc=tc: (m["t"] * (tc - rc) + rc) * m["s"], t=t, s=scale)
        if tints:
            c = ImageMath.lambda_eval(lambda m: m["c"] * m["h"], c=c, h=tints[ch])
        out.append(_to_8bit(c, True))
    mask = _to_8bit(_grey_float(alpha_tex, target, alpha_srgb), False)
    out.append(mask)
    res = Image.merge("RGBA", out)
    _save_png(res, dst)
    return CookResult(has_alpha=mask.getextrema()[0] < 255, size=res.size)


# ------------------------------------------------------------------------------------------------- metas ---
def texture_importer(kind: str, cap: int) -> dict:
    """The importer block every Quantum texture meta uses (Textures/Clothing/Tops/Hoodie/*.png.meta).
    Fields: src/Editor/AssetLibrary.cpp ParseTextureImporter + src/Renderer/Texture.h TextureImportSettings:
    textureType 0 Default / 1 NormalMap (forces linear), filterMode 2 Trilinear, wrapMode 0 Repeat,
    compression 2 HighQuality BCn, maxTextureSize = the cap (the engine downsizes on import past it)."""
    if kind not in KINDS:
        raise ValueError(f"kind must be one of {KINDS}, not {kind!r}")
    return {
        "anisoLevel": 16,
        "compression": 2,
        "filterMode": 2,
        "generateMipmaps": True,
        "isSRGB": kind == "color",
        "maxTextureSize": cap,
        "textureType": 1 if kind == "normal" else 0,
        "wrapMode": 0,
    }


def write_texture_meta(dst_png: Path, kind: str, cap: int, project_root: Path = PROJECT) -> str:
    """Write <dst_png>.meta (engine format, keeps an existing GUID). Returns the GUID.
    "folder" is the texture's folder relative to project_root (forward slashes), as the editor writes it."""
    dst_png = Path(dst_png)
    folder = dst_png.parent.resolve().relative_to(Path(project_root).resolve()).as_posix()
    guid = existing_guid(dst_png) or new_guid()
    write_json(meta_path(dst_png), {
        "folder": folder,
        "guid": guid,
        "importer": texture_importer(kind, cap),
        "metaVersion": 1,
        "type": "texture",
    })
    return guid
