# Render-level check of the Forest house's baked GI (GI Volumes + scenes/Forest.gi): pictures, not just numbers on the
# CPU side - the same lesson as house_render_check.py. Two engine runs of the same cameras, the scene as it is and with
# its GI bake unplugged (the volumes then skip, exactly as before GI):
#   - lit:  a room with only its own circuit on, looking up at its ceiling: GI must make the ceiling clearly brighter
#           (bounce off the floor and walls is what lights a ceiling);
#   - dark: only that room on, looking round a neighbour it opens onto: GI must not make the neighbour brighter
#           (no bounce leaking through walls; through an open doorway a little is right, so a small margin).
#
#   python tools/scenes/house_gi_check.py [<out dir>]
# Exit 0 when every check passes; failures are listed with their shots.
import json
import os
import sys

from PIL import Image, ImageStat

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import house_gi as hg  # noqa: E402
import house_lights as hl  # noqa: E402
import house_shots as hs  # noqa: E402

REPO = hs.REPO
SCN = os.path.join(REPO, 'project', 'scenes')
CEIL_GAIN = 1.08      # the lit room's ceiling, mean luminance with GI over without: at least this
DARK_MARGIN = 2.0     # the dark neighbour, mean luminance (0..255) with GI minus without: at most this
# (lit room, its dark neighbour)
PAIRS = [('living room', 'entrance hall'), ('kitchen-dining', 'entrance hall'), ('entrance hall', 'living room'),
         ('master bedroom', 'upstairs den'), ("child's bedroom", 'landing'), ('upstairs den', 'upstairs bathroom')]


def W(lx, ly, lz):  # house-lights (pack) space -> world: Pack Layout under Abandoned House, turned 90 deg
    return (lz + 1.0, ly - 0.485, -7.0 - lx)


def cams_for(pk, rooms):
    by = {r['name']: r for r in rooms}
    cams = []
    for lit, dark in PAIRS:
        for role, rn in (('lit', lit), ('dark', dark)):
            r = by[rn]
            boxes, _ = hg.cover(r, pk.fit)
            b = max(boxes, key=lambda b: (b[1] - b[0]) * (b[5] - b[4]))
            cx, cz = (b[0] + b[1]) / 2, (b[4] + b[5]) / 2
            if role == 'lit':   # from the middle, up at the ceiling a metre or so away
                cam, tgt = (cx, r['floor'] + 1.5, cz), (cx + 1.0, r['ceil'], cz + 0.6)
            elif b[1] - b[0] >= b[5] - b[4]:
                cam, tgt = (b[0] + 0.5, r['floor'] + 1.6, cz), (b[1], r['floor'] + 1.0, cz)
            else:
                cam, tgt = (cx, r['floor'] + 1.6, b[4] + 0.5), (cx, r['floor'] + 1.0, b[5])
            name = f"{role}_{lit.replace(' ', '_').replace(chr(39), '')}"
            cams.append((name, W(*cam), *hs.look(W(*cam), W(*tgt)), hl.ROOM_CIRCUITS[lit][0]))
    return cams


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'b', 'house_gi_check')
    pk, rooms = hg.house_rooms()
    cams = cams_for(pk, rooms)
    sc = json.load(open(os.path.join(SCN, 'Forest.json'), encoding='utf-8'))
    for e in sc['empties']:
        if 'GI Volume' in e:
            e['GI Volume']['Bake File'] = 'scenes/none.gi'
    nogi = os.path.join(SCN, 'Forest_gicheck_off.json')
    open(nogi, 'w', encoding='utf-8', newline='\n').write(json.dumps(sc, indent=2) + '\n')
    os.environ.setdefault('TARTARUS_SHOT_SIZE', '640x360')
    shots = {}
    try:
        # one run per lit circuit (TARTARUS_LIGHTS_ONLY is per run), both scenes
        for circuit in dict.fromkeys(c[-1] for c in cams):
            mine = [c[:4] for c in cams if c[-1] == circuit]
            for tag, scene in (('gi', 'Forest.json'), ('off', 'Forest_gicheck_off.json')):
                d = os.path.join(out, tag)
                hs.run_engine([(f'{tag}_{n}', p, y, pi) for n, p, y, pi in mine], d, None, scene, circuit)
                for n, *_ in mine:
                    shots[(tag, n)] = os.path.join(d, f'{os.path.splitext(scene)[0]}_{tag}_{n}.png')
    finally:
        os.remove(nogi)

    def mean(p):
        return ImageStat.Stat(Image.open(p).convert('L')).mean[0]
    fails = []
    for n, *_ in cams:
        g, o = mean(shots[('gi', n)]), mean(shots[('off', n)])
        if n.startswith('lit_'):
            ok = g >= o * CEIL_GAIN
            print(f'{"ok  " if ok else "FAIL"} {n}: ceiling {o:.1f} -> {g:.1f} with GI (x{g / max(o, 1e-3):.2f}, need x{CEIL_GAIN})')
        else:
            ok = g - o <= DARK_MARGIN
            print(f'{"ok  " if ok else "FAIL"} {n}: dark neighbour {o:.1f} -> {g:.1f} with GI (+{g - o:.1f}, max +{DARK_MARGIN})')
        if not ok:
            fails.append((n, shots[('gi', n)], shots[('off', n)]))
    for n, a, b in fails:
        print(f'  {n}: {a} vs {b}')
    print(f'{len(cams) - len(fails)}/{len(cams)} passed')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
