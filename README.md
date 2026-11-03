# Tartarus Engine

A custom C++ / OpenGL 3D game engine (GLFW, EnTT, PhysX, Dear ImGui) with its own editor.

## Assets that are not in git

Most of the content the engine runs on comes from licensed third-party packs: the Quantum characters,
the MC Core Motion mocap, the AKS-74U and Remington 870, KriptoFX and Knife blood, and sound built from
Tactical Shooter, Sonniss, OpenAIR and EchoThief recordings. Those licences don't allow redistribution,
and this repository is public, so the files themselves are kept on the team's shared Google Drive
folder, **Tartarus Assets**. Only content made for this project is in git: code, scenes, materials and
controllers, the `.meta` files of every asset, and procedural textures, meshes and sounds.

### Setting up a clone

1. Get access to the shared **Tartarus Assets** folder (ask the project owner). Install
   [Google Drive for desktop](https://www.google.com/drive/download/) and add the folder to your
   drive: open it in the browser, then **Organize → Add shortcut → My Drive**.
2. Clone the repo, then copy the assets in:
   ```
   powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -Source "G:\My Drive\Tartarus Assets"
   ```
   Use whatever path Drive for desktop shows for the folder. Each external file's `.meta` carries
   `"payload": {"sha256", "size"}`; the script copies every missing payload from the Drive's `Store\`
   (about 32 GB). A local file that differs is reported, not overwritten, unless you pass `-Force`.
   Run it again with `-VerifyOnly` at any time, and after pulling, because the list can change.
3. Build and run (below).

Without step 2 the editor still opens, but characters, weapons, animations and sounds are missing.

### How it fits together

| Where | What |
|---|---|
| Drive `Tartarus Assets\Store\` | A content-addressed store of the files the project uses that git doesn't have: `Store\<sha256[0:2]>\<sha256><ext>`. About 32 GB (8,150 files). Files never move on the Drive when they move in the repo. |
| Drive `Tartarus Assets\Raw\` | The source packs, unzipped and sorted by type, including the parts we don't use. See `Raw\SOURCES.md`. About 29 GB. |
| Drive `Tartarus Assets\Licenses\` | The license of every source. |
| `.gitignore` (the "Third-party asset payloads" block) | Ignores model, texture and audio files under `project/assets`, with an allow-list of the ones made for this project. |
| The `.meta` files | Tracked, including those of the files on the Drive. They hold each payload's `sha256` and `size`, so the hash travels with the file through moves. GUIDs never change, so every scene, material and controller reference keeps working. |

`project/external_assets.csv` is retired. Migrating the Drive from its old `Used\` mirror
(`python tools/assets/asset_manifest.py migrate-store <Used> <Store> --move`, a server-side move that
writes `store_migration.log` for rollback) has not been run yet; it happens when this branch merges.

The editor deletes a `.meta` file when its asset has disappeared (`AssetDatabase::ScanProject`). Files
with a `payload` in their `.meta` are exempt: a clone that hasn't fetched yet keeps them.
Without the exemption, a GUID would be re-minted once the file arrived, and its references would break.

### Adding or changing a third-party asset

1. Put the file in `project/assets/...` as usual and let the editor create its `.meta`.
   (A scene JSON too big for GitHub goes the same way: gitignore it under `project/scenes`, keep its `.meta`.)
2. Write the hashes into the `.meta` files: `python tools/assets/asset_manifest.py build .`
3. Copy the new or changed files to the Drive:
   `python tools/assets/asset_manifest.py export . "<Drive>\Tartarus Assets\Store"`.
   If the source pack is new, also put it under `Raw\` and add it to `Raw\SOURCES.md`.
4. Commit the `.meta` files and any `.mat` or other data. Never commit the file itself: `git status`
   must not show it, and `.gitignore` takes care of that.

Content you made yourself (procedural, modelled, or recorded by you) can live in git. Add it to the
allow-list at the end of the `.gitignore` block instead.

Check a checkout with `python tools/assets/asset_manifest.py verify <dir> [--hash]`, or the Store with
`verify <Store> --repo <repo>`.

### Rebuilding derived data from Raw

- **Blood:** `--import-blood-fx "<Drive>\Tartarus Assets\Raw\Effects\VolumetricBloodFX"` and
  `--import-knife-fx "<Drive>\Tartarus Assets\Raw\Effects"`. The Store already holds their output.
- **Audio:** set `TARTARUS_AUDIO_SRC` to `<Drive>\Tartarus Assets\Raw\Audio\audio-src`, then run the
  `tools/audio` build scripts.
- **First-person weapon clips:** the Blender sources are in `Raw\Weapons\*Blender`.

Raw is too large to sync in full on a small disk. With Drive for desktop in its default streaming mode,
files download only when opened.

### Git history

The third-party files were removed from the whole history in October 2026, not only from the current
tree. Don't push branches made from a clone older than that: they still carry the files. Rebase them
onto the new `main`, or recreate them.

## Build and run

Install Visual Studio's C++ toolchain and CMake. Engine, editor and gameplay are all native C++;
gameplay and editor tools build as hot-reloadable modules. See the [documentation index](docs/README.md)
for gameplay code, the in-editor Script IDE and curve tools.

    cmake --build build --config Release --target TartarusEngine

`run-editor.cmd` rebuilds the checkout and launches the editor.

## Tests

    build\Release\TartarusEngine.exe --unit-tests
    build\Release\TartarusEngine.exe --smoke-test tests\smoke-scenes

Before committing or pushing, run `python tools/assets/check_git_assets.py`. This checks the
Git index, including force-added files, against the external asset ignore rules.
