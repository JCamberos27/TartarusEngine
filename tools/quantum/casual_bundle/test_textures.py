"""Unit tests for textures.py (synthetic images in a temp dir; nothing under project/ is touched).
Run from tools/quantum:  python -m unittest
"""
from __future__ import annotations

import json
import os
import tempfile
import time
import unittest
from pathlib import Path

from PIL import Image

from . import textures as T


def srgb8(linear: float) -> int:
    return round(T.linear_to_srgb(linear) * 255)


def write_rgbe(path: Path, w: int, h: int, pixel: tuple[int, int, int, int], rle: bool) -> None:
    """A Radiance file filled with one RGBE quad, flat or new-style RLE."""
    data = bytearray(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n" + f"-Y {h} +X {w}\n".encode())
    for _ in range(h):
        if rle:
            data += bytes([2, 2, w >> 8, w & 255])
            for c in range(4):                     # one run per component (w <= 127)
                data += bytes([128 + w, pixel[c]])
        else:
            data += bytes(pixel) * w
    path.write_bytes(bytes(data))


class TempDirCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()


class FitSizeTests(unittest.TestCase):
    def test_fit(self):
        self.assertEqual(T.fit_size((4096, 4096), 2048), (2048, 2048))
        self.assertEqual(T.fit_size((4096, 2048), 2048), (2048, 1024))
        self.assertEqual(T.fit_size((512, 256), 2048), (512, 256))      # never upscaled
        self.assertEqual(T.fit_size((8192, 8), 2048), (2048, 2))
        self.assertEqual(T.fit_size((100, 100), 0), (100, 100))         # 0 = no cap


class CookTests(TempDirCase):
    def test_opaque_alpha_dropped_and_downscaled(self):
        src = self.dir / "a.png"
        Image.new("RGBA", (128, 64), (10, 200, 30, 255)).save(src)
        r = T.cook_texture(src, self.dir / "out" / "a.png", 64, "color")
        self.assertEqual(r.size, (64, 32))
        self.assertFalse(r.has_alpha)
        with Image.open(self.dir / "out" / "a.png") as im:
            self.assertEqual(im.mode, "RGB")
            self.assertEqual(im.getpixel((5, 5)), (10, 200, 30))       # flat colour survives the linear resize

    def test_real_alpha_kept(self):
        src = self.dir / "a.png"
        im = Image.new("RGBA", (32, 32), (255, 0, 0, 255))
        im.putpixel((3, 3), (255, 0, 0, 0))
        im.save(src)
        r = T.cook_texture(src, self.dir / "b.png", 2048, "color")
        self.assertTrue(r.has_alpha)
        self.assertEqual(r.size, (32, 32))
        with Image.open(self.dir / "b.png") as out:
            self.assertEqual(out.mode, "RGBA")
            self.assertEqual(out.getpixel((3, 3))[3], 0)

    def test_bmp_to_png_and_grey_and_16bit(self):
        Image.new("RGB", (16, 16), (1, 2, 3)).save(self.dir / "c.bmp")
        r = T.cook_texture(self.dir / "c.bmp", self.dir / "c.png", 2048, "data")
        with Image.open(self.dir / "c.png") as im:
            self.assertEqual(im.format, "PNG")
            self.assertEqual(im.getpixel((0, 0)), (1, 2, 3))
        self.assertFalse(r.has_alpha)
        Image.new("L", (8, 8), 77).save(self.dir / "g.png")
        T.cook_texture(self.dir / "g.png", self.dir / "g_out.png", 2048, "data")
        with Image.open(self.dir / "g_out.png") as im:
            self.assertEqual((im.mode, im.getpixel((0, 0))), ("L", 77))
        Image.new("I;16", (8, 8), 65535).save(self.dir / "w16.png")
        T.cook_texture(self.dir / "w16.png", self.dir / "w16_out.png", 2048, "data")
        with Image.open(self.dir / "w16_out.png") as im:
            self.assertEqual((im.mode, im.getpixel((0, 0))), ("L", 255))

    def test_colour_resized_in_linear_light_data_raw(self):
        # 1-px black/white stripes averaged 2:1 -> linear 0.5: sRGB ~188 for colour, raw ~128 for data/normal.
        im = Image.new("RGB", (64, 64))
        im.putdata([(255, 255, 255) if x % 2 else (0, 0, 0) for y in range(64) for x in range(64)])
        im.save(self.dir / "s.png")
        T.cook_texture(self.dir / "s.png", self.dir / "s_col.png", 32, "color")
        T.cook_texture(self.dir / "s.png", self.dir / "s_dat.png", 32, "data")
        T.cook_texture(self.dir / "s.png", self.dir / "s_nrm.png", 32, "normal")
        with Image.open(self.dir / "s_col.png") as c, Image.open(self.dir / "s_dat.png") as d, \
                Image.open(self.dir / "s_nrm.png") as n:
            self.assertAlmostEqual(c.getpixel((16, 16))[0], srgb8(0.5), delta=3)
            self.assertAlmostEqual(d.getpixel((16, 16))[0], 128, delta=3)
            self.assertEqual(d.getpixel((16, 16)), n.getpixel((16, 16)))

    def test_cache(self):
        src = self.dir / "a.png"
        Image.new("RGB", (64, 64), (9, 9, 9)).save(src)
        dst = self.dir / "a_out.png"
        self.assertFalse(T.cook_texture(src, dst, 32, "color").skipped)
        self.assertTrue(T.cook_texture(src, dst, 32, "color").skipped)
        self.assertFalse(T.cook_texture(src, dst, 32, "color", force=True).skipped)
        self.assertFalse(T.cook_texture(src, dst, 16, "color").skipped)        # different cap -> recooked
        self.assertEqual(T.cook_texture(src, dst, 16, "color").size, (16, 16))
        future = time.time() + 10
        os.utime(src, (future, future))                                      # source newer -> recooked
        self.assertFalse(T.cook_texture(src, dst, 16, "color").skipped)

    def test_deterministic(self):
        im = Image.new("RGBA", (50, 40))
        im.putdata([(x * 5 % 256, y * 6 % 256, (x * y) % 256, 255 - x) for y in range(40) for x in range(50)])
        im.save(self.dir / "d.png")
        T.cook_texture(self.dir / "d.png", self.dir / "d1.png", 32, "color")
        T.cook_texture(self.dir / "d.png", self.dir / "d2.png", 32, "color")
        self.assertEqual((self.dir / "d1.png").read_bytes(), (self.dir / "d2.png").read_bytes())

    def test_radiance_flat_and_rle(self):
        # (128, 64, 0, E=129) = (1.0, 0.5, 0) linear (+half-step rounding, as Ward's reader).
        for rle in (False, True):
            src = self.dir / f"h{rle}.bmp"                        # the export names them .bmp
            write_rgbe(src, 16, 4, (128, 64, 0, 129), rle)
            self.assertEqual(T.source_size(src), (16, 4))
            r = T.cook_texture(src, self.dir / f"h{rle}_c.png", 2048, "color")
            self.assertEqual(r.size, (16, 4))
            with Image.open(self.dir / f"h{rle}_c.png") as im:
                px = im.getpixel((7, 2))
                self.assertEqual(px[0], 255)
                self.assertAlmostEqual(px[1], srgb8(64.5 / 128), delta=1)
                self.assertLessEqual(px[2], srgb8(0.5 / 128) + 1)
            T.cook_texture(src, self.dir / f"h{rle}_d.png", 2048, "data")
            with Image.open(self.dir / f"h{rle}_d.png") as im:
                self.assertAlmostEqual(im.getpixel((0, 0))[1], 128, delta=1)

    def test_bad_kind(self):
        with self.assertRaises(ValueError):
            T.cook_texture(self.dir / "x.png", self.dir / "y.png", 64, "albedo")


class AdjustedAlbedoTests(TempDirCase):
    def _src(self, rgba=(200, 100, 50, 255), size=(16, 16)):
        p = self.dir / "base.png"
        Image.new("RGBA", size, rgba).save(p)
        return p

    @staticmethod
    def _lin(v8):
        return T.srgb_to_linear(v8 / 255)

    def test_identity_matches_source(self):
        T.bake_adjusted_albedo(self._src(), self.dir / "o.png", 2048)
        with Image.open(self.dir / "o.png") as im:
            self.assertEqual(im.mode, "RGB")                      # opaque alpha dropped
            for a, b in zip(im.getpixel((0, 0)), (200, 100, 50)):
                self.assertAlmostEqual(a, b, delta=1)

    def test_ue_graph(self):
        sat, con, bri, cc, cca = 0.6, 1.3, 0.9, (0.2, 0.0, 0.1), 0.25
        T.bake_adjusted_albedo(self._src(), self.dir / "o.png", 2048, saturation=sat, contrast=con, brightness=bri,
                               cc_color=cc, cc_amount=cca)
        x = [self._lin(v) for v in (200, 100, 50)]
        lum = 0.2126 * x[0] + 0.7152 * x[1] + 0.0722 * x[2]
        want = []
        for c, k in zip(x, cc):
            d = c + (lum - c) * (1 - sat)
            p = max(d * bri, 0) ** con
            want.append(srgb8(p + (k - p) * cca))
        with Image.open(self.dir / "o.png") as im:
            for a, b in zip(im.getpixel((3, 3)), want):
                self.assertAlmostEqual(a, b, delta=1)

    def test_saturation_zero_is_grey_and_brightness_clamps(self):
        T.bake_adjusted_albedo(self._src(), self.dir / "g.png", 2048, saturation=0.0)
        with Image.open(self.dir / "g.png") as im:
            r, g, b = im.getpixel((0, 0))
            self.assertTrue(abs(r - g) <= 1 and abs(g - b) <= 1)
        T.bake_adjusted_albedo(self._src(), self.dir / "b.png", 2048, brightness=5.0)
        with Image.open(self.dir / "b.png") as im:
            self.assertEqual(im.getpixel((0, 0))[0], 255)       # clamped like UE's GBuffer

    def test_oversaturation(self):
        T.bake_adjusted_albedo(self._src(), self.dir / "s.png", 2048, saturation=1.5)
        with Image.open(self.dir / "s.png") as im:
            r, g, b = im.getpixel((0, 0))
            self.assertGreater(r, 200)
            self.assertLess(b, 50)

    def test_alpha_kept_and_alpha_src(self):
        src = self._src(rgba=(10, 20, 30, 40))
        T.bake_adjusted_albedo(src, self.dir / "a.png", 2048, saturation=0.5)
        with Image.open(self.dir / "a.png") as im:
            self.assertEqual(im.getpixel((0, 0))[3], 40)
        mask = self.dir / "mask.png"
        Image.new("RGBA", (8, 8), (128, 128, 128, 255)).save(mask)      # resampled to the base's 16x16
        r = T.bake_adjusted_albedo(src, self.dir / "m.png", 2048, alpha_src=mask, alpha_src_srgb=False)
        self.assertTrue(r.has_alpha)
        with Image.open(self.dir / "m.png") as im:
            self.assertEqual(im.size, (16, 16))
            self.assertAlmostEqual(im.getpixel((8, 8))[3], 128, delta=1)
        T.bake_adjusted_albedo(src, self.dir / "m2.png", 2048, alpha_src=mask, alpha_src_srgb=True, force=True)
        with Image.open(self.dir / "m2.png") as im:
            self.assertAlmostEqual(im.getpixel((8, 8))[3], round(T.srgb_to_linear(128 / 255) * 255), delta=1)


class HairAlbedoTests(TempDirCase):
    def setUp(self):
        super().setUp()
        # 256x4 maps: root gradient 0..255 along x; alpha mask = 255 except a 0 column; ID = constant 255
        g = Image.new("RGBA", (256, 4))
        g.putdata([(x, x, x, 255) for y in range(4) for x in range(256)])
        g.save(self.dir / "root.png")
        a = Image.new("RGBA", (256, 4), (255, 255, 255, 255))
        for y in range(4):
            a.putpixel((100, y), (0, 0, 0, 255))
            a.putpixel((101, y), (128, 128, 128, 255))
        a.save(self.dir / "alpha.png")
        Image.new("RGBA", (256, 4), (255, 255, 255, 255)).save(self.dir / "id.png")

    def test_root_to_tip(self):
        root, tip = (0.2, 0.05, 0.0), (0.0, 0.1, 0.3)
        r = T.bake_hair_albedo(self.dir / "alpha.png", self.dir / "root.png", self.dir / "h.png", 2048,
                               root, tip, brightness=1.0, noise_mean=0.0, root_srgb=False, alpha_srgb=False)
        self.assertTrue(r.has_alpha)
        self.assertEqual(r.size, (256, 4))
        with Image.open(self.dir / "h.png") as im:
            self.assertEqual(im.mode, "RGBA")
            self.assertEqual(im.getpixel((0, 0))[:3], tuple(srgb8(c) for c in root))       # t = 0 is the root
            self.assertEqual(im.getpixel((255, 0))[:3], tuple(srgb8(c) for c in tip))
            self.assertEqual(im.getpixel((100, 0))[3], 0)
            self.assertEqual(im.getpixel((101, 0))[3], 128)                                # raw when not sRGB
            self.assertEqual(im.getpixel((50, 0))[3], 255)

    def test_brightness_noise_variation_and_srgb_mask(self):
        root = tip = (0.1, 0.1, 0.1)
        T.bake_hair_albedo(self.dir / "alpha.png", self.dir / "root.png", self.dir / "h.png", 2048, root, tip,
                           brightness=2.0, id_tex=self.dir / "id.png", value_variation=0.5, alpha_srgb=True)
        k = 2.0 * (1 + T.HAIR_NOISE_MEAN) * (1 + 0.5 * (1.0 - 0.5))          # id = 1 -> lerp(1, 1.5, 0.5)
        with Image.open(self.dir / "h.png") as im:
            self.assertAlmostEqual(im.getpixel((10, 1))[0], srgb8(0.1 * k), delta=1)
            self.assertEqual(im.getpixel((101, 0))[3], round(T.srgb_to_linear(128 / 255) * 255))
        T.bake_hair_albedo(self.dir / "alpha.png", self.dir / "root.png", self.dir / "c.png", 2048, root, tip,
                           brightness=50.0)
        with Image.open(self.dir / "c.png") as im:
            self.assertEqual(im.getpixel((10, 1))[:3], (255, 255, 255))                    # clamped like UE

    def test_hue_variation(self):
        """HueShift about the grey axis: id 0 (no turn) leaves the tint (1, 1-h, 1-h); id 1/3 (a 120-degree turn,
        any direction) permutes it; the average over a full turn is grey (3-2h)/3."""
        h = 0.5

        def tint(idv):
            p = self.dir / f"id_{idv}.png"
            Image.new("L", (4, 1), idv).save(p)
            return tuple(b.getpixel((0, 0)) for b in T._hue_tints(p, (4, 1), False, h))

        for idv in (0, 255):                                      # no turn / one full turn
            for got, want in zip(tint(idv), (1.0, 0.5, 0.5)):
                self.assertAlmostEqual(got, want, places=4)
        third = tint(85)                                          # 85/255 = a third of a turn
        self.assertEqual(sorted(round(x, 4) for x in third), [0.5, 0.5, 1.0])
        self.assertAlmostEqual(third[0], 0.5, places=4)           # the 1.0 moved off red
        ramp = self.dir / "ramp.png"
        im = Image.new("L", (256, 1))
        im.putdata(list(range(256)))
        im.save(ramp)
        for band in T._hue_tints(ramp, (256, 1), False, h):       # averaged over a full turn: grey
            self.assertAlmostEqual(sum(band.getpixel((x, 0)) for x in range(256)) / 256, (3 - 2 * h) / 3, delta=0.01)
        # Through the bake: grey hair x tint (1, 0.5, 0.5) at id 0.
        Image.new("RGBA", (256, 4), (0, 0, 0, 255)).save(self.dir / "id0.png")
        grey = (0.2, 0.2, 0.2)
        T.bake_hair_albedo(self.dir / "alpha.png", self.dir / "root.png", self.dir / "hue.png", 2048, grey, grey,
                           id_tex=self.dir / "id0.png", hue_variation=h, noise_mean=0.0, id_srgb=False)
        with Image.open(self.dir / "hue.png") as out:
            self.assertEqual(out.getpixel((10, 0))[:3], (srgb8(0.2), srgb8(0.1), srgb8(0.1)))

    def test_cap_and_cache(self):
        a = T.bake_hair_albedo(self.dir / "alpha.png", self.dir / "root.png", self.dir / "h.png", 64,
                               (0, 0, 0), (1, 1, 1))
        self.assertEqual(a.size, (64, 1))
        self.assertTrue(T.bake_hair_albedo(self.dir / "alpha.png", self.dir / "root.png", self.dir / "h.png", 64,
                                           (0, 0, 0), (1, 1, 1)).skipped)


class MetaTests(TempDirCase):
    def test_meta_format_and_guid_kept(self):
        proj = self.dir / "project"
        png = proj / "assets" / "Characters" / "Quantum" / "Textures" / "T_X_Normal.png"
        png.parent.mkdir(parents=True)
        Image.new("RGB", (4, 4)).save(png)
        g1 = T.write_texture_meta(png, "normal", 2048, project_root=proj)
        meta = json.loads((png.parent / "T_X_Normal.png.meta").read_text(encoding="utf-8"))
        self.assertEqual(meta, {
            "folder": "assets/Characters/Quantum/Textures",
            "guid": g1,
            "importer": {"anisoLevel": 16, "compression": 2, "filterMode": 2, "generateMipmaps": True,
                         "isSRGB": False, "maxTextureSize": 2048, "textureType": 1, "wrapMode": 0},
            "metaVersion": 1,
            "type": "texture",
        })
        self.assertRegex(g1, r"^[0-9a-f]{16}$")
        g2 = T.write_texture_meta(png, "color", 4096, project_root=proj)
        self.assertEqual(g1, g2)
        meta = json.loads((png.parent / "T_X_Normal.png.meta").read_text(encoding="utf-8"))
        self.assertTrue(meta["importer"]["isSRGB"])
        self.assertEqual((meta["importer"]["textureType"], meta["importer"]["maxTextureSize"]), (0, 4096))
        self.assertFalse(T.texture_importer("data", 2048)["isSRGB"])

    def test_matches_repo_example(self):
        """The importer block equals the hand-checked Hoodie BaseColor meta in the repo (read-only)."""
        from .common import PROJECT
        ex = PROJECT / "assets/Characters/Quantum/Textures/Clothing/Tops/Hoodie/T_Hoodie_Blue_BaseColor.png.meta"
        if not ex.exists():
            self.skipTest("example meta not present")
        self.assertEqual(json.loads(ex.read_text(encoding="utf-8"))["importer"], T.texture_importer("color", 2048))


if __name__ == "__main__":
    unittest.main()
