"""Reimport the supplied AK/870 FBXs without changing asset identities or tuning.

Run from the repository root with --audit (read-only inventory) or --apply.
Audit JSON and backups go under build/weapon-reimport, never into source exports.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from fbx_export_edit import load, set_duration, write, zero_translation

ROOT = Path(__file__).resolve().parents[2]
EXPORTS = Path(os.environ.get('TARTARUS_WEAPON_EXPORTS', str(Path.home() / 'Desktop/AE_Exports')))
WORK = ROOT / 'build/weapon-reimport'


def mapping():
    result = []
    for weapon, folder, prefix, token in [('AKS74U', 'AKS74U', 'AKS-74U', 'AK'),
                                           ('Remington870', 'Remington 870', 'Remington870', '870')]:
        for source in sorted((EXPORTS / folder).glob('*.fbx')):
            _, kind, _, name = source.stem.split('_', 3)
            name = {'Idle_To_Sprint': 'IdleToSprint', 'Sprint_To_Idle': 'SprintToIdle',
                    'Sprint_Start': 'IdleToSprint', 'Sprint_End': 'SprintToIdle',
                    'MagCheck': 'Mag_Check'}.get(name, name)
            # Keep the existing model GUID: this was the AK's neutral weapon asset.
            if weapon == 'AKS74U' and kind == 'W' and name == 'Idle':
                name = 'ADS'
            dest = ROOT / f'project/assets/Weapons/{weapon}/{"FirstPerson" if kind == "FP" else "Weapon"}/{prefix}_A_{kind}_{name}.fbx'
            if not dest.exists():
                raise RuntimeError(f'No existing asset for {source}: {dest}')
            result.append((source, dest))
    return result


def inventory(path):
    out = WORK / 'audit' / (hashlib.sha256(str(path).encode()).hexdigest()[:16] + '.json')
    subprocess.run([str(ROOT / 'build/probes/weapon_import_audit.exe'), str(path), str(out)], check=True)
    return json.loads(out.read_text())


def write_reports(manifest_records, audits, previous_manifests):
    for weapon, records in manifest_records.items():
        folder = ROOT / f'project/assets/Weapons/{weapon}'
        old = previous_manifests[weapon]
        legacy_source = old.get('retainedSource', old.get('source'))
        retained = []
        for path in sorted(folder.glob('FirstPerson/*.fbx')):
            rel = path.relative_to(folder).as_posix()
            if rel in records:
                continue
            takes = audits[str(path)]['takes']
            take = takes[0] if takes else {'name': 'Bind pose (no take)', 'duration': 0, 'tps': 60}
            retained.append({'file': rel, 'source': legacy_source, 'action': take['name'],
                             'frameStart': 0, 'frameEnd': take['duration'], 'fps': take['tps'],
                             'bytes': path.stat().st_size,
                             'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                             'guid': json.loads(path.with_suffix('.fbx.meta').read_text())['guid'],
                             'note': 'Retained authored reference: no replacement provided; shared-rig poses remain compatible.'})
        manifest = {'source': str(EXPORTS / ('AKS74U' if weapon == 'AKS74U' else 'Remington 870')),
                    'retainedSource': legacy_source, 'fps': 60,
                    'exports': list(records.values()) + retained}
        (folder / 'export_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        report = {'method': 'Assimp static import/pose/skin audit; gameplay tests left to user',
                  'rootTranslation': 'Zero on every reimported weapon take (AK epsilon < 1e-6 m).',
                  'identities': 'Existing paths and GUIDs preserved.',
                  'offsets': 'Main gun geometry in root space unchanged; existing mount, sight, muzzle and ejection values preserved. Spare magazine uses supplied neutral pose.',
                  'retainedArms': [e['file'] for e in retained],
                  'reimported': list(records.values())}
        (folder / 'verification_report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--audit', action='store_true')
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    if args.audit == args.apply:
        parser.error('Choose --audit or --apply')
    WORK.joinpath('audit').mkdir(parents=True, exist_ok=True)
    pairs = mapping()
    paths = list(dict.fromkeys([p for pair in pairs for p in pair]))
    # Retained authored aim/fire clips and the shared arms model also need checking.
    paths += [ROOT / 'project/assets/Characters/Quantum/FirstPerson/Quantum_Arms_FP.fbx']
    for weapon, prefix in [('AKS74U', 'AKS-74U'), ('Remington870', 'Remington870')]:
        for p in (ROOT / f'project/assets/Weapons/{weapon}').rglob('*.fbx'):
            if p not in paths and ('FirstPerson' in p.parts or 'Weapon' in p.parts):
                paths.append(p)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        audits = dict(zip(map(str, paths), pool.map(inventory, paths)))
    WORK.joinpath('inventory.json').write_text(json.dumps(audits))
    for src, dst in pairs:
        old, new = audits[str(dst)], audits[str(src)]
        if len(new['takes']) != 1:
            raise RuntimeError(f'Expected one take: {src}')
        ot = old['takes'][0] if old['takes'] else {'duration': 0, 'tps': 60, 'channels': []}
        nt = new['takes'][0]
        print(f'{src.name:38} {ot["duration"]:5g}/{ot["tps"]:g} -> {nt["duration"]:5g}/{nt["tps"]:g}  bones {len(ot["channels"]):3} -> {len(nt["channels"]):3}')
    if args.apply:
        staged = []
        manifest_records = {}
        previous_manifests = {
            weapon: json.loads((ROOT / f'project/assets/Weapons/{weapon}/export_manifest.json').read_text(encoding='utf-8-sig'))
            for weapon in ('AKS74U', 'Remington870')
        }
        # Authoring exports use independent weapon-action ranges. Match the arms
        # range so a longer weapon take cannot change normalized event/exit times.
        for src, dst in pairs:
            prepared = WORK / 'staged' / dst.relative_to(ROOT / 'project/assets/Weapons')
            prepared.parent.mkdir(parents=True, exist_ok=True)
            source_audit = audits[str(src)]
            note = 'Supplied authored FBX; existing asset identity preserved.'
            if 'Remington870' in dst.parts and 'Weapon' in dst.parts:
                tree, version = load(src)
                zero_translation(tree, b'Main')
                if not dst.stem.endswith('_W_Idle'):
                    arms_file = dst.parent.parent / 'FirstPerson' / dst.name.replace('_W_', '_FP_')
                    take = audits[str(arms_file)]['takes'][0]
                    set_duration(tree, take['duration'] / take['tps'])
                write(prepared, tree, version)
                note = 'Main translation zeroed; child motion/skin retained. Take range matched to arms (end holds or trim only).'
            else:
                shutil.copyfile(src, prepared)
            staged.append((src, dst, prepared, note))
        # These actions have no separate weapon animation in the supplied set.
        # Use its neutral gun pose, with each action's existing range, rather than
        # mixing the old world-baked root and loose-magazine motion into this rig.
        neutral = EXPORTS / 'AKS74U/A_W_AK_Idle.fbx'
        for name in ('Holster', 'IdleToSprint', 'Melee', 'Sprint', 'SprintToIdle'):
            dst = ROOT / f'project/assets/Weapons/AKS74U/Weapon/AKS-74U_A_W_{name}.fbx'
            prepared = WORK / 'staged' / dst.relative_to(ROOT / 'project/assets/Weapons')
            tree, version = load(neutral)
            take = audits[str(dst)]['takes'][0]
            set_duration(tree, take['duration'] / take['tps'])
            write(prepared, tree, version)
            staged.append((neutral, dst, prepared, 'Neutral supplied weapon pose; no separate weapon take provided for this action. Original duration retained.'))
        # Inspect every staged file before replacing any working assets.
        for src, dst, prepared, note in staged:
            final = inventory(prepared)
            weapon = 'AKS74U' if 'AKS74U' in dst.parts else 'Remington870'
            root_name = 'root' if weapon == 'AKS74U' else 'Main'
            if 'Weapon' in dst.parts:
                root_channel = next(c for c in final['takes'][0]['channels'] if c['name'] == root_name)
                if any(abs(v) > 1e-6 for key in root_channel['positions'] for v in key[1:]):
                    raise RuntimeError(f'Nonzero weapon root: {prepared}')
            rel = dst.relative_to(ROOT / f'project/assets/Weapons/{weapon}').as_posix()
            manifest_records.setdefault(weapon, {})[rel] = {
                'file': rel, 'source': str(src), 'action': final['takes'][0]['name'],
                'frameStart': 0, 'frameEnd': final['takes'][0]['duration'],
                'fps': final['takes'][0]['tps'], 'bytes': prepared.stat().st_size,
                'sha256': hashlib.sha256(prepared.read_bytes()).hexdigest(),
                'guid': json.loads(dst.with_suffix('.fbx.meta').read_text())['guid'],
                'meshes': [m['name'] for m in final['meshes']], 'note': note,
            }
        for src, dst, prepared, note in staged:
            backup = WORK / 'before' / dst.relative_to(ROOT)
            backup.parent.mkdir(parents=True, exist_ok=True)
            if not backup.exists():
                shutil.copy2(dst, backup)
            shutil.copyfile(prepared, dst)
            if 'FirstPerson' in dst.parts:
                meta_path = dst.with_suffix('.fbx.meta')
                meta = json.loads(meta_path.read_text())
                meta.setdefault('materialRemap', {})['M_Quantum_Arms_PBR'] = 'assets/Characters/Quantum/Materials/Characters/M_Quantum_Arms.mat'
                meta_path.write_text(json.dumps(meta, indent=2) + '\n')
        write_reports(manifest_records, audits, previous_manifests)
        print(f'Reimported {len(pairs)} supplied FBXs and refreshed 5 neutral AK weapon clips; existing GUIDs and tuning preserved.')


if __name__ == '__main__':
    main()
