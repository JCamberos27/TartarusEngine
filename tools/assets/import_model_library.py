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
#   python tools/assets/import_model_library.py <repo> --gallery-only [--exe ...]   (the gallery from project/assets)
import argparse
import json
import math
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
    'Steel_Blade': {'base': [0.62, 0.62, 0.64], 'metal': 1.0, 'rough': 0.28},
}
FLAT_MATERIALS = {
    'Cabinets_glass': 'Glass_Clear', 'Clocks_glass': 'Glass_Clear', 'Emissive': 'Window_Light', 'Plastic': 'Plastic_White',
    ('Knife', 'DefaultMaterial'): 'Steel_Blade',
}
# Materials made from another set's maps with new colour / finish: the door hardware's "White" set paints the lever
# and rosettes flat white, so brass multiplies its albedo by a brass tint (the detail - keyholes, screws - stays)
# and drops its metal / roughness maps for polished metal. 'maps' = the source maps kept.
DERIVED = {'Door_Hardware_Brass': {'from': 'Door_Hardware_White', 'maps': ['albedo', 'normal', 'ao'],
                                   'base': [0.85, 0.64, 0.3], 'metal': 1.0, 'rough': 0.3}}
# Texture sets on single-sided cloth: drawn from both sides (a curtain seen from the room is its back).
DOUBLE_SIDED = {'Curtains_Ab', 'Curtains_Cd'}
# Texture sets that are see-through glass: drawn in the Transparent queue.
GLASS_SETS = {'Coffeemaker_Glass': 0.3, 'Shower_Glass': 0.25}
# Emissive sets: the lit parts of lamps and clock faces.
EMISSIVE = {'Lamps_B': ([1.0, 0.86, 0.68], 3.0), 'Lamps_C': ([1.0, 0.9, 0.75], 3.0),
            'Lamps_Table_On': ([1.0, 0.86, 0.68], 3.0), 'Clocks': ([1.0, 1.0, 1.0], 1.0)}

# Files whose units are off by a power of ten (no unit metadata, or wrong): extra import scale.
SCALE = {'Door_Op_1': 0.1, 'Door_Op_2': 0.1, 'Knife': 0.01}
# Kit files that stack every variant in one place: the nodes left out (importer excludeNodes). Each door keeps
# lock set 1 (lever handle + cylinder) with deadbolt 11 and their strike plates, the hanging (unhooked) chain and
# one house number.
DOOR_EXCLUDE = ([f'Lock_{i}' for i in range(2, 15) if i != 11] + [f'Strike_plate_{i}' for i in range(2, 15) if i != 11]
                + ['Chain_closed'] + [f'Number_{i}' for i in range(10) if i != 7])
# Files that ship their second LOD in place over the first (both drew, z-fighting): LOD00 kept.
LOD_EXCLUDE = {'Couch': ['Couch_LOD01'], 'Bed': ['Bed_king_main_LOD01'], 'Book_Pile_A': ['Book_pile_A_LOD01'],
               'Book_Single_A': ['Book_single_A_LOD01'], 'Book_Single_B': ['Book_single_B_LOD01']}
EXCLUDE = {'Door_Op_1': DOOR_EXCLUDE, 'Door_Op_2': DOOR_EXCLUDE, **LOD_EXCLUDE}
# Extra copies of a file, each leaving out more nodes: the door kit's leaf (to hinge) and frame (to fix in the wall).
# They share the whole door's pivot, so both dropped at one spot make the closed door.
# Interior: no peephole, house number, chain or deadbolt.
DOOR_INTERIOR = {'Leaf_Interior': ['Frame_group', 'Peephole', 'Numbers', 'Lock_11', 'Door_catch'],
                 'Frame_Interior': ['Door_group', 'Chain_open', 'Frame_catch', 'Strike_plate_11']}
SPLITS = {'Door_Op_1': {'Leaf': ['Frame_group'], 'Frame': ['Door_group'], **DOOR_INTERIOR},  # Door_Op_2's nodes are named otherwise
          # the switch kit is four switches side by side: one flat single rocker
          'Light_Switches': {'Single': ['Switch_Double_Classic', 'Switch_Double_Flat', 'Switch_Single_Classic']}}
