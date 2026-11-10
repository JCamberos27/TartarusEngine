"""Install the .38 Special payloads into project/assets/Weapons/Revolver38 and record them.

    python -B tools/weapons/install_revolver38.py --staged build/revolver38-export \
        --raw "<Drive>/Tartarus Assets/Raw/Weapons/38_Special" [--used "<Drive>/Tartarus Assets/Used"]

- Copies the exporter's FBXs (tools/weapons/export_revolver38.py) and the author_3p.py ThirdPerson clips
  (already written into the project folder) plus the raw textures under engine names.
- Writes a .meta beside every payload, keeping an existing GUID, and the two .mat files.
- Upserts every payload into project/external_assets.csv (path,size,sha256), after the Remington's rows.
- With --used, mirrors each payload to the shared Drive's "Used" tree, laid out like this repo
  (tools/assets/fetch-assets.ps1 copies them back).
Payloads are git-ignored; the .meta/.mat/csv changes are what gets committed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import secrets
import shutil

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / 'project'
REL = 'assets/Weapons/Revolver38'
DEST = PROJECT / REL

# engine name -> (raw file under 38_Special, sRGB, textureType: 1 = normal map)
TEXTURES = {
    'Revolver38_BaseColor.png': ('Textures/38_S_Revolver_Black_Metal_Albedo.png', True, 0),
    'Revolver38_Metallic.png': ('Textures/38_S_Revolver_Metallic.png', False, 0),
    'Revolver38_Roughness.png': ('Textures/38_S_Revolver_Roughness.png', False, 0),
    'Revolver38_Normal.png': ('Textures/38_S_Revolver_Normal.png', False, 1),
    'Revolver38_AO.png': ('Textures/T_Revolver_Ao.png', False, 0),
    'Bullet38_BaseColor.png': ('Bullet/Bullet_Textures/Bullet_Albedo.png', True, 0),
    'Bullet38_Metallic.png': ('Bullet/Bullet_Textures/Bullet_Metallic.png', False, 0),
    'Bullet38_Roughness.png': ('Bullet/Bullet_Textures/Bullet_Roughness.png', False, 0),
    'Bullet38_Normal.png': ('Bullet/Bullet_Textures/Bullet_Normal.png', False, 1),
    'Bullet38_AO.png': ('Bullet/Bullet_Textures/T_Bullet_Ao.png', False, 0),
}
MATERIALS = {
    'Revolver38': ('.38 Special Revolver', 'Revolver38'),
    'Bullet38': ('.38 Special Cartridge', 'Bullet38'),
}
REMAP_WEAPON = {'Revolver': f'{REL}/Materials/Revolver38.mat', 'Bullet': f'{REL}/Materials/Bullet38.mat'}
REMAP_ROUND = {'Bullet': f'{REL}/Materials/Bullet38.mat'}


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def write_json(path, data):
    text = json.dumps(data, indent=2) + '\n'
    if not path.exists() or path.read_text(encoding='utf-8-sig') != text:
        path.write_text(text, encoding='utf-8')


def guid_of(meta):
    if meta.exists():
        return json.loads(meta.read_text(encoding='utf-8-sig'))['guid']
    return secrets.token_hex(8)


def meta_for(path, body):
    meta = path.with_name(path.name + '.meta')
    data = {'folder': path.parent.relative_to(PROJECT).as_posix(), 'guid': guid_of(meta)}
    data.update(body)
    data['metaVersion'] = 1
    write_json(meta, dict(sorted(data.items())))
    return data['guid']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--staged', required=True, type=Path)
    ap.add_argument('--raw', required=True, type=Path)
    ap.add_argument('--used', type=Path)
    a = ap.parse_args()
    staged = a.staged if a.staged.is_absolute() else ROOT / a.staged
    payloads = []

    for sub in ('FirstPerson', 'Weapon', 'ThirdPerson', 'Textures', 'Materials'):
        (DEST / sub).mkdir(parents=True, exist_ok=True)
    for src in sorted(staged.rglob('*.fbx')):
        dst = DEST / src.relative_to(staged)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    for src in sorted(DEST.rglob('*.fbx')):
        remap = REMAP_WEAPON if src.name == 'Revolver38_A_W_Idle.fbx' else REMAP_ROUND if src.parent == DEST else None
        body = {'type': 'model'}
        if remap:
            body['materialRemap'] = remap
        meta_for(src, body)
        payloads.append(src)

    guids = {}
    for name, (raw, srgb, kind) in TEXTURES.items():
        dst = DEST / 'Textures' / name
        shutil.copy2(a.raw / raw, dst)
        guids[name] = meta_for(dst, {'type': 'texture', 'importer': {
            'anisoLevel': 16, 'compression': 2, 'filterMode': 2, 'generateMipmaps': True, 'isSRGB': srgb,
            'maxTextureSize': 4096, 'textureType': kind, 'wrapMode': 0}})
        payloads.append(dst)

    for mat, (title, stem) in MATERIALS.items():
        tex = lambda s: f'{REL}/Textures/{stem}_{s}.png'
        slots = {'_AOMap': 'AO', '_AlbedoMap': 'BaseColor', '_MetallicMap': 'Metallic', '_NormalMap': 'Normal',
                 '_RoughnessMap': 'Roughness'}
        path = DEST / 'Materials' / f'{mat}.mat'
        write_json(path, {
            'factorsScaleMaps': True, 'matVersion': 2, 'name': title,
            'properties': {
                '_AOMap': tex('AO'), '_AlbedoMap': tex('BaseColor'), '_BaseColor': [1.0, 1.0, 1.0],
                '_DoubleSided': True, '_EmissiveColor': [0.0, 0.0, 0.0], '_EmissiveMap': '', '_EmissiveStrength': 1.0,
                '_Metallic': 1.0, '_MetallicMap': tex('Metallic'), '_MetallicRoughnessMap': '',
                # Both normal maps are DirectX-style (green down): measured, the green-flipped field is the curl-free one.
                '_NormalFlipY': True,
                '_NormalMap': tex('Normal'), '_Roughness': 1.0, '_RoughnessMap': tex('Roughness'),
                '_Triplanar': False, '_TriplanarScale': 1.0},
            'shader': 'engine://Standard.shader',
            'textureGuids': {k: guids[f'{stem}_{v}.png'] for k, v in slots.items()}})
        meta_for(path, {'type': 'material'})

    # external_assets.csv: replace this weapon's rows, placed after the Remington's.
    csv = PROJECT / 'external_assets.csv'
    lines = csv.read_text(encoding='utf-8').splitlines()
    head, rows = lines[0], [r for r in lines[1:] if not r.startswith(REL + '/')]
    mine = [f'{p.relative_to(PROJECT).as_posix()},{p.stat().st_size},{sha(p)}' for p in sorted(payloads)]
    at = max((i for i, r in enumerate(rows) if r.startswith('assets/Weapons/Remington870/')), default=len(rows) - 1) + 1
    csv.write_text('\n'.join([head] + rows[:at] + mine + rows[at:]) + '\n', encoding='utf-8')

    if a.used:
        for p in payloads:
            out = a.used / 'project' / p.relative_to(PROJECT)
            out.parent.mkdir(parents=True, exist_ok=True)
            if not out.exists() or out.stat().st_size != p.stat().st_size or sha(out) != sha(p):
                shutil.copy2(p, out)
    print(f'installed {len(payloads)} payloads under {REL}' + (' and mirrored to Used' if a.used else ''))


if __name__ == '__main__':
    main()
