# Checks the Quantum character assets on disk: what's missing, what's wired wrong, and what it all costs in VRAM.
#
# - Models: every .fbx under Models/ and FirstPerson/ has a .meta, and every material the FBX names has a
#   materialRemap entry pointing at a .mat that exists (an unmapped slot falls back to the FBX's own
#   material, whose textures live on the artist's drive - that slot draws blank).
# - Materials: every texture slot points at a file that exists (exact case, so a case-sensitive platform
#   finds it too), and its textureGuids entry is the texture's own GUID.
# - Texture import settings: colour maps (albedo, emissive) sRGB; data maps (normal, AO, roughness,
#   metallic) linear; normal maps typed Normal Map; the max size the slot's folder should have
#   (--clothing-max, --body-max).
# - Orphans: .mat files no model, colourway folder or wardrobe skin uses; textures no .mat uses; .meta
#   files without their asset; textures without a .meta.
# - VRAM: each texture's size on the GPU after the import cap and block compression, with mips; totals
#   per folder and per model (its default materials).
#
# Usage: python tools/quantum/audit_quantum_assets.py [--csv out.csv] [--clothing-max 2048] [--body-max 4096]
# Exit code: 0 clean, 1 problems found.
import argparse, collections, glob, json, os, re, struct, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
PROJECT = os.path.join(REPO, 'project')
QUANTUM = 'assets/Characters/Quantum'

COLOUR_SLOTS = {'_AlbedoMap', '_EmissiveMap'}
DATA_SLOTS = {'_NormalMap', '_AOMap', '_RoughnessMap', '_MetallicMap', '_MetallicRoughnessMap'}
TYPE_NORMAL = 1  # TextureImportSettings::Type::NormalMap
COMP_NONE = 0

ap = argparse.ArgumentParser()
ap.add_argument('--csv')
ap.add_argument('--clothing-max', type=int, default=0, help='expected maxTextureSize under Textures/Clothing (0 = not checked)')
ap.add_argument('--body-max', type=int, default=0, help='expected maxTextureSize elsewhere (0 = not checked)')
args = ap.parse_args()

problems = collections.defaultdict(list)  # kind -> messages


def rel(p):
    return os.path.relpath(p, PROJECT).replace('\\', '/')


def absp(r):
    return os.path.join(PROJECT, r.replace('/', os.sep))


def exact_case_exists(r):
    """True if project-relative path r exists with exactly this spelling (Windows ignores case)."""
    cur = PROJECT
    for part in r.split('/'):
        try:
            names = os.listdir(cur)
        except OSError:
            return False
        if part not in names:
            return False
        cur = os.path.join(cur, part)
    return True


def load_json(path):
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except Exception as e:
        problems['unreadable'].append(f'{rel(path)}: {e}')
        return None


def png_info(path):
    with open(path, 'rb') as f:
        head = f.read(26)
    if head[:8] != b'\x89PNG\r\n\x1a\n':
        return None
    w, h = struct.unpack('>II', head[16:24])
    return w, h, head[25]  # colour type: 0 grey, 2 RGB, 3 palette, 4 grey+alpha, 6 RGBA


def fbx_materials(path):
    """Material names a binary FBX declares ("<name>\\x00\\x01Material")."""
    with open(path, 'rb') as f:
        data = f.read()
    return sorted({m.decode('utf-8', 'replace') for m in re.findall(rb'([\x20-\x7e]{1,200})\x00\x01Material', data)})


# ---- textures -------------------------------------------------------------------------------------------
textures = {}  # rel path -> dict(meta..., w, h, ctype, vram)
for png in glob.glob(os.path.join(PROJECT, QUANTUM, '**', '*.png'), recursive=True):
    r = rel(png)
    info = png_info(png)
    meta = load_json(png + '.meta') if os.path.exists(png + '.meta') else None
    if meta is None:
        problems['texture without .meta'].append(r)
    imp = (meta or {}).get('importer', {})
    t = {'guid': (meta or {}).get('guid', ''), 'srgb': imp.get('isSRGB', True), 'type': imp.get('textureType', 0),
         'max': imp.get('maxTextureSize', 2048), 'comp': imp.get('compression', 0), 'mips': imp.get('generateMipmaps', True)}
    if info:
        w, h, ctype = info
        t.update(w=w, h=h, ctype=ctype)
    textures[r] = t