# Files that lie on their side as exported: importer upAxis "Z" (up +Z, front -Y) or "-Z" (up -Z, front +Y).
UP_AXIS = [(re.compile(r'^Curtains_'), '-Z')]

# Pivot: bottom centre by default. Modular building pieces snap at a corner; ceiling lamps hang from their top.
CORNER_PIVOT = re.compile(r'^(Ceiling|Floor|Wall)_')
TOP_PIVOT = re.compile(r'(Ceiling|Chandelier|Fluorescent)', re.I)
# A part (door, drawer, lid ...) keeps its body's pivot, so dropping both at one spot assembles the piece.
PART_TOKENS = {'Door', 'Drawer', 'Lid', 'Extension', 'Glass', 'Pillow', 'L', 'R', 'A', 'B', 'C', 'D', 'E', 'F'}
PART_WORDS = {'Door', 'Drawer', 'Lid', 'Extension', 'Glass', 'Pillow'}
# Names that read as a part but are a piece of their own (a wall module with a doorway), and parts whose body the
# name doesn't give.
NOT_PARTS = {'Wall_3m_Door'}
PART_OF = {'Electricbox_Door': 'Electricbox_Main', 'Vase_B_Sticks': 'Vase_B',
           # the crown moulding's returns at either end of a run of wall cabinets
           'Kitchen_Cabinet_End_L': 'Kitchen_Cabinet_A', 'Kitchen_Cabinet_End_R': 'Kitchen_Cabinet_A',
           'Papers_Box_Lid_A': 'Papers_Box_Open_A', 'Papers_Box_Lid_B': 'Papers_Box_Open_B'}
# Children exported with a stray quarter turn against their parent (importer unrotateNodes): every glass door's pane
# lay flat across its frame, the fan's blades stood on edge, the stove's hatch and the wall clock's glass and hands
# stuck out of their faces. Dropping the child's own rotation puts each where it belongs.
UNROTATE = {
    'Kitchen_Cabinet_A_Door_B_L': ['Kitchen_cabinet_A_door_glass'], 'Kitchen_Cabinet_A_Door_B_R': ['Kitchen_cabinet_A_door_glass'],
    'Kitchen_Cabinet_B_Door_B_L': ['Kitchen_cabinet_B_door_glass'], 'Kitchen_Cabinet_B_Door_B_R': ['Kitchen_cabinet_B_door_glass'],
    'Kitchen_Cabinet_Corner_Door_B': ['Kitchen_cabinet_corner_door_glass'],
    'Bookshelf_Door_A_L': ['Bookshelf_door_A_L_glass'], 'Bookshelf_Door_A_R': ['Bookshelf_door_A_R_glass'],
    'Dresser_Large_A_Extension_Door_L': ['Dresser_large_A_extension_door_L_glass'],
    'Dresser_Large_A_Extension_Door_R': ['Dresser_large_A_extension_door_R_glass'],
    'Stove': ['Stove_hatch'], 'Clock_Wall_Analog': ['Clock_wall_glass', 'Clock_pointer_minute', 'Clock_pointer_hour'],
    'Lamp_Ceilingfan': ['Lamp_ceilingfan_blades'], 'Lamp_Wall': ['Lamp_wall_shade'],
}
# Children exported out of place (importer moveNodes, meters in the file's space): the right glass door's pane sits a
# door's width to its left (moved to mirror the left door's, 5.3 cm in from the outer edge); the oven door, once
# upright, stands 35 cm inside the stove (moved flush with its front); the wall lamp's shade sits behind the wall
# (moved over the bulb socket, 17.75 cm out, wide end down, sunk over it like the table lamps' in assemble_kits.py).
MOVE = {'Kitchen_Cabinet_B_Door_B_R': [{'node': 'Kitchen_cabinet_B_door_glass', 'by': [1.003, 0.0, 0.0]}],
        'Stove': [{'node': 'Stove_hatch', 'by': [0.0, 0.0, 0.35]}],
        'Lamp_Wall': [{'node': 'Lamp_wall_shade', 'by': [0.001, -0.195, 0.225]}],
        # assemble_kits.py stood the fridge door in front of the body, its shelf bins showing through a 9 cm gap
        'Fridge_Assembled': [{'node': 'Fridge_door', 'by': [0.0, 0.0, -0.088]}]}

