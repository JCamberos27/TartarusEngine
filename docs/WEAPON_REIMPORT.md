# AKS74U and Remington 870 authored-export reimport

The private asset store now retains the supplied inputs under
`Tartarus Assets/Raw/Weapons/AE_Exports`. Set `TARTARUS_WEAPON_EXPORTS` to that
folder before repeating the import. The derived FBXs live in `Tartarus Assets/Used`,
at the paths recorded in `project/external_assets.csv`; they are never committed to Git.
The import report and backups remain local build artifacts.

The October 2026 reimport uses `Desktop/AE_Exports/AKS74U` and
`Desktop/AE_Exports/Remington 870`. The original export folders are read-only
inputs. Existing project paths and `.fbx.meta` GUIDs are retained, including the
AK neutral weapon model's historical `AKS-74U_A_W_ADS.fbx` filename.

46 supplied FBXs replace their matching assets. AK `Idle_To_Sprint` and
`Sprint_To_Idle`, 870 `Sprint_Start` and `Sprint_End`, and 870 `MagCheck` map to
the existing project names. The five AK weapon actions without a supplied
weapon take (Holster, IdleToSprint, Melee, Sprint, SprintToIdle) use the supplied
neutral weapon pose with their original durations. They no longer mix the old
world-baked root and loose-magazine motion into the new weapon rig.

The supplied AK root is already zero within floating-point precision. The 870
`Main` bone still had a 1.60246 cm translation. Its bind translation and constant
animation translation are zeroed in the project copies. Child curves, mesh
data, weights and bone offsets are retained. The attachment's inverse-root
transform keeps the weapon geometry in the same root-relative coordinates;
the existing mount offset, muzzle, sight and ejection tuning remains valid.

The independent 870 weapon-action ranges differ from their paired arms ranges.
Their motion already aligns at frame zero. End holds extend shorter takes;
the Reload_Loop_End take is trimmed from 118 to 98 frames. This preserves
normalized controller event and exit timing without stretching the authored
motion. The neutral Idle take retains its supplied 30-frame range.

The arms exports contain 89 animated nodes rather than the older control-rich
264-node exports. The engine's existing rest-frame correction maps them onto
the shared Quantum arms rig. Imported hand, head and gun-socket poses match the
previous clips; the actually weighted arms bones also remain compatible.
The supplied arm FBXs are copied byte-for-byte. Their preview material name
`M_Quantum_Arms_PBR` is mapped to the existing Quantum arms material.

No replacement was supplied for either Aim reference or for the AK arm Fire
clip and legacy ADS reference. These assets remain intact. Each weapon's
`export_manifest.json` and `verification_report.json` lists supplied,
derived-neutral and retained assets separately with GUIDs and content hashes.

## Repeating the import

Build the read-only Assimp inventory tool from the repository root:

```bat
tools\probes\build_probe.bat weapon_import_audit
```

Use Python with NumPy installed:

```text
python -B tools/weapons/reimport_authored_exports.py --audit
python -B tools/weapons/reimport_authored_exports.py --apply
```

The FBX metadata helper uses Blender 5.2's standalone binary parser/writer;
it does not launch Blender. `TARTARUS_BLENDER_FBX_ADDON` can point to another
installation's `io_scene_fbx` directory. Staged imports and the original FBX
backups are under `build/weapon-reimport`; source exports are never modified.

The final static audit checked all 51 updated assets, zero weapon roots, paired
870 durations, authored arm file hashes, material names, geometry in root space,
GUIDs and controller references. Root normalization introduced no measured
geometry change. Gameplay and visual testing are left to the user.
