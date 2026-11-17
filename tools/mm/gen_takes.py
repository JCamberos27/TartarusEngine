r"""Builds tools/mm/takes.csv: which mocap takes the motion matching bake uses, their trims and their tags.

    python tools/mm/gen_takes.py [--source <pack folder>] [--out tools/mm/takes.csv]

The pack folder (anims/, rig/, manifest.json) comes from --source or TARTARUS_MM_SOURCE; it is never hard-coded.
Only the manifest is read (names and lengths), so this runs in a second.

Curation (docs/MOTION_MATCHING_HANDOFF.md, decision 4, and the budgets in decision 6):
- Exaggerated takes (`_S`) are left out but kept, tagged `exaggerated`.
- Dodges and strolls are left out for v1, tagged (`dodge`, `stroll`).
- Every take loses TRIM seconds at each end (the actor settling in and out).
- Each pattern (the name without its take number) gets a budget of seconds, filled from its takes in order
  (Take001 first), so a pattern shot five times doesn't crowd out one shot once. The budgets put the database at
  about 1.3 h, inside the 150 MB pose and 40 MB feature budgets.
Per-frame labels (gait, crouch, strafe, airborne, prone) are computed by the bake, not here; the tags here are per take:
the group, the pattern's kind (idle, turn, jump, start, strafe, loop, circles, snakes, card, stops, transition, accel),
`forward` (takes that walk forward along curves: loop, circles, snakes - the root's facing is checked on these), and
the name suffixes (`fo`, `ml`, `fixed`).

The pack's info sheet (`MotionMatchingLibrary_2025 Info Sheet.pdf`, archived with the pack on the Drive under
`Raw\Animations\Quantum Stage - Motion Matching Library\Docs`) is a PDF, so what it adds is copied into SHEET below
by hand: `_S` is the exaggerated version, `_M_L` the medium and large jumps; `_FO` it doesn't explain (its takes are
plain idle variations). It also calls one take without `_S` exaggerated, and flags one pattern as incomplete.
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TRIM = 1.0  # seconds off each end of a take
MIN_KEEP = 10.0  # a take contributing less than this is left out

# Seconds kept per pattern, by group; PATTERN_BUDGET overrides by pattern.
GROUP_BUDGET = {
    'Stand': 75.0, 'Walk': 90.0, 'Jog': 90.0, 'Run': 90.0, 'Crouch': 70.0,
    'Transitions': 120.0, 'AccelerationDeceleration': 240.0, 'Stroll': 0.0,
}
PATTERN_BUDGET = {
    'Stand_Idle_Neutral': 60.0, 'Stand_Idle_Fidgets': 60.0, 'Stand_Idle_LookAround': 60.0,
    'Stand_Idle_Neutral_FO': 60.0, 'Stand_Idle_Variations_FO': 60.0, 'Crouch_Idles': 60.0,
    'Stand_Step_Fixed_360': 60.0, 'Stand_Step_Turned_360': 60.0,
    'Stand_Jump_360': 60.0, 'Stand_Jump_InPlace': 60.0, 'Stand_Jump_360_M_L': 60.0,
    'Stand_Repositions_Star': 60.0, 'Stand_Dodge_360': 0.0,
}

# From the pack's info sheet: takes it marks that their names don't. (exaggerated, note)
SHEET = {
    'Stand_to_Walk_Take002': (True, 'info sheet: "Exaggerated version" (no _S in its name)'),
    'Crouch_Strafe_Star_Take001': (False, 'info sheet: "Pattern incomplete"'),
    'Stand_Jump_360_M_L_Take001': (False, 'info sheet: medium and large jumps (_M_L)'),
    'Stand_Jump_360_Take001': (False, 'info sheet: small jumps'),
}


def parse(name):
    m = re.match(r'^(.*?)_Take(\d+)$', name)
    base, take = (m.group(1), int(m.group(2))) if m else (name, 0)
    exaggerated = bool(re.search(r'(^|_)S($|_)', base))
    pattern = re.sub(r'_S(?=$|_)', '', base)
    group = pattern.split('_')[0]
    if group == 'Jogg':  # the pack spells one file Jogg_Card_withStops
        group = 'Jog'
        pattern = 'Jog' + pattern[4:]
    return group, pattern, take, exaggerated


def tags_for(group, pattern, exaggerated):
    t = [group.lower()]
    p = pattern.lower()
    rules = [
        ('idle', r'idle'), ('turn', r'turn_360|step_(fixed|turned)_360'), ('jump', r'jump'), ('dodge', r'dodge'),
        ('start', r'stand_to_|crouch_to_'), ('interrupted', r'interrupted'), ('strafe', r'strafe'),
        ('loop', r'_loop'), ('circles', r'circles'), ('snakes', r'snakes'), ('card', r'_card'),
        ('stops', r'withstops'), ('transition', r'^transitions'), ('accel', r'^accelerationdeceleration'),
        ('reposition', r'repositions'), ('fo', r'_fo(_|$)'), ('ml', r'_m_l(_|$)'), ('fixed', r'_fixed(_|$)'),
    ]
    t += [tag for tag, rx in rules if re.search(rx, p)]
    if re.search(r'_loop|circles|snakes', p) and 'strafe' not in t:
        t.append('forward')
    if exaggerated:
        t.append('exaggerated')
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--source', default=os.environ.get('TARTARUS_MM_SOURCE', ''))
    ap.add_argument('--out', default=str(ROOT / 'tools' / 'mm' / 'takes.csv'))
    a = ap.parse_args()
    manifest = Path(a.source) / 'manifest.json' if a.source else None
    if not manifest or not manifest.is_file():
        print('gen_takes: set TARTARUS_MM_SOURCE (or --source) to the mocap pack folder holding manifest.json')
        return 2
    anims = json.loads(manifest.read_text(encoding='utf-8-sig'))['animations']
    rows = []
    for an in anims:
        group, pattern, take, exaggerated = parse(an['name'])
        exaggerated = exaggerated or SHEET.get(an['name'], (False, ''))[0]
        rows.append(dict(name=an['name'], group=group, pattern=pattern, take=take, seconds=float(an['length_seconds']),
                         frames=int(an['num_frames']), exaggerated=exaggerated,
                         tags=tags_for(group, pattern, exaggerated)))
    rows.sort(key=lambda r: (r['group'], r['pattern'], r['exaggerated'], r['take']))

    used = {}
    for r in rows:
        r['include'], r['trim_start'], r['trim_end'], r['notes'] = 0, TRIM, 0.0, ''
        budget = PATTERN_BUDGET.get(r['pattern'], GROUP_BUDGET.get(r['group'], 60.0))
        sheet_note = SHEET.get(r['name'], (False, ''))[1]
        if r['exaggerated']:
            r['notes'] = 'exaggerated: kept for later' + ('; ' + sheet_note if sheet_note else '')
            continue
        r['notes'] = sheet_note
        if budget <= 0.0:
            r['notes'] = 'left out of v1'
            continue
        left = budget - used.get(r['pattern'], 0.0)
        keep = min(r['seconds'] - 2.0 * TRIM, left)
        if keep < MIN_KEEP:
            r['notes'] = 'pattern budget used up' + ('; ' + r['notes'] if r['notes'] else '')
            continue
        r['include'] = 1
        r['trim_end'] = round(TRIM + keep, 3)
        used[r['pattern']] = used.get(r['pattern'], 0.0) + keep

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    cols = ['name', 'group', 'pattern', 'take', 'seconds', 'frames', 'include', 'trim_start', 'trim_end', 'tags', 'notes']
    with out.open('w', encoding='utf-8', newline='\n') as f:
        f.write('# Generated by tools/mm/gen_takes.py from the pack manifest - edit the script, not this file.\n')
        f.write(','.join(cols) + '\n')
        for r in rows:
            vals = dict(r, tags=';'.join(r['tags']), seconds=f"{r['seconds']:.3f}", trim_start=f"{r['trim_start']:.3f}",
                        trim_end=f"{r['trim_end']:.3f}")
            f.write(','.join(str(vals[c]).replace(',', ' ') for c in cols) + '\n')
    inc = [r for r in rows if r['include']]
    secs = sum(r['trim_end'] - r['trim_start'] for r in inc)
    by_group = {}
    for r in inc:
        by_group[r['group']] = by_group.get(r['group'], 0.0) + r['trim_end'] - r['trim_start']
    print(f'gen_takes: {len(inc)} of {len(rows)} takes, {secs / 60.0:.1f} min ({secs / 3600.0:.2f} h) -> {out}')
    for g, s in sorted(by_group.items()):
        print(f'  {g:26s} {s / 60.0:5.1f} min')
    return 0


if __name__ == '__main__':
    sys.exit(main())
