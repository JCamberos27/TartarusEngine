import contextlib
import csv
import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import asset_manifest


class AssetExportTests(unittest.TestCase):
    def test_same_size_changed_payload_is_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo, used = root / 'repo', root / 'Used'
            rel = 'project/assets/Weapons/Example.fbx'
            source, destination = repo / rel, used / rel
            source.parent.mkdir(parents=True)
            destination.parent.mkdir(parents=True)
            source.write_bytes(b'new!')
            destination.write_bytes(b'old!')
            with (repo / 'project/external_assets.csv').open('w', newline='') as f:
                writer = csv.writer(f)
                writer.writerow(['path', 'size', 'sha256'])
                writer.writerow([rel[len('project/'):], 4, hashlib.sha256(b'new!').hexdigest()])
            with patch.object(asset_manifest, 'external_files', return_value=[(rel, rel)]):
                with contextlib.redirect_stdout(io.StringIO()):
                    asset_manifest.export(str(repo), str(used))
            self.assertEqual(destination.read_bytes(), b'new!')
            self.assertEqual((used / 'project/external_assets.csv').read_bytes(),
                             (repo / 'project/external_assets.csv').read_bytes())


class AssetVerifyTests(unittest.TestCase):
    def _folder(self, root, extra_git=False):
        rel = 'project/assets/Weapons/Example.fbx'
        (root / rel).parent.mkdir(parents=True)
        (root / rel).write_bytes(b'data')
        with (root / 'project/external_assets.csv').open('w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['path', 'size', 'sha256'])
            writer.writerow([rel[len('project/'):], 4, hashlib.sha256(b'data').hexdigest()])
        if extra_git:
            (root / '.git').mkdir()
        # Not an asset: a script and a stale .meta, as were found on the Drive.
        (root / 'project/assets/Scripts').mkdir(parents=True)
        (root / 'project/assets/Scripts/NpcController.cs').write_text('class NpcController {}')
        (root / 'project/assets/Weapons/Example.fbx.meta').write_text('{}')

    def test_used_folder_with_unlisted_files_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            used = Path(directory)
            self._folder(used)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                result = asset_manifest.verify(str(used), True)
            self.assertEqual(result, 1)
            self.assertIn('2 unlisted', out.getvalue())
            self.assertIn('assets/Scripts/NpcController.cs', out.getvalue())

    def test_repo_checkout_is_not_checked_for_unlisted_files(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            self._folder(repo, extra_git=True)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(asset_manifest.verify(str(repo), True), 0)


if __name__ == '__main__':
    unittest.main()
