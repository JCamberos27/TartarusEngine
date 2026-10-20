# project/scenes/AssetPerf.json: the model library under load, for --perf-bench. Apartment_Floor3 (the furnished
# corridor and four apartments) stacked three storeys high, about 3,000 library models; the player spawns on the
# middle storey so a floor above and one below are in reach of every pass (shadows, lights). The source floor's NPCs
# are left out: the bench measures rendering, not AI. Deterministic: same input, same file.
#
# Neither scene is committed any more (the Apartment Hallway replaced the content scenes); build them on demand:
#   python tools/scenes/build_apartment.py <repo> --floor      (writes Apartment_Floor3.json, this script's source)
#   python tools/scenes/build_asset_perf.py <repo>
#   TartarusEngine.exe --perf-bench project/scenes/AssetPerf.json --perf-res 1920x1080
import copy
import json
import os
import sys

STOREYS = 3
SPAWN_STOREY = 1  # 0-based: the middle one
SOURCE = 'Apartment_Floor3.json'


def main():
    repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else '.')
    scenes = os.path.join(repo, 'project', 'scenes')
    with open(os.path.join(scenes, SOURCE), encoding='utf-8') as f:
        src = json.load(f)
    kinds = [k for k in ('empties', 'boxes', 'models') if k in src]
    ents = {e['id']: e for k in kinds for e in src[k]}

    def root(e):
        while e['parentId'] != -1:
            e = ents[e['parentId']]
        return e['name']

    floor = [e for e in ents.values() if root(e) == 'Floor 3']
    # storey height: the top of the floor's highest box (its ceiling slab)
    top = max(b['position'][1] + abs(b['scale'][1]) / 2 for b in src['boxes'] if root(b) == 'Floor 3')
    height = round(top + 0.005, 3)

    out = {k: v for k, v in src.items() if k not in kinds}
    out.update({k: [] for k in kinds})
    kind_of = {e['id']: k for k in kinds for e in src[k]}
    step = 10 ** len(str(max(ents)))  # each storey's ids in their own block
    order = 0
    for s in range(STOREYS):
        for e in sorted(floor, key=lambda e: e['order']):
            c = copy.deepcopy(e)
            c['id'] = e['id'] + s * step
            if c['parentId'] != -1:
                c['parentId'] += s * step
            else:
                c['name'] = f'Storey {s + 1}'
                c['position'] = [c['position'][0], c['position'][1] + s * height, c['position'][2]]
            c['order'] = order
            order += 1
            out[kind_of[e['id']]].append(c)
    spawn = [e for e in ents.values() if root(e) == 'Player Spawn']
    for e in sorted(spawn, key=lambda e: e['order']):
        c = copy.deepcopy(e)
        c['id'] = e['id'] + STOREYS * step
        if c['parentId'] != -1:
            c['parentId'] += STOREYS * step
        else:
            c['position'] = [c['position'][0], c['position'][1] + SPAWN_STOREY * height, c['position'][2]]
        c['order'] = order
        order += 1
        out[kind_of[e['id']]].append(c)

    path = os.path.join(scenes, 'AssetPerf.json')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(out, f, indent=2, sort_keys=True)
        f.write('\n')
    print(f'build_asset_perf: {path} - {STOREYS} storeys of {height} m, '
          f'{len(out["models"])} models, {len(out["boxes"])} boxes, {len(out["empties"])} empties')


if __name__ == '__main__':
    main()
