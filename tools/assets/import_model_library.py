# Imports the model library in the shared Drive's Raw\Architecture, Raw\Furniture and Raw\Props into
# project/assets, ready to drag into a scene:
#   - models  -> <Pack>/Models/*.fbx, each .meta with its scale, pivot and a materialRemap onto the .mat files below
#   - maps    -> <Pack>/Textures/<Set>/... (the ones the Standard shader uses; height / packed "Arm" maps are skipped)
#   - .mat    -> <Pack>/Materials/M_<Set>.mat, one per texture set, shared by every model that uses it
#   - (--gallery) project/scenes/AssetGallery.json, every model laid out in labelled rows
# The payloads (fbx, png, jpg, tga) are git-ignored; afterwards run asset_manifest.py build + export so they reach
# the Drive's Used folder. Pivots and sizes come from the engine itself (--model-report on the Raw files), so the
# exe must be built first.
#
#   python tools/assets/import_model_library.py <repo> [--raw <Raw dir>] [--exe <TartarusEngine.exe>] [--gallery]
import argparse
import json
import os
import re
import secrets
import shutil
import subprocess
import sys

DEFAULT_RAW = os.path.expanduser(r'~\OneDrive\Desktop\Atrocity Exhibition Drive\Tartarus Assets\Raw')

# --- map types -------------------------------------------------------------------------------------------------
# Suffix (after the set name) -> slot. Order matters: the first match wins.
MAP_PATTERNS = [
    ('skip', r'(height|hm|arm)'),
    ('albedo', r'(base_?color|diffuse|albedo|dm)'),
    ('normal', r'(normal|nor_gl|nm)'),
    ('metallic', r'(metallic|mm)'),
    ('roughness', r'(roughness|rough|rm)'),
    ('ao', r'(ambient_occlusion|occlusion|ao)'),
    ('emissive', r'(emissive)'),
]
MAP_RE = [(kind, re.compile(r'^(?P<set>.+?)[_ ]' + pat + r'(?:_\d+k)?$', re.I))
          for kind, pat in MAP_PATTERNS]
IMAGE_EXT = ('.png', '.jpg', '.jpeg', '.tga')


def classify(stem):
    for kind, rx in MAP_RE:
        m = rx.match(stem)
        if m:
            return kind, m.group('set')
    return None, None


# --- packs -----------------------------------------------------------------------------------------------------
# Each pack: dest folder under project/assets, its models (Raw-relative files) and its texture folders
# (Raw-relative, each holding one or more sets). Set names are tidied by SET_NAMES.
def room(name):
    return {'dest': f'Furniture/{name}', 'models': [f'Furniture/{name}/Models/*.fbx'],
            'textures': [f'Furniture/{name}/Textures/*']}


