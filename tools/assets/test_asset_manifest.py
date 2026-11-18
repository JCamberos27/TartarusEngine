import contextlib
import csv
import hashlib
import io
import shutil
import subprocess
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


GIT = shutil.which('git')


@unittest.skipUnless(GIT, 'prune needs git')
class AssetPruneTests(unittest.TestCase):
    """prune lists ignored payloads the CSV doesn't list (a retired pack file still on disk) and deletes them only
    with --apply; listed payloads, tracked files, .meta files and local leftovers are never touched."""

    def test_prune_dry_run_then_apply(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            subprocess.run([GIT, 'init', '-q', str(repo)], check=True)
            (repo / '.gitignore').write_text('project/assets/**/*.png\nproject/assets/**/*.bak\n')
            pack = repo / 'project/assets/Characters/Pack'
            (pack / 'Old').mkdir(parents=True)
            files = {
                'Listed.png': b'keep',          # in the CSV
                'Old/Retired.png': b'gone',     # ignored, not in the CSV: stale
                'Old/Retired.png.meta': b'{}',  # not ignored: git's business, never pruned
                'Old/Notes.png.bak': b'x',      # ignored leftover, not a payload
                'Readme.txt': b'tracked',       # not ignored
            }
            for name, data in files.items():
                (pack / name).write_bytes(data)
            (repo / 'project/assets/Other.png').write_bytes(b'outside the folder')
            with (repo / 'project/external_assets.csv').open('w', newline='') as f:
                writer = csv.writer(f)
                writer.writerow(['path', 'size', 'sha256'])
                # The CSV spells folders in git's case; a different case on disk is still the listed file.
                writer.writerow(['assets/Characters/pack/Listed.png', 4, hashlib.sha256(b'keep').hexdigest()])

            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                stale = asset_manifest.prune(str(repo), 'project/assets/Characters/Pack')
            self.assertEqual(stale, ['project/assets/Characters/Pack/Old/Retired.png'])
            self.assertIn('dry run', out.getvalue())
            self.assertTrue((pack / 'Old/Retired.png').exists())

            with contextlib.redirect_stdout(io.StringIO()):
                asset_manifest.prune(str(repo), str(pack), apply=True)  # an absolute folder works too
            self.assertFalse((pack / 'Old/Retired.png').exists())
            for name in ('Listed.png', 'Old/Retired.png.meta', 'Old/Notes.png.bak', 'Readme.txt'):
                self.assertTrue((pack / name).exists(), name)
            self.assertTrue((repo / 'project/assets/Other.png').exists())

    def test_prune_refuses_a_folder_outside_project(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            (repo / 'project').mkdir()
            (repo / 'project/external_assets.csv').write_text('path,size,sha256\n')
            with self.assertRaises(SystemExit):
                asset_manifest.prune(str(repo), 'tools')


POWERSHELL = shutil.which('powershell') or shutil.which('pwsh')


@unittest.skipUnless(POWERSHELL, 'fetch-assets.ps1 needs PowerShell')
class FetchAssetsTests(unittest.TestCase):
    """fetch-assets.ps1 updates same-size files whose sha256 changed, but only from a Drive copy that matches the CSV."""

    def _run(self, repo, *args):
        out = subprocess.run([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                              str(repo / 'tools/assets/fetch-assets.ps1'), *args],
                             capture_output=True, text=True, timeout=120)
        return out.returncode, out.stdout + out.stderr

    def test_same_size_rehash_updates_only_from_matching_drive_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo, drive = root / 'repo', root / 'Tartarus Assets'
            (repo / 'tools/assets').mkdir(parents=True)
            (repo / '.git').mkdir()
            shutil.copy(Path(__file__).with_name('fetch-assets.ps1'), repo / 'tools/assets/fetch-assets.ps1')
            files = {  # name: (CSV content, checkout content or None, Drive content)
                'added.wav': (b'AAAA', None, b'AAAA'),
                'recooked.wav': (b'NEW!', b'OLD!', b'NEW!'),
                'drive_stale.wav': (b'NEW!', b'OLD!', b'OLD!'),
                'current.wav': (b'SAME', b'SAME', b'SAME'),
            }
            (repo / 'project/assets/Audio').mkdir(parents=True)
            (drive / 'Used/project/assets/Audio').mkdir(parents=True)
            with (repo / 'project/external_assets.csv').open('w', newline='') as f:
                writer = csv.writer(f)
                writer.writerow(['path', 'size', 'sha256'])
                for name, (want, have, on_drive) in files.items():
                    writer.writerow(['assets/Audio/' + name, len(want), hashlib.sha256(want).hexdigest()])
                    if have is not None:
                        (repo / 'project/assets/Audio' / name).write_bytes(have)
                    (drive / 'Used/project/assets/Audio' / name).write_bytes(on_drive)

            code, out = self._run(repo, '-Source', str(drive))
            self.assertEqual(code, 1, out)  # drive_stale.wav is still wrong
            self.assertIn('1 copied (missing), 1 updated (different size or sha256), 1 already current', out)
            self.assertIn("not copied (Drive copy doesn't match the CSV)  assets/Audio/drive_stale.wav", out)
            self.assertIn('changed    assets/Audio/drive_stale.wav', out)
            audio = repo / 'project/assets/Audio'
            self.assertEqual((audio / 'added.wav').read_bytes(), b'AAAA')
            self.assertEqual((audio / 'recooked.wav').read_bytes(), b'NEW!')
            self.assertEqual((audio / 'drive_stale.wav').read_bytes(), b'OLD!')
            self.assertEqual(list(audio.glob('*.fetchpart')), [])

            # Hashes are cached in .git: a second check hashes nothing, and -SizeOnly ignores content.
            code, out = self._run(repo, '-VerifyOnly')
            self.assertEqual(code, 1, out)
            self.assertIn('1 the same size but a different sha256 (0 hashed, the rest cached)', out)
            (audio / 'drive_stale.wav').write_bytes(b'NEW!')  # new mtime and content: rehashed
            code, out = self._run(repo, '-VerifyOnly')
            self.assertEqual(code, 0, out)
            self.assertIn('(1 hashed, the rest cached)', out)
            (audio / 'current.wav').write_bytes(b'XXXX')
            code, out = self._run(repo, '-VerifyOnly', '-SizeOnly')
            self.assertEqual(code, 0, out)


if __name__ == '__main__':
    unittest.main()
