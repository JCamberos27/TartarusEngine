"""Unit tests for materials.py (synthetic UE JSON + images in a temp dir; nothing under project/ is written).
Run from tools/quantum:  python -m unittest
"""
from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from . import materials as M
from .common import EXPORT_ROOT, PROJECT

UE = "/Game/Casual_Wear_Bundle"
MASTER = f"{UE}/Materials/Quanum_MasterMaterial/M_Quantum_Master_Material"
DEFAULT_TEX = f"{UE}/Textures/MasterMaterial_Textures/T_Quantum_SempleTexture_BaseColor"
HOODIE = f"{UE}/Textures/Cloth/Hoodie"


def master_mi(**scalars) -> dict:
    """A minimal M_Quantum_Master_Material instance like the export writes."""
    tex = {"BaseColor": f"{HOODIE}/T_Hoodie_Blue_BaseColor", "Normal": f"{HOODIE}/T_Hoodie_Normal",
           "ORM": f"{HOODIE}/T_Hoodie_OcclusionRoughnessMetallic"}
    return {
        "class": "MaterialInstanceConstant", "parent": MASTER, "base_material": MASTER,
        "scalars": {"RefractionDepthBias": 0.0, **scalars}, "vectors": {}, "textures": dict(tex),
        "static_switches": {"Emissive": True, "Custom_Alpha_Map": True, "AO": True, "Metallic": True,
                            "Roughness": True},
        "all_texture_params": {**tex, "Emissive": None, "AO_Texture": DEFAULT_TEX, "Roughness_Texture": DEFAULT_TEX,
                               "Metallic_Texture": DEFAULT_TEX, "Alpha_Map": DEFAULT_TEX},
    }


def hair_mi(base="M_Hair_Master", **scalars) -> dict:
    hb = f"{UE}/Textures/Hair/Short_Hair"
    tex = {"Alpha": f"{hb}/T_shorthair_Alpha", "T_Root": f"{hb}/T_shorthair_Vgradient",
           "Unique_Hair_Value": f"{hb}/T_shorthair_Random"}
    s = {"EdgeMaskContrast": 1.0, "EdgeMaskMin": 3.0, "VertexColorStrength": 1.0, "Brightness": 0.2,
         "Roughness": 1.0, "Scatter": 0.2, "RandomValueVariation": 1.8, "RefractionDepthBias": 0.0}
    s.update(scalars)
    return {
        "class": "MaterialInstanceConstant", "parent": f"{UE}/Materials/Hair/Base/{base}",
        "base_material": f"{UE}/Materials/Hair/Base/{base}", "scalars": s,
        "vectors": {"Tangent": [0, -1, 0, 1], "TangentB": [0, 0, -0.3, 1], "RootColor": [0.104, 0.077, 0.056, 1],
                    "TipColor": [0.365, 0.277, 0.218, 1]},
        "textures": dict(tex),
        "static_switches": {"Variation": True, "UsesVertexColors": False, "isDyed": False,
                            "UseDiffuseBaseColor": False, "HideHair": False},
        "all_texture_params": {**tex, "Depth": f"{UE}/Textures/Hair/Beard/T_Beard_depth"},
    }


def repo_paths(ue: dict) -> dict:
    """UE package -> repo path for every texture the JSON mentions."""
    out = {}
    for v in list(ue["textures"].values()) + list(ue["all_texture_params"].values()):
        if v:
            out[v] = "assets/Characters/Quantum/Textures/" + v.rsplit("/", 1)[-1] + ".png"
    return out


class ClassifyTests(unittest.TestCase):
    def test_families(self):
        self.assertEqual(M.classify(master_mi()), "master")
        self.assertEqual(M.classify(hair_mi()), "hair")
        self.assertEqual(M.classify(hair_mi("M_F_Hair_Master")), "hair")
        self.assertEqual(M.classify(hair_mi("M_FaceHair_Master")), "facehair")
        self.assertEqual(M.classify(hair_mi("M_F_FaceHair_Master")), "facehair")
        eye = {"class": "MaterialInstanceConstant", "base_material": f"{UE}/Materials/Male_Body/Eye/M_Eye"}
        self.assertEqual(M.classify(eye), "eye")
        self.assertEqual(M.classify({"class": "Material"}, "M_Teeth"), "eye")
        self.assertEqual(M.classify({"class": "Material"}, "M_Quantum_Master_Material"), "other")
        self.assertEqual(M.classify({"class": "MaterialInstanceConstant", "base_material": "/Game/X/M_Odd"}), "other")


