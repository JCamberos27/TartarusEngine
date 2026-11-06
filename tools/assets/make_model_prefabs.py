"""One .prefab (and .meta) next to every library model, so the models appear in the asset browser as droppable prefabs.

For each .fbx/.obj/.glb/.gltf under project/assets/{Furniture,Architecture,Props,Environments/Apartment} (Characters is
never touched) this writes <stem>.prefab beside the model: one model entity pointing at the model's guid, plus a collider.
Materials are not written: the model's own .meta materialRemap (import_model_library.py) already binds them, so the
prefab instance gets the same ones.

Bounds: the model payloads (FBX/PNG) are gitignored, so they are read from --payload-root (a checkout that has them)
with the engine's own `TartarusEngine.exe --model-report`. It reports min/max in metres with the .meta's scale and pivot
applied, exactly the space the prefab entity draws the model in. No extra FBX reader needed.

Colliders (same JSON shape unity_prefabs.py writes): a box fitted to the bounds; a triangle mesh (shape 4) for
everything under Architecture/ except doors, which stay boxes (openables.py's Openable component belongs to scene
generators, so these prefabs do not carry one). A box flat on an axis becomes a 4 cm slab on the -side, as
unity_prefabs.THIN_BOX does for floors.

  python tools/assets/make_model_prefabs.py [--payload-root <checkout>] [--exe <TartarusEngine.exe>] [--dry] [--force]
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys

from import_model_library import write_json, write_meta

ROOTS = ['Furniture', 'Architecture', 'Props', 'Environments/Apartment']
MODEL_EXT = ('.fbx', '.obj', '.glb', '.gltf')
THIN = 0.04
REPORT = re.compile(r'\[ModelReport\] (.*) size (\S+) (\S+) (\S+) min (\S+) (\S+) (\S+) max (\S+) (\S+) (\S+) meshes ')


def models(assets):
    """[(rel path under assets, abs path)] of every model that has a model .meta in the clone."""
    out = []
    for root in ROOTS:
        for d, _, files in os.walk(os.path.join(assets, *root.split('/'))):
            for f in sorted(files):
                if f.lower().endswith(tuple(e + '.meta' for e in MODEL_EXT)):
                    p = os.path.join(d, f[:-5])
                    out.append((os.path.relpath(p, assets).replace(os.sep, '/'), p))
    return out


def bounds(exe, payload_assets, rels):
    """{rel: (min, max)} from the engine, one call per folder."""
    folders = sorted({os.path.dirname(r) for r in rels})
    found = {}
    for folder in folders:
        out = subprocess.run([exe, '--model-report', os.path.join(payload_assets, *folder.split('/'))],
                             capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
        for line in out.splitlines():
            m = REPORT.match(line.strip())
            if m:
                path = os.path.normpath(m.group(1)).replace(os.sep, '/')
                rel = path[len(os.path.normpath(payload_assets).replace(os.sep, '/')) + 1:]
                v = [float(x) for x in m.groups()[4:10]]
                found[rel.lower()] = (v[0:3], v[3:6])
    return found


def collider(rel, lo, hi):
    if rel.startswith('Architecture/') and 'door' not in os.path.basename(rel).lower():
        return {'friction': 0.6, 'isTrigger': False, 'shape': 4}
    centre = [(a + b) / 2 for a, b in zip(lo, hi)]
    half = [(b - a) / 2 for a, b in zip(lo, hi)]
    for i in range(3):
        if half[i] < 0.005 and max(half) > 0.025:
            centre[i] -= (THIN - 2 * half[i]) / 2
            half[i] = THIN / 2
    return {'center': [round(c, 6) for c in centre], 'friction': 0.6, 'halfExtents': [round(h, 6) for h in half],
            'isTrigger': False, 'shape': 0}


def prefab(rel, guid, col):
    entity = {'collider': col, 'id': 0, 'name': os.path.splitext(os.path.basename(rel))[0], 'order': 0, 'parentId': -1,
              'path': 'assets/' + rel, 'pathGuid': guid, 'position': [0.0, 0.0, 0.0], 'rotation': [0.0, 0.0, 0.0, 1.0],
              'scale': [1.0, 1.0, 1.0], 'static': True}
    return {'boxes': [], 'empties': [], 'formatVersion': 4, 'models': [entity]}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--repo', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    ap.add_argument('--payload-root', default=r'C:\Users\jacob\Desktop\TartarusEngine')
    ap.add_argument('--exe')
    ap.add_argument('--dry', action='store_true')
    ap.add_argument('--force', action='store_true')
    args = ap.parse_args()
    assets = os.path.abspath(os.path.join(args.repo, 'project', 'assets'))
    payload_assets = os.path.join(args.payload_root, 'project', 'assets')
    exe = args.exe or os.path.join(args.payload_root, 'build', 'Release', 'TartarusEngine.exe')

    todo, counts = [], collections.Counter()
    for rel, path in models(assets):
        stem = os.path.splitext(path)[0]
        top = '/'.join(rel.split('/')[:2 if rel.startswith('Environments') else 1])
        if os.path.exists(stem + '.prefab') and not args.force:
            counts[top, 'exists'] += 1
        else:
            todo.append((rel, path, top))
    if args.dry:
        for (top, what), n in sorted(counts.items()):
            print(f'{top}: {n} {what}')
        for top, n in sorted(collections.Counter(t for _, _, t in todo).items()):
            print(f'{top}: {n} to write')
        return
    found = bounds(exe, payload_assets, [r for r, _, _ in todo])
    for rel, path, top in todo:
        b = found.get(rel.lower())
        if not b:
            counts[top, 'no bounds (skipped)'] += 1
            print('no bounds for', rel, file=sys.stderr)
            continue
        with open(path + '.meta', encoding='utf-8') as f:
            guid = json.load(f)['guid']
        out = os.path.splitext(path)[0] + '.prefab'
        write_json(out, prefab(rel, guid, collider(rel, *b)))
        write_meta(out, 'assets/' + os.path.dirname(rel), 'prefab')
        counts[top, 'written'] += 1
    for (top, what), n in sorted(counts.items()):
        print(f'{top}: {n} {what}')


if __name__ == '__main__':
    main()