# --- gallery --------------------------------------------------------------------------------------------------
# Where each loose part sits on its body (everything not listed sits as authored, which for most kits is in place).
# The kits lay their parts out in a row beside the body at the right height and depth:
#   'row'         the parts on one body move together along X until they're centred on it
#   'centre'      centred on the body in X
#   'lid'         centred on the body in X and Z, resting on its top
#   'in'          centred on the body in X and Z, at the height as authored (sticks in a vase, a lid already at height)
#   ('span', x0, x1)  centred in X on that span of the body (its own space, meters)
#   ('place', x, z, yaw)  turned `yaw` degrees about Y, its centre at (x, z) of the body (height as authored)
#   ('on', stem)  the rule after it, against another part (already seated) instead of the body
#   ('as', stem)  an alternative to that part (glass doors for wooden ones): shown on a second copy of the body, in its place
SEATS = {
    'Cabinet_A_Door_A_L': 'row', 'Cabinet_A_Door_A_R': 'row', 'Cabinet_A_Door_B_L': 'row', 'Cabinet_A_Door_B_R': 'row',
    'Bookshelf_Door_A_L': 'row', 'Bookshelf_Door_A_R': 'row',
    'Dresser_Large_A_Extension': 'centre',
    'Dresser_Large_A_Extension_Door_L': ('on', 'Dresser_Large_A_Extension', 'row'),
    'Dresser_Large_A_Extension_Door_R': ('on', 'Dresser_Large_A_Extension', 'row'),
    'Bathroom_Box_A_Lid': 'lid', 'Cardboard_Box_A_Lid': 'lid', 'Toy_Box_A_Lid': 'lid',
    'Vase_B_Sticks': 'in', 'Papers_Box_Lid_A': 'in', 'Papers_Box_Lid_B': 'in',
    'Kitchen_Cabinet_A_Door_B_L': ('as', 'Kitchen_Cabinet_A_Door_A_L'), 'Kitchen_Cabinet_A_Door_B_R': ('as', 'Kitchen_Cabinet_A_Door_A_R'),
    'Kitchen_Cabinet_B_Door_B_L': ('as', 'Kitchen_Cabinet_B_Door_A_L'), 'Kitchen_Cabinet_B_Door_B_R': ('as', 'Kitchen_Cabinet_B_Door_A_R'),
    'Kitchen_Cabinet_Corner_Door_B': ('as', 'Kitchen_Cabinet_Corner_Door_A'),
    # the corner unit's top is a pentagon: its door closes the 45-degree face across the notch at its front left,
    # running from (-0.451, -0.049) to (0.049, 0.451) in the body's space and facing front left
    'Kitchen_Cabinet_Corner_Door_A': ('place', -0.218, 0.218, -45.0),
}
# Other files' parts that also close a body, each where the shared layout puts it plus an offset (meters): the sink
# doors are laid out on the vanity's left bay; a second pair closes its right bay.
# The crown moulding's end returns are laid out on Kitchen_Cabinet_A; every wall cabinet shares its top and depth.
EXTRA = {'Bathroom_Sink_Body': [('Bathroom_Sink_Door_L', [0.0, 0.0, 0.0]), ('Bathroom_Sink_Door_R', [0.0, 0.0, 0.0]),
                                ('Bathroom_Sink_Door_L', [0.919, 0.0, 0.0]), ('Bathroom_Sink_Door_R', [0.919, 0.0, 0.0])],
         'Kitchen_Cabinet_B': [('Kitchen_Cabinet_End_L', [1.2, 0.0, 0.0]), ('Kitchen_Cabinet_End_R', [1.2, 0.0, 0.0])],
         'Kitchen_Cabinet_Corner': [('Kitchen_Cabinet_End_L', [3.0, 0.0, 0.0])]}


