"""Tests for metas.py (python -m unittest, from tools/quantum). Temp dirs only; the engine is never run."""
import os
import tempfile
import unittest
from pathlib import Path

from casual_bundle import common, metas

REPORT = """\
[info]  Assets: something unrelated
[ModelReport] D:/Packs/Quantum - Modular Casual Bundle/Export/Mesh/Male/Cloth/Hoodie/SKM_Hoodie_Black.fbx size 1.027 0.642 0.334 min -0.513 0.916 -0.143 max 0.514 1.558 0.191 meshes 1 materials MI_Hoodie_Black
[ModelReport]   tris: 17748
[ModelReport]   skin: 9771 vertices, 0 over 8 influences (most 8), largest weight lost 0.0%
[ModelReport]   bind: root 0.000 0.000 0.000 pelvis 0.000 0.959 0.023
[warn]  Model 'x.fbx' has more than 512 bones
[ModelReport] D:/Packs/Body.fbx size 1.108 1.809 0.410 min -0.554 -0.003 -0.129 max 0.554 1.806 0.281 meshes 11 materials M_EyeOcclusion|MI_EyeRefractive_Inst|MI_EyeRefractive_Inst_4|Fbx Default Material
[ModelReport] D:/Packs/Empty.fbx size 0.000 0.000 0.000 min 0.000 0.000 0.000 max 0.000 0.000 0.000 meshes 1 materials
[ModelReport] FAILED D:/Packs/Broken.fbx
[ModelReport] 4 models, 1 failed
"""

BODY_SLOTS = [  # SKM_Quantum_Body_Full's first slots, as manifest.json has them
    {"slot": "M_Eyeshell", "material": "/Game/Casual_Wear_Bundle/Materials/Eyes/M_EyeOcclusion"},
    {"slot": "M_Saliva", "material": "/Game/Casual_Wear_Bundle/Materials/Eyes/MI_lacrimal_fluid_Inst"},
    {"slot": "M_Teeth", "material": "/Game/Casual_Wear_Bundle/Materials/Teeth/M_Teeth"},
    {"slot": "M_EyeRight", "material": "/Game/Casual_Wear_Bundle/Materials/Eyes/MI_EyeRefractive_Inst"},
    {"slot": "M_EyeLeft", "material": "/Game/Casual_Wear_Bundle/Materials/Eyes/MI_EyeRefractive_Inst"},
    {"slot": "M_EyeEdge", "material": "/Game/Casual_Wear_Bundle/Materials/Eyes/MI_lacrimal_fluid_Inst"},
    {"slot": "M_Quantum_Body", "material": "/Game/Casual_Wear_Bundle/Materials/Body/MI_Quantum_Body.MI_Quantum_Body"},
]


class ReportParseTests(unittest.TestCase):
    def test_headers_parsed_detail_and_logs_ignored(self):
        r = metas.parse_model_report(REPORT)
        self.assertEqual(r["D:/Packs/Quantum - Modular Casual Bundle/Export/Mesh/Male/Cloth/Hoodie/SKM_Hoodie_Black.fbx"],
                         ["MI_Hoodie_Black"])  # a path with spaces and " - " in it
        self.assertEqual(r["D:/Packs/Body.fbx"],
                         ["M_EyeOcclusion", "MI_EyeRefractive_Inst", "MI_EyeRefractive_Inst_4", "Fbx Default Material"])
        self.assertEqual(r["D:/Packs/Empty.fbx"], [])
        self.assertIsNone(r["D:/Packs/Broken.fbx"])
        self.assertEqual(len(r), 4)

    def test_chunks_respect_command_line_limit(self):
        paths = ["x" * 1000] * 70
        batches = metas._chunks(paths, 100)
        self.assertEqual(sum(len(b) for b in batches), 70)
        self.assertGreater(len(batches), 1)
        for b in batches:
            self.assertLess(100 + sum(len(p) + 3 for p in b), metas._MAX_CMDLINE + 1)


