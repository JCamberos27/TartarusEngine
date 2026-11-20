"""Turns an NPC_TEST_RECORD folder (game_NNNNN.jpg + scene_NNNNN.jpg) into one MP4: the two views side by side.

    python tools/mm/frames_to_video.py <record dir> <out.mp4> --fps 60|30 [--scale 0.5]

--fps is the rate the folder was recorded at: 60 for the scenarios NpcTest.cpp's kScenarios marks EveryFrame (mmview,
mmdb, flame, labloco, testbed), 30 for the rest (every other frame of the fixed 60 Hz). A wrong one plays at 2x or 0.5x.

For the owner's sign-off on `--npc-test mmview` (docs/MOTION_MATCHING_HANDOFF.md, Phase 2). Needs OpenCV (cv2); no ffmpeg.
"""
import argparse
import sys
from pathlib import Path

import cv2
import numpy as np


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('record')
    ap.add_argument('out')
    ap.add_argument('--fps', type=float, required=True, help='60: an EveryFrame scenario; 30: the rest')
    ap.add_argument('--scale', type=float, default=0.5)
    a = ap.parse_args()
    rec = Path(a.record)
    games = sorted(rec.glob('game_*.jpg'))
    if not games:
        print(f'frames_to_video: no game_*.jpg in {rec}')
        return 2
    writer = None
    skipped = 0
    for g in games:
        s = rec / g.name.replace('game_', 'scene_')
        left = cv2.imread(str(g))
        if left is None:  # a frame cut short (the run killed mid-write): leave it out
            skipped += 1
            continue
        right = cv2.imread(str(s)) if s.exists() else None
        if right is None:
            right = np.zeros_like(left)
        if right.shape != left.shape:
            right = cv2.resize(right, (left.shape[1], left.shape[0]))
        frame = np.hstack([left, right])
        if a.scale != 1.0:
            frame = cv2.resize(frame, (int(frame.shape[1] * a.scale) // 2 * 2, int(frame.shape[0] * a.scale) // 2 * 2),
                               interpolation=cv2.INTER_AREA)
        if writer is None:
            writer = cv2.VideoWriter(a.out, cv2.VideoWriter_fourcc(*'mp4v'), a.fps, (frame.shape[1], frame.shape[0]))
        writer.write(frame)
    if writer is None:
        print(f'frames_to_video: none of the {len(games)} game_*.jpg in {rec} could be read')
        return 2
    writer.release()
    print(f'frames_to_video: {len(games) - skipped} frames -> {a.out}' + (f' ({skipped} unreadable, left out)' if skipped else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