def body_stem(stem, stems):
    """The body a part file belongs to ('Cabinet_A_Door_A_L' -> 'Cabinet_A'), from the stems in its pack; None if
    it is a piece of its own."""
    if stem in PART_OF:
        return PART_OF[stem] if PART_OF[stem] in stems else None
    if stem in NOT_PARTS:
        return None
    tokens = stem.split('_')
    for n in range(len(tokens) - 1, 0, -1):
        rest = tokens[n:]
        if not any(t in PART_WORDS for t in rest) or not all(t in PART_TOKENS for t in rest):
            continue
        if '_'.join(tokens[:n]) in stems:
            return '_'.join(tokens[:n])
    return None


def own_pivot(pack, stem, lo, hi):
    """The offset that puts a model's pivot where it is useful, from its bounds."""
    cx, cz = (lo[0] + hi[0]) / 2, (lo[2] + hi[2]) / 2
    if pack == 'Architecture/Building' and CORNER_PIVOT.match(stem):
        return [-lo[0], -lo[1], -lo[2]]
    if TOP_PIVOT.search(stem):
        return [-cx, -hi[1], -cz]
    return [-cx, -lo[1], -cz]


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
    # Only the gallery, from the models already in project/assets (no Raw folder needed): first brings their metas'
    # node fixes and own pivots up to date with the tables above.
    ap.add_argument('--gallery-only', action='store_true')
    args = ap.parse_args()
    repo, raw = os.path.abspath(args.repo), os.path.abspath(args.raw)
    exe = os.path.abspath(args.exe) if args.exe else os.path.join(repo, 'build', 'Release', 'TartarusEngine.exe')
    assets = os.path.join(repo, 'project', 'assets')
    if args.gallery_only:
        update_metas(repo, exe)
        write_gallery(repo, exe)
        return

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
        p['_DoubleSided'] = name in DOUBLE_SIDED
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
        return own_pivot(m['pack'], m['stem'], m['min'], m['max'])

    by_pack_stem = {(m['pack'], m['stem']): m for m in models}

    def body_of(m):
        if m.get('split_of'):
            return by_pack_stem[(m['pack'], m['split_of'])]
        stem = body_stem(m['stem'], {s for p, s in by_pack_stem if p == m['pack']})
        return by_pack_stem[(m['pack'], stem)] if stem else None

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
                    'materialImportMode': 1, 'mergeByMaterial': True, 'optimizeGraph': True}
        if any(abs(v) > 1e-4 for v in m['offset']):
            importer['pivotOffset'] = m['offset']
        if m['up'] != 'Y':
            importer['upAxis'] = m['up']
        if m.get('exclude') or m['stem'] in EXCLUDE:
            importer['excludeNodes'] = m.get('exclude') or EXCLUDE[m['stem']]
        if m['stem'] in UNROTATE:
            importer['unrotateNodes'] = UNROTATE[m['stem']]
        if m['stem'] in MOVE:
            importer['moveNodes'] = MOVE[m['stem']]
        remap = {mat: mat_paths[name] for mat, (_, name) in m['remap'].items()}
        m['guid'] = write_meta(dst, rel(folder), 'model', {'importer': importer, 'materialRemap': remap})
        m['path'] = rel(dst)

    print(f'import_model_library: {len(models)} models, {len(sets)} texture sets, {len(mat_paths)} materials')
    update_metas(repo, exe)  # texture sizes by use, and the fixes for the _Assembled files

    if args.gallery:
        write_gallery(repo, exe)