class SlotNameTests(unittest.TestCase):
    def test_fbx_names_are_material_names_with_index_suffix_for_repeats(self):
        self.assertEqual(metas.predict_fbx_material_names(BODY_SLOTS),
                         ["M_EyeOcclusion", "MI_lacrimal_fluid_Inst", "M_Teeth", "MI_EyeRefractive_Inst",
                          "MI_EyeRefractive_Inst_4", "MI_lacrimal_fluid_Inst_5", "MI_Quantum_Body"])

    def test_slot_without_material_is_the_fbx_default(self):
        slots = [{"slot": "lambert40", "material": "/Game/X/MI_F_Haircut_Bobcut"}, {"slot": "M_Beret4", "material": ""},
                 {"slot": "Default_Material3", "material": None}]
        self.assertEqual(metas.predict_fbx_material_names(slots),
                         ["MI_F_Haircut_Bobcut", metas.FBX_DEFAULT_MATERIAL, metas.FBX_DEFAULT_MATERIAL])

    def test_match_slots(self):
        reported = ["M_EyeOcclusion", "MI_lacrimal_fluid_Inst", "M_Teeth", "MI_EyeRefractive_Inst",
                    "MI_EyeRefractive_Inst_4", "MI_Quantum_Body"]          # M_EyeEdge has no geometry
        pairs, unused, problems = metas.match_slots(BODY_SLOTS, reported)
        self.assertEqual(pairs["M_EyeLeft"], "MI_EyeRefractive_Inst_4")
        self.assertEqual(pairs["M_Quantum_Body"], "MI_Quantum_Body")
        self.assertEqual(unused, ["M_EyeEdge"])
        self.assertEqual(problems, [])
        # A name no slot explains is a problem; a failed import is one too.
        _, _, problems = metas.match_slots(BODY_SLOTS, reported + ["lambert9"])
        self.assertEqual(len(problems), 1)
        self.assertEqual(len(metas.match_slots(BODY_SLOTS, None)[2]), 1)

    def test_default_material_with_slot_index(self):
        slots = [{"slot": "M_Quantum_Arms3", "material": "/Game/X/MI_Quantum_Old_Arms"},
                 {"slot": "M_Quantum_Body11", "material": ""}]
        pairs, unused, problems = metas.match_slots(slots, ["MI_Quantum_Old_Arms", "Fbx Default Material 1"])
        self.assertEqual(pairs, {"M_Quantum_Arms3": "MI_Quantum_Old_Arms", "M_Quantum_Body11": "Fbx Default Material 1"})
        self.assertEqual((unused, problems), ([], []))

    def test_repeated_slot_name_after_an_empty_slot(self):
        # The first 'M_Body' has no geometry (unmatched); the second must still key as 'M_Body#1', the key the importer
        # rebuilds with slot_keys(), or its remap is silently dropped.
        slots = [{"slot": "M_Body", "material": "/Game/X/MI_Unused"}, {"slot": "M_Body", "material": "/Game/X/MI_Skin"}]
        pairs, unused, problems = metas.match_slots(slots, ["MI_Skin"])
        self.assertEqual(metas.slot_keys(slots), ["M_Body", "M_Body#1"])
        self.assertEqual(pairs, {"M_Body#1": "MI_Skin"})
        self.assertEqual((unused, problems), (["M_Body"], []))


