"""Tests for refs.py (python -m unittest, from tools/quantum): a made-up repo in a temp dir, never the real one."""
import json
import tempfile
import unittest
from pathlib import Path

from casual_bundle import refs

Q = "assets/Characters/Quantum"
G = {  # GUIDs of the fake pack's .meta files
    "SKM_A": "a000000000000001", "SKM_Old": "a000000000000002", "Body": "a000000000000003",
    "T_A_N": "b000000000000001", "T_Scene": "b000000000000002", "T_Entity": "b000000000000003",
}


def _w(path: Path, text) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text if isinstance(text, str) else json.dumps(text, indent=2), encoding="utf-8")


def _mat(*textures, guids=None) -> dict:
    return {"matVersion": 2, "properties": {f"_Map{i}": t for i, t in enumerate(textures)},
            "textureGuids": guids or {}}


def build_repo(root: Path) -> None:
    """A tiny pack with every kind of reference refs.py knows about."""
    p = root / "project"
    pack = p / Q
    files = {
        # payloads (content irrelevant)
        "Models/Clothing/Male/Tops/SKM_A.fbx": "", "Models/Clothing/Male/Tops/SKM_Old.fbx": "",
        "Models/European/Body.fbx": "",
        "Textures/T_A_Black.png": "", "Textures/T_A_Red.png": "", "Textures/T_A_N.png": "",
        "Textures/T_Old.png": "", "Textures/T_Orphan.png": "", "Textures/T_Scene.png": "",
        "Textures/T_Case.png": "", "Textures/T_Skip.png": "", "Textures/T_Entity.png": "",
        "Textures/Skin/T_Body.png": "", "Textures/Skin/T_Body_Afro.png": "",
        # materials
        "Materials/Clothing/Tops/A/M_A_Black.mat": _mat(f"{Q}/Textures/T_A_Black.png", guids={"_Map9": G["T_A_N"]}),
        "Materials/Clothing/Tops/A/M_A_Red.mat": _mat(f"{Q}/Textures/T_A_Red.png"),          # colourway sibling
        "Materials/Clothing/Tops/Old/M_Old.mat": _mat(f"{Q}/Textures/T_Old.png"),
        "Materials/Characters/M_Quantum_Body.mat": _mat(f"{Q}/Textures/Skin/T_Body.png"),
        "Materials/Characters/M_Quantum_Body_Afro.mat": _mat(f"{Q}/Textures/Skin/T_Body_Afro.png"),  # skin variant
        "Materials/Characters/M_Eye.mat": _mat(),                                             # nobody's colourway
        # the wardrobe: root-relative model paths, a folder catalog, skins
        "Quantum.wardrobe": {"root": Q, "itemFolders": ["Models/Clothing"], "excludeFolders": ["Hats"],
                             "skinMaterials": ["M_Quantum_Body"],
                             "bodies": {"Male": {"parts": [{"part": "Torso", "model": "Models/European/Body.fbx"}],
                                                 "races": [{"name": "European", "skin": ""},
                                                           {"name": "Afro", "skin": "_Afro"}]}}},
    }
    for rel, content in files.items():
        _w(pack / rel, content)
    metas = {
        "Models/Clothing/Male/Tops/SKM_A.fbx": {"guid": G["SKM_A"], "type": "model", "folder": f"{Q}/Models/Clothing",
                                                "materialRemap": {"M_A": f"{Q}/Materials/Clothing/Tops/A/M_A_Black.mat"}},
        "Models/Clothing/Male/Tops/SKM_Old.fbx": {"guid": G["SKM_Old"], "type": "model",
                                                  "materialRemap": {"M_O": f"{Q}/Materials/Clothing/Tops/Old/M_Old.mat"}},
        "Models/European/Body.fbx": {"guid": G["Body"], "type": "model",
                                     "materialRemap": {"M_Quantum_Body": f"{Q}/Materials/Characters/M_Quantum_Body.mat"}},
        "Textures/T_A_N.png": {"guid": G["T_A_N"], "type": "texture"},
        "Textures/T_Scene.png": {"guid": G["T_Scene"], "type": "texture"},
        "Textures/T_Entity.png": {"guid": G["T_Entity"], "type": "texture"},
        "Textures/T_Gone.png": {"guid": "c000000000000001", "type": "texture"},   # orphan .meta, payload gone
    }
    for rel, m in metas.items():
        _w(pack / (rel + ".meta"), m)
    # Payload listed in the CSV but not on this machine.
    _w(p / "external_assets.csv", f"path,size,sha256\nassets/Other/X.fbx,1,00\n{Q}/Textures/T_CsvOnly.png,1,00\n")
    # Referencing files outside the pack.
    _w(p / "assets/Characters/Outfits/Q/One.outfit",
       {"wardrobe": f"{Q}/Quantum.wardrobe", "items": {"Top": {"item": f"{Q}/Models/Clothing/Male/Tops/SKM_A.fbx"}},
        "missing": f"{Q}/Models/Clothing/Male/Tops/SKM_Never.fbx"})
    _w(p / "scenes/S.json", '{\n  "entities": [\n    {\n      "guid": "%s",\n      "texture": {"path": "", "pathGuid": "%s"}\n    }\n  ],\n'
       '  "folders": ["%s/Models/Clothing/Male"]\n}\n' % (G["T_Entity"], G["T_Scene"], Q))
    _w(root / "src/Game/X.cpp", 'const char* kPath = "assets/characters/quantum/Textures/T_Case.png"; // %s/Textures/T_Orphan.png\n' % Q)
    _w(root / "tools/quantum/casual_bundle/fake.py", f'P = "{Q}/Textures/T_Skip.png"\n')   # the importer's own code
    _w(root / "build/Release/assets/copy.mat", _mat(f"{Q}/Textures/T_Orphan.png"))       # build output


class RefsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls._tmp.name)
        build_repo(cls.root)
        cls.index = refs.scan(cls.root)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def kinds(self, target):
        return sorted({r.kind for r in self.index.refs_by_target.get(f"{Q}/{target}", [])})

    def test_inventory(self):
        ix = self.index
        self.assertIn(f"{Q}/Textures/T_CsvOnly.png", ix.quantum_files)           # CSV-only payload
        self.assertIn(f"{Q}/Textures/T_Gone.png", ix.assets)                     # orphan .meta's asset
        self.assertNotIn("assets/Other/X.fbx", ix.quantum_files)
        self.assertEqual(ix.guid_to_path[G["SKM_A"]], f"{Q}/Models/Clothing/Male/Tops/SKM_A.fbx")

    def test_reference_kinds(self):
        self.assertEqual(self.kinds("Models/Clothing/Male/Tops/SKM_A.fbx"), ["path"])
        self.assertEqual(self.kinds("Textures/T_A_N.png"), ["guid"])             # .mat textureGuids
        self.assertEqual(self.kinds("Textures/T_Scene.png"), ["guid"])           # scene pathGuid
        self.assertEqual(self.kinds("Textures/T_Case.png"), ["path-ci"])         # quoted, wrong case
        self.assertEqual(self.kinds("Models/European/Body.fbx"), ["path"])       # wardrobe root-relative
        self.assertEqual(self.kinds("Models/Clothing"), ["folder"])              # wardrobe itemFolders
        self.assertEqual(self.kinds("Models/Clothing/Male"), ["folder"])
        self.assertEqual(self.kinds("Models/Clothing/Male/Tops/SKM_Never.fbx"), ["missing"])
        # Not references: an entity's own "guid", a comment, this package's code, build output, a meta's "folder".
        self.assertEqual(self.kinds("Textures/T_Entity.png"), [])
        self.assertEqual(self.kinds("Textures/T_Orphan.png"), [])
        self.assertEqual(self.kinds("Textures/T_Skip.png"), [])
        self.assertFalse(any(r.file.endswith("SKM_A.fbx.meta") and r.kind == "folder" for r in self.index.refs))
        self.assertIn(G["T_Scene"], self.index.refs_by_guid)

    def test_retire_marks_transitively(self):
        retired = refs.retire_candidates(self.index)
        expect = {f"{Q}/{p}" for p in (
            "Models/Clothing/Male/Tops/SKM_Old.fbx", "Materials/Clothing/Tops/Old/M_Old.mat", "Textures/T_Old.png",
            "Textures/T_Orphan.png", "Textures/T_Skip.png", "Textures/T_Entity.png", "Textures/T_CsvOnly.png",
            "Textures/T_Gone.png", "Materials/Characters/M_Eye.mat")}
        self.assertEqual(set(retired), expect)
        # Alive: the colourway sibling and its texture, the skin variant and its texture, GUID-only references.
        for p in ("Materials/Clothing/Tops/A/M_A_Red.mat", "Textures/T_A_Red.png", "Textures/T_A_N.png",
                  "Materials/Characters/M_Quantum_Body_Afro.mat", "Textures/Skin/T_Body_Afro.png",
                  "Textures/T_Scene.png", "Textures/T_Case.png", "Models/European/Body.fbx"):
            self.assertNotIn(f"{Q}/{p}", retired)

    def test_keep_will_exist_rewrites_catalog(self):
        old = f"{Q}/Models/Clothing/Male/Tops/SKM_Old.fbx"
        retired = refs.retire_candidates(self.index, keep={old})
        self.assertNotIn(f"{Q}/Textures/T_Old.png", retired)                     # kept through its .meta and .mat
        retired = refs.retire_candidates(self.index, keep={f"{Q}/Textures/"})    # folder prefix
        self.assertFalse(any(p.startswith(f"{Q}/Textures/") for p in retired))
        retired = refs.retire_candidates(self.index, will_exist={f"{Q}/materials/clothing/tops/old/M_Old.mat"})
        self.assertNotIn(f"{Q}/Textures/T_Old.png", retired)                     # case-blind
        self.assertIn(old, retired)
        # The import rewrites Body.fbx's remap to nothing: the skin chain goes.
        retired = refs.retire_candidates(self.index, rewrites={f"{Q}/Models/European/Body.fbx": set()})
        self.assertIn(f"{Q}/Materials/Characters/M_Quantum_Body_Afro.mat", retired)
        self.assertIn(f"{Q}/Textures/Skin/T_Body.png", retired)
        retired = refs.retire_candidates(self.index, catalog_is_reference=True)
        self.assertNotIn(old, retired)

    def test_dangling(self):
        retired = refs.retire_candidates(self.index) + [f"{Q}/Textures/T_Scene.png"]
        d = refs.dangling(self.index, retired=retired)
        targets = {(r.file, r.target) for r in d}
        self.assertIn(("project/scenes/S.json", f"{Q}/Textures/T_Scene.png"), targets)      # retired: dangles
        self.assertIn(("project/assets/Characters/Outfits/Q/One.outfit",
                       f"{Q}/Models/Clothing/Male/Tops/SKM_Never.fbx"), targets)           # missing today
        self.assertFalse(any(r.file.endswith("M_Old.mat") for r in d))                     # retired files don't count
        d = refs.dangling(self.index, will_exist={f"{Q}/Models/Clothing/Male/Tops/SKM_Never.fbx"}, retired=retired)
        self.assertNotIn(f"{Q}/Models/Clothing/Male/Tops/SKM_Never.fbx", {r.target for r in d})
        # A folder dangles once nothing is left under it.
        everything = sorted(self.index.assets)
        d = refs.dangling(self.index, retired=everything)
        self.assertIn(f"{Q}/Models/Clothing/Male", {r.target for r in d if r.kind == "folder"})

    def test_summary(self):
        s = refs.summary(self.index)
        self.assertEqual(s["csv_only"], 1)
        self.assertGreater(s["refs_by_source_type"]["mat"], 0)
        self.assertEqual(s["top_files"][0][1], max(n for _, n in s["top_files"]))
        self.assertEqual(refs.source_type("project/scenes/S.json"), "scene")
        self.assertEqual(refs.source_type("tools/scenes/sandbox_template.json"), "template")
        self.assertEqual(refs.owner_of(f"project/{Q}/Models/A.fbx.meta"), f"{Q}/Models/A.fbx")
        self.assertIsNone(refs.owner_of("project/scenes/S.json"))


if __name__ == "__main__":
    unittest.main()