def project_models(repo, exe):
    """Every model in the packs' Models folders as the engine imports it: {pack: {stem: model}}, each with its
    project path, guid, pivot offset and bounds (meters, the model's own space, pivot applied)."""
    assets = os.path.join(repo, 'project', 'assets')
    folders = [os.path.join(assets, *p['dest'].split('/'), 'Models') for p in PACKS]
    report = model_report(exe, [f for f in folders if os.path.isdir(f)])
    out = {}
    for pack, folder in zip([p['dest'] for p in PACKS], folders):
        for f in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
            if os.path.splitext(f)[1].lower() != '.fbx' or not os.path.exists(os.path.join(folder, f + '.meta')):
                continue
            path = os.path.join(folder, f)
            r = report.get(os.path.normcase(os.path.normpath(path)))
            if not r:
                sys.exit(f'import_model_library: no report for {path}')
            with open(path + '.meta', encoding='utf-8') as fh:
                meta = json.load(fh)
            out.setdefault(pack, {})[os.path.splitext(f)[0]] = {
                'pack': pack, 'stem': os.path.splitext(f)[0], 'file': path, 'guid': meta['guid'],
                'path': os.path.relpath(path, os.path.join(repo, 'project')).replace(os.sep, '/'),
                'pivot': meta.get('importer', {}).get('pivotOffset', [0.0, 0.0, 0.0]), 'lo': r['min'], 'hi': r['max']}
    return out


# Gallery pieces smaller than this (meters, largest side) cast no shadow, as apartment_kit.py's loose props.
TINY_PROP = 0.15
# Texture size by what it covers: the largest model (meters, largest side) that uses each map decides.
TEXTURE_TIERS = [(0.3, 512), (1.0, 1024), (float('inf'), 2048)]
IMAGE_PATH = re.compile(r'assets/[^"]+\.(?:png|jpe?g|tga)', re.I)


def size_textures(repo, library):
    """Each library texture's maxTextureSize from the largest model that uses it (TEXTURE_TIERS): a pen's maps
    needn't be the size of a sofa's. Maps no library model uses, and any a scene puts on a box (walls, floors), are
    left as they are. Returns how many metas changed."""
    project = os.path.join(repo, 'project')
    need = {}  # texture path -> size
    mats = {}
    for ms in library.values():
        for m in ms.values():
            with open(m['file'] + '.meta', encoding='utf-8') as f:
                remap = json.load(f).get('materialRemap', {})
            side = max(h - l for l, h in zip(m['lo'], m['hi']))
            tier = next(s for limit, s in TEXTURE_TIERS if side <= limit)
            for mat in remap.values():
                if mat not in mats:
                    with open(os.path.join(project, mat), encoding='utf-8') as f:
                        mats[mat] = IMAGE_PATH.findall(f.read())
                for tex in mats[mat]:
                    need[tex] = max(need.get(tex, 0), tier)
    scenes = os.path.join(project, 'scenes')
    for name in os.listdir(scenes):
        if name.endswith('.json'):
            with open(os.path.join(scenes, name), encoding='utf-8') as f:
                for box in json.load(f).get('boxes', []):
                    for tex in IMAGE_PATH.findall(json.dumps(box.get('materials', []))):
                        need.pop(tex, None)
    changed = 0
    for tex, size in sorted(need.items()):
        path = os.path.join(project, tex) + '.meta'
        if not os.path.exists(path):
            continue
        with open(path, encoding='utf-8') as f:
            meta = json.load(f)
        if meta.get('importer', {}).get('maxTextureSize') != size:
            meta['importer']['maxTextureSize'] = size
            write_json(path, meta)
            changed += 1
    sizes = {}
    for size in need.values():
        sizes[size] = sizes.get(size, 0) + 1
    print('import_model_library: library textures by max size', dict(sorted(sizes.items())))
    return changed


