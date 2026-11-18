r"""Builds tools/mm/takes.csv: which mocap takes the motion matching bake uses, their trims and their tags.

    python tools/mm/gen_takes.py [--source <pack folder>] [--out tools/mm/takes.csv]

The pack folder (anims/, rig/, manifest.json) comes from --source or TARTARUS_MM_SOURCE; it is never hard-coded.
Only the manifest is read (names and lengths), so this runs in a second.

Curation (docs/MOTION_MATCHING_HANDOFF.md, decision 4 and decision 6; the measurements in docs/mm_audit.md):
- Every take of the pack is baked whole (less TRIM seconds at each end, the actor settling in and out), except:
  - the exaggerated takes (`_S`, and one the info sheet names), kept for later as a style variant, tagged `exaggerated`;
  - the dodges, left out by the owner's choice, tagged `dodge`.
  (Until 2026-11-17 each pattern had a budget of seconds that kept a take's start and cut its tail - Run_Loop_Take002
  kept 37 s of 152 - and the strolls were left out. The audit found no cruise redundant enough to drop - its steady
  windows' nearest neighbour in another take is mostly over 1.0 in normalized features - so nothing is cut.)
- The mirrored takes: the strafes of the styles the audit found lopsided (left against right steady seconds more than
  1.25 apart: the stroll's and the jog's strafes) are baked a second time, mirrored left for right (a row named
  `<take>_Mirror`, `source` the take, tagged `mirrored`; MotionBake::MirrorPose).
Per-frame labels (gait, style, crouch, strafe, airborne, prone) are computed by the bake, not here; the tags here are per
take: the group, the pattern's kind (idle, turn, jump, start, strafe, loop, circles, snakes, card, stops, transition,
accel), `forward` (takes that walk forward along curves: loop, circles, snakes - the root's facing is checked on these),
the name suffixes (`fo`, `ml`, `fixed`), and the take's intent where its group doesn't say it: `style=<a>+<b>` (the styles
it holds - MotionClipIntent): a start's style is where it goes (Stand_to_Jog: jog), the accelerations and transitions hold
several (each frame's from its speed), the run takes walk back between their sprints (walk + run).

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

# The mirrored takes (docs/mm_audit.md, "Left / right balance"): the strafes of these groups.
MIRROR_GROUPS = ('Stroll', 'Jog')

# The intent of the takes whose group doesn't name one style (MotionClipIntent): pattern regex -> the styles they hold.
STYLE_RULES = [
    (r'^Stand_to_Walk', 'walk'), (r'^Stand_to_Jog', 'jog'), (r'^Stand_to_Run', 'run'),
    (r'^AccelerationDeceleration', 'walk+jog+run'),
    (r'^Transitions_Movement_Jogs', 'walk+jog'), (r'^Transitions_Movement', 'walk+jog+run+crouch'),
]

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
    for rx, styles in STYLE_RULES:
        if re.search(rx, pattern):
            t.append('style=' + styles)
            break
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--source', default=os.environ.get('TARTARUS_MM_SOURCE', ''))
    ap.add_argument('--out', default=None)
    # The full-pack audit (tools/mm/audit.py, docs/mm_audit.md): every take - exaggerated ones too, tagged - with the same
    # trims and no budgets, to measure what the whole pack holds before choosing what to keep.
    ap.add_argument('--audit', action='store_true')
    a = ap.parse_args()
    if a.out is None:
        a.out = str(ROOT / 'work' / 'mm_audit' / 'takes_audit.csv') if a.audit else str(ROOT / 'tools' / 'mm' / 'takes.csv')
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

    mirrored = []
    for r in rows:
        r['include'], r['trim_start'], r['trim_end'], r['notes'], r['source'] = 0, TRIM, 0.0, '', ''
        sheet_note = SHEET.get(r['name'], (False, ''))[1]
        r['trim_end'] = round(r['seconds'] - TRIM, 3)
        if a.audit:
            r['include'], r['notes'] = 1, sheet_note
            continue
        if r['exaggerated']:
            r['notes'] = 'exaggerated: kept for later' + ('; ' + sheet_note if sheet_note else '')
            continue
        if 'dodge' in r['tags']:
            r['notes'] = "left out (the owner's choice)"
            continue
        r['notes'] = sheet_note
        r['include'] = 1
        if r['group'] in MIRROR_GROUPS and 'strafe' in r['tags']:
            m = dict(r, name=r['name'] + '_Mirror', source=r['name'], tags=r['tags'] + ['mirrored'],
                     notes='mirrored: the audit found the ' + r['group'].lower() + ' strafes lopsided')
            mirrored.append(m)
    rows += mirrored
    rows.sort(key=lambda r: (r['group'], r['pattern'], r['exaggerated'], r['take'], r['name']))

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    cols = ['name', 'source', 'group', 'pattern', 'take', 'seconds', 'frames', 'include', 'trim_start', 'trim_end', 'tags', 'notes']
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