class FbxMaterialNamesTests(unittest.TestCase):
    def test_batched_run_and_cache(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            a, b = root / "A b.fbx", root / "B.fbx"
            a.write_bytes(b"a")
            b.write_bytes(b"bb")
            cache = root / "fbx_materials.json"
            calls = []

            def runner(inputs, exe):
                calls.append(list(inputs))
                return "\n".join(f"[ModelReport] {Path(p).as_posix()} size 1 1 1 min 0 0 0 max 1 1 1 meshes 1 "
                                 f"materials M_{Path(p).stem.replace(' ', '')}|M_Other" for p in inputs)

            r = metas.fbx_material_names([a, b], cache=cache, runner=runner)
            self.assertEqual(r[a], ["M_Ab", "M_Other"])
            self.assertEqual(len(calls), 1)                  # one process for both files
            self.assertEqual(len(calls[0]), 2)
            self.assertTrue(cache.exists())

            r2 = metas.fbx_material_names([a, b], cache=cache, runner=runner)
            self.assertEqual(r, r2)
            self.assertEqual(len(calls), 1)                  # all from the cache

            b.write_bytes(b"changed!")                       # new size: re-reported, alone
            metas.fbx_material_names([a, b], cache=cache, runner=runner)
            self.assertEqual(calls[-1], [str(b)])

    def test_unreported_file_raises(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "X.fbx"
            f.write_bytes(b"x")
            with self.assertRaises(RuntimeError):
                metas.fbx_material_names([f], cache=None, runner=lambda inputs, exe: "[ModelReport] 0 models, 0 failed")


class ModelMetaTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.project = Path(self._tmp.name) / "project"
        self.rel = "assets/Characters/Quantum/Models/European/Quantum_Head.fbx"
        (self.project / self.rel).parent.mkdir(parents=True)
        (self.project / self.rel).write_bytes(b"fbx")

    def tearDown(self):
        self._tmp.cleanup()

    def meta(self):
        return common.read_json(self.project / (self.rel + ".meta"))

    def test_keeps_guid_and_importer_replaces_remap(self):
        common.write_json(self.project / (self.rel + ".meta"), {
            "folder": "wrong/folder", "guid": "0123456789abcdef", "displayName": "Head",
            "importer": {"simplify": 0.4, "skinIgnoresMeshNode": True},
            "materialRemap": {"M_Old": "assets/Old.mat", "M_Quantum_Head": "assets/H.mat"},
            "metaVersion": 1, "type": "model"})
        data = metas.write_model_meta(self.rel, {"MI_Quantum_Head": "assets/Characters/Quantum/Materials/H.mat"},
                                      importer_defaults={"simplify": 1.0, "upAxis": "Z"},
                                      project=self.project)
        m = self.meta()
        self.assertEqual(m, data)
        self.assertEqual(m["guid"], "0123456789abcdef")
        self.assertEqual(m["importer"], {"simplify": 0.4, "skinIgnoresMeshNode": True, "upAxis": "Z"})  # existing wins
        self.assertEqual(m["materialRemap"], {"MI_Quantum_Head": "assets/Characters/Quantum/Materials/H.mat"})
        self.assertEqual(m["folder"], "assets/Characters/Quantum/Models/European")
        self.assertEqual(m["displayName"], "Head")
        # An explicit override beats the existing key.
        metas.write_model_meta(self.rel, {"A": "b.mat"}, importer_overrides={"simplify": 0.5}, project=self.project)
        self.assertEqual(self.meta()["importer"]["simplify"], 0.5)

    def test_new_meta_and_no_rewrite_when_unchanged(self):
        data = metas.write_model_meta(self.rel, {}, project=self.project)
        self.assertEqual(len(data["guid"]), 16)
        self.assertNotIn("importer", data)
        self.assertNotIn("materialRemap", data)
        self.assertEqual((data["type"], data["metaVersion"]), ("model", 1))
        meta = self.project / (self.rel + ".meta")
        os.utime(meta, ns=(1_000_000_000, 1_000_000_000))
        metas.write_model_meta(self.rel, {}, project=self.project)
        self.assertEqual(meta.stat().st_mtime_ns, 1_000_000_000)       # same content: file left alone
        self.assertTrue(meta.read_bytes().endswith(b"}\n"))            # engine style: LF + trailing newline


if __name__ == "__main__":
    unittest.main()