PACKS = [
    {'dest': 'Architecture/Building', 'models': ['Architecture/Building/Models/*.fbx'],
     'textures': ['Architecture/Building/Ceilings/*', 'Architecture/Building/Floors/*', 'Architecture/Building/Walls/*',
                  'Architecture/Building/PolyHaven_2K']},
    {'dest': 'Architecture/Doors', 'models': ['Architecture/Door/Option_1/Door_Op_1.fbx', 'Architecture/Door/Option_2/Door_Op_2.fbx'],
     'textures': ['Architecture/Door/Textures/White/4096',
                  'Props/Electrical/Hardware/Hardware_Textures/Hardware_White_Color/2048',
                  'Props/Electrical/Hardware/Hardware_Textures/Hardware_Dark_Color/2048',
                  'Props/Electrical/Hardware/Numbers_Textutes/Option_1/2048',
                  'Props/Electrical/Hardware/Numbers_Textutes/Option_2/2048']},
    room('Bathroom'), room('Bedroom'), room('Decor'), room('Garage'), room('Kids_Room'), room('Kitchen'),
    room('Lighting'), room('Living_Room'), room('Office'),
    {'dest': 'Furniture/Seating',
     'models': ['Furniture/Arm_Chair_01/Arm_Chair_01.fbx', 'Furniture/Chesterfield_Sofa/Models/Chesterfield_Sofa.fbx',
                'Furniture/Pillow/Pillow.fbx', 'Furniture/Sofa_01/Sofa_01.fbx', 'Furniture/Sofa_02/Sofa_02.fbx',
                'Furniture/Sofa_03/Sofa_03.fbx', 'Furniture/Sofa_04/Sofa_04.fbx'],
     'textures': ['Furniture/Arm_Chair_01/Textures', 'Furniture/Chesterfield_Sofa/Textures/4096',
                  'Furniture/Sofa_01/Textures', 'Furniture/Sofa_02/Textures', 'Furniture/Sofa_03/Textures']},
    {'dest': 'Props/Cross', 'models': ['Props/Cross/Cross.fbx'], 'textures': ['Props/Cross/Textures']},
    {'dest': 'Props/David', 'models': ['Props/David/David.fbx'], 'textures': []},
    {'dest': 'Props/Electrical', 'models': ['Props/Electrical/Light_Switches/Models/Light_Switches.fbx',
                                            'Props/Electrical/Outlets/Outlet_Electrical.fbx', 'Props/Electrical/Outlets/Outlet_Gfi.fbx'],
     'textures': ['Props/Electrical/Light_Switches/Light_Switches_Textures/*', 'Props/Electrical/Outlets/Textures/*']},
    {'dest': 'Props/Holy_Bible', 'models': ['Props/Holy_Bible/Holy_Bible.fbx'], 'textures': ['Props/Holy_Bible/Textures']},
    # D.fbx is a byte-for-byte twin of Knife.fbx in geometry; only the one is imported.
    {'dest': 'Props/Knife', 'models': ['Props/Knife/Models/Knife.fbx'], 'textures': []},
    # The *_Lodpack files hold every LOD in one file (they would draw on top of each other): skipped.
    {'dest': 'Props/Medical_Supplies', 'models': ['Props/Medical_Supplies/Models/*.fbx'], 'skip': ['_Lodpack'],
     'textures': ['Props/Medical_Supplies/Textures/Pbr_Tex_Sheet3']},
    {'dest': 'Props/Money', 'models': ['Props/Money/Models/Money_Usa.fbx'], 'textures': ['Props/Money/Textures']},
]

# Raw set key (from the file names) -> tidy set name.
SET_NAMES = {
    'Door_1_W_4_K': 'Door_White', 'Hardware_2_K_W': 'Door_Hardware_White', 'Hardware_2_K_D': 'Door_Hardware_Dark',
    'Numbers_Op1_2_K': 'Door_Numbers_A', 'Numbers_Op2_2_K': 'Door_Numbers_B',
    'beige_wall_001': 'PH_Beige_Wall', 'dark_wooden_planks': 'PH_Dark_Wooden_Planks', 'laminate_floor_02': 'PH_Laminate_Floor',
    'Arm_Chair01': 'Arm_Chair_01', 'Mat_Sofa_Vintage_03': 'Chesterfield', 'Sofa01': 'Sofa_01', 'Sofa02': 'Sofa_02',
    'Sofa03': 'Sofa_03', 'Tex_Sheet3_Low_Tex_Sheet3': 'Medical_Supplies',
    'Money_Usa_Money_1_2_5_10': 'Money_Small_Bills', 'Money_Usa_Money_20_50_100': 'Money_Large_Bills',
    'Light_Switches_Light_Switches_White': 'Light_Switch_White', 'Light_Switches_Light_Switches_Grey': 'Light_Switch_Grey',
    'Light_Switches_Light_Switches_Steel': 'Light_Switch_Steel', 'Light_Switches_Light_Switches_Tan': 'Light_Switch_Tan',
    'Outlets_White': 'Outlet_White', 'Outlets_Metal': 'Outlet_Metal', 'Outlets': 'Outlet_Shared',
    'Wall_Blue': 'Wall_Blue', 'Wall_Brown': 'Wall_Brown', 'Wall_Green': 'Wall_Green',
}
# Sets that only ship some maps borrow the rest from another set (same surface, recoloured).
INHERIT = {'Wall_Blue': 'Wall_Default', 'Wall_Brown': 'Wall_Default', 'Wall_Green': 'Wall_Default',
           'Outlet_White': 'Outlet_Shared', 'Outlet_Metal': 'Outlet_Shared'}
# Map-only sets that are never a material of their own.
NOT_A_MATERIAL = {'Outlet_Shared'}

