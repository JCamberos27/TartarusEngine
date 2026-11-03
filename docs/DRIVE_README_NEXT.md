# Tartarus Assets

The licensed models, textures, animations and sounds the TartarusEngine game needs
(https://github.com/JCamberos27/TartarusEngine), kept here because they may not go on GitHub.
For the team only: don't share them outside it.

- **`Store\`**: exactly the files the project uses that aren't in git, stored by content:
  `Store\<first 2 hex of sha256>\<sha256><ext>`. About 32 GB (8,150 files). This is
  all you need to run the game. The repo's `.meta` files say which hash is which file.
- **`Raw\`**: the source packs, unzipped and sorted by type, including parts we don't use yet
  (the decal packs in `Raw\Decals` are kept as their original RARs). About 28 GB. See
  `Raw\SOURCES.md` for what each one is.
- **`Licenses\`**: the license type of every source, and the license files the packs came with.

## Getting the game running

1. Install Google Drive for desktop. Add this folder to your drive: in the browser, select it,
   then **Organize -> Add shortcut -> My Drive**.
2. Clone the repo, then run this from the repo folder (use the path Drive for desktop shows for
   this folder):
   ```
   powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -Source "G:\My Drive\Tartarus Assets"
   ```
   It copies each payload your checkout is missing from `Store` and reports anything still missing.
   A local file that differs is reported, not overwritten (use `-Force` to replace it).
3. Build and run (`run-editor.cmd`).

After pulling, run it again (or with `-VerifyOnly`): the list of files can change.

## Adding assets

Follow "Adding or changing a third-party asset" in the repo's `README.md`. In short: put the file in the project, then run
`python tools/assets/asset_manifest.py build .` (writes the hash into each `.meta`) and
`python tools/assets/asset_manifest.py export . "<this folder>\Store"`. Commit the `.meta` files,
never the asset itself. New source packs go in `Raw\` and get a line in `Raw\SOURCES.md`, and their
license goes in `Licenses\`.

**Don't rename or move files in `Store\`.** A file's name is its hash; the game finds it by that. Files
can move freely in the repo: the hash travels in the `.meta`, so nothing here changes.

(`project/external_assets.csv` and the old `Used\` mirror are retired.)
