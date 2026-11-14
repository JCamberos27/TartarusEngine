# The Forest house's GI Volumes (baked bounce light, src/Renderer/GIData.h): one or more per room, covering the whole
# room, each listing the light circuits it bakes a layer for. Run after the house is built (build_forest.py) and before
# `--bake-gi scenes/Forest.json`; it edits project/scenes/Forest.json in place:
#   - removes the old GI Volumes and the rooms' Bounce fill lights (the hand-placed stand-ins for the bounce),
#   - adds the volumes under 'House Lights' (the lights' own space, pack coordinates).
#
#   python tools/scenes/house_gi.py [--dry] [--preview]
#
# --preview writes Forest_preview.json (1 m probes, its own scenes/Forest_preview.gi) for a quick whole-house check
# bake (TARTARUS_GI_BOUNCES=1) before the full one; Forest.json is left alone.
#
# A room's cover: the biggest box round its first uncovered cell (house_lights.room_box: grown over the room's own
# cells, pushed out to the wall faces + 5 cm), again until what's left is a sliver - so an L-shaped room or a recess
# gets a box of its own and no corner is left without bounce. Layers, at most GIVolumes::kMaxLayers = 4: the room's
# circuits (its own and the lamps standing in it), then the circuits of the rooms it opens onto (a doorway: the rooms'
# cells touch), then the Outside (moon and sky through the windows), which every volume has.
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import forest_house as fh  # noqa: E402
import house_lights as hl  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
SCENE = os.path.join(REPO, 'project', 'scenes', 'Forest.json')
BAKE_FILE = 'scenes/Forest.gi'
SPACING = 0.5
FADE = 0.05
INTENSITY = 2.0          # Jacob's pick (2026-11-14): physical bounce x2 reads like the old fill without its flatness
MAX_LAYERS = 4
MAX_BOXES = 10           # per room
SLIVER = 0.35            # m^2 of a room left uncovered that isn't worth a box (door jambs, a skirting's gap)
CELL = hl.ROOM_CELL


def house_rooms():
    """The rooms exactly as house_lights.wire sees them: the pack with the same edits add_house makes."""
    notes = {}
    pk = fh.Pack(os.path.join(HERE, 'forest_house_pack.json'))
    fh.open_sealed_doors(pk, notes)
    fh.floor_the_stairwell(pk, notes)
    fh.clear_walls(pk, notes)
    fh.ease_side_door(pk, notes)
    fh.trash_house(pk, random.Random(301), notes)
    return pk, hl.named_rooms(pk.fit, fh.floor_rects(pk), fh)


def cover(room, fit):
    """Boxes (x0, x1, y0, y1, z0, z1) covering the room's cells."""
    cells = room['cells']
    left = {hl.cell_key(x, z): (x, z) for x, z in cells}
    boxes = []
    while left and len(boxes) < MAX_BOXES and len(left) * CELL * CELL > SLIVER:
        # start from the uncovered cell deepest in what's left (its most uncovered neighbours): a recess, not its rim
        def depth(k):
            return sum((k[0] + dx, k[1] + dz) in left for dx in (-3, 0, 3) for dz in (-3, 0, 3))
        k0 = max(left, key=depth)
        # grown over the cells no box holds yet: boxes abut (a few cm of overlap where they meet) instead of re-baking
        # what an earlier one already covers
        b = hl.room_box(list(left.values()), left[k0], fit, room['floor'], room['ceil'])
        boxes.append(b)
        for k in [k for k, (x, z) in left.items() if b[0] - 0.01 <= x <= b[1] + 0.01 and b[4] - 0.01 <= z <= b[5] + 0.01]:
            del left[k]
    return boxes, len(left) * CELL * CELL


def grow_light_boxes(lights, rooms, covers):
    """Each lamp's Light > Bounds grown over the boxes of its room it doesn't reach, where the grown box still holds only
    this room (no cell of another room, and mostly this room's floor): a recess or the other leg of an L-shaped room
    gets the lamp's light instead of staying black. Returns how many lights grew."""
    grown = 0
    for e in lights:
        r = hl.room_of(rooms, e['position'], 0.6)
        L = e['Light']
        if not r or not L.get('Bounds') or r['name'] not in covers:
            continue
        others = [hl.cell_key(x, z) for o in rooms if o is not r and o['storey'] == r['storey'] for x, z in o['cells']]
        mine = {hl.cell_key(x, z) for x, z in r['cells']}
        c, sz = L['Bounds Center'], L['Bounds Size']
        box = [c[0] - sz[0] / 2, c[0] + sz[0] / 2, c[1] - sz[1] / 2, c[1] + sz[1] / 2, c[2] - sz[2] / 2, c[2] + sz[2] / 2]
        changed = False
        for b in sorted(covers[r['name']], key=lambda b: -(b[1] - b[0]) * (b[5] - b[4])):
            u = [min(box[0], b[0]), max(box[1], b[1]), box[2], box[3], min(box[4], b[4]), max(box[5], b[5])]
            if u[:2] + u[4:] == box[:2] + box[4:]:
                continue
            i0, i1 = hl.cell_key(u[0] + 0.15, 0)[0], hl.cell_key(u[1] - 0.15, 0)[0]
            j0, j1 = hl.cell_key(0, u[4] + 0.15)[1], hl.cell_key(0, u[5] - 0.15)[1]
            if any(i0 <= i <= i1 and j0 <= j <= j1 for i, j in others):
                continue
            inside = sum(1 for i, j in mine if i0 <= i <= i1 and j0 <= j <= j1)
            if inside * CELL * CELL < 0.85 * (u[1] - u[0]) * (u[5] - u[4]):
                continue
            box, changed = u, True
        if changed:
            L['Bounds Center'] = [round((box[0] + box[1]) / 2, 3), round((box[2] + box[3]) / 2, 3), round((box[4] + box[5]) / 2, 3)]
            L['Bounds Size'] = [round(box[1] - box[0], 3), round(box[3] - box[2], 3), round(box[5] - box[4], 3)]
            grown += 1
    return grown