for meta in glob.glob(os.path.join(PROJECT, QUANTUM, '**', '*.meta'), recursive=True):
    asset = meta[:-5]
    if not os.path.exists(asset):
        problems['.meta without its asset'].append(rel(asset))
guid_to_tex = {t['guid']: r for r, t in textures.items() if t['guid']}


def vram(t, used_as_normal=False):
    if 'w' not in t:
        return 0
    w, h = t['w'], t['h']
    cap = t['max'] if t['max'] > 0 else max(w, h)
    while max(w, h) > cap:
        w, h = max(1, w // 2), max(1, h // 2)
    ctype = t['ctype']
    if t['comp'] == COMP_NONE:
        bpp = 4.0  # RGBA8 (Texture uploads 3/1-channel sources as RGB8/R8 too, but pads rows; close enough)
        bpp = {0: 1.0, 4: 2.0, 2: 3.0}.get(ctype, 4.0)
    elif used_as_normal or t['type'] == TYPE_NORMAL:
        bpp = 1.0  # BC5
    elif ctype in (0,):
        bpp = 0.5  # BC4
    elif ctype in (4, 6):
        bpp = 1.0  # BC3 (alpha; the encoder may pick BC1 if it's all opaque)
    else:
        bpp = 0.5  # BC1
    return w * h * bpp * (4.0 / 3.0 if t['mips'] else 1.0)


# ---- materials ------------------------------------------------------------------------------------------
materials = {}  # rel path -> {slot: texture rel}
for mat in glob.glob(os.path.join(PROJECT, QUANTUM, '**', '*.mat'), recursive=True):
    r = rel(mat)
    j = load_json(mat)
    if j is None:
        continue
    props = j.get('properties', {})
    guids = j.get('textureGuids', {})
    slots = {}
    for slot, v in props.items():
        if not slot.endswith('Map') or not isinstance(v, str) or not v:
            continue
        slots[slot] = v
        if not os.path.exists(absp(v)):
            by_guid = guid_to_tex.get(guids.get(slot, ''))
            problems['material texture missing'].append(f'{r} {slot}: {v}' + (f' (its GUID is now {by_guid})' if by_guid else ''))
            continue
        if not exact_case_exists(v):
            problems['texture path case differs'].append(f'{r} {slot}: {v}')
        t = textures.get(v)
        if t is None:
            continue
        g = guids.get(slot)
        if g and g != t['guid']:
            problems['textureGuids stale'].append(f'{r} {slot}: {g} != {t["guid"]} ({v})')
        if not g:
            problems['textureGuids missing'].append(f'{r} {slot}')
        if slot in COLOUR_SLOTS and not t['srgb']:
            problems['colour map imported linear'].append(f'{v} ({slot} of {os.path.basename(r)})')
        if slot in DATA_SLOTS and t['srgb']:
            problems['data map imported as sRGB'].append(f'{v} ({slot} of {os.path.basename(r)})')
        if slot == '_NormalMap' and t['type'] != TYPE_NORMAL:
            problems['normal map not typed Normal Map'].append(v)
    materials[r] = slots

# ---- models ---------------------------------------------------------------------------------------------
models = {}  # rel -> {material name: mat rel}
for fbx in sorted(glob.glob(os.path.join(PROJECT, QUANTUM, '**', '*.fbx'), recursive=True)):
    r = rel(fbx)
    if '/_import/' in r:
        continue
    meta = load_json(fbx + '.meta') if os.path.exists(fbx + '.meta') else None
    if meta is None:
        problems['model without .meta'].append(r)
        continue
    remap = meta.get('materialRemap', {})
    for name, m in remap.items():
        if not os.path.exists(absp(m)):
            problems['materialRemap target missing'].append(f'{r}: {name} -> {m}')
    names = fbx_materials(fbx)
    for n in names:
        if n not in remap:
            problems['FBX material not remapped (draws the FBX\'s own, blank)'].append(f'{r}: {n}')
    for n in remap:
        if names and n not in names:
            problems['materialRemap entry for a material the FBX doesn\'t have'].append(f'{r}: {n}')
    models[r] = remap

# ---- orphans --------------------------------------------------------------------------------------------
wardrobe = load_json(absp(QUANTUM + '/Quantum.wardrobe')) or {}
used_mats = set()
for remap in models.values():
    used_mats.update(remap.values())
colourway_dirs = {os.path.dirname(m) for m in used_mats}
skin_names = set(wardrobe.get('skinMaterials', []))
for body in wardrobe.get('bodies', {}).values():
    for race in body.get('races', []):
        if race.get('skin'):
            skin_names.add(os.path.splitext(os.path.basename(race['skin']))[0])
for m in materials:
    stem = os.path.splitext(os.path.basename(m))[0]
    if m in used_mats or os.path.dirname(m) in colourway_dirs:
        continue
    if any(stem.startswith(s) for s in skin_names):
        continue
    problems['material nothing uses'].append(m)
used_tex = {v for slots in materials.values() for v in slots.values()}
for t in textures:
    if t not in used_tex:
        problems['texture no material uses'].append(t)

# ---- expected import caps -------------------------------------------------------------------------------
for r, t in textures.items():
    want = args.clothing_max if '/Textures/Clothing/' in r else args.body_max
    if want and t['max'] != want:
        problems[f'maxTextureSize not the expected cap'].append(f'{r}: {t["max"]} (want {want})')

# ---- VRAM -----------------------------------------------------------------------------------------------
normal_used = {v for slots in materials.values() for s, v in slots.items() if s == '_NormalMap'}
by_folder = collections.Counter()
for r, t in textures.items():
    parts = r.split('/')
    folder = '/'.join(parts[3:6]) if len(parts) > 6 else '/'.join(parts[3:-1])
    by_folder[folder] += vram(t, r in normal_used)
total = sum(by_folder.values())

rows = []
for m, remap in models.items():
    texs = {v for mat in remap.values() for v in materials.get(mat, {}).values()}
    mb = sum(vram(textures[v], v in normal_used) for v in texs if v in textures) / 2**20
    rows.append((m, len(remap), len(texs), mb))
rows.sort(key=lambda x: -x[3])

# ---- report ---------------------------------------------------------------------------------------------
print(f'[QuantumAudit] {len(models)} models, {len(materials)} materials, {len(textures)} textures')
dims = collections.Counter((t.get('w'), t.get('h')) for t in textures.values())
print('[QuantumAudit] source sizes: ' + ', '.join(f'{w}x{h} x{n}' for (w, h), n in dims.most_common()))
print(f'[QuantumAudit] all textures resident: {total / 2**30:.2f} GB VRAM (after import caps + compression, with mips)')
for f, b in by_folder.most_common(12):
    print(f'    {b / 2**20:8.0f} MB  {f}')
print('[QuantumAudit] heaviest models (default materials):')
for m, nm, nt, mb in rows[:12]:
    print(f'    {mb:7.0f} MB  {nt:2d} textures  {m}')
bad = 0
for kind in sorted(problems):
    items = problems[kind]
    bad += len(items)
    print(f'[QuantumAudit] {kind}: {len(items)}')
    for it in items[:15]:
        print(f'    {it}')
    if len(items) > 15:
        print(f'    ... {len(items) - 15} more')
if args.csv:
    with open(args.csv, 'w', encoding='utf-8') as f:
        f.write('kind,detail\n')
        for kind, items in sorted(problems.items()):
            for it in items:
                f.write(f'"{kind}","{it}"\n')
print(f'[QuantumAudit] {bad} problem(s)')
sys.exit(1 if bad else 0)
