"""The weapon combo matrix: `--npc-test combos` logs -> one Markdown table (worst cell first) and a labelled MP4 per recorded cell.

    python tools/combo_matrix.py <run.log>... --out matrix.md [--videos] [--slow 0.5]

Each "[Combo] ROW" line is a cell (weapon/action/state): PASS, FLAG (the worst event, scored against its meter's threshold:
1.0 = just over) or SETUP (the weapon never got ready). Several logs (one per weapon, say) make one table; a cell in more
than one log keeps its last row. --videos turns each recorded cell's folder (TARTARUS_COMBOS_CLIPS) into <folder>.mp4: the
player's view beside the world body, the cell and its worst event burned in. --slow plays them slower (0.5 = half speed).
Needs OpenCV (cv2) for --videos.
"""
import argparse
import re
from pathlib import Path

ROW = re.compile(r'^\[Combo\] ROW (.*)$')
EV = re.compile(r'^\[Combo\] EV ([^|]*)\|(.*)$')
SUP = re.compile(r'^\[Combo\] SUPPORT ([^|]*)\|grip_fp=([\d.]+)\|grip_tp=([\d.]+)\|reach=([\d.]+)$')


def parse(logs):
    rows, events = {}, {}
    for log in logs:
        with open(log, encoding='utf-8', errors='replace') as f:
            for line in f:
                line = line.rstrip()
                m = ROW.match(line)
                if m:
                    p = m.group(1).split('|')
                    if len(p) < 9:
                        continue
                    cell, result, score, worst, fp, tp, bounce, states, clip = p[:9]
                    rows[cell] = dict(cell=cell, result=result, score=float(score), worst=worst, fp=int(fp.split('=')[1]),
                                      tp=int(tp.split('=')[1]), bounce=bounce.split('=')[1], states=states, clip=clip)
                    events[cell] = []
                    continue
                m = SUP.match(line)
                if m and m.group(1) in rows:
                    rows[m.group(1)]['support'] = f'{float(m.group(2)):.1f} / {float(m.group(3)):.1f} cm, reach {float(m.group(4)):.2f}'
                    continue
                m = EV.match(line)
                if m and m.group(1) in events:
                    events[m.group(1)].append(m.group(2))
    return rows, events


def order(rows):
    rank = {'SETUP': 0, 'FLAG': 1, 'PASS': 2}
    return sorted(rows.values(), key=lambda r: (rank.get(r['result'], 3), -r['score'], r['cell']))


def write_md(rows, events, out):
    ordered = order(rows)
    n = len(ordered)
    flagged = sum(r['result'] == 'FLAG' for r in ordered)
    setup = sum(r['result'] == 'SETUP' for r in ordered)
    lines = [f'# Weapon combo matrix', '',
             f'{n} cells: {n - flagged - setup} PASS, {flagged} FLAG, {setup} SETUP. Score = the worst event over its meter\'s '
             'threshold (1.0 = just over). FP = first-person meter, TP = third-person (world body) meter. Bounce = the largest '
             'gait cycle\'s hand travel in a held state (mm). Support arm = the left hand\'s largest grip gap FP / TP and its '
             'largest reach (of the arm\'s length) through the action, whatever the thresholds.', '',
             'Scores: a pop, grip gap, settle or bounce = its size over its threshold (FP pop 0.6 cm, grip 1 cm, bounce 15 mm; '
             'TP pop 0.4 cm / 0.5 deg, settle 0.5 cm / 0.6 deg, grip 1.5 cm). A stretch = 1 + % past the arm\'s length / 10 and '
             'an arm in the eye = 1 + how far inside 6 cm it is / 6 (both have no size threshold), so either outranks an event '
             'barely over its line. An FP hand pop, grip gap or stretch while that hand is out of the view (the arms\' FOV, '
             '10% slack) is listed as "off screen" and not scored; the states column counts them.', '',
             '| # | cell | result | score | worst | FP ev | TP ev | bounce mm | support arm | states | clip |',
             '|---|---|---|---|---|---|---|---|---|---|---|']
    for i, r in enumerate(ordered, 1):
        clip = r['clip'] if r['clip'] != '-' else ''
        if clip and Path(clip + '.mp4').exists():
            clip += '.mp4'
        lines.append(f"| {i} | {r['cell']} | {r['result']} | {r['score']:.2f} | {r['worst']} | {r['fp']} | {r['tp']} | "
                     f"{r['bounce']} | {r.get('support', '-')} | {r['states'].replace('|', '/')} | {clip} |")
    lines += ['', '## Events per flagged cell', '']
    for r in ordered:
        if r['result'] != 'FLAG':
            continue
        lines.append(f"**{r['cell']}** ({r['score']:.2f})")
        lines += [f'- `{e}`' for e in events.get(r['cell'], [])]
        lines.append('')
    Path(out).write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return ordered


def make_video(folder, label, worst, slow):
    import cv2
    import numpy as np
    games = sorted(Path(folder).glob('game_*.jpg'))
    if not games:
        return None
    out = Path(str(folder) + '.mp4')
    writer = None
    h = 540
    for g in games:
        a = cv2.imread(str(g))
        b = cv2.imread(str(g).replace('game_', 'scene_'))
        if a is None:
            continue
        if b is None:
            b = np.zeros_like(a)
        a = cv2.resize(a, (int(a.shape[1] * h / a.shape[0]), h))
        b = cv2.resize(b, (int(b.shape[1] * h / b.shape[0]), h))
        frame = np.hstack([a, b])
        bar = np.zeros((70, frame.shape[1], 3), np.uint8)
        cv2.putText(bar, label, (12, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (255, 255, 255), 2, cv2.LINE_AA)
        cv2.putText(bar, worst, (12, 60), cv2.FONT_HERSHEY_SIMPLEX, 0.65, (120, 200, 255), 1, cv2.LINE_AA)
        cv2.putText(frame, 'FP (player view)', (12, h - 14), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2, cv2.LINE_AA)
        cv2.putText(frame, 'TP (world body)', (a.shape[1] + 12, h - 14), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2, cv2.LINE_AA)
        frame = np.vstack([bar, frame])
        if writer is None:
            writer = cv2.VideoWriter(str(out), cv2.VideoWriter_fourcc(*'mp4v'), 60.0 * slow, (frame.shape[1], frame.shape[0]))
        writer.write(frame)
    if writer:
        writer.release()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--out', required=True)
    ap.add_argument('--videos', action='store_true')
    ap.add_argument('--slow', type=float, default=1.0)
    a = ap.parse_args()
    rows, events = parse(a.logs)
    if a.videos:
        for r in rows.values():
            if r['clip'] != '-' and Path(r['clip']).is_dir():
                speed = '' if a.slow == 1.0 else f'  ({a.slow:g}x speed)'
                v = make_video(r['clip'], r['cell'].replace('/', '  ') + speed, f"{r['result']} {r['score']:.2f}: {r['worst']}", a.slow)
                print(f'{r["cell"]}: {v}')
    ordered = write_md(rows, events, a.out)
    print(f'{a.out}: {len(ordered)} cells, {sum(r["result"] == "FLAG" for r in ordered)} flagged')


if __name__ == '__main__':
    main()
