# Passes over a converted Unity scene before it is written: doors shut, small colliders stripped,
# openables deduped, spawns put on the terrain, auto LODs, props settled. Split out of unity_prefabs.py.
import copy
import json
import math
import os
import re
import subprocess

from import_model_library import write_json, write_meta
from unity_arch import ARCH_SCALE, arch_class
from unity_space import _qmul, _qrot, _world_of, r6


SMALL_PROP = 0.35  # metres: a prop smaller than this every way has no collider (strip_small_colliders)


def shut_ajar(pack, out):
    """A door the scene left ajar (the Office's dark metal doors, 20-60 degrees off the wall): the engine's Openable takes
    the authored pose as shut, so it would swing a further 90 degrees into the wall, or across its own doorway. The leaf,
    or the pivot it hangs on at its origin, turns back to the 90 degree step (in its parent) that sets it in its frame: wall
    (a box collider) just behind its hinge edge and just past its latch edge, 1 m up, and none across the leaf itself.
    The pack swings them ajar either way, so the nearest step is only the tie-break."""
    world = _world_of(out)
    ents = world.ents
    doors = set()
    for e in out['models']:
        if 'Openable' in e:
            doors.add(e['id'])
            doors.add(e['parentId'])
    boxes = []
    for e in ents.values():
        c = e.get('collider') or {}
        if e['id'] in doors or c.get('shape', -1) != 0 or c.get('isTrigger'):
            continue
        pos, rot, sc = world(e)
        inv = (-rot[0], -rot[1], -rot[2], rot[3])
        boxes.append((pos, inv, sc, c.get('center') or [0.0, 0.0, 0.0], c['halfExtents']))

    def walled(p):
        for pos, inv, sc, ce, he in boxes:
            v = _qrot(inv, tuple(a - b for a, b in zip(p, pos)))
            if all(abs(v[a] / (sc[a] or 1.0) - ce[a]) * abs(sc[a]) <= he[a] * abs(sc[a]) + 0.02 for a in range(3)):
                return True
        return False
    for leaf in out['models']:
        motion = (leaf.get('Openable') or {}).get('Motion')
        if motion not in ('Hinge Left', 'Hinge Right'):
            continue
        par = ents.get(leaf['parentId'])
        at_origin = all(abs(c) < 0.02 for c in leaf['position']) and abs(abs(leaf['rotation'][3]) - 1.0) < 1e-4
        pivot = par if par is not None and at_origin else leaf
        x, y, z, w = pivot['rotation']
        if abs(x) > 1e-3 or abs(z) > 1e-3:
            continue
        yaw = math.degrees(2.0 * math.atan2(y, w))
        off = (yaw + 45.0) % 90.0 - 45.0
        if abs(off) < 2.0:
            continue
        col = leaf.get('collider') or {}
        sc = world(leaf)[2]
        width = 2.0 * (col.get('halfExtents') or [0.45])[0] * abs(sc[0]) if col.get('shape', 0) == 0 else 0.9
        side = 1.0 if motion == 'Hinge Left' else -1.0
        pp = world(pivot)[0]
        pq = world(ents[pivot['parentId']])[1] if pivot['parentId'] in ents else (0.0, 0.0, 0.0, 1.0)
        best = None
        for k in range(4):
            h = math.radians(90.0 * k) * 0.5
            q = (0.0, math.sin(h), 0.0, math.cos(h))
            d = _qrot(_qmul(pq, q), (side, 0.0, 0.0))
            n = math.hypot(d[0], d[2]) or 1.0
            d = (d[0] / n, 0.0, d[2] / n)
            at = lambda t: (pp[0] + d[0] * t, pp[1] + 1.0, pp[2] + d[2] * t)  # noqa: E731
            score = walled(at(-0.15)) + walled(at(width + 0.15)) - 2 * walled(at(0.5 * width))
            turn = abs((yaw - 90.0 * k + 180.0) % 360.0 - 180.0)
            if best is None or (score, -turn) > (best[0], -best[1]):
                best = (score, turn, q)
        if best[0] <= 0:
            pack.report['ajar doors shut to the nearest 90 degrees (no wall found either side)'].append(pivot['name'])
        pivot['rotation'] = r6(best[2])
        pack.counts['ajar doors shut'] += 1