class MasterPlanTests(unittest.TestCase):
    def test_plain_master(self):
        ue = master_mi(Brightness=0.75)
        p = M.plan_material(ue, repo_paths(ue), "M_Hoodie_Blue")
        self.assertIsNone(p.bake)
        self.assertEqual(p.warnings, [])
        self.assertEqual(p.ignored, ["scalar RefractionDepthBias=0"])
        slots = {t.slot: (t.repo_path.rsplit("/", 1)[-1], t.kind) for t in p.textures}
        self.assertEqual(slots, {
            "_AlbedoMap": ("T_Hoodie_Blue_BaseColor.png", "color"),
            "_NormalMap": ("T_Hoodie_Normal.png", "normal"),
            "_MetallicRoughnessMap": ("T_Hoodie_OcclusionRoughnessMetallic.png", "data"),
            "_AOMap": ("T_Hoodie_OcclusionRoughnessMetallic.png", "data"),
        })
        self.assertEqual(p.base_color, [0.75, 0.75, 0.75])
        self.assertTrue(p.normal_flip_y)
        self.assertEqual((p.metallic, p.roughness), (1.0, 1.0))
        self.assertFalse(p.double_sided)
        self.assertTrue(M.plan_material(ue, repo_paths(ue), "x", master_double_sided=True).double_sided)

    def test_adjustments_bake(self):
        for scalars in ({"Saturation": 0.8}, {"Contrast": 1.2}, {"Brightness": 2.0},
                        {"Color Correction Value": 0.3}):
            ue = master_mi(**scalars)
            p = M.plan_material(ue, repo_paths(ue), "M_Hoodie_Blue", ue_name="MI_Hoodie_Blue")
            self.assertIsNotNone(p.bake, scalars)
            self.assertEqual(p.bake.kind, "adjusted")
            self.assertEqual(p.base_color, [1.0, 1.0, 1.0])
            self.assertEqual(p.albedo_path, "assets/Characters/Quantum/Textures/T_Hoodie_Blue_BaseColor_Baked.png")
            self.assertNotIn("_AlbedoMap", [t.slot for t in p.textures])
            self.assertEqual(p.bake.sources, {"base": f"{HOODIE}/T_Hoodie_Blue_BaseColor"})
        ue = master_mi(Brightness=0.5, **{"Color Correction Value": 0.4})
        ue["vectors"]["Color Correction"] = [0.3, 0.1, 0.0, 1.0]
        p = M.plan_material(ue, repo_paths(ue), "M", baked_repo_path="assets/b.png")
        self.assertEqual(p.bake.params, {"saturation": 1.0, "contrast": 1.0, "brightness": 0.5,
                                         "cc_color": (0.3, 0.1, 0.0), "cc_amount": 0.4})
        self.assertEqual(p.albedo_path, "assets/b.png")

    def test_custom_alpha_map_off(self):
        ue = master_mi()
        ue["static_switches"]["Custom_Alpha_Map"] = False
        ue["textures"]["Alpha_Map"] = f"{UE}/Textures/Male_Body/European/T_Quantum_Basemesh_Head_BlackMask_BaseColor"
        p = M.plan_material(ue, repo_paths(ue), "M")
        self.assertEqual(p.bake.sources["alpha_map"], ue["textures"]["Alpha_Map"])
        self.assertTrue(p.bake.params["alpha_src_srgb"])
        self.assertEqual(p.warnings, [])

    def test_roughness_normal_strength_and_unknowns(self):
        ue = master_mi(**{"Roughness Intensity": 0.85, "Normal Strength": 0.3, "Wetness": 2.0})
        ue["vectors"]["Tint"] = [1, 0, 0, 1]
        ue["textures"]["Detail"] = f"{UE}/Textures/X/T_Detail"
        p = M.plan_material(ue, repo_paths(ue), "M")
        self.assertEqual(p.roughness, 0.85)
        self.assertEqual(p.normal_strength, 0.3)
        self.assertEqual(sorted(w.split("=")[0] for w in p.warnings),
                         ["unknown scalar Wetness", "unknown texture Detail", "unknown vector Tint"])
        ue = master_mi(**{"Roughness Intensity": 1.58})
        p = M.plan_material(ue, repo_paths(ue), "M")
        self.assertEqual(p.roughness, 1.0)
        self.assertTrue(any("Roughness Intensity" in w for w in p.warnings))

    def test_switch_off_and_missing_path_warn(self):
        ue = master_mi()
        ue["static_switches"]["Roughness"] = False
        paths = repo_paths(ue)
        del paths[ue["textures"]["Normal"]]
        p = M.plan_material(ue, paths, "M")
        self.assertTrue(any("static switch Roughness=False" in w for w in p.warnings))
        self.assertTrue(any(w.startswith("Normal: no repo path") for w in p.warnings))


