"""Tests for build_map.py. Run from tools/quantum:  python -m unittest casual_bundle.test_build_map

The end-to-end tests build a tiny fake export (manifest + model report + material JSON) and a fake repo pack in a
temp folder, so they need neither the real export nor the real repo files.
"""
from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from casual_bundle import build_map as bm
from casual_bundle import common

UE = common.UE_ROOT
Q = common.QUANTUM


# ------------------------------------------------------------------------------------------------- helpers ---
class NameHelpers(unittest.TestCase):
    def test_tokens_apply_synonyms_and_typos(self):
        self.assertEqual(bm.tokens("SKM_Jeans_Jacket_BlueLight"), ["SKM", "Jeans", "Jacket", "Light", "Blue"])
        self.assertEqual(bm.tokens("MI_Hat_Classic_Balck"), ["MI", "Hat", "Classic", "Black"])
        self.assertEqual(bm.tokens("SKM_Vivan_Basemesh"), ["SKM", "Vivian", "Basemesh"])

    def test_canon_stem_gender_and_male_prefix(self):
        self.assertEqual(bm.canon_stem("SKM_Male_Jersey_Bege_D", "Male"), "SKM_Jersey_Beige_D")
        self.assertEqual(bm.canon_stem("SKM_M_Puffy_Vest_Bege", "Male"), "SKM_Puffy_Vest_Beige")
        self.assertEqual(bm.canon_stem("SKM_Shirt_RolledUp_Blue", "Female"), "SKM_F_Shirt_RolledUp_Blue")
        self.assertEqual(bm.canon_stem("SKM_Hair_F_Irokez_2", "Female"), "SKM_F_Hair_Irokez_2")

    def test_strip_variant_and_colour(self):
        words = bm.variant_words("clothing")
        self.assertEqual(bm.strip_variant("SKM_Hoodie_Burgundy_Print", words), "SKM_Hoodie")
        self.assertEqual(bm.strip_variant("SKM_F_Tank_Top_Afro_Beige", words), "SKM_F_Tank_Top")
        self.assertEqual(bm.strip_variant("SKM_F_Vivian_Arms_Tatto_L_R", bm.variant_words("body")), "SKM_F_Vivian_Arms")
        self.assertEqual(bm.colour_of("M_Hoodie_Burgundy_Print"), "Burgundy_Print")
        self.assertEqual(bm.colour_of("M_Puffer_Jacket_Black_Clear"), "Black_Clear")

    def test_mesh_keys(self):
        self.assertEqual(bm.mesh_key("SKM_TShirt_Black"), bm.mesh_key("SKM_Tshirt_Black"))
        self.assertEqual(bm.mesh_key("SKM_Hoodie_HoodUp"), bm.mesh_key("SKM_Hoodie_Hood_Up"))
        self.assertEqual(bm.mesh_key_sorted("SKM_Quantum_Head_Young"), bm.mesh_key_sorted("Quantum_Young_Head"))

    def test_material_keys(self):
        self.assertEqual(bm.mat_key("MI_F_Jersey_Bordo"), bm.mat_key("M_Woman_Jersey_Burgundy"))
        self.assertEqual(bm.mat_key("MI_M_Puffy_Vest_DarkBlue"), bm.mat_key("M_Puffy_Vest_Dark_Blue"))
        self.assertEqual(bm.mat_key("MI_Postal_Bag_Bege_D"), bm.mat_key("M_Postal_Bag_Beige"))
        self.assertEqual(bm.mat_key("MI_Jacket_Classic_Black_Linen"), bm.mat_key("M_Jacket_Classic_Black(Lines)"))
        self.assertEqual(bm.sorted_mat_key("MI_Jeans_Jacket_Collar_Bege"), bm.sorted_mat_key("M_Jeans_Jacket_Beige_Fur"))
        self.assertEqual(bm.skin_stem("MI_Quantum_Old_Arms"), "M_Quantum_Arms_Old")