def strip_small_colliders(pack, out, bounds):
    """A free-standing prop under SMALL_PROP every way (a lamp, a switch, a basket, a plate on the floor) loses its
    colliders, its helper children's too: the player's capsule only snags on them. Openables keep theirs (a drawer)."""
    keep = {e['id'] for e in out['models'] if 'Openable' in e}
    gone = set()
    for e in out['models']:
        b = bounds.get(e['id'])
        if (not e.get('collider') or e['id'] in keep or e['parentId'] in keep or b is None or
                arch_class(pack.name, e) is not False):
            continue
        if max(h - l for l, h in zip(*b)) < SMALL_PROP:
            del e['collider']
            gone.add(e['id'])
            pack.counts['colliders dropped from small props (under 35 cm)'] += 1
    n = len(out['empties'])
    out['empties'] = [e for e in out['empties'] if not (e.get('collider') and 'path' not in e and e['parentId'] in gone)]
    pack.counts['colliders dropped from small props (under 35 cm)'] += n - len(out['empties'])


def dedupe_openables(out):
    """Two prefabs can each carry the same door in the same place (an elevator car and the wall around its shaft). Only
    one of them opens under Interact, so the twin would stay shut, in view and in the way: the later one is removed.
    (A leaf has no children; the removed entity's id is simply left unused.)"""
    world = _world_of(out)
    kept, twins = [], []
    for e in out['models']:
        if 'Openable' not in e:
            continue
        pos, rot, _ = world(e)
        twin = any(k[0] == e['path'] and k[3] == e['Openable']['Motion'] and
                   max(abs(a - b) for a, b in zip(k[1], pos)) < 0.1 and abs(sum(a * b for a, b in zip(k[2], rot))) > 0.999
                   for k in kept)
        if twin:
            twins.append(e)
        else:
            kept.append((e['path'], pos, rot, e['Openable']['Motion']))
    gone = {id(t) for t in twins}
    out['models'] = [e for e in out['models'] if id(e) not in gone]
    return len(twins)


def _walk(objs):
    for o in objs:
        yield o
        yield from _walk(o.children)


def spawn_on_terrain(conv, pack, out, keep):
    """The player spawn must stand above the scene's terrain mesh: one outside the terrain (the Unity rig can sit anywhere)
    moves to a point a quarter of the way across it, and one below the surface is lifted to a metre above it."""
    ter = next((e for e in out['models'] if e.get('path') in conv.terrain_mats and e['parentId'] == -1), None)
    spawn = next((e for e in keep if e['name'] == 'Player Spawn'), None)
    if ter is None or spawn is None:
        return
    vs = []
    with open(os.path.join(pack.repo, 'project', *ter['path'].split('/')), encoding='utf-8') as f:
        for line in f:
            if line.startswith('v '):
                _, x, y, z = line.split()
                vs.append((float(x), float(y), float(z)))
    nx = next(i for i, v in enumerate(vs) if v[2] != vs[0][2])
    nz = len(vs) // nx
    x0, x1, z0, z1 = vs[nx - 1][0], vs[0][0], vs[0][2], vs[-1][2]   # the OBJ is mirrored: x runs from 0 down to -width

    def height(x, z):
        i = min(max(round((x1 - x) / (x1 - x0) * (nx - 1)), 0), nx - 1)
        j = min(max(round((z - z0) / (z1 - z0) * (nz - 1)), 0), nz - 1)
        return vs[j * nx + i][1] * ter['scale'][1] + ter['position'][1]
    lx, lz = spawn['position'][0] - ter['position'][0], spawn['position'][2] - ter['position'][2]
    if not (x0 <= lx <= x1 and z0 <= lz <= z1):
        lx, lz = x1 + (x0 - x1) * 0.25, z0 + (z1 - z0) * 0.25
        spawn['position'][0], spawn['position'][2] = r6([lx + ter['position'][0]])[0], r6([lz + ter['position'][2]])[0]
        pack.report['player spawn outside the terrain (moved a quarter of the way across it)'].append(pack.name)
    ground = height(lx, lz)
    if spawn['position'][1] < ground + 1.0:
        spawn['position'][1] = round(ground + 1.0, 6)
        pack.report['player spawn below the terrain (lifted to 1 m above it)'].append(pack.name)


