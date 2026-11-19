"""Tests for csvrows.py (python -m unittest, from tools/quantum). Temp dirs only; never the real CSV or the Drive."""
import csv
import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from casual_bundle import csvrows

Q = "assets/Characters/Quantum"


def _sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


class SwapRowsTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.project = Path(self._tmp.name) / "project"
        self.csv = self.project / "external_assets.csv"
        files = {
            "assets/Audio/a.wav": b"aa",
            f"{Q}/Models/Old/SKM_Old.fbx": b"old",              # retired by prefix
            f"{Q}/Textures/T_Gone.png": b"gone",               # retired by path
            f"{Q}/Textures/T_Same.png": b"same",               # re-listed, unchanged
            f"{Q}/Textures/T_Recooked.png": b"v1",             # re-listed with new content
            "assets/Zeta/z.fbx": b"z",
        }
        self.project.mkdir(parents=True)
        with open(self.csv, "w", encoding="utf-8", newline="") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(["path", "size", "sha256"])
            for p, data in files.items():   # deliberately unsorted, like a hand-inserted row
                w.writerow([p, len(data), _sha(data)])
        for p, data in files.items():
            (self.project / p).parent.mkdir(parents=True, exist_ok=True)
            (self.project / p).write_bytes(data)
        (self.project / f"{Q}/Textures/T_Recooked.png").write_bytes(b"version 2")
        (self.project / f"{Q}/Textures/Hats/T_Cap, Black.png").parent.mkdir(parents=True)
        (self.project / f"{Q}/Textures/Hats/T_Cap, Black.png").write_bytes(b"cap")   # a comma: csv quoting

    def tearDown(self):
        self._tmp.cleanup()

    def test_swap(self):
        before = self.csv.read_bytes()
        adds = [f"{Q}/Textures/T_Same.png", f"{Q}/Textures/T_Recooked.png", f"{Q}/Textures/Hats/T_Cap, Black.png"]
        removes = [f"{Q}/Models/Old/", f"{Q}/textures/t_gone.png", f"{Q}/Textures/T_Recooked.png"]
        diff = csvrows.swap_rows(self.csv, removes, adds, dry_run=True)
        self.assertEqual(self.csv.read_bytes(), before)                     # dry run writes nothing
        self.assertFalse(diff.written)

        diff = csvrows.swap_rows(self.csv, removes, adds)
        self.assertEqual(diff.removed, [f"{Q}/Models/Old/SKM_Old.fbx", f"{Q}/Textures/T_Gone.png"])
        self.assertEqual(diff.added, [f"{Q}/Textures/Hats/T_Cap, Black.png"])
        self.assertEqual(diff.changed, [f"{Q}/Textures/T_Recooked.png"])   # removed AND added = changed
        self.assertEqual(diff.unchanged, [f"{Q}/Textures/T_Same.png"])
        self.assertEqual((diff.rows_before, diff.rows_after), (6, 5))
        self.assertIn("5 rows", diff.summary())

        rows = csvrows.read_rows(self.csv)
        self.assertEqual([r.path for r in rows], sorted((r.path for r in rows), key=lambda p: ("project/" + p).lower()))
        by_path = {r.path: r for r in rows}
        self.assertEqual(by_path[f"{Q}/Textures/T_Recooked.png"].sha256, _sha(b"version 2"))
        self.assertEqual(by_path[f"{Q}/Textures/T_Recooked.png"].size, 9)
        text = self.csv.read_text(encoding="utf-8")
        self.assertTrue(text.startswith("path,size,sha256\n"))
        self.assertNotIn("\r", text)
        self.assertIn(f'"{Q}/Textures/Hats/T_Cap, Black.png"', text)

    def test_missing_payload_raises_before_writing(self):
        before = self.csv.read_bytes()
        with self.assertRaises(FileNotFoundError):
            csvrows.swap_rows(self.csv, [f"{Q}/"], [f"{Q}/Textures/T_NotThere.png"])
        self.assertEqual(self.csv.read_bytes(), before)


class UsedTests(unittest.TestCase):
    def test_retired_used_list(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "retired_used_paths.txt"
            lines = csvrows.write_retired_used_list(
                [f"{Q}/Textures/T_B.png", f"{Q}/Textures/T_B.png.meta", f"{Q}/Models/A.fbx"], out)
            self.assertEqual(lines, ["Used\\project\\assets\\Characters\\Quantum\\Models\\A.fbx",
                                     "Used\\project\\assets\\Characters\\Quantum\\Textures\\T_B.png"])
            body = [l for l in out.read_text(encoding="utf-8").splitlines() if not l.startswith("#")]
            self.assertEqual(body, lines)
            # A re-run keeps the earlier runs' entries (their files still have to leave the Drive).
            again = csvrows.write_retired_used_list([f"{Q}/Textures/T_C.png"], out)
            self.assertEqual(again, lines + ["Used\\project\\assets\\Characters\\Quantum\\Textures\\T_C.png"])

    def test_mirror_copies_only_what_differs(self):
        with tempfile.TemporaryDirectory() as d:
            project, used = Path(d) / "project", Path(d) / "Used"
            for name, data in (("a.png", b"new"), ("b.png", b"same")):
                (project / Q / name).parent.mkdir(parents=True, exist_ok=True)
                (project / Q / name).write_bytes(data)
            (used / "project" / Q).mkdir(parents=True)
            (used / "project" / Q / "a.png").write_bytes(b"old")
            (used / "project" / Q / "b.png").write_bytes(b"same")
            self.assertEqual(csvrows.mirror_to_used([f"{Q}/a.png", f"{Q}/b.png"], used, project), (1, 1))
            self.assertEqual((used / "project" / Q / "a.png").read_bytes(), b"new")


class ExclusionTests(unittest.TestCase):
    def test_exclusions_hitting_the_pack(self):
        import asset_manifest   # csvrows.manifest_exclusions put tools/assets on sys.path
        with patch.object(asset_manifest, "EXCLUDE_PREFIXES", (f"project/{Q}/Textures/Clothing/Hats/",
                                                               "project/assets/Characters/ybot/")):
            self.assertEqual(csvrows.manifest_exclusions(), [f"project/{Q}/Textures/Clothing/Hats/"])

    def setUp(self):
        csvrows.manifest_exclusions()   # imports asset_manifest (read-only)


if __name__ == "__main__":
    unittest.main()