class HairPlanTests(unittest.TestCase):
    def test_hair(self):
        ue = hair_mi(Roughness=3.5)
        p = M.plan_material(ue, repo_paths(ue), "M_Hair_Long", ue_name="MI_Hair_Long")
        self.assertEqual(p.family, "hair")
        self.assertEqual(p.textures, [])
        self.assertEqual(p.bake.kind, "hair")
        self.assertEqual(p.bake.sources["root"], ue["textures"]["T_Root"])
        self.assertEqual(p.bake.sources["id"], ue["textures"]["Unique_Hair_Value"])
        self.assertEqual(p.bake.params["root_color"], (0.104, 0.077, 0.056))
        self.assertEqual(p.bake.params["value_variation"], 1.8)
        self.assertTrue(p.bake.params["root_srgb"] and p.bake.params["alpha_srgb"] and p.bake.params["id_srgb"])
        self.assertEqual(p.albedo_path, "assets/Characters/Quantum/Textures/T_Hair_Long_Albedo.png")
        self.assertEqual((p.roughness, p.metallic, p.double_sided), (1.0, 0.0, True))
        self.assertEqual(p.alpha_cutoff, M.HAIR_CUTOFF)
        self.assertEqual(p.warnings, [])
        self.assertIn("scalar Scatter=0.2", p.ignored)

    def test_linear_textures_and_no_variation(self):
        ue = hair_mi()
        ue["textures"]["Alpha"] = f"{UE}/Textures/Hair/Beard/T_Beard_Opacity"
        ue["textures"]["T_Root"] = f"{UE}/Textures/Hair/Beard/T_Beard_root"
        ue["static_switches"]["Variation"] = False
        p = M.plan_material(ue, repo_paths(ue), "M_Beard")
        self.assertFalse(p.bake.params["alpha_srgb"])
        self.assertFalse(p.bake.params["root_srgb"])
        self.assertEqual(p.bake.params["value_variation"], 0.0)
        self.assertNotIn("id", p.bake.sources)
        p = M.plan_material(ue, repo_paths(ue), "M_Beard", texture_srgb={ue["textures"]["T_Root"]: True})
        self.assertTrue(p.bake.params["root_srgb"])

    def test_facehair_and_inherited_texture(self):
        ue = hair_mi("M_FaceHair_Master", RandomHueVariation=0.16, VertexColorStrength=28.0, Param=0.2357)
        ue["textures"]["Root"] = ue["textures"].pop("T_Root")
        ue["all_texture_params"]["Root"] = ue["all_texture_params"].pop("T_Root")
        ue["textures"]["Unique_Hair_Value"] = None                     # override None -> inherited default
        ue["all_texture_params"]["Unique_Hair_Value"] = f"{UE}/Textures/Hair/Beard/T_Beard_ID"
        ue["static_switches"]["UsesVertexColors"] = True
        p = M.plan_material(ue, repo_paths(ue), "M_FaceHair")
        self.assertEqual(p.family, "facehair")
        self.assertEqual(p.bake.sources["root"], ue["textures"]["Root"])
        self.assertEqual(p.bake.sources["id"], f"{UE}/Textures/Hair/Beard/T_Beard_ID")
        self.assertEqual(p.bake.params["hue_variation"], 0.16)          # baked, so no warning
        heads = sorted(w.split("=")[0].split(":")[0] for w in p.warnings)
        self.assertEqual(heads, ["UsesVertexColors", "scalar Param"])
        ue["static_switches"]["Variation"] = False                      # bypasses both per-strand terms
        p = M.plan_material(ue, repo_paths(ue), "M_FaceHair")
        self.assertEqual((p.bake.params["hue_variation"], p.bake.params["value_variation"]), (0.0, 0.0))
        self.assertNotIn("id", p.bake.sources)
        self.assertFalse(any("UsesVertexColors" in w for w in p.warnings))

    def test_dyed_switch_warns(self):
        ue = hair_mi()
        ue["static_switches"]["isDyed"] = True
        p = M.plan_material(ue, repo_paths(ue), "M")
        self.assertTrue(any("isDyed" in w for w in p.warnings))