# Material name in the FBX -> set name, where the names don't match on their own. (model stem, material) wins
# over a bare material name.
MATERIAL_SETS = {
    'Door_Mat': 'Door_White', 'Hardware_Mat': 'Door_Hardware_Brass', 'Numbers_Mat': 'Door_Numbers_A',
    'Dressers_A': 'Dressers_Clean', 'Lamps_B_on': 'Lamps_B', 'Lamps_C_on': 'Lamps_C',
    'Curtains_AB_rod': 'Curtains_Ab', 'Curtains_CD_rod': 'Curtains_Cd', 'sofa_03_fringe': 'Sofa_03',
    'Chesterfields': 'Chesterfield', 'mat_sofa_vintage_03': 'Chesterfield', 'Armchair_01': 'Arm_Chair_01', 'TexSheet3': 'Medical_Supplies',
    'Money_1_2_5_10': 'Money_Small_Bills', 'Money_20_50_100': 'Money_Large_Bills',
    ('Cross', 'lambert1'): 'Cross', ('Holy_Bible', 'lambert1'): 'Holy_Bible',
    ('Light_Switches', 'lambert1'): 'Light_Switch_White',
    ('Outlet_Electrical', 'lambert1'): 'Outlet_White', ('Outlet_Gfi', 'lambert1'): 'Outlet_White',
}
# Materials with no texture set: hand-tuned flat PBR. Written to the pack of the first model that uses them.
FLAT = {
    'Glass_Clear': {'base': [0.92, 0.95, 0.97], 'metal': 0.0, 'rough': 0.04, 'opacity': 0.18},
    'Window_Light': {'base': [0.0, 0.0, 0.0], 'metal': 0.0, 'rough': 0.6, 'emissive': [0.85, 0.92, 1.0], 'strength': 2.5},
    'Plastic_White': {'base': [0.86, 0.86, 0.84], 'metal': 0.0, 'rough': 0.45},
    'Marble_White': {'base': [0.86, 0.85, 0.82], 'metal': 0.0, 'rough': 0.32},
    'Steel_Blade': {'base': [0.62, 0.62, 0.64], 'metal': 1.0, 'rough': 0.28},
}
FLAT_MATERIALS = {
    'Cabinets_glass': 'Glass_Clear', 'Clocks_glass': 'Glass_Clear', 'Emissive': 'Window_Light', 'Plastic': 'Plastic_White',
    ('David', 'DefaultMaterial'): 'Marble_White', ('Knife', 'DefaultMaterial'): 'Steel_Blade',
}
# Materials made from another set's maps with new colour / finish: the door hardware's "White" set paints the lever
# and rosettes flat white, so brass multiplies its albedo by a brass tint (the detail - keyholes, screws - stays)
# and drops its metal / roughness maps for polished metal. 'maps' = the source maps kept.
DERIVED = {'Door_Hardware_Brass': {'from': 'Door_Hardware_White', 'maps': ['albedo', 'normal', 'ao'],
                                   'base': [0.85, 0.64, 0.3], 'metal': 1.0, 'rough': 0.3}}
# Texture sets that are see-through glass: drawn in the Transparent queue.
GLASS_SETS = {'Coffeemaker_Glass': 0.3, 'Shower_Glass': 0.25}
# Emissive sets: the lit parts of lamps and clock faces.
EMISSIVE = {'Lamps_B': ([1.0, 0.86, 0.68], 3.0), 'Lamps_C': ([1.0, 0.9, 0.75], 3.0),
            'Lamps_Table_On': ([1.0, 0.86, 0.68], 3.0), 'Clocks': ([1.0, 1.0, 1.0], 1.0)}

# Files whose units are off by a power of ten (no unit metadata, or wrong): extra import scale.
SCALE = {'Door_Op_1': 0.1, 'Door_Op_2': 0.1, 'David': 0.01, 'Knife': 0.01}
# Kit files that stack every variant in one place: the nodes left out (importer excludeNodes). Each door keeps
# lock set 1 (lever handle + cylinder) with deadbolt 11 and their strike plates, the hanging (unhooked) chain and
# one house number.
DOOR_EXCLUDE = ([f'Lock_{i}' for i in range(2, 15) if i != 11] + [f'Strike_plate_{i}' for i in range(2, 15) if i != 11]
                + ['Chain_closed'] + [f'Number_{i}' for i in range(10) if i != 7])