MAX_RANGE = 8.0


def reach_walls(lights):
    """Each house light's Range out to its Bounds box's farthest corner (at most MAX_RANGE): the range is only a cost
    cut-off - the box already stops the light at the walls - and a 4.6 m chandelier left a 10 m room's far wall black."""
    n = 0
    for e in lights:
        L = e['Light']
        if not L.get('Bounds') or L.get('Type') == 'Directional':
            continue
        c, sz, p = L['Bounds Center'], L['Bounds Size'], e['position']
        far = max(sum((c[a] + sx * sz[a] / 2 - p[a]) ** 2 for a, sx in zip(range(3), sgn)) ** 0.5
                  for sgn in [(i, j, k_) for i in (-1, 1) for j in (-1, 1) for k_ in (-1, 1)])
        want = round(min(max(L['Range'], far + 0.2), MAX_RANGE), 2)
        if want > L['Range'] + 0.05:
            L['Range'] = want
            n += 1
    return n


def read_peaks(path):
    """A .gi file's brightest probe per volume and layer (GIData.h's TGI1 layout): {volume: {circuit: peak}}."""
    import struct
    import numpy as np
    data = open(path, 'rb').read()
    assert data[:4] == b'TGI1', path
    o = 8
    (count,) = struct.unpack_from('<I', data, o)
    o += 4

    def string():
        nonlocal o
        (n,) = struct.unpack_from('<I', data, o)
        s = data[o + 4:o + 4 + n].decode('utf-8')
        o += 4 + n
        return s
    out = {}
    for _ in range(count):
        name = string()
        o += 4 * (3 + 9 + 3)
        dims = struct.unpack_from('<3i', data, o)
        o += 12
        (layers,) = struct.unpack_from('<I', data, o)
        o += 4
        n = dims[0] * dims[1] * dims[2]
        out[name] = {}
        for _ in range(layers):
            circuit = string()
            sh = np.frombuffer(data, dtype='<f2', count=n * 12, offset=o).astype(np.float32).reshape(n, 12)
            o += n * 24
            out[name][circuit] = float(sh[:, [0, 4, 8]].max())
    return out