def scene_bounds(conv, scene):
    """World AABB of every model/box entity of an in-memory scene, by entity id (the engine's --scene-bounds on a temp copy
    whose names carry the id, since bounds come back in registry order)."""
    import shutil
    import tempfile
    tmp = tempfile.mkdtemp(prefix='unity_bounds_')
    try:
        sc = copy.deepcopy(scene)
        for k in ('models', 'boxes'):
            for e in sc[k]:
                e['name'] = f"{e['name']}~{e['id']}"
        src = os.path.join(tmp, 'bounds_tmp.json')
        write_json(src, sc)
        run = subprocess.run([conv.exe, '--project', os.path.join(conv.pack.repo, 'project'), '--scene-bounds', src, '--out', tmp],
                             capture_output=True, text=True, encoding='utf-8', errors='replace')
        res_path = os.path.join(tmp, 'scenes', 'bounds_tmp.json')
        if run.returncode != 0 or not os.path.exists(res_path):
            raise RuntimeError(f'--scene-bounds failed (exit {run.returncode}):\n' + (run.stdout or '')[-3000:])
        res = json.load(open(res_path, encoding='utf-8'))
        out = {}
        for x in res['entities']:
            out[int(x['path'].split('/')[-1].rsplit('~', 1)[1])] = (list(x['min']), list(x['max']))
        return out
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# Generated LODs for the heavy meshes a pack ships without any (the forest's Oreopanax shrubs, two cedars, the Roldana
# plants, the mushrooms: ~9M triangles, every one drawn at any distance). Each such model entity becomes an engine LOD
# Group whose levels are the model and Simplify variants of it (copies of the file beside it, like the one-node variants
# above, so Drive payloads; their metas carry the importer's Simplify ratio). The last level is never culled, so
# nothing that drew before disappears. levels: (Simplify ratio, screen-relative height it holds down to).
AUTO_LOD = {
    'SeedMesh_Mossy_Cedrus_Forest': dict(min_tris=5000, levels=((1.0, 0.25), (0.4, 0.1), (0.15, 0.0))),
}


def model_tris(exe, repo, rels):
    """Triangles of each project-relative model path, as imported with its meta (the engine's --model-report)."""
    if not rels:
        return {}
    absf = {os.path.normcase(os.path.normpath(os.path.join(repo, 'project', *r.split('/')))): r for r in rels}
    out = subprocess.run([exe, '--model-report', *absf], capture_output=True, text=True, encoding='utf-8',
                         errors='replace').stdout
    tris, cur = {}, None
    for line in out.splitlines():
        line = line.strip()
        m = re.match(r'\[ModelReport\] (.*) size \S+ \S+ \S+ min ', line)
        if m:
            cur = absf.get(os.path.normcase(os.path.normpath(m.group(1))))
            continue
        m = re.match(r'\[ModelReport\]\s+tris: (\d+)', line)
        if m and cur:
            tris[cur] = int(m.group(1))
            cur = None
    return tris


def auto_lod(exe, repo, pack_name, scene, counts, report=None):
    cfg = AUTO_LOD.get(pack_name)
    if not cfg:
        return
    from unity_import import copy_if_changed
    every = scene['boxes'] + scene['empties'] + scene['models']
    ids = {e['id']: e for e in every}

    def drawn_without_lod(e):
        if 'LOD Group' in e or e.get('active', True) is False:
            return False
        p, depth = ids.get(e.get('parentId')), 0
        while p is not None and depth < 256:
            if 'LOD Group' in p or p.get('active', True) is False:
                return False
            p, depth = ids.get(p.get('parentId')), depth + 1
        return True

    cands = [e for e in scene['models'] if f'/{pack_name}/' in e.get('path', '') and e['path'].lower().endswith('.fbx')
             and '_autoLOD' not in e['path'] and drawn_without_lod(e)]
    tris = model_tris(exe, repo, sorted({e['path'] for e in cands}))
    heavy = sorted(p for p, t in tris.items() if t >= cfg['min_tris'])
    if not heavy:
        return
    variants = {}  # (path, level) -> (path, guid)
    for rel in heavy:
        src = os.path.join(repo, 'project', *rel.split('/'))
        meta = json.load(open(src + '.meta', encoding='utf-8'))
        stem, ext = os.path.splitext(rel)
        for i, (ratio, _) in enumerate(cfg['levels']):
            if ratio >= 1.0:
                variants[(rel, i)] = (rel, meta['guid'])
                continue
            vrel = f'{stem}_autoLOD{i}{ext}'
            dst = os.path.join(repo, 'project', *vrel.split('/'))
            copy_if_changed(src, dst)
            extra = {'importer': dict(meta['importer'], simplify=ratio)}
            if 'materialRemap' in meta:
                extra['materialRemap'] = meta['materialRemap']
            variants[(rel, i)] = (vrel, write_meta(dst, os.path.dirname(vrel), 'model', extra))
    if report is not None:
        after = model_tris(exe, repo, sorted(v[0] for k, v in variants.items() if k[1] > 0))
        for rel in heavy:
            report.append(f"{rel.split('/')[-1]}: " + ' / '.join(
                str(tris[rel] if i == 0 else after.get(variants[(rel, i)][0], '?')) for i in range(len(cfg['levels']))))
    heavy = set(heavy)
    chosen = {e['id'] for e in cands if e['path'] in heavy}
    next_id = max(ids) + 1
    next_order = max(e.get('order', 0) for e in every) + 1
    models, empties = [], scene['empties']
    thresholds = [h for _, h in cfg['levels']] + [0.0] * (4 - len(cfg['levels']))
    for e in scene['models']:
        if e['id'] not in chosen:
            models.append(e)
            continue
        # The entity keeps its id, transform and any children, as an empty; the levels hang under a group below it.
        empties.append({k: v for k, v in e.items() if k not in ('path', 'pathGuid', 'materials')})
        group = {'id': next_id, 'name': f"{e['name']} LODs", 'order': next_order, 'parentId': e['id'],
                 'position': [0.0, 0.0, 0.0], 'rotation': [0.0, 0.0, 0.0, 1.0], 'scale': [1.0, 1.0, 1.0],
                 'LOD Group': {'LOD 0': thresholds[0], 'LOD 1': thresholds[1], 'LOD 2': thresholds[2],
                               'LOD 3': thresholds[3], 'Size': 0.0}}
        if 'static' in e:
            group['static'] = e['static']
        empties.append(group)
        gid = next_id
        next_id += 1
        next_order += 1
        for i in range(len(cfg['levels'])):  # in level order: the group's children are its levels
            vrel, vguid = variants[(e['path'], i)]
            lv = {'id': next_id, 'name': f"{e['name']}_LOD{i}", 'order': next_order, 'parentId': gid, 'path': vrel,
                  'pathGuid': vguid, 'position': [0.0, 0.0, 0.0], 'rotation': [0.0, 0.0, 0.0, 1.0],
                  'scale': [1.0, 1.0, 1.0]}
            for k in ('materials', 'static'):
                if k in e:
                    lv[k] = e[k]
            models.append(lv)
            next_id += 1
            next_order += 1
        counts['model entities given generated LODs'] += 1
    scene['models'] = models