class ModelReport(unittest.TestCase):
    TEXT = (
        "[ModelReport] D:/x/Export/Casual_Wear_Bundle/Mesh/Male/Cloth/Hoodie/SKM_Hoodie_Black.fbx size 1 1 1 "
        "min -0.5 0.0 -0.2 max 0.5 1.0 0.2 meshes 1 materials MI_Hoodie_Black\n"
        "[ModelReport]   tris: 100\n"
        "[ModelReport]   skin: 80 vertices, 0 over 8 influences (most 4), largest weight lost 0.0%\n"
        "[warn]  something unrelated\n"
        "[ModelReport] D:/x/Export/Casual_Wear_Bundle/Mesh/Female/Head_Modules/SM/Beret/SM_F_Beret_Bege.fbx size 1 1 1 "
        "min -0.1 0.0 -0.1 max 0.1 0.2 0.1 meshes 1 materials Fbx Default Material\n"
        "[ModelReport]   tris: 50\n"
        "[ModelReport] 2 models, 0 failed\n"
    )

    def test_parse(self):
        geo = bm.parse_mesh_report(self.TEXT)
        hood = geo[bm.export_key("Export/Casual_Wear_Bundle/Mesh/Male/Cloth/Hoodie/SKM_Hoodie_Black.fbx")]
        self.assertEqual((hood.verts, hood.tris, hood.bmax), (80, 100, (0.5, 1.0, 0.2)))
        beret = geo[bm.export_key("Export\\Casual_Wear_Bundle\\Mesh\\Female\\Head_Modules\\SM\\Beret\\SM_F_Beret_Bege.fbx")]
        self.assertEqual((beret.verts, beret.count()), (None, 50))
        self.assertTrue(bm.report_complete(self.TEXT))
        self.assertFalse(bm.report_complete(self.TEXT.rsplit("[ModelReport] 2", 1)[0]))

    def test_same_shape_tolerance(self):
        a = bm.Geo(verts=10, bmin=(0, 0, 0), bmax=(1, 1, 1))
        self.assertTrue(a.same_shape(bm.Geo(verts=10, bmin=(0.001, 0, 0), bmax=(1, 1, 1))))
        self.assertFalse(a.same_shape(bm.Geo(verts=10, bmin=(0.01, 0, 0), bmax=(1, 1, 1))))
        self.assertFalse(a.same_shape(bm.Geo(verts=11, bmin=(0, 0, 0), bmax=(1, 1, 1))))


# --------------------------------------------------------------------------------------------- fake world ---
SHAPE_A = ((-0.5, 0.0, -0.2), (0.5, 1.0, 0.2))
SHAPE_B = ((-0.4, 0.0, -0.2), (0.4, 1.2, 0.2))
SHAPE_HAT = ((-0.1, 1.6, -0.1), (0.1, 1.8, 0.1))


class FakeWorld:
    """A miniature export + repo: enough to exercise every rule once."""

    def __init__(self, root: Path):
        self.root = root
        self.assets: dict[str, dict] = {}
        self.materials: dict[str, dict] = {}
        self.report: list[str] = []
        self.project = root / "project"

    # ---- export side
    def mesh(self, rel: str, mats: list[tuple[str, str]], shape=SHAPE_A, verts=100, cls="SkeletalMesh"):
        ue = f"{UE}/Mesh/{rel}"
        file = f"Export/Casual_Wear_Bundle/Mesh/{rel}.fbx"
        self.assets[ue] = {"class": cls, "file": file, "status": "ok",
                           "material_slots": [{"slot": s, "material": (f"{UE}/Materials/{m}" if m else None)}
                                              for s, m in mats]}
        lo, hi = shape
        self.report.append(f"[ModelReport] D:/x/{file} size 1 1 1 min {' '.join(map(str, lo))} "
                           f"max {' '.join(map(str, hi))} meshes 1 materials x")
        self.report.append(f"[ModelReport]   tris: {verts * 2}")
        if cls == "SkeletalMesh":
            self.report.append(f"[ModelReport]   skin: {verts} vertices, 0 over 8 influences")

    def mi(self, rel: str, textures: dict[str, str], base="Quanum_MasterMaterial/M_Quantum_Master_Material"):
        ue = f"{UE}/Materials/{rel}"
        self.assets[ue] = {"class": "MaterialInstanceConstant", "file": f"Export/Casual_Wear_Bundle/Materials/{rel}.json"}
        tex = {k: f"{UE}/Textures/{v}" for k, v in textures.items()}
        self.materials[ue] = {"class": "MaterialInstanceConstant", "parent": f"{UE}/Materials/{base}",
                              "base_material": f"{UE}/Materials/{base}", "textures": tex,
                              "all_texture_params": dict(tex)}
        for t in tex.values():
            self.texture(t[len(UE) + len("/Textures/"):])

    def texture(self, rel: str, ext="png"):
        self.assets[f"{UE}/Textures/{rel}"] = {"class": "Texture2D",
                                               "file": f"Export/Casual_Wear_Bundle/Textures/{rel}.{ext}"}

    # ---- repo side
    def repo_file(self, rel: str, data: dict | None = None, remap: dict | None = None):
        p = self.project / Q / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        if data is not None:
            p.write_text(json.dumps(data), encoding="utf-8")
        else:
            p.write_bytes(b"x")
        if remap is not None:
            common.write_json(common.meta_path(p), {"guid": "0" * 16, "materialRemap": remap, "type": "model"})

    def build(self) -> bm.BuildResult:
        geo = bm.parse_mesh_report("\n".join(self.report))
        repo = bm.Repo.scan(self.project)
        return bm.build({"assets": self.assets}, geo, repo, load_material=lambda ue, info: self.materials[ue])


