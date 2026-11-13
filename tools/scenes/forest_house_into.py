# Put a freshly built Forest house (forest_house.py) into the committed Forest and ForestDay scenes, leaving the rest of
# each scene exactly as it is. The Forest scene carries hand edits build_forest.py doesn't make (mushrooms out, shadows
# off on ground cover), so the house is swapped in rather than the whole Forest rebuilt.
#
#   python tools/scenes/forest_house_into.py [<repo>]
# Builds the Forest in memory, takes its 'Abandoned House', its 'Yard' and the grass's density mask (the lawn follows the
# walk and the driveway), replaces those in project/scenes/Forest.json, then derives ForestDay.json from it
# (build_forest_day.py), so the night Forest keeps its own sky, fog and grade.
import json
import os
import sys

import build_forest as bf
import build_forest_day as bfd
import scene_kit as k


def subtree(ents, root):
    kids = {}
    for e in ents:
        kids.setdefault(e['parentId'], []).append(e)
    out, todo = [], [root]
    while todo:
        i = todo.pop()
        for e in kids.get(i, []):
            out.append(e)
            todo.append(e['id'])
    return out


def main():
    repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..'))
    built, counts = bf.build(repo)
    path = os.path.join(repo, 'project', 'scenes', 'Forest.json')
    with open(path, encoding='utf-8') as f:
        d = json.load(f)
    keys = ('empties', 'boxes', 'models')
    old = [e for key in keys for e in d[key]]
    new = [(key, e) for key in keys for e in getattr(built, key)]
    moved = {}
    for name in ('Abandoned House', 'Yard'):
        top_old = next(e for e in d['empties'] if e['name'] == name)
        top_new = next(e for _, e in new if e['name'] == name)
        gone = {top_old['id']} | {e['id'] for e in subtree(old, top_old['id'])}
        for key in keys:
            d[key] = [e for e in d[key] if e['id'] not in gone]
        keep = {top_new['id']} | {e['id'] for e in subtree([e for _, e in new], top_new['id'])}
        base = max(e['id'] for key in keys for e in d[key]) + 1
        remap = {}
        for _, e in sorted(((key, e) for key, e in new if e['id'] in keep), key=lambda ke: ke[1]['id']):
            remap[e['id']] = base + len(remap)
        for key, e in new:
            if e['id'] in keep:
                c = json.loads(json.dumps(e))
                c['id'] = c['order'] = remap[e['id']]
                c['parentId'] = top_old['parentId'] if e['id'] == top_new['id'] else remap[e['parentId']]
                d[key].append(c)
        moved[name] = (len(gone), len(keep))
    # the grass's density mask: the lawn round the house follows the walk and the driveway
    grass_new = next(e for _, e in new if 'Grass Area' in e)
    for key in keys:
        for e in d[key]:
            if 'Grass Area' in e:
                e['Grass Area'] = grass_new['Grass Area']
    s = k.Scene({})
    s.data = {key: v for key, v in d.items() if key not in keys}
    s.boxes, s.empties, s.models = d['boxes'], d['empties'], d['models']
    s.write(path)
    print('forest_house_into: ' + ', '.join(f'{v} {k_}' for k_, v in counts.items() if k_ in (
        'sealed doors opened', 'out of doorways', 'knocked over', 'shoved', 'fell off', 'taken', 'rubbish',
        'books on the floor', 'leaves blown in', 'decals', 'quiet shadows')) +
          f'; ' + ', '.join(f'{n} {a} -> {b}' for n, (a, b) in moved.items()) + f' entities -> {path}')
    sys.argv = [sys.argv[0], repo]
    bfd.main()


if __name__ == '__main__':
    main()
