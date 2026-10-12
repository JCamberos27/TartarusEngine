# Builds the apartment scenes from tools/scenes/apartment_kit.py (the plan and the Builder) and the households in
# tools/scenes/households/:
#   project/scenes/Apartment_3Bed.json    the family's apartment on its own (the FPS benchmark level)
#   project/scenes/Apartment_Floor3.json  (--floor) the whole floor: the family in 310, three more apartments of the
#                                         same plan (311-313) and the corridor between them
#
#   python tools/scenes/build_apartment.py <repo> [--floor] [--out <scene.json>]
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apartment_kit import FURNISHED, Builder, floor_units  # noqa: E402
from households import corridor, family_310, recluse_313, roommates_312, widow_311  # noqa: E402

HOUSEHOLDS = {310: family_310, 311: widow_311, 312: roommates_312, 313: recluse_313}


def build(b):
    b.night()
    if not b.floor:
        b.apartment(FURNISHED, (0.0, 0.0, 1, 1), family_310, name='Apartment')
    else:
        floor = b.s.empty('Floor 3')
        for number, xf in floor_units():
            b.apartment(number, xf, HOUSEHOLDS[number], floor)
        b.common('Corridor', corridor.corridor, floor)
        b.root = floor
    b.audio()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('repo')
    ap.add_argument('--out')
    ap.add_argument('--floor', action='store_true', help='the whole floor: apartments 310-313 and their corridor')
    args = ap.parse_args()
    repo = os.path.abspath(args.repo)
    b = Builder(repo, floor=args.floor)
    build(b)
    problems = b.validate()
    for line in problems:
        print('build_apartment: check:', line)
    out = args.out or os.path.join(repo, 'project', 'scenes',
                                   'Apartment_Floor3.json' if args.floor else 'Apartment_3Bed.json')
    b.s.write(out)
    print(f'build_apartment: {len(b.s.models)} models, {len(b.s.boxes)} boxes, {len(b.s.empties)} empties -> {out}')


if __name__ == '__main__':
    main()
