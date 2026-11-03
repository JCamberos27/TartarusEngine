import contextlib
import csv
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import asset_manifest


def quiet(fn, *args, **kwargs):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        result = fn(*args, **kwargs)
    return result, out.getvalue()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def make_repo(root, files):
    """files: {project-relative path: bytes}; each gets a LF .meta with 2-space indent."""
    for rel, data in files.items():
        p = root / 'project' / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        Path(str(p) + '.meta').write_text('{\n  "guid": "abc",\n  "type": "model"\n}\n', newline='\n')


def external(root, rels):
    return patch.object(asset_manifest, 'external_files',
                        return_value=[('project/' + r, 'project/' + r) for r in rels])


class BuildTests(unittest.TestCase):
    def test_build_writes_payload_merge_preserving_and_removes_stale(self):
        with tempfile.TemporaryDirectory() as d:
            repo = Path(d)
            make_repo(repo, {'assets/A.fbx': b'aaaa', 'assets/Old.fbx': b'old'})
            (repo / 'project/assets/B.png').write_bytes(b'nometa')
            (repo / 'project/assets/Old.fbx.meta').write_text(
                '{\n    "payload": {"sha256": "x", "size": 1},\n    "guid": "g"\n}\n', newline='\n')
            with external(repo, ['assets/A.fbx', 'assets/B.png']):
                _, out = quiet(asset_manifest.build, str(repo))
                self.assertIn('1 without a .meta', out)
                self.assertIn('1 stale', out)
                meta = (repo / 'project/assets/A.fbx.meta').read_bytes().decode()
                self.assertEqual(meta, '{\n  "guid": "abc",\n  "type": "model",\n  "payload": {\n'
                                       f'    "sha256": "{sha(b"aaaa")}",\n    "size": 4\n  }}\n}}\n')
                self.assertEqual((repo / 'project/assets/Old.fbx.meta').read_text(), '{\n    "guid": "g"\n}\n')
                before = (repo / 'project/assets/A.fbx.meta').stat().st_mtime_ns
                _, out = quiet(asset_manifest.build, str(repo))  # rerun: nothing changes, uses the cache
                self.assertIn('0 .meta updated', out)
                self.assertEqual((repo / 'project/assets/A.fbx.meta').stat().st_mtime_ns, before)
            self.assertTrue((repo / asset_manifest.CACHE_REL).is_file())


class ImportCsvTests(unittest.TestCase):
    def test_import_csv_seeds_payload_without_hashing(self):
        with tempfile.TemporaryDirectory() as d:
            repo = Path(d)
            make_repo(repo, {'assets/A.fbx': b'aaaa'})
            with (repo / 'project/external_assets.csv').open('w', newline='') as f:
                w = csv.writer(f)
                w.writerow(['path', 'size', 'sha256'])
                w.writerow(['assets/A.fbx', 4, 'f' * 64])
                w.writerow(['assets/Gone.fbx', 1, 'e' * 64])
            _, out = quiet(asset_manifest.import_csv, str(repo))
            self.assertIn('1 .meta payloads seeded', out)
            self.assertIn('1 without a .meta', out)
            data = json.loads((repo / 'project/assets/A.fbx.meta').read_text())
            self.assertEqual(data['payload'], {'sha256': 'f' * 64, 'size': 4})
            self.assertEqual(list(data)[:2], ['guid', 'type'])


class StoreTests(unittest.TestCase):
    def test_export_verify_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            repo, store = root / 'repo', root / 'Store'
            rels = ['assets/Props/A.FBX', 'assets/Other/B.png']
            make_repo(repo, {rels[0]: b'aaaa', rels[1]: b'bbbbb'})
            (repo / '.git').mkdir()
            with external(repo, rels):
                quiet(asset_manifest.build, str(repo))
                _, out = quiet(asset_manifest.export, str(repo), str(store), 'project/assets/Props')
                self.assertIn('1 copied', out)
                self.assertTrue((store / sha(b'aaaa')[:2] / (sha(b'aaaa') + '.fbx')).is_file())
                self.assertEqual(quiet(asset_manifest.verify, str(store), False, str(repo))[0], 1)  # B missing
                _, out = quiet(asset_manifest.export, str(repo), str(store))
                self.assertIn('1 copied, 1 already', out)
            code, out = quiet(asset_manifest.verify, str(store), False, str(repo))
            self.assertEqual(code, 0)
            self.assertIn('0 unreferenced', out)
            (store / 'zz').mkdir()
            (store / 'zz' / ('d' * 64 + '.png')).write_bytes(b'x')
            self.assertIn('1 unreferenced', quiet(asset_manifest.verify, str(store), False, str(repo))[1])
            self.assertEqual(quiet(asset_manifest.verify, str(repo), True)[0], 0)
            (repo / 'project' / rels[1]).write_bytes(b'ccccc')  # same size, different content
            self.assertEqual(quiet(asset_manifest.verify, str(repo), False)[0], 0)
            code, out = quiet(asset_manifest.verify, str(repo), True)
            self.assertEqual(code, 1)
            self.assertIn('1 different', out)
            (repo / 'project' / rels[0]).unlink()
            self.assertIn('1 missing', quiet(asset_manifest.verify, str(repo), False)[1])

    def _used(self, root):
        used = root / 'Used'
        for rel, data in {'assets/A.fbx': b'aaaa', 'assets/Dir/A2.fbx': b'aaaa', 'assets/B.PNG': b'bb'}.items():
            (used / 'project' / rel).parent.mkdir(parents=True, exist_ok=True)
            (used / 'project' / rel).write_bytes(data)
        return used

    def test_migrate_copy_with_duplicate(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            used, store = self._used(root), root / 'Store'
            _, out = quiet(asset_manifest.migrate_store, str(used), str(store), True, False)
            self.assertIn('2 would be copied', out)
            self.assertFalse(store.exists())
            _, out = quiet(asset_manifest.migrate_store, str(used), str(store))
            self.assertIn('2 copied', out)
            self.assertIn('1 duplicate', out)
            self.assertEqual(len(list(used.rglob('*.*'))), 3)
            lines = (store / 'store_migration.log').read_text().splitlines()
            self.assertEqual(len(lines), 2)
            self.assertIn(f'{sha(b"bb")}\t.png\tproject/assets/B.PNG', lines)
            self.assertTrue((store / sha(b'bb')[:2] / (sha(b'bb') + '.png')).is_file())

    def test_migrate_move_leaves_duplicate(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            used, store = self._used(root), root / 'Store'
            _, out = quiet(asset_manifest.migrate_store, str(used), str(store), False, True)
            self.assertIn('2 moved', out)
            left = [p for p in used.rglob('*') if p.is_file()]
            self.assertEqual(len(left), 1)  # the duplicate stays where it was
            self.assertTrue((store / sha(b'aaaa')[:2] / (sha(b'aaaa') + '.fbx')).is_file())


if __name__ == '__main__':
    unittest.main()