def standard_world(root: Path) -> FakeWorld:
    w = FakeWorld(root)
    # repo: an old hoodie (default colour Black) with a Unity-only Orange colourway, a kept balaclava, eye mats
    w.repo_file("Models/Clothing/Male/Tops/SKM_Hoodie.fbx",
                remap={"M_Hoodie1": f"{Q}/Materials/Clothing/Tops/Hoodie/M_Hoodie_Black.mat"})
    w.repo_file("Materials/Clothing/Tops/Hoodie/M_Hoodie_Black.mat",
                {"properties": {"_AlbedoMap": f"{Q}/Textures/Clothing/Tops/Hoodie/T_Hoodie_Black_BaseColor.png"}})
    w.repo_file("Materials/Clothing/Tops/Hoodie/M_Hoodie_Orange.mat",
                {"properties": {"_AlbedoMap": f"{Q}/Textures/Clothing/Tops/Hoodie/T_Hoodie_Orange_BaseColor.png"}})
    w.repo_file("Textures/Clothing/Tops/Hoodie/T_Hoodie_Black_BaseColor.png")
    w.repo_file("Textures/Clothing/Tops/Hoodie/T_Hoodie_Orange_BaseColor.png")
    w.repo_file("Models/Clothing/Male/Balaclava/SM_Balaclava_Crime.fbx",
                remap={"M": f"{Q}/Materials/Clothing/Balaclava/M_Balaclava_Crime_Black.mat"})
    w.repo_file("Materials/Clothing/Balaclava/M_Balaclava_Crime_Black.mat", {"properties": {}})
    w.repo_file("Materials/Characters/M_Teeth.mat", {"properties": {}})
    w.repo_file("Materials/Characters/M_Quantum_Body_Afro.mat", {"properties": {}})
    # export: hoodie in 3 colours (same shape) + a differently shaped sweater-like variant
    w.mi("Cloth/Hoodie/MI_Hoodie_Black", {"BaseColor": "Cloth/Hoodie/T_Hoodie_Black_BaseColor",
                                          "Normal": "Cloth/Hoodie/T_Hoodie_Normal_D"})
    w.mi("Cloth/Hoodie/MI_Hoodie_Bordo", {"BaseColor": "Cloth/Hoodie/T_Hoodie_Bordo_BaseColor",
                                          "Normal": "Cloth/Hoodie/T_Hoodie_Normal_D"})
    w.mesh("Male/Cloth/Hoodie/SKM_Hoodie_Bordo", [("M_Hoodie", "Cloth/Hoodie/MI_Hoodie_Bordo")])
    w.mesh("Male/Cloth/Hoodie/SKM_Hoodie_Black", [("M_Hoodie", "Cloth/Hoodie/MI_Hoodie_Black")])
    w.mesh("Male/Cloth/Hoodie/SKM_Hoodie_Long_Bege", [("M_Hoodie", "Cloth/Hoodie/MI_Hoodie_Black")], shape=SHAPE_B)
    # a female-only new item with a skin slot
    w.mi("Male_Body/MI_Quantum_Body_Afro", {"BaseColor": "Male_Body/T_Body_Afro_BaseColor"})
    w.mi("Backpacks/Backpack_Casual/MI_F_Backpack_Casual_Bege", {"BaseColor": "Backpacks/T_F_Backpack_Bege"})
    w.mesh("Female/Backpack_Modules/Backpack_Casual/SKM_F_Backpack_Casual_Bege",
           [("M_Bp", "Backpacks/Backpack_Casual/MI_F_Backpack_Casual_Bege"), ("M_Body", "Male_Body/MI_Quantum_Body_Afro")])
    # headwear: SKM cap Black; SM cap Black (duplicate) + SM cap Camo (SM-only colour); SM-only beret, no material
    w.mi("Head_Modul/Cap/MI_Cap_Black", {"BaseColor": "Head_Modul/Cap/T_Cap_Black_BaseColor"})
    w.mesh("Male/Head_Modul/Cap/SKM_Cap_Black", [("M_Cap", "Head_Modul/Cap/MI_Cap_Black")], shape=SHAPE_HAT)
    w.mesh("Male/Head_Modul/Cap/SM/SM_Cap_Black", [("M_Cap", "Head_Modul/Cap/MI_Cap_Black")], shape=SHAPE_HAT,
           cls="StaticMesh")
    w.mesh("Male/Head_Modul/Cap/SM/SM_Cap_Camo", [("M_Cap", "Head_Modul/Cap/MI_Cap_Black")], shape=SHAPE_HAT,
           cls="StaticMesh")
    w.mesh("Female/Head_Modules/SM/Beret/SM_F_Beret_Black", [("M_Beret3", "")], shape=SHAPE_HAT, cls="StaticMesh")
    # hair colour that only one haircut wears joins its family folder
    w.mi("Hair/Hair/MI_Hair_Long", {"Alpha": "Hair/T_hair_Alpha"}, base="Hair/Base/M_Hair_Master")
    w.mi("Hair/Hair/MI_Hair_Long_Red", {"Alpha": "Hair/T_hair_Alpha"}, base="Hair/Base/M_Hair_Master")
    w.mesh("Male/Head_Modul/Hairs/SKM_Hair_Long", [("M_Hair", "Hair/Hair/MI_Hair_Long")], verts=300)
    w.mesh("Male/Head_Modul/Hairs/SKM_Irokez_Red", [("M_Hair", "Hair/Hair/MI_Hair_Long_Red")], verts=400)
    # skipped things
    w.mi("Male_Body/Teeth/MI_Teeth", {"BaseColor": "Male_Body/Teeth_Textures/T_Teeth"}, base="Male_Body/Teeth/M_Teeth")
    w.mesh("Male/Body/Quantum_FaceRig/SKM_Quantum_FaceRig", [("t", "Male_Body/Teeth/MI_Teeth")], verts=900)
    w.mesh("Male/Preset/Characters/SKM_Preset_Male_01", [("x", "Cloth/Hoodie/MI_Hoodie_Black")], verts=999)
    w.mesh("Male/Body/European/SKM_Quantum_Legs_Survival", [("b", "Male_Body/MI_Quantum_Body_Afro")], verts=50)
    w.assets[f"{UE}/Demo/Textures/T_Button"] = {"class": "Texture2D", "file": "Export/x/T_Button.png"}
    w.texture("Hair/Base/T_Unused")
    w.assets[f"{UE}/Materials/Chest/Arafatka/MI_Scarf_Red"] = {"class": "MaterialInstanceConstant",
                                                               "file": "Export/x/MI_Scarf_Red.json"}
    w.materials[f"{UE}/Materials/Chest/Arafatka/MI_Scarf_Red"] = {"base_material": "x", "textures": {},
                                                                  "all_texture_params": {}}
    w.assets[f"{UE}/Demo/Animations/Walk"] = {"class": "AnimSequence", "file": "Export/x/Walk.fbx"}
    return w