EXCLUDE = {'Door_Op_1': DOOR_EXCLUDE, 'Door_Op_2': DOOR_EXCLUDE}
# Extra copies of a file, each leaving out more nodes: the door kit's leaf (to hinge) and frame (to fix in the wall).
# They share the whole door's pivot, so both dropped at one spot make the closed door.
# Interior: no peephole, house number, chain or deadbolt.
DOOR_INTERIOR = {'Leaf_Interior': ['Frame_group', 'Peephole', 'Numbers', 'Lock_11', 'Door_catch'],
                 'Frame_Interior': ['Door_group', 'Chain_open', 'Frame_catch', 'Strike_plate_11']}
SPLITS = {'Door_Op_1': {'Leaf': ['Frame_group'], 'Frame': ['Door_group'], **DOOR_INTERIOR},  # Door_Op_2's nodes are named otherwise
          # the switch kit is four switches side by side: one flat single rocker
          'Light_Switches': {'Single': ['Switch_Double_Classic', 'Switch_Double_Flat', 'Switch_Single_Classic']}}
# Files that lie on their side as exported: importer upAxis "Z" (up +Z, front -Y) or "-Z" (up -Z, front +Y).
UP_AXIS = [(re.compile(r'^Curtains_'), '-Z'), (re.compile(r'^Lamp_Ceilingfan$'), 'Z')]

# Pivot: bottom centre by default. Modular building pieces snap at a corner; ceiling lamps hang from their top.
CORNER_PIVOT = re.compile(r'^(Ceiling|Floor|Wall)_')
TOP_PIVOT = re.compile(r'(Ceiling|Chandelier|Fluorescent)', re.I)
# A part (door, drawer, lid ...) keeps its body's pivot, so dropping both at one spot assembles the piece.
PART_TOKENS = {'Door', 'Drawer', 'Lid', 'Extension', 'Glass', 'Pillow', 'L', 'R', 'A', 'B', 'C', 'D', 'E', 'F'}
PART_WORDS = {'Door', 'Drawer', 'Lid', 'Extension', 'Glass', 'Pillow'}


