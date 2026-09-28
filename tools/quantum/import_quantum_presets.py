# Turns the Quantum pack's 60 preset characters into outfit presets (.outfit, OutfitSystem::LoadPreset).
#
# The presets ship as merged FBXs (not imported - see _import/IMPORT_NOTES.md), but each still names the
# item meshes it was built from. Those names are matched to the items we did import; the result is the
# artist's own outfits as presets, and a check that the wardrobe's rules allow what the artist made
# (TartarusEngine --outfit-audit resolves every one).
#
# Usage: python tools/quantum/import_quantum_presets.py [pack folder]
#   pack folder defaults to ~/Desktop/Quantum Characters (it needs casual_fbx/Presets).
import glob, json, os, re, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
PROJECT = os.path.join(REPO, 'project')
QUANTUM = 'assets/Characters/Quantum'
OUT = os.path.join(PROJECT, 'assets', 'Characters', 'Outfits', 'Quantum')
PACK = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.expanduser('~'), 'Desktop', 'Quantum Characters')

# Pack mesh name -> our stem, where the import renamed or merged it. None = part of another item or the body.
ALIASES = {
    'skm_m_leather_jacket': 'SKM_Leather_Jacket', 'skm_m_leather_jacket_hoodie': 'SKM_Leather_Jacket_Hoodie',
    'skm_male_jersey': 'SKM_Jersey', 'skm_flip_flops': 'SKM_Flip_Flops', 'skm_f_flip_flops': 'SKM_F_Flip_Flops',
    'skm_head_scalp': 'SKM_Hair_Skin',
    'skm_jacket_winter_open_hood_opened': 'SKM_Puffer_Jacket',  # the male pack has only the hood-up cut, which takes no hat
    'skm_f_jacket_winter_open_hood_opened': 'SKM_F_Jacket_Winter_Open_Hood_Open',
    'skm_f_haircut_ponytail_cap_glassses': 'SKM_F_Haircut_Ponytail_Cap_Glasses_Alt',
    'skm_beard_full': 'SKM_Beard_Long', 'skm_beard_06': 'SKM_Beard_02',          # nearest of the beards we have
    # built into the jacket or top they come with
    'skm_jeans_jacket_tshirt_tucked': None, 'skm_tshirt_bomber': None, 'skm_f_tshirt1': None,
    'skm_f_tshirt_tucked_jeans_jacket': None, 'skm_f_tshirt_tucked_jeans_vest': None,
    'skm_jacket_classic': None, 'skm_coat': None,
    # the body
    'skm_quantum_wirst': None, 'skm_quantum_hands': None, 'skm_f_vivian_wrist': None, 'skm_f_vivian_arms': None,
    'skm_f_vivian_body': None, 'quantum_feet_shoes': None, 'quantum_legs': None, 'quantum_legs_breeches': None,
    'quantum_face_detals': None,
}
RACES = {
    'quantum_head': 'European', 'quantum_head_afro': 'Afro', 'quantum_head_asian': 'Asian',
    'quantum_old_head': 'Old', 'quantum_young_head': 'Young',
    'skm_f_vivian_head': 'European', 'skm_f_vivian_head_afro': 'Afro', 'skm_f_vivian_old_head': 'Old',
}
# Folder -> slot, as Quantum.wardrobe classifies (item overrides included).
FOLDER_SLOTS = {'Hair': 'Hair', 'Beard': 'Beard', 'Hats': 'Hat', 'WithHair': 'Hat', 'Glasses': 'Glasses',
                'Tops': 'Top', 'Outerwear': 'Outerwear', 'Fur': 'Collar', 'Pants': 'Pants', 'Shoes': 'Shoes',
                'Bags': 'Bag', 'Accessories': 'Wrist'}