class BuildTests(unittest.TestCase):
    def test_master_mat(self):
        ue = master_mi(**{"Normal Strength": 0.7})
        paths = repo_paths(ue)
        p = M.plan_material(ue, paths, "M_Hoodie_Blue")
        guids = {paths[ue["textures"]["BaseColor"]]: "0f428ac2677a3c02", paths[ue["textures"]["Normal"]]: "1" * 16,
                 paths[ue["textures"]["ORM"]]: "2" * 16}
        mat = M.build_mat(p, guids, has_alpha=False)
        self.assertNotIn("alphaCutoff", mat)
        self.assertNotIn("renderQueue", mat)
        self.assertEqual(mat["shader"], "engine://Standard.shader")
        self.assertEqual((mat["matVersion"], mat["factorsScaleMaps"], mat["name"]), (2, True, "M_Hoodie_Blue"))
        pr = mat["properties"]
        self.assertEqual(pr["_MetallicRoughnessMap"], pr["_AOMap"])
        self.assertEqual(pr["_RoughnessMap"], "")
        self.assertTrue(pr["_NormalFlipY"])
        self.assertEqual(pr["_NormalStrength"], 0.7)
        self.assertEqual(mat["textureGuids"], {"_AlbedoMap": "0f428ac2677a3c02", "_NormalMap": "1" * 16,
                                               "_MetallicRoughnessMap": "2" * 16, "_AOMap": "2" * 16})
        alpha = M.build_mat(p, guids, has_alpha=True)
        self.assertEqual((alpha["alphaCutoff"], alpha["renderQueue"]), (M.MASTER_CUTOFF, 1))

    def test_same_keys_as_repo_mats(self):
        """Every property key of the existing Quantum mats is written (read-only check against the repo)."""
        base = PROJECT / "assets/Characters/Quantum/Materials"
        examples = [base / "Clothing/Tops/Hoodie/M_Hoodie_Blue.mat", base / "Characters/M_Brows_Lashes.mat",
                    base / "Clothing/Balaclava/M_Balaclava_Crime_Black.mat"]
        examples = [e for e in examples if e.exists()]
        if not examples:
            self.skipTest("example mats not present")
        ue = master_mi()
        mat = M.build_mat(M.plan_material(ue, repo_paths(ue), "M"), {}, False)
        for e in examples:
            ex = json.loads(e.read_text(encoding="utf-8"))
            self.assertLessEqual(set(ex["properties"]), set(mat["properties"]), e.name)
            self.assertLessEqual(set(ex) - {"alphaCutoff", "renderQueue", "textureGuids"}, set(mat), e.name)

    def test_hair_mat(self):
        ue = hair_mi()
        p = M.plan_material(ue, repo_paths(ue), "M_Hair_Long", ue_name="MI_Hair_Long")
        mat = M.build_mat(p, {p.albedo_path: "abcdefabcdefabcd"}, has_alpha=True)
        self.assertEqual(mat["properties"]["_AlbedoMap"], p.albedo_path)
        self.assertEqual(mat["properties"]["_NormalMap"], "")
        self.assertTrue(mat["properties"]["_DoubleSided"])
        self.assertEqual((mat["alphaCutoff"], mat["renderQueue"]), (M.HAIR_CUTOFF, 1))
        self.assertEqual(mat["textureGuids"], {"_AlbedoMap": "abcdefabcdefabcd"})

    def test_eye_unsupported(self):
        p = M.plan_material({"class": "MaterialInstanceConstant", "base_material": "/Game/M_Eye"}, {}, "M_Eye")
        self.assertFalse(p.supported)
        with self.assertRaises(ValueError):
            M.build_mat(p, {}, False)


class WriteAndBakeTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_write_mat_keeps_guid(self):
        path = self.dir / "Mats" / "M_X.mat"
        g1 = M.write_mat(path, {"name": "M_X"})
        meta = json.loads((self.dir / "Mats" / "M_X.mat.meta").read_text(encoding="utf-8"))
        self.assertEqual(meta, {"guid": g1, "metaVersion": 1, "type": "material"})
        self.assertEqual(M.write_mat(path, {"name": "M_X", "v": 2}), g1)
        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["v"], 2)
        self.assertTrue(path.read_bytes().endswith(b"}\n"))
        self.assertNotIn(b"\r\n", path.read_bytes())
        self.assertNotEqual(M.write_mat(path, {"name": "M_X"}, keep_guid=False), g1)

    def test_execute_bakes(self):
        export = self.dir / "export"
        export.mkdir()
        src = {}

        def make(pkg, img):
            p = export / (pkg.rsplit("/", 1)[-1] + ".png")
            img.save(p)
            src[pkg] = p

        ue = master_mi(Saturation=0.5)
        make(ue["textures"]["BaseColor"], Image.new("RGBA", (32, 32), (200, 50, 50, 255)))
        p = M.plan_material(ue, repo_paths(ue), "M_Hoodie_Blue", ue_name="MI_Hoodie_Blue")
        out = self.dir / "baked.png"
        r = M.execute_bake(p, src.__getitem__, out, 16)
        self.assertEqual((r.size, r.has_alpha), ((16, 16), False))
        self.assertTrue(out.exists())

        hu = hair_mi()
        make(hu["textures"]["Alpha"], Image.new("RGBA", (32, 32), (0, 0, 0, 255)))
        make(hu["textures"]["T_Root"], Image.new("RGBA", (32, 32), (128, 128, 128, 255)))
        make(hu["textures"]["Unique_Hair_Value"], Image.new("RGBA", (32, 32), (128, 128, 128, 255)))
        hp = M.plan_material(hu, repo_paths(hu), "M_Hair_Long")
        r = M.execute_bake(hp, src.__getitem__, self.dir / "hair.png", 2048)
        self.assertTrue(r.has_alpha)
        mat = M.build_mat(hp, {}, r.has_alpha)
        self.assertEqual(mat["renderQueue"], 1)
        self.assertIsNone(M.execute_bake(M.plan_material(master_mi(), {}, "M"), src.__getitem__, out, 16))


@unittest.skipUnless((EXPORT_ROOT / "Export").exists(), "Casual Bundle export not on this machine")
class ExportSurveyTests(unittest.TestCase):
    """Read-only: every exported material JSON plans without an exception, and the master/hair instances produce a
    .mat whose only warnings are the known, reviewed ones."""

    def test_all_exported_materials(self):
        root = EXPORT_ROOT / "Export" / "Casual_Wear_Bundle" / "Materials"
        known = ("UsesVertexColors", "scalar Param", "Roughness Intensity")
        families = {}
        for dirpath, _, files in os.walk(root):
            for f in files:
                if not f.endswith(".json"):
                    continue
                ue = json.loads(Path(dirpath, f).read_text(encoding="utf-8"))
                name = f[:-5]
                p = M.plan_material(ue, _AnyPath(), name, ue_name=name)
                families[p.family] = families.get(p.family, 0) + 1
                if p.family in ("master", "hair", "facehair"):
                    self.assertTrue(p.supported, name)
                    M.build_mat(p, {}, has_alpha=True)
                    for w in p.warnings:
                        self.assertTrue(w.startswith(known), f"{name}: {w}")
        self.assertEqual(families.get("master"), 393)
        self.assertEqual(families.get("hair", 0) + families.get("facehair", 0), 33)


class _AnyPath(dict):
    """texture_repo_paths stand-in: every package maps to a dummy repo path."""

    def get(self, k, d=None):
        return f"assets/x/{k.rsplit('/', 1)[-1]}.png" if k else d


if __name__ == "__main__":
    unittest.main()