def norm(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


def guid():
    while True:
        g = secrets.token_hex(8)
        if g != '0' * 16:
            return g


def glob_raw(raw, pattern):
    import glob
    return sorted(glob.glob(os.path.join(raw, pattern.replace('/', os.sep))))


def write_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write('\n')


def write_meta(path, rel_folder, kind, extra=None, existing_ok=True):
    meta_path = path + '.meta'
    old = {}
    if existing_ok and os.path.exists(meta_path):
        with open(meta_path, encoding='utf-8') as f:
            old = json.load(f)
    data = {'folder': rel_folder, 'guid': old.get('guid') or guid(), 'metaVersion': 1, 'type': kind}
    data.update(extra or {})
    write_json(meta_path, data)
    return data['guid']


def model_report(exe, paths):
    out = subprocess.run([exe, '--model-report', *paths], capture_output=True, text=True, encoding='utf-8', errors='replace')
    rows = {}
    rx = re.compile(r'\[ModelReport\] (.*) size (\S+) (\S+) (\S+) min (\S+) (\S+) (\S+) max (\S+) (\S+) (\S+) meshes (\d+) materials ?(.*)$')
    for line in out.stdout.splitlines():
        m = rx.match(line.strip())
        if m:
            v = list(map(float, m.groups()[1:10]))
            rows[os.path.normcase(os.path.normpath(m.group(1)))] = {
                'min': v[3:6], 'max': v[6:9], 'meshes': int(m.group(11)),
                'materials': [x for x in m.group(12).split('|') if x]}
    if 'failed' not in out.stdout or ' 0 failed' not in out.stdout:
        sys.exit('import_model_library: --model-report failed:\n' + out.stdout[-2000:] + out.stderr[-2000:])
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('repo')
    ap.add_argument('--raw', default=os.environ.get('TARTARUS_RAW', DEFAULT_RAW))
    ap.add_argument('--exe')
    ap.add_argument('--gallery', action='store_true')
    args = ap.parse_args()
    repo, raw = os.path.abspath(args.repo), os.path.abspath(args.raw)
    exe = args.exe or os.path.join(repo, 'build', 'Release', 'TartarusEngine.exe')
    assets = os.path.join(repo, 'project', 'assets')

    # 1. Texture sets.
    sets = {}  # name -> {'pack': dest, 'maps': {kind: abs src}}
    for pack in PACKS:
        for pattern in pack['textures']:
            for folder in glob_raw(raw, pattern):
                if not os.path.isdir(folder):
                    continue
                for f in sorted(os.listdir(folder)):
                    stem, ext = os.path.splitext(f)
                    if ext.lower() not in IMAGE_EXT:
                        continue
                    kind, key = classify(stem)
                    if kind is None:
                        sys.exit(f'import_model_library: no map type for {os.path.join(folder, f)}')
                    if kind == 'skip':
                        continue
                    name = SET_NAMES.get(key, key)
                    s = sets.setdefault(name, {'pack': pack['dest'], 'maps': {}})
                    if kind in s['maps'] and s['maps'][kind] != os.path.join(folder, f):
                        sys.exit(f'import_model_library: two {kind} maps for set {name}')
                    s['maps'][kind] = os.path.join(folder, f)

    # 2. Models and their bounds / material names (from the engine, so scale matches the import exactly).
    models = []  # {'pack', 'src', 'stem'}
    for pack in PACKS:
        for pattern in pack['models']:
            for src in glob_raw(raw, pattern):
                if any(s in os.path.basename(src) for s in pack.get('skip', [])):
                    continue
                models.append({'pack': pack['dest'], 'src': src, 'stem': os.path.splitext(os.path.basename(src))[0]})
    # Folders, not files: 380 paths overflow the Windows command line. The report recurses into each.
    report = model_report(exe, sorted({os.path.dirname(m['src']) for m in models}))
    for m in models:
        r = report.get(os.path.normcase(os.path.normpath(m['src'])))
        if not r:
            sys.exit(f'import_model_library: no report for {m["src"]}')
        k = SCALE.get(m['stem'], 1.0)
        m['scale'] = k
        lo, hi = [x * k for x in r['min']], [x * k for x in r['max']]
        m['up'] = next((a for rx, a in UP_AXIS if rx.match(m['stem'])), 'Y')
        if m['up'] == 'Z':  # (x, y, z) -> (x, z, -y), as the engine's correction
            lo, hi = [lo[0], lo[2], -hi[1]], [hi[0], hi[2], -lo[1]]
        elif m['up'] == '-Z':  # (x, y, z) -> (x, -z, y)
            lo, hi = [lo[0], -hi[2], lo[1]], [hi[0], -lo[2], hi[1]]
        m['min'], m['max'] = lo, hi
        m['materials'] = r['materials']
    for m in list(models):
        for suffix, extra in SPLITS.get(m['stem'], {}).items():
            models.append(dict(m, stem=f'{m["stem"]}_{suffix}', split_of=m['stem'],
                               exclude=EXCLUDE.get(m['stem'], []) + extra))

    # 3. Material assignment.
    by_norm = {norm(n): n for n in sets if n not in NOT_A_MATERIAL}

    def set_for(stem, mat):
        for key in ((stem, mat), mat):
            if key in FLAT_MATERIALS:
                return 'flat', FLAT_MATERIALS[key]
            if key in MATERIAL_SETS:
                return 'set', MATERIAL_SETS[key]
        n = norm(mat)
        if n in by_norm:
            return 'set', by_norm[n]
        hits = [s for k, s in by_norm.items() if k.startswith(n) or (len(n) >= 6 and n in k)]
        if len(hits) == 1:
            return 'set', hits[0]
        hits = [s for s in hits if norm(s).startswith(n + 'clean')] or hits
        if len(hits) == 1:
            return 'set', hits[0]
        return None, hits

    used_sets, used_flat, unmapped = {}, {}, []
    for m in models:
        m['remap'] = {}
        for mat in m['materials']:
            kind, name = set_for(m['stem'], mat)
            if kind is None:
                unmapped.append(f'{m["stem"]}: {mat} (candidates {name})')
                continue
            if kind == 'set' and name not in sets and name not in DERIVED:
                unmapped.append(f'{m["stem"]}: {mat} -> missing set {name}')
                continue
            (used_sets if kind == 'set' else used_flat).setdefault(name, m['pack'])
            m['remap'][mat] = (kind, name)
    if unmapped:
        sys.exit('import_model_library: unmapped materials:\n  ' + '\n  '.join(unmapped))

    # 4. Copy textures + write their metas; write one .mat per set (every set, not only the used ones: the
    #    colour variants - walls, switches, outlets, door hardware - are there to swap in).
    def rel(p):
        return os.path.relpath(p, os.path.join(repo, 'project')).replace(os.sep, '/')

    tex_dest = {}  # (set, kind) -> project-relative path
    for name, s in sorted(sets.items()):
        folder = os.path.join(assets, *s['pack'].split('/'), 'Textures', name)
        for kind, src in s['maps'].items():
            dst = os.path.join(folder, os.path.basename(src))
            os.makedirs(folder, exist_ok=True)
            if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(src):
                shutil.copy2(src, dst)
            linear = kind != 'albedo' and kind != 'emissive'
            write_meta(dst, rel(folder), 'texture', {'importer': {
                'anisoLevel': 8, 'compression': 1, 'filterMode': 2, 'generateMipmaps': True, 'isSRGB': not linear,
                'maxTextureSize': 2048, 'textureType': 1 if kind == 'normal' else 0, 'wrapMode': 0}})
            tex_dest[(name, kind)] = rel(dst)

    mat_paths = {}

    def write_mat(name, pack, props, queue=None, opacity=None):
        folder = os.path.join(assets, *pack.split('/'), 'Materials')
        path = os.path.join(folder, f'M_{name}.mat')
        data = {'factorsScaleMaps': True, 'matVersion': 2, 'name': f'M_{name}', 'properties': props,
                'shader': 'engine://Standard.shader'}
        if queue is not None:
            data['renderQueue'] = queue
        if opacity is not None:
            data['opacity'] = opacity
        write_json(path, data)
        write_meta(path, rel(folder), 'material')
        mat_paths[name] = rel(path)

    def props_base():
        return {'_AOMap': '', '_AlbedoMap': '', '_BaseColor': [1.0, 1.0, 1.0], '_DoubleSided': False,
                '_EmissiveColor': [0.0, 0.0, 0.0], '_EmissiveMap': '', '_EmissiveStrength': 1.0, '_Metallic': 0.0,
                '_MetallicMap': '', '_MetallicRoughnessMap': '', '_NormalMap': '', '_Roughness': 0.6,
                '_RoughnessMap': '', '_Triplanar': False, '_TriplanarScale': 1.0}

    slot = {'albedo': '_AlbedoMap', 'normal': '_NormalMap', 'metallic': '_MetallicMap', 'roughness': '_RoughnessMap',
            'ao': '_AOMap', 'emissive': '_EmissiveMap'}
    for name, s in sorted(sets.items()):
        if name in NOT_A_MATERIAL:
            continue
        p = props_base()
        kinds = {}
        if name in INHERIT:
            kinds.update({k: tex_dest[(INHERIT[name], k)] for k in sets[INHERIT[name]]['maps']})
        kinds.update({k: tex_dest[(name, k)] for k in s['maps']})
        for kind, path in kinds.items():
            p[slot[kind]] = path
        if 'metallic' in kinds:
            p['_Metallic'] = 1.0
        if 'roughness' in kinds:
            p['_Roughness'] = 1.0
        if name in EMISSIVE and 'emissive' in kinds:
            p['_EmissiveColor'], p['_EmissiveStrength'] = EMISSIVE[name]
        if name in GLASS_SETS:
            write_mat(name, s['pack'], p, queue=2, opacity=GLASS_SETS[name])
        else:
            write_mat(name, s['pack'], p)
    for name, d in sorted(DERIVED.items()):
        p = props_base()
        for kind in d['maps']:
            if kind in sets[d['from']]['maps']:
                p[slot[kind]] = tex_dest[(d['from'], kind)]
        p['_BaseColor'], p['_Metallic'], p['_Roughness'] = d['base'], d['metal'], d['rough']
        write_mat(name, sets[d['from']]['pack'], p)
    for name, pack in sorted(used_flat.items()):
        f = FLAT[name]
        p = props_base()
        p['_BaseColor'], p['_Metallic'], p['_Roughness'] = f['base'], f['metal'], f['rough']
        if 'emissive' in f:
            p['_EmissiveColor'], p['_EmissiveStrength'] = f['emissive'], f['strength']
        write_mat(name, pack, p, queue=2 if 'opacity' in f else None, opacity=f.get('opacity'))

    # 5. Pivots: bottom centre, corner for building modules, top for ceiling lamps; parts follow their body.
    def own_offset(m):
        lo, hi = m['min'], m['max']
        cx, cz = (lo[0] + hi[0]) / 2, (lo[2] + hi[2]) / 2
        if m['pack'] == 'Architecture/Building' and CORNER_PIVOT.match(m['stem']):
            return [-lo[0], -lo[1], -lo[2]]
        if TOP_PIVOT.search(m['stem']):
            return [-cx, -hi[1], -cz]
        return [-cx, -lo[1], -cz]

    by_pack_stem = {(m['pack'], m['stem']): m for m in models}

    def body_of(m):
        if m.get('split_of'):
            return by_pack_stem[(m['pack'], m['split_of'])]
        tokens = m['stem'].split('_')
        for n in range(len(tokens) - 1, 0, -1):
            rest = tokens[n:]
            if not any(t in PART_WORDS for t in rest) or not all(t in PART_TOKENS for t in rest):
                continue
            body = by_pack_stem.get((m['pack'], '_'.join(tokens[:n])))
            if body:
                return body
        return None

    def root_of(m):
        seen = set()
        while True:
            b = body_of(m)
            if not b or b['stem'] in seen:
                return m
            seen.add(m['stem'])
            m = b

    for m in models:
        root = root_of(m)
        m['body'] = root['stem'] if root is not m else None
        m['offset'] = [round(v, 4) for v in own_offset(root)]

    # 6. Models + metas.
    for m in models:
        folder = os.path.join(assets, *m['pack'].split('/'), 'Models')
        dst = os.path.join(folder, m['stem'] + os.path.splitext(m['src'])[1])
        os.makedirs(folder, exist_ok=True)
        if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(m['src']):
            shutil.copy2(m['src'], dst)
        importer = {'globalScale': m['scale'], 'importAnimations': False, 'importNormals': True, 'importSkeleton': False,
                    'materialImportMode': 1, 'optimizeGraph': True}
        if any(abs(v) > 1e-4 for v in m['offset']):
            importer['pivotOffset'] = m['offset']
        if m['up'] != 'Y':
            importer['upAxis'] = m['up']
        if m.get('exclude') or m['stem'] in EXCLUDE:
            importer['excludeNodes'] = m.get('exclude') or EXCLUDE[m['stem']]
        remap = {mat: mat_paths[name] for mat, (_, name) in m['remap'].items()}
        m['guid'] = write_meta(dst, rel(folder), 'model', {'importer': importer, 'materialRemap': remap})
        m['path'] = rel(dst)

    print(f'import_model_library: {len(models)} models, {len(sets)} texture sets, {len(mat_paths)} materials')

    if args.gallery:
        write_gallery(repo, models)


def write_gallery(repo, models):
    """project/scenes/AssetGallery.json: one row per pack along +X, rows stepping back along -Z, on a
    prototype-grid floor with the Sandbox's sun, sky and post settings."""
    scenes = os.path.join(repo, 'project', 'scenes')
    sandbox = json.load(open(os.path.join(scenes, 'Sandbox.json'), encoding='utf-8'))
    skip = {'boxes', 'empties', 'models', 'libraryMaterials', 'libraryModels', 'libraryPrefabs', 'librarySounds',
            'libraryTextures'}
    scene = {k: v for k, v in sandbox.items() if k not in skip}
    scene['formatVersion'] = 4
    boxes, empties, ents = [], [], []
    ids = iter(range(1, 1_000_000))
    order = iter(range(1_000_000))
    ident = [0.0, 0.0, 0.0, 1.0]

    def empty(name, parent=-1, pos=(0, 0, 0)):
        e = {'id': next(ids), 'name': name, 'order': next(order), 'parentId': parent, 'position': list(pos),
             'rotation': ident, 'scale': [1.0, 1.0, 1.0]}
        empties.append(e)
        return e['id']

    lighting = empty('Lighting')
    for e in sandbox['empties']:
        if e['name'] == 'Sun':
            sun = json.loads(json.dumps(e))
            sun.update({'id': next(ids), 'order': next(order), 'parentId': lighting})
            empties.append(sun)

    # Parts sit with their body; every other model gets its own slot along the row.
    groups = {}
    for m in models:
        groups.setdefault(m['pack'], []).append(m)
    row_z, gap = 0.0, 0.6
    rows = []
    for pack in [p['dest'] for p in PACKS]:
        ms = groups.get(pack, [])
        if not ms:
            continue
        parent = empty(pack.replace('/', ' / '), pos=(0.0, 0.0, row_z))
        x, depth, height = 0.0, 0.0, 0.0
        slots = {}
        for m in ms:
            key = m['body'] or m['stem']
            if key not in slots:
                body = next(b for b in ms if b['stem'] == key)
                fam = [b for b in ms if (b['body'] or b['stem']) == key]
                lo = min(b['min'][0] + b['offset'][0] for b in fam)
                hi = max(b['max'][0] + b['offset'][0] for b in fam)
                d = max(b['max'][2] - b['min'][2] for b in fam)
                slots[key] = x - lo
                x += hi - lo + gap
                depth = max(depth, d)
                height = max(height, max(b['max'][1] - b['min'][1] for b in fam))
            # Hanging pieces (pivot at the top) are lifted so they stand on the floor here.
            lift = max(0.0, -(m['min'][1] + m['offset'][1])) if not m['body'] else 0.0
            if m['body']:
                root = next(b for b in ms if b['stem'] == m['body'])
                lift = max(0.0, -(root['min'][1] + root['offset'][1]))
            ents.append({'id': next(ids), 'name': m['stem'], 'order': next(order), 'parentId': parent,
                         'path': m['path'], 'pathGuid': m['guid'], 'position': [round(slots[key], 3), round(lift, 3), 0.0],
                         'rotation': ident, 'scale': [1.0, 1.0, 1.0]})
        rows.append((pack, row_z, x, depth, height))
        row_z -= max(depth, 1.0) + 6.0
    # Floor: prototype grid box under everything.
    width = max(r[2] for r in rows) + 4.0
    length = -row_z + 4.0
    grid = 'assets/Materials/Prototype/proto_grid_light.png'
    grid_guid = json.load(open(os.path.join(repo, 'project', grid + '.meta'), encoding='utf-8'))['guid']
    boxes.append({'collider': {'friction': 0.8, 'isTrigger': False}, 'color': [1.0, 1.0, 1.0],
                  'id': next(ids), 'name': 'Floor', 'order': next(order), 'parentId': -1,
                  'position': [round(width / 2 - 2.0, 3), -0.05, round(-length / 2 + 2.0, 3)], 'rotation': ident,
                  'scale': [round(width, 3), 0.1, round(length, 3)],
                  'materials': [{'embedded': {
                      'albedoMap': {'path': grid, 'pathGuid': grid_guid}, 'aoMap': '', 'baseColor': [1, 1, 1],
                      'emissiveColor': [0, 0, 0], 'emissiveMap': '', 'emissiveStrength': 1.0, 'factorsScaleMaps': True,
                      'metallic': 0.0, 'metallicMap': '', 'metallicRoughnessMap': '', 'normalMap': '', 'roughness': 0.9,
                      'roughnessMap': '', 'triplanar': True, 'triplanarScale': 1.0}}]})
    scene.update({'boxes': boxes, 'empties': empties, 'models': ents})
    write_json(os.path.join(scenes, 'AssetGallery.json'), scene)
    # Review cameras for --smoke-shots (TARTARUS_SHOT_CAMERAS): each row in windows about 7 m wide.
    cams = []
    for pack, z, w, d, h in rows:
        name = pack.replace('/', '_')
        n = max(1, round(w / 7.0))
        for i in range(n):
            cx = w * (i + 0.5) / n
            span = max(w / n, h * 1.6, 0.4)
            cams.append(f'{name}_{i + 1}:{cx:.2f},{h * 0.55 + span * 0.12:.2f},{z + d / 2 + span * 0.75:.2f},-90,-10')
    with open(os.path.join(repo, 'build', 'gallery_cameras.txt'), 'w') as f:
        f.write(';'.join(cams))
    print(f'import_model_library: wrote scenes/AssetGallery.json ({len(ents)} models in {len(rows)} rows)')


if __name__ == '__main__':
    main()