class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.world = standard_world(Path(self.tmp.name))
        self.res = self.world.build()
        self.by_ue = {r.ue_asset: r for r in self.res.rows if r.ue_asset}

    def tearDown(self):
        self.tmp.cleanup()

    def row(self, rel: str) -> common.MapRow:
        return self.by_ue[f"{UE}/{rel}"]

    def test_every_mapped_asset_once_and_nothing_unclassified(self):
        mapped = [ue for ue, i in self.world.assets.items() if i["class"] in bm.MAPPED_CLASSES]
        self.assertEqual(sorted(mapped), sorted(self.by_ue))
        self.assertEqual(len([r for r in self.res.rows if r.ue_asset]), len(mapped))
        self.assertEqual(self.res.unclassified, [])
        self.assertNotIn(f"{UE}/Demo/Animations/Walk", self.by_ue)
        self.assertEqual(self.res.conflicts, [])

    def test_colour_variants_fold_into_base_with_old_default_colour(self):
        black = self.row("Mesh/Male/Cloth/Hoodie/SKM_Hoodie_Black")
        bordo = self.row("Mesh/Male/Cloth/Hoodie/SKM_Hoodie_Bordo")
        self.assertEqual((black.action, black.repo_path), ("replace", f"{Q}/Models/Clothing/Male/Tops/SKM_Hoodie.fbx"))
        self.assertEqual((bordo.action, bordo.base_of), ("skip", black.ue_asset))
        self.assertEqual(black.item_group, "Male/Tops/Hoodie")

    def test_different_geometry_is_its_own_item(self):
        long = self.row("Mesh/Male/Cloth/Hoodie/SKM_Hoodie_Long_Bege")
        self.assertEqual(long.action, "new")
        self.assertEqual(long.repo_path, f"{Q}/Models/Clothing/Male/Tops/SKM_Hoodie_Long.fbx")

    def test_new_female_item_and_colourway_paths(self):
        bp = self.row("Mesh/Female/Backpack_Modules/Backpack_Casual/SKM_F_Backpack_Casual_Bege")
        self.assertEqual((bp.action, bp.repo_path), ("new", f"{Q}/Models/Clothing/Female/Bags/SKM_F_Backpack_Casual.fbx"))
        mat = self.row("Materials/Backpacks/Backpack_Casual/MI_F_Backpack_Casual_Bege")
        self.assertEqual(mat.repo_path, f"{Q}/Materials/Clothing/Bags/F_Backpack_Casual/M_F_Backpack_Casual_Beige.mat")
        tex = self.row("Textures/Backpacks/T_F_Backpack_Bege")
        self.assertEqual(tex.repo_path, f"{Q}/Textures/Clothing/Bags/F_Backpack_Casual/T_F_Backpack_Bege.png")

    def test_material_replace_new_and_skin(self):
        self.assertEqual(self.row("Materials/Cloth/Hoodie/MI_Hoodie_Black").action, "replace")
        bordo = self.row("Materials/Cloth/Hoodie/MI_Hoodie_Bordo")
        self.assertEqual((bordo.action, bordo.repo_path),
                         ("new", f"{Q}/Materials/Clothing/Tops/Hoodie/M_Hoodie_Burgundy.mat"))
        skin = self.row("Materials/Male_Body/MI_Quantum_Body_Afro")
        self.assertEqual((skin.action, skin.repo_path), ("replace", f"{Q}/Materials/Characters/M_Quantum_Body_Afro.mat"))
        self.assertEqual(self.row("Textures/Male_Body/T_Body_Afro_BaseColor").repo_path,
                         f"{Q}/Textures/Afro/Body/T_Body_Afro_BaseColor.png")

    def test_shared_texture_appears_once(self):
        normal = self.row("Textures/Cloth/Hoodie/T_Hoodie_Normal_D")
        self.assertEqual(normal.repo_path, f"{Q}/Textures/Clothing/Tops/Hoodie/T_Hoodie_Normal_D.png")
        self.assertIn("2 material(s)", normal.notes)

    def test_static_mesh_duplicate_and_sm_only_colour(self):
        skm = self.row("Mesh/Male/Head_Modul/Cap/SKM_Cap_Black")
        self.assertEqual((skm.action, skm.repo_path), ("new", f"{Q}/Models/Clothing/Male/Headwear/SKM_Cap.fbx"))
        dup = self.row("Mesh/Male/Head_Modul/Cap/SM/SM_Cap_Black")
        self.assertEqual((dup.action, dup.base_of), ("skip", skm.ue_asset))
        self.assertIn("SM duplicate", dup.notes)
        camo = self.row("Mesh/Male/Head_Modul/Cap/SM/SM_Cap_Camo")
        self.assertEqual((camo.action, camo.repo_path), ("new", f"{Q}/Models/Clothing/Male/Headwear/SM_Cap.fbx"))
        self.assertNotIn("/Hats/", camo.repo_path)

    def test_sm_only_headwear_without_material(self):
        beret = self.row("Mesh/Female/Head_Modules/SM/Beret/SM_F_Beret_Black")
        self.assertEqual((beret.action, beret.repo_path),
                         ("new", f"{Q}/Models/Clothing/Female/Headwear/SM_F_Beret.fbx"))
        self.assertIn("NO MATERIAL", beret.notes)

    def test_hair_colour_joins_family_folder(self):
        red = self.row("Materials/Hair/Hair/MI_Hair_Long_Red")
        self.assertEqual(red.repo_path, f"{Q}/Materials/Hair/Hair_Long/M_Hair_Long_Red.mat")
        self.assertEqual(self.row("Mesh/Male/Head_Modul/Hairs/SKM_Irokez_Red").repo_path,
                         f"{Q}/Models/Clothing/Male/Hair/SKM_Irokez.fbx")

    def test_skips(self):
        self.assertIn("face rig", self.row("Mesh/Male/Body/Quantum_FaceRig/SKM_Quantum_FaceRig").notes)
        self.assertIn("Preset", self.row("Mesh/Male/Preset/Characters/SKM_Preset_Male_01").notes)
        self.assertIn("Survival", self.row("Mesh/Male/Body/European/SKM_Quantum_Legs_Survival").notes)
        self.assertIn("MetaHuman", self.row("Materials/Male_Body/Teeth/MI_Teeth").notes)
        self.assertIn("Arafatka", self.row("Materials/Chest/Arafatka/MI_Scarf_Red").notes)
        self.assertEqual(self.row("Textures/Hair/Base/T_Unused").action, "skip")
        self.assertIn("Demo", self.row("Demo/Textures/T_Button").notes)

    def test_keep_old_rows(self):
        keep = {r.repo_path: r for r in self.res.rows if r.action == "keep-old"}
        self.assertIn(f"{Q}/Models/Clothing/Male/Balaclava/SM_Balaclava_Crime.fbx", keep)
        self.assertIn(f"{Q}/Materials/Clothing/Balaclava/M_Balaclava_Crime_Black.mat", keep)
        self.assertIn(f"{Q}/Materials/Characters/M_Teeth.mat", keep)
        self.assertIn(f"{Q}/Materials/Clothing/Tops/Hoodie/M_Hoodie_Orange.mat", keep)       # Unity-only colour
        self.assertIn(f"{Q}/Textures/Clothing/Tops/Hoodie/T_Hoodie_Orange_BaseColor.png", keep)
        self.assertNotIn(f"{Q}/Materials/Clothing/Tops/Hoodie/M_Hoodie_Black.mat", keep)   # replaced instead
        self.assertTrue(all(r.ue_asset == "" for r in keep.values()))
        self.assertFalse(any(r.action == "retire" for r in self.res.rows))

    def test_deterministic_and_written_csv_round_trips(self):
        again = self.world.build()
        self.assertEqual(self.res.rows, again.rows)
        out = Path(self.tmp.name) / "map.csv"
        common.write_map(self.res.rows, out)
        self.assertEqual(common.read_map(out), self.res.rows)
        self.assertEqual(self.res.rows, sorted(self.res.rows, key=bm.row_sort_key))


class RealMap(unittest.TestCase):
    """Invariants of the committed tools/quantum/casual_bundle_map.csv against the real manifest (when present)."""

    def test_real_map_covers_manifest(self):
        if not (common.MAP_CSV.exists() and common.EXPORT_MANIFEST.exists()):
            self.skipTest("real map or export not available")
        rows = common.read_map()
        assets = common.load_manifest()["assets"]
        mapped = sorted(ue for ue, i in assets.items() if i["class"] in bm.MAPPED_CLASSES)
        self.assertEqual(sorted(r.ue_asset for r in rows if r.ue_asset), mapped)
        self.assertFalse([r for r in rows if "UNCLASSIFIED" in r.notes])
        self.assertFalse([r for r in rows if r.action in ("replace", "new") and "/Hats/" in r.repo_path])
        targets = [r.repo_path for r in rows if r.action in ("replace", "new", "keep-old")]
        self.assertEqual(len(targets), len(set(targets)))
        for r in rows:
            self.assertIn(r.action, common.ACTIONS)
            self.assertIn(r.role, common.ROLES)
            if r.base_of:
                self.assertEqual(r.action, "skip")


if __name__ == "__main__":
    unittest.main()