def update_metas(repo, exe):
    """Brings the imported models' metas up to date with UNROTATE, MOVE, LOD_EXCLUDE and NOT_PARTS (a piece that took a body's pivot
    gets its own back), without the Raw folder."""
    changed = 0
    library = project_models(repo, exe)
    for pack, ms in library.items():
        for stem, m in ms.items():
            base = stem[:-len('_Assembled')] if stem.endswith('_Assembled') else stem
            with open(m['file'] + '.meta', encoding='utf-8') as f:
                meta = json.load(f)
            imp = dict(meta['importer'])
            imp['mergeByMaterial'] = True
            up = next((a for rx, a in UP_AXIS if rx.match(base)), 'Y')
            if up != imp.get('upAxis', 'Y') and not stem.endswith('_Assembled'):
                # a new up axis: the pivot is found again from the bounds under it
                imp.pop('pivotOffset', None)
                imp.pop('upAxis', None)
                if up != 'Y':
                    imp['upAxis'] = up
                if 'unrotateNodes' not in imp and base in UNROTATE:
                    imp['unrotateNodes'] = UNROTATE[base]
                meta['importer'] = imp
                write_json(m['file'] + '.meta', meta)
                r = model_report(exe, [m['file']])[os.path.normcase(os.path.normpath(m['file']))]
                imp = dict(imp, pivotOffset=[round(v, 4) for v in own_pivot(pack, stem, r['min'], r['max'])])
            if base in UNROTATE:
                imp['unrotateNodes'] = UNROTATE[base]
            if stem in MOVE:
                imp['moveNodes'] = MOVE[stem]
            if base in LOD_EXCLUDE and not stem.endswith('_Assembled'):
                imp['excludeNodes'] = LOD_EXCLUDE[base]
            if stem in NOT_PARTS:
                # the bounds already have the old pivot applied: the fix adds to it
                fix = own_pivot(pack, stem, m['lo'], m['hi'])
                imp['pivotOffset'] = [round(a + b, 4) for a, b in zip(m['pivot'], fix)]
            if imp != meta['importer']:
                meta['importer'] = imp
                write_json(m['file'] + '.meta', meta)
                changed += 1
    changed += size_textures(repo, library)
    for name in sorted(DOUBLE_SIDED):
        for path in glob_raw(os.path.join(repo, 'project', 'assets'), f'*/*/Materials/M_{name}.mat'):
            with open(path, encoding='utf-8') as f:
                mat = json.load(f)
            if not mat['properties'].get('_DoubleSided'):
                mat['properties']['_DoubleSided'] = True
                write_json(path, mat)
                changed += 1
    print(f'import_model_library: {changed} model metas and materials updated')


