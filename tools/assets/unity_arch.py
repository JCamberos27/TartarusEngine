# Architecture-kit scale fixes for the Unity packs (walls, floors, roofs scaled to the engine's grid).
# Split out of unity_prefabs.py.
import collections
import os
import re

from unity_space import r6


# Per-pack prefab scale overrides: prefab (root object) name -> uniform scale, applied wherever the prefab is emitted so the
# prefab files and every scene instance agree (colliders and child parts ride on the root entity). Measured against the
# 1.81 m Quantum rig: the office's metal desk and leather chair are too tall.
SCALE_OVERRIDES = {
    'OfficeAndPoliceStation': {'Desk_metal': 0.75 / 0.90, 'Chair_office_leather': 1.10 / 1.28},
}


# Architecture scale, matched to the Apartment Hallway's doors (Door_Op_1: 0.98 x 2.08 m frame, 0.81 x 2.0 m leaf) and the
# 1.81 m Quantum rig. The alley kit's doors are 2.49 m (narrowest alley 4+ m wide): x0.82 = 2.04 m. The office doors are
# 2.29 m: x0.9 = 2.06 m. The suburb doors are 2.15 m: x0.97 = 2.09 m. The factor lives in the building meshes themselves
# (their .meta globalScale, set in Converter.set_correction), so prefab files and scene instances keep scale 1; the scene
# layout scales about the world origin by the same factor, so the walls still tile and the props (kept at 1.0) still sit
# on the floors.
ARCH_SCALE = {'ModularCityAlleyPack': 0.82, 'OfficeAndPoliceStation': 0.9, 'SuburbNeighborhoodHousePack': 0.97}
ARCH_FOLDERS = {'ModularCityAlleyPack': {'Walls', 'Floors', 'Roofs', 'Decals'},
                'OfficeAndPoliceStation': {'Building'},
                'SuburbNeighborhoodHousePack': {'Building', 'Ground'}}
# building-mounted kit that lives in the Props folder (free-standing props stay out)
ARCH_PROP_RE = {'ModularCityAlleyPack': re.compile(
    r'^(Airduct|Awning|Balcony|Commercial$|Distant_Building|FireEscape|MetalPlateRoof_|MetalPlate_A$|Neon_Sign_|Sign_|Pipe_|'
    r'PlasticHanging|PowerLines_|Window_MetalBars_|WindowShutter|Door_MetalBars_|Tree_Groundplate|WireStraight)'),
    # wall-to-wall kit that tiles with the building: toilet stalls and cubicle screens
    'OfficeAndPoliceStation': re.compile(r'^(Toilet_partition|Tablescreen_)'),
    'SuburbNeighborhoodHousePack': re.compile(r'^(?!)')}


def arch_mesh(pack_name, path):
    """True when a model file (or one of its @node variants) is a building piece whose mesh carries ARCH_SCALE."""
    path = path.replace(chr(92), '/')
    if pack_name not in ARCH_SCALE or '/Meshes/' not in path:
        return False
    seg = path.split('/Meshes/')[-1].split('/')
    if seg[0] in ARCH_FOLDERS[pack_name]:
        return True
    return bool(ARCH_PROP_RE[pack_name].match(re.sub(r'@.*$', '', os.path.splitext(seg[-1])[0])))


def arch_class(pack_name, e):
    """True for a building piece, False for a free-standing prop; None when the entity is not a model."""
    path = e.get('path')
    if not path:
        return None
    seg = path.replace(chr(92), '/').split('/Meshes/')[-1].split('/')
    name = re.sub(r' \(\d+\)$', '', e['name'])
    if seg[0] in ARCH_FOLDERS[pack_name]:
        return True
    return bool(ARCH_PROP_RE[pack_name].match(name))


def arch_rescale(pack, out, prefab, unscaled=False):
    """Lay out an emitted prefab or scene for ARCH_SCALE, given that the building meshes already carry it. The result is
    what scaling each top-level building entity by the factor would give (its children shrink with it, a scene's
    top-level positions scale about the origin, props keep their size), with entity scales left at 1 wherever the mesh's
    own scale covers it: an entity only gets a residual scale where it and its asset disagree (a box, a prop nested under
    a building piece, a building mesh placed as a free-standing prop). unscaled=True instead undoes the meshes' scale, for
    the --scene-bounds read of the scene as Unity laid it out."""
    s = ARCH_SCALE.get(pack.name)
    if s is None:
        return
    g = 1.0 if unscaled else s
    ents = out['boxes'] + out['empties'] + out['models']
    ids = {e['id']: e for e in ents}
    boxes = {id(e) for e in out['boxes']}   # by identity: a list test compared whole dicts, entity by entity
    kids = collections.defaultdict(list)
    for e in ents:
        kids[e['parentId'] if e['parentId'] in ids else -1].append(e)

    def walk(e, need_par, applied_par, rig):
        par = ids.get(e['parentId'])
        top = par is None or 'path' not in par
        rig = rig or e['name'] in ('Player Spawn', 'Sun')  # the spawn's rig and the template Sun are not layout
        kind = arch_class(pack.name, e)
        if kind is None and id(e) in boxes:
            kind = True
        # once inside a scaled building piece (a door leaf under its frame's pivot group) the factor is already applied
        fresh = need_par == 1.0
        grow = g if kind and not rig and fresh and (par is None if prefab else top) else 1.0
        need = need_par * grow  # the entity's size factor, as scaling the building entities would give it
        asset = s if e.get('path') and arch_mesh(pack.name, e['path']) else 1.0
        own = need / (asset * applied_par)  # what the entity's own scale still has to add
        layout = g if not prefab and top and fresh and (not rig or e['name'] == 'Player Spawn') else 1.0
        move = layout * need_par / applied_par
        if move != 1.0:
            e['position'] = r6([c * move for c in e['position']])
        col = e.get('collider')
        if col and 'path' not in e and id(e) not in boxes and not kids[e['id']] and col.get('shape') in (0, 1, 2):
            for k in ('center', 'halfExtents'):  # a collider-only helper: the shape itself takes the size, not the entity
                if k in col:
                    col[k] = r6([c * own for c in col[k]])
            own = 1.0
        if abs(own - 1.0) > 1e-9:
            e['scale'] = r6([c * own for c in e['scale']])
        if asset != 1.0:  # entity-space lengths measured on the unscaled mesh (a mesh collider follows the mesh)
            col = e.get('collider') or {}
            for k in ('center', 'halfExtents'):
                if k in col:
                    col[k] = r6([c * asset for c in col[k]])
            op = e.get('Openable') or {}
            if op.get('Motion', '').startswith('Slide') and op.get('Amount', 0) > 0:
                op['Amount'] = round(op['Amount'] * asset, 3)
        if kind is not None and not prefab and top and not rig and not unscaled:
            pack.counts[f'architecture scaled x{s}' if kind else 'free-standing props kept at 1.0'] += 1
        for k in kids[e['id']]:
            walk(k, need, applied_par * own, rig)

    for e in kids[-1]:
        walk(e, 1.0, 1.0, False)