def slot_of(rel, stem):
    low = stem.lower()
    if 'socks' in low and 'boots' not in low: return 'Socks'
    if 'headphones' in low: return 'Headphones'
    for folder in reversed(rel.split('/')[:-1]):
        if folder in FOLDER_SLOTS:
            s = FOLDER_SLOTS[folder]
            if s == 'Wrist': return 'Wrist L' if stem.endswith('_L') else 'Wrist R' if stem.endswith('_R') else None
            return s
    return None


def catalog():
    items = {}  # (gender, lower stem) -> (slot, project-relative path)
    for folder in ['Models/Clothing', 'Models/Hair', 'Models/Female/Hair', 'Models/Beard']:
        for path in glob.glob(os.path.join(PROJECT, QUANTUM, folder, '**', '*.fbx'), recursive=True):
            rel = os.path.relpath(path, PROJECT).replace('\\', '/')
            if '/StaticMesh/' in rel: continue
            stem = os.path.splitext(os.path.basename(path))[0]
            slot = slot_of(rel, stem)
            if not slot: continue
            gender = 'Female' if '/Female/' in rel or stem.startswith('SKM_F_') else 'Male'
            items[(gender, stem.lower())] = (slot, rel)
    return items


def mesh_names(path):
    data = open(path, 'rb').read()
    names = set(m.decode('latin1') for m in re.findall(rb'([A-Za-z][A-Za-z0-9_]{2,60})\x00\x01Model', data))
    return sorted(n for n in names if re.match(r'^(SKM|SkM|Quantum|Hair)_', n))


def hair_stem(name):
    # "Hair_Short001" -> "SKM_Hair_Short_01"
    m = re.match(r'^Hair_(.+?)(\d{3})?$', name)
    return 'SKM_Hair_' + m.group(1) + ('_' + m.group(2)[1:] if m.group(2) else '')


def main():
    items = catalog()
    presets = sorted(glob.glob(os.path.join(PACK, 'casual_fbx', 'Presets', 'Male', 'SKM_Preset_Male_*.fbx'))) + \
              sorted(glob.glob(os.path.join(PACK, 'casual_fbx', 'Presets', 'Female', 'SKM_F_Preset_Female_*.fbx')))
    if not presets:
        sys.exit('no presets under ' + PACK)
    os.makedirs(OUT, exist_ok=True)
    unmatched = {}
    for path in presets:
        gender = 'Female' if '_F_Preset_' in path else 'Male'
        number = re.search(r'_(\d+)\.fbx$', path).group(1)
        race, chosen = None, {}
        for name in mesh_names(path):
            low = name.lower()
            if low in RACES: race = RACES[low]; continue
            if low.startswith('quantum_face_detals'): continue  # the head's eyes/teeth, per race
            stem = hair_stem(name) if name.startswith('Hair_') else name
            if low in ALIASES:
                stem = ALIASES[low]
                if stem is None: continue
            hit = items.get((gender, stem.lower()))
            if not hit and gender == 'Female' and stem.lower().startswith('skm_'):
                hit = items.get((gender, 'skm_f_' + stem[4:].lower()))
            if not hit:
                unmatched.setdefault(name, []).append(gender + ' ' + number)
                continue
            slot, rel = hit
            # Two meshes for one slot (Jeans_Jacket + Jeans_Jacket_Collar): the longer name is the full item.
            if slot not in chosen or len(rel) > len(chosen[slot]): chosen[slot] = rel
        out = {'wardrobe': QUANTUM + '/Quantum.wardrobe', 'gender': gender, 'race': race or 'European',
               'items': {slot: {'item': rel} for slot, rel in sorted(chosen.items())}}
        name = 'Quantum_%s_%s.outfit' % (gender, number)
        with open(os.path.join(OUT, name), 'w', newline='\n') as f:
            json.dump(out, f, indent=2)
            f.write('\n')
    print('wrote %d presets to %s' % (len(presets), os.path.relpath(OUT, REPO)))
    for name, where in sorted(unmatched.items()):
        print('  no item for %-40s (%s)' % (name, ', '.join(where)))


if __name__ == '__main__':
    main()