def settle_props(pack, out, b0, b1):
    """After ARCH_SCALE scaled the layout about the origin, put every free-standing prop back on what it rested on in the
    unscaled scene (--scene-bounds before/after): its pivot may not be at its base, and a prop stacked on another prop
    (a box on a table) must follow that prop, which kept its size. The gap above the support scales with a building
    support and stays as is on a prop."""
    s = ARCH_SCALE[pack.name]
    ids = {e['id']: e for e in out['boxes'] + out['empties'] + out['models']}
    skip = {e['id'] for e in ids.values() if e['name'] in ('Player Spawn', 'Sun')}
    for e in ids.values():
        q = e
        while q['parentId'] in ids:
            q = ids[q['parentId']]
            if q['id'] in skip:
                skip.add(e['id'])
                break
    box_ids = {e['id'] for e in out['boxes']}
    cls = {}
    for e in out['models']:
        if e['id'] in skip or e['id'] not in b0 or e['id'] not in b1:
            continue
        par = ids.get(e['parentId'])
        cls[e['id']] = None if (par is not None and 'path' in par) else arch_class(pack.name, e)
    cur = {i: [list(b1[i][0]), list(b1[i][1])] for i in cls}
    decals = {i for i, e in ((i, ids[i]) for i in cls) if (e['path'].replace(chr(92), '/').split('/Meshes/')[-1].split('/')[0]
                                                         in ('Decals',))}
    supports = [i for i in cls if i not in decals and i not in box_ids]
    props = sorted((i for i in cls if cls[i] is False), key=lambda i: b0[i][0][1])
    fixed = skipped = 0
    for i in props:
        lo, hi = b0[i]
        cx, cz = (lo[0] + hi[0]) / 2, (lo[2] + hi[2]) / 2
        best = None
        for j in supports:
            if j == i:
                continue
            slo, shi = b0[j]
            if slo[0] <= cx <= shi[0] and slo[2] <= cz <= shi[2] and shi[1] <= lo[1] + 0.05 and (best is None or shi[1] > b0[best][1][1]):
                best = j
        if best is None or lo[1] - b0[best][1][1] > 0.3:
            continue
        gap = lo[1] - b0[best][1][1]
        want = cur[best][1][1] + (s if cls[best] else 1.0) * gap
        dy = want - cur[i][0][1]
        if abs(dy) <= 0.004:
            continue
        if abs(dy) > 0.4:
            skipped += 1
            continue
        ids[i]['position'][1] = round(ids[i]['position'][1] + dy, 6)
        cur[i][0][1] += dy
        cur[i][1][1] += dy
        fixed += 1
    pack.counts['props re-seated on their floor after the architecture scale'] += fixed
    if skipped:
        pack.counts['props left unseated (> 0.4 m off)'] += skipped