def write_gallery(repo, exe):
    """project/scenes/AssetGallery.json: one row per pack along +X, rows stepping back along -Z, on a
    prototype-grid floor with the Sandbox's sun, sky and post settings. Each piece is shown put together: a kit
    with an _Assembled file (assemble_kits.py) shows that, and the loose parts left over sit on their body
    (SEATS), alternative parts on a second copy of it."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from assemble_kits import FITS, KITS
    # body stem -> the part files its _Assembled file already holds
    absorbed = {os.path.splitext(os.path.basename(rel))[0]: parts for rel, parts in FITS.items()}
    absorbed.update({os.path.splitext(os.path.basename(rel))[0]: ([] if callable(fix) else fix[1])
                     for rel, fix in KITS.items()})
    library = project_models(repo, exe)

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

    yaws = {}  # stem -> degrees about Y, for parts seated turned ('place')

    def turn(v, deg):
        """(x, z) of v turned `deg` about Y (right-handed: +90 takes +Z to +X)."""
        r = math.radians(deg)
        return v[0] * math.cos(r) + v[2] * math.sin(r), -v[0] * math.sin(r) + v[2] * math.cos(r)

    def moved(m, off):
        """A model's bounds with its entity at `off` from the body's (and turned, if it is)."""
        lo, hi = m['lo'], m['hi']
        if yaws.get(m['stem']):
            xz = [turn((x, 0.0, z), yaws[m['stem']]) for x in (lo[0], hi[0]) for z in (lo[2], hi[2])]
            lo = [min(p[0] for p in xz), lo[1], min(p[1] for p in xz)]
            hi = [max(p[0] for p in xz), hi[1], max(p[1] for p in xz)]
        return [a + o for a, o in zip(lo, off)], [a + o for a, o in zip(hi, off)]

    def family(body, parts):
        """[(model, its entity's offset from the body's)] with every part seated (SEATS), and their bounds."""
        placed = {body['stem']: (body['lo'], body['hi'])}
        out = [(body, [0.0, 0.0, 0.0])]
        rows = {}  # target stem -> the parts seated on it by 'row' (they move together)
        todo = []
        for p in parts:
            rule, target = SEATS.get(p['stem'], 'raw'), body['stem']
            if isinstance(rule, tuple) and rule[0] == 'as':
                rule = 'raw'  # only seated in place of the other part (write_gallery)
            if isinstance(rule, tuple) and rule[0] == 'on':
                target, rule = rule[1], rule[2]
            todo.append((p, rule, target))
        todo.sort(key=lambda t: t[2] != body['stem'])  # what other parts sit on goes first
        for p, rule, target in todo:
            # Kit files share one layout, so a point of one file's raw space lands on the same point of another's
            # when their entities are offset by the difference of their pivots.
            off = [b - q for b, q in zip(body['pivot'], p['pivot'])]
            lo, hi = moved(p, off)
            tlo, thi = placed[target]
            d = [0.0, 0.0, 0.0]
            if rule == 'centre':
                d[0] = (tlo[0] + thi[0]) / 2 - (lo[0] + hi[0]) / 2
            elif isinstance(rule, tuple) and rule[0] == 'span':
                d[0] = (rule[1] + rule[2]) / 2 - (lo[0] + hi[0]) / 2
            elif isinstance(rule, tuple) and rule[0] == 'place':
                # its own centre (height as authored) turned about its entity's origin, then put at (x, z)
                yaws[p['stem']] = rule[3]
                c = [(a + b) / 2 for a, b in zip(p['lo'], p['hi'])]
                cx, cz = turn(c, rule[3])
                off = [rule[1] - cx, off[1], rule[2] - cz]
            elif rule == 'in':
                d = [(tlo[0] + thi[0]) / 2 - (lo[0] + hi[0]) / 2, 0.0, (tlo[2] + thi[2]) / 2 - (lo[2] + hi[2]) / 2]
            elif rule == 'lid':
                d = [(tlo[0] + thi[0]) / 2 - (lo[0] + hi[0]) / 2, thi[1] - 0.25 * (hi[1] - lo[1]) - lo[1],
                     (tlo[2] + thi[2]) / 2 - (lo[2] + hi[2]) / 2]
            elif rule == 'row':
                rows.setdefault(target, []).append((p, off))
                continue
            off = [a + b for a, b in zip(off, d)]
            placed[p['stem']] = moved(p, off)
            out.append((p, off))
        for target, group in rows.items():
            tlo, thi = placed[target]
            glo = min(moved(p, off)[0][0] for p, off in group)
            ghi = max(moved(p, off)[1][0] for p, off in group)
            dx = (tlo[0] + thi[0]) / 2 - (glo + ghi) / 2
            for p, off in group:
                off = [off[0] + dx, off[1], off[2]]
                placed[p['stem']] = moved(p, off)
                out.append((p, off))
        return out

    def bounds(members):
        return ([min(moved(m, o)[0][i] for m, o in members) for i in range(3)],
                [max(moved(m, o)[1][i] for m, o in members) for i in range(3)])

    row_z, gap = 0.0, 0.6
    rows = []
    for pack in [p['dest'] for p in PACKS]:
        ms = library.get(pack, {})
        if not ms:
            continue
        stems = {s for s in ms if not s.endswith('_Assembled')}
        assembled = {s for s in stems if s + '_Assembled' in ms and s in absorbed}
        hidden = {p for s in assembled for p in absorbed[s]}
        groups = {}  # body stem -> its parts
        for s in sorted(stems):
            b = body_stem(s, stems)
            while b and body_stem(b, stems):
                b = body_stem(b, stems)
            groups.setdefault(b or s, [])
            if b:
                groups[b].append(s)
        parent = empty(pack.replace('/', ' / '), pos=(0.0, 0.0, row_z))
        x, depth, height = 0.0, 0.0, 0.0
        for stem in sorted(groups):
            if stem in hidden and not groups[stem]:
                continue  # a loose part made for an assembled piece: shown in it
            body = ms[stem]
            parts = [ms[p] for p in groups[stem] if p not in hidden]
            alts = [p for p in parts if isinstance(SEATS.get(p['stem']), tuple) and SEATS[p['stem']][0] == 'as']
            extras = [(ms[e], [b - q + d for b, q, d in zip(body['pivot'], ms[e]['pivot'], off)])
                      for e, off in EXTRA.get(stem, [])]
            members = family(body, [p for p in parts if p not in alts]) + extras
            slots = []
            if stem in assembled:
                # The assembled file in the body's place. Its pivot was set for the whole piece, so it lines up by
                # the body's own geometry, which is the same in both files: its bounds where it fills the body's
                # footprint (left, bottom, back).
                a = ms[stem + '_Assembled']
                d = [body['lo'][0] - a['lo'][0], body['lo'][1] - a['lo'][1], body['lo'][2] - a['lo'][2]]
                if len(members) == 1:
                    d = [0.0, 0.0, 0.0]
                slots.append([(a, d)] + members[1:])
            else:
                slots.append(members)
            if alts:
                # The alternative parts in place of the ones they stand for, on a second copy of the body.
                instead = {SEATS[p['stem']][1]: p for p in alts}
                whole = family(body, [ms[p] for p in groups[stem] if ms[p] not in alts])  # with the assembled parts
                seated = dict((m['stem'], o) for m, o in whole)
                mem = [(m, o) for m, o in whole if m['stem'] not in instead] + extras
                for r, p in instead.items():
                    rlo, rhi = moved(ms[r], seated[r])
                    if yaws.get(r):
                        # turned the same way, its centre on the other's (bottoms level)
                        yaws[p['stem']] = yaws[r]
                        c = [(a + b) / 2 for a, b in zip(p['lo'], p['hi'])]
                        cx, cz = turn(c, yaws[r])
                        mem.append((p, [(rlo[0] + rhi[0]) / 2 - cx, rlo[1] - p['lo'][1], (rlo[2] + rhi[2]) / 2 - cz]))
                        continue
                    # its corner at the other's: left, bottom, front
                    mem.append((p, [rlo[0] - p['lo'][0], rlo[1] - p['lo'][1], rhi[2] - p['hi'][2]]))
                slots.append(mem)
            for n, mem in enumerate(slots):
                lo, hi = bounds(mem)
                sx = x - lo[0]
                x += hi[0] - lo[0] + gap
                depth = max(depth, hi[2] - lo[2])
                height = max(height, hi[1] - lo[1])
                lift = max(0.0, -lo[1])  # hanging pieces (pivot at the top) stand on the floor here
                for i, (m, o) in enumerate(mem):
                    name = m['stem'] + (' (alternative parts)' if n and not i else '')
                    half = math.radians(yaws.get(m['stem'], 0.0)) / 2
                    rot = [0.0, round(math.sin(half), 6), 0.0, round(math.cos(half), 6)] if half else ident
                    ent = {'id': next(ids), 'name': name, 'order': next(order), 'parentId': parent,
                           'path': m['path'], 'pathGuid': m['guid'],
                           'position': [round(sx + o[0], 4), round(lift + o[1], 4), round(o[2], 4)],
                           'rotation': rot, 'scale': [1.0, 1.0, 1.0]}
                    if max(h - l for l, h in zip(m['lo'], m['hi'])) < TINY_PROP:
                        ent['castShadows'] = 0  # nothing to see in its shadow, and every shadow map draws it
                    ents.append(ent)
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
    os.makedirs(os.path.join(repo, 'build'), exist_ok=True)
    with open(os.path.join(repo, 'build', 'gallery_cameras.txt'), 'w') as f:
        f.write(';'.join(cams))
    print(f'import_model_library: wrote scenes/AssetGallery.json ({len(ents)} models in {len(rows)} rows)')


if __name__ == '__main__':
    main()
