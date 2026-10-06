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


if __name__ == '__main__':
    unittest.main()
