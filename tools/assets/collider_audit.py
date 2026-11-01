# Audits the colliders of a converted pack scene against what is drawn: counts, per finding kind, the colliders that don't fit
# a game (see KINDS), and lists the worst. World bounds come from the engine (--scene-bounds via unity_prefabs.scene_bounds).
#
#   python tools/assets/collider_audit.py <scene.json> [<scene.json> ...] [--exe <exe>] [--list N]
import argparse
import collections
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import unity_prefabs as up  # noqa: E402

KINDS = {
    'flat': 'box collider with a 0-thick side (PhysX drops it: nothing to stand on)',
    'oversized': 'box collider reaching > 15 cm past the drawn mesh on some side',
    'unit box': "Unity's default 1 m box on a mesh that isn't 1 m",
    'clutter': 'solid collider on a prop under 35 cm every way, or named as clutter (not an Openable)',
    'flat dressing': 'solid collider on a rug, carpet, decal, poster, painting or curtain',
    'mesh on furniture': 'triangle-mesh collider on a prop (not architecture)',
}
def words(e):
    base = re.sub(r' \(\d+\)$', '', e['name']) + ' ' + os.path.basename(e.get('path', ''))[:-4]
    return set(re.split(r'[^a-z]+', base.lower()))


def box_aabb(world, e, c):
    pos, rot, sc = world(e)
    ce, he = c.get('center') or [0.0, 0.0, 0.0], c['halfExtents']
    lo, hi = [1e9] * 3, [-1e9] * 3
    for sx in (-1, 1):
        for sy in (-1, 1):
            for sz in (-1, 1):
                v = [(ce[0] + sx * he[0]) * sc[0], (ce[1] + sy * he[1]) * sc[1], (ce[2] + sz * he[2]) * sc[2]]
                w = up._qrot(rot, tuple(v))
                for a in range(3):
                    lo[a] = min(lo[a], pos[a] + w[a])
                    hi[a] = max(hi[a], pos[a] + w[a])
    return lo, hi


def audit(scene_path, exe, arch, repo):
    s = json.load(open(scene_path, encoding='utf-8'))
    out = {k: s.get(k, []) for k in ('models', 'empties', 'boxes')}
    world = up._world_of(out)

    class C:
        pass
    conv = C()
    conv.exe, conv.pack = exe, C()
    conv.pack.repo = repo
    bounds = up.scene_bounds(conv, s)
    found = collections.defaultdict(list)
    total = 0
    for e in out['models'] + out['empties']:
        c = e.get('collider')
        if not c:
            continue
        total += 1
        # a collider-only helper child belongs to the model above it
        owner = e if 'path' in e else world.ents.get(e['parentId'])
        if not owner or owner['id'] not in bounds or c.get('isTrigger'):
            continue
        lo, hi = bounds[owner['id']]
        size = [h - l for l, h in zip(lo, hi)]
        shape = c.get('shape', 0)
        is_arch = arch(owner)
        if shape == 0:
            he = c.get('halfExtents') or [0, 0, 0]
            if max(he) > 0 and min(he) < 0.005:
                found['flat'].append((owner['name'], 0.0))
                continue
            if he == [0.5, 0.5, 0.5] and not all(0.8 < x < 1.2 for x in size):
                found['unit box'].append((owner['name'], max(abs(x - 1.0) for x in size)))
            blo, bhi = box_aabb(world, e, c)
            over = max(max(l - bl, bh - h) for l, h, bl, bh in zip(lo, hi, blo, bhi))
            if over > 0.15 and not is_arch:
                found['oversized'].append((owner['name'], over))
        if is_arch or 'Openable' in owner or 'Openable' in world.ents.get(owner['parentId'], {}):
            continue
        if max(size) < up.SMALL_PROP or words(owner) & up.CLUTTER_WORDS:
            found['clutter'].append((owner['name'], max(size)))
        elif words(owner) & up.DRESSING_WORDS:
            found['flat dressing'].append((owner['name'], max(size)))
        elif shape == 4:
            found['mesh on furniture'].append((owner['name'], max(size)))
    return total, found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scenes', nargs='+')
    ap.add_argument('--exe', default=os.path.abspath('b/Release/TartarusEngine.exe'))
    ap.add_argument('--list', type=int, default=8)
    ap.add_argument('--repo', default='.', help='the repo whose project/ the bounds load the models from')
    a = ap.parse_args()
    for sp in a.scenes:
        pack = next((p for p in up.ARCH_SCALE if os.path.basename(sp).startswith(p)), None)
        arch = (lambda e: bool(pack) and bool(up.arch_class(pack, e)))
        total, found = audit(sp, os.path.abspath(a.exe), arch, os.path.abspath(a.repo))
        print(f'## {os.path.basename(sp)}: {total} colliders')
        for k, why in KINDS.items():
            items = sorted(found.get(k, []), key=lambda x: -x[1])
            names = collections.Counter(n.split(' (')[0] for n, _ in items)
            print(f'  {k}: {len(items)}  ({why})')
            if a.list and items:
                print('     ' + ', '.join(f'{n} x{c}' for n, c in names.most_common(a.list)))


if __name__ == '__main__':
    main()