def neighbours(rooms):
    """Room name -> the names of the rooms it opens onto (their cells touch: a doorway or an archway)."""
    keys = {r['name']: {hl.cell_key(x, z) for x, z in r['cells']} for r in rooms}
    out = {r['name']: set() for r in rooms}
    for a in rooms:
        if a['name'].startswith(('ground room', 'upstairs room')):
            continue
        ka = keys[a['name']]
        for b in rooms:
            if b is a or b['storey'] != a['storey']:
                continue
            kb = keys[b['name']]
            if any((i + di, j + dj) in kb for i, j in ka for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                out[a['name']].add(b['name'])
    return out


def main():
    dry = '--dry' in sys.argv
    preview = '--preview' in sys.argv
    spacing = 1.0 if preview else SPACING
    bake_file = 'scenes/Forest_preview.gi' if preview else BAKE_FILE
    out_scene = SCENE.replace('Forest.json', 'Forest_preview.json') if preview else SCENE
    pk, rooms = house_rooms()
    sc = json.load(open(SCENE, encoding='utf-8'))
    lights_root = next(e for e in sc['empties'] if e['name'] == 'House Lights')
    lights = [e for e in sc['empties'] if e.get('parentId') == lights_root['id'] and 'Light' in e and 'Light Circuit' in e
              and not e['name'].startswith('Bounce ')]
    # circuits by room: a light's room is where it hangs (or stands); the stairwell's belong to the landing and the hall
    by_room = {r['name']: [] for r in rooms}
    for e in lights:
        r = hl.room_of(rooms, e['position'], 0.6)
        c = e['Light Circuit']['Circuit']
        if r and c not in by_room[r['name']]:
            by_room[r['name']].append(c)
    for rname, (circuit, _, _) in hl.ROOM_CIRCUITS.items():
        if rname in by_room and circuit not in by_room[rname]:
            by_room[rname].insert(0, circuit)
    main_circuit = {rname: c for rname, (c, _, _) in hl.ROOM_CIRCUITS.items()}
    nb = neighbours(rooms)

    vols = []
    covers = {}
    for r in rooms:
        own = by_room[r['name']]
        if not own and r['name'].startswith(('ground room', 'upstairs room')):
            continue
        layers = list(own)
        for n in sorted(nb[r['name']]):
            c = main_circuit.get(n)
            if c and c not in layers:
                layers.append(c)
        layers = layers[:MAX_LAYERS - 1]
        boxes, left = cover(r, pk.fit)
        covers[r['name']] = boxes
        print(f"{r['name']:22} {len(r['cells']) * CELL * CELL:5.1f} m2  {len(boxes)} boxes"
              f"{f' ({left:.2f} m2 uncovered)' if left > 0.05 else ''}  layers: {', '.join(layers) or '-'} + Outside")
        for n, b in enumerate(boxes):
            vols.append((f"GI {r['name']}" + (f' {n + 1}' if len(boxes) > 1 else ''), b, layers))
    # the stairwell: two storeys tall, no floor of its own
    x0, x1, z0, z1 = hl.STAIRWELL
    vols.append(('GI stairwell', (x0, x1, fh.FLOOR_1 - 0.05, fh.CEIL_2 + 0.05, z0, z1), ['Stairs']))

    # --prune-from <preview .gi>: drop the circuit layers the preview bake found empty in a volume (a neighbour behind a
    # shut door or a wall), so the full bake doesn't spend time on them. The Outside always stays.
    if '--prune-from' in sys.argv:
        peaks = read_peaks(os.path.join(REPO, 'project', sys.argv[sys.argv.index('--prune-from') + 1]))
        dropped = 0
        for i, (name, b, layers) in enumerate(vols):
            pk_ = peaks.get(name)
            if not pk_:
                continue
            best = max(pk_.values())
            keep = [c for c in layers if pk_.get(c, best) >= 0.02 * best]
            dropped += len(layers) - len(keep)
            vols[i] = (name, b, keep)
        print(f'pruned {dropped} empty layers (from the preview bake)')
    print(f'{grow_light_boxes(lights, rooms, covers)} lights: Bounds grown into the rest of their room')
    print(f'{reach_walls(lights)} lights: Range out to their box')
    for e in lights:  # coverage report: what still falls short of its box
        L = e['Light']
        if not L.get('Bounds'):
            continue
        c, sz, p = L['Bounds Center'], L['Bounds Size'], e['position']
        far = max(sum((c[a] + sg[a] * sz[a] / 2 - p[a]) ** 2 for a in range(3)) ** 0.5
                  for sg in [(i, j, k_) for i in (-1, 1) for j in (-1, 1) for k_ in (-1, 1)])
        if far > L['Range'] + 0.3:
            print(f"  short: {e['name']} reaches {L['Range']} m of its box's {far:.1f} m")
    probes = 0
    for name, b, layers in vols:
        dims = [max(2, round((b[2 * a + 1] - b[2 * a]) / spacing + 0.4999) + 1) for a in range(3)]
        probes += dims[0] * dims[1] * dims[2] * (len(layers) + 1)
    print(f'{len(vols)} volumes, {probes} probe-layers (x2 bounces)')
    if dry:
        return

    sc['empties'] = [e for e in sc['empties'] if 'GI Volume' not in e and
                     not (e.get('parentId') == lights_root['id'] and e['name'].startswith('Bounce '))]
    nid = 1 + max(e['id'] for k_ in ('empties', 'models', 'boxes') for e in sc[k_])
    for name, b, layers in vols:
        sc['empties'].append({
            'GI Volume': {'Bake File': bake_file, 'Circuits': ';'.join(layers), 'Fade': FADE, 'Intensity': INTENSITY,
                          'Outside': True, 'Size': [round(b[1] - b[0], 3), round(b[3] - b[2], 3), round(b[5] - b[4], 3)],
                          'Spacing': spacing},
            'id': nid, 'name': name, 'order': nid, 'parentId': lights_root['id'],
            'position': [round((b[0] + b[1]) / 2, 3), round((b[2] + b[3]) / 2, 3), round((b[4] + b[5]) / 2, 3)],
            'rotation': [0.0, 0.0, 0.0, 1.0], 'scale': [1.0, 1.0, 1.0]})
        nid += 1
    with open(out_scene, 'w', encoding='utf-8', newline='\n') as f:
        f.write(json.dumps(sc, indent=2) + '\n')
    print('->', out_scene)


if __name__ == '__main__':
    main()
