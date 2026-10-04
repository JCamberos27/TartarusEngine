# Assets that are not in git

Most of the content the engine runs on comes from licensed third-party packs: the Quantum characters,
the MC Core Motion mocap, the AKS-74U and Remington 870, KriptoFX and Knife blood, and sound built from
Tactical Shooter, Sonniss, OpenAIR and EchoThief recordings. Those licences don't allow redistribution,
and this repository is public, so the files themselves are kept on the team's shared Google Drive
folder, **Tartarus Assets**. Only content made for this project is in git:
- code
- scenes
- materials and controllers
- the `.meta` files of every asset
- procedural textures, meshes and sounds

## Setting up a clone

1. Get access to the shared **Tartarus Assets** folder (ask the project owner). Install
   [Google Drive for desktop](https://www.google.com/drive/download/) and add the folder to your
   drive: open it in the browser, then **Organize → Add shortcut → My Drive**.
2. Clone the repo, then copy the assets in:
   ```
   powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -Source "G:\My Drive\Tartarus Assets"
   ```
   Use whatever path Drive for desktop shows for the folder. The script copies `Used\project\...`
   over your checkout (about 10 GB). It then checks every file in `project/external_assets.csv` and
   lists anything missing. Run it again with `-VerifyOnly` at any time, and after pulling, because
   the list can change.
3. Build and run as usual (`run-editor.cmd`).

Without step 2 the editor still opens, but characters, weapons, animations and sounds are missing.
The console then warns that N files listed in `external_assets.csv` are missing.

## How it fits together

| Where | What |
|---|---|
| Drive `Tartarus Assets\Used\` | Exactly the files the project uses that git doesn't have, at the same paths as in this repo (`Used\project\assets\...`). About 10 GB. |
| Drive `Tartarus Assets\Raw\` | The source packs, unzipped and sorted by type, including the parts we don't use. See `Raw\SOURCES.md`. About 29 GB. |
| `.gitignore` (the "Third-party asset payloads" block) | Ignores model, texture and audio files under `project/assets`, with an allow-list of the ones made for this project. |
| `project/external_assets.csv` | Path, size and SHA-256 of every file in `Used`. Tracked. |
| The `.meta` files | Tracked, including those of the files on the Drive. GUIDs never change, so every scene, material and controller reference keeps working. |

The editor deletes a `.meta` file when its asset has disappeared (`AssetDatabase::ScanProject`). Files
listed in `external_assets.csv` are exempt: a clone that hasn't fetched yet keeps their `.meta` files.
Without the exemption, a GUID would be re-minted once the file arrived, and its references would break.

## Adding or changing a third-party asset

1. Put the file in `project/assets/...` as usual and let the editor create its `.meta`.
2. Rebuild the list:
   ```
   python tools/assets/asset_manifest.py build .
   ```
3. Copy the new or changed files to the Drive:
   ```
   python tools/assets/asset_manifest.py export . "<Drive>\Tartarus Assets\Used"
   ```
   If the source pack is new, also put it under `Raw\` and add it to `Raw\SOURCES.md`.
4. Commit the `.meta`, any `.mat` or other data, and `project/external_assets.csv`. Never commit the
   file itself: `git status` must not show it, and `.gitignore` takes care of that.

Content you made yourself (procedural, modelled, or recorded by you) can live in git. Add it to the
allow-list at the end of the `.gitignore` block instead.

Check a Drive folder or a checkout against the list with
`python tools/assets/asset_manifest.py verify <dir> [--hash]`.

## Rebuilding derived data from Raw

- **Blood:** `--import-blood-fx "<Drive>\Tartarus Assets\Raw\Effects\VolumetricBloodFX"` and
  `--import-knife-fx "<Drive>\Tartarus Assets\Raw\Effects"` (docs/BLOOD_FX.md). `Used` already
  holds their output.
- **Audio:** set `TARTARUS_AUDIO_SRC` to `<Drive>\Tartarus Assets\Raw\Audio\audio-src`, then run the
  `tools/audio` build scripts (docs/AUDIO.md).
- **First-person weapon clips:** the Blender sources are in `Raw\Weapons\*Blender`
  (docs/FPS_ANIMATION_SYSTEM.md).

Raw is too large to sync in full on a small disk. With Drive for desktop in its default streaming mode,
files download only when opened.

## Git history

The third-party files were removed from the whole history in October 2026, not only from the current
tree. Don't push branches made from a clone older than that: they still carry the files. Rebase them
onto the new `main`, or recreate them.
