# The Forest's night trailer: a 60 fps fly-through recorded in the engine (TARTARUS_CINEMATIC, src/Tests/Cinematic.h).
# The forest canopy and sky, the trail through the fog, the walk up to the house, then room by room as each room's
# light comes on, and a crane up and away with the whole house lit.
#
#   python tools/scenes/forest_trailer.py <out dir> [--size 1920x1080] [--preview]
# ffmpeg: TARTARUS_FFMPEG, else the imageio-ffmpeg package's. --preview renders each shot's keys as stills instead.
import json
import math
import os
import sys

import house_shots as hs


def look(cam, t):
    d = [t[i] - cam[i] for i in range(3)]
    h = math.hypot(d[0], d[2]) or 1e-6
    return math.degrees(math.atan2(d[2], d[0])), math.degrees(math.atan2(d[1], h))


E = 1.65
# (name, seconds, fov, [(camera, look-at), ...] spread evenly over the shot, [(seconds in, circuit, on), ...]);
# each shot starts with a cut. World metres; the house's front door is at about (1, 0, 24).
SHOTS = [
    ('sky', 9.0, 55, [
        ([4, 14, -40], [4, 22, -10]),
        ([4, 12, -38], [4, 12, 0]),
        ([2, 6, -37], [0, 2, -20]),
    ], []),
    ('trail', 11.0, 50, [
        ([-1.0, 1.5, -36], [1.5, 1.4, -24]),
        ([1.0, 1.5, -26], [-1.0, 1.4, -14]),
        ([-2.0, 1.5, -15], [0.0, 1.5, -4]),
        ([-0.5, 1.6, -5], [0.5, 2.2, 10]),
    ], []),
    ('approach', 9.0, 45, [
        ([0.5, 1.7, 3], [1.0, 3.0, 26]),
        ([1.0, 1.7, 10], [1.0, 2.8, 26]),
        ([1.0, 1.7, 16], [1.0, 2.4, 26]),
    ], []),
    ('hall', 6.0, 60, [
        ([3.0, 1.85, 25.4], [-5, 1.5, 26.8]),
        ([1.4, 1.85, 25.8], [-6, 1.5, 26.8]),
    ], [(1.0, 'Hall', True)]),
    ('living', 6.0, 60, [
        ([9.2, 1.85, 25.0], [13.5, 1.2, 30.0]),
        ([9.7, 1.85, 25.9], [13.8, 1.2, 30.8]),
    ], [(1.0, 'Living room', True)]),
    ('kitchen', 6.0, 60, [
        ([-7.5, 1.85, 25.0], [-12, 1.3, 29.5]),
        ([-8.3, 1.85, 25.6], [-12.8, 1.3, 30.2]),
    ], [(1.0, 'Kitchen', True)]),
    ('stairs', 6.0, 60, [
        ([6.8, 4.75, 29.6], [1.0, 1.4, 30.2]),
        ([6.2, 4.75, 29.0], [1.0, 1.0, 29.4]),
    ], [(1.0, 'Stairs', True)]),
    ('master', 6.0, 60, [
        ([-7.0, 4.75, 24.9], [-12, 4.0, 27.0]),
        ([-7.4, 4.75, 25.0], [-12, 4.0, 26.8]),
    ], []),
    ('child', 6.0, 60, [
        ([9.3, 4.7, 25.2], [13, 4.0, 27.5]),
        ([10.0, 4.7, 25.6], [13.5, 4.0, 28.5]),
    ], [(1.0, "Child's bedroom", True)]),
    ('outro', 12.0, 50, [
        ([1.0, 1.7, 13], [1.0, 3.0, 27]),
        ([1.0, 3.0, 4], [1.0, 3.5, 27]),
        ([1.0, 8.0, -6], [1.0, 4.0, 27]),
        ([1.0, 16.0, -14], [1.0, 8.0, 30]),
    ], [(0.5, '*', True)]),
]


def build(ffmpeg, out='trailer.mp4', warmup=300):
    keys, lights, t = [], [], 0.0
    for name, dur, fov, pts, cues in SHOTS:
        n = len(pts)
        for k, (p, tg) in enumerate(pts):
            yaw, pitch = look(p, tg)
            keys.append({'t': round(t + dur * k / (n - 1), 3), 'pos': p, 'yaw': round(yaw, 2), 'pitch': round(pitch, 2),
                         'fov': fov, **({'cut': True} if k == 0 and keys else {})})
        lights += [{'t': round(t + off, 3), 'circuit': c, 'on': on} for off, c, on in cues]
        t += dur
        keys[-1]['t'] = round(t - 1 / 60, 3)    # the shot's last frame; the next frame is the cut
    return {'out': out, 'ffmpeg': ffmpeg, 'warmup': warmup, 'crf': 14, 'keys': keys, 'lights': lights}


def main():
    a = sys.argv[1:]
    out = os.path.abspath(a[0])
    size = a[a.index('--size') + 1] if '--size' in a else '1920x1080'
    os.makedirs(out, exist_ok=True)
    os.environ['TARTARUS_SHOT_SIZE'] = size
    if '--preview' in a:
        os.environ['TARTARUS_SHOT_FOV'] = '50'
        cams = [(f'{n}_{k}', p, *look(p, tg)) for n, _, _, pts, _ in SHOTS for k, (p, tg) in enumerate(pts)]
        print('engine exit', hs.run_engine(cams, out, True, 'Forest.json'))
        return
    ffmpeg = os.environ.get('TARTARUS_FFMPEG')
    if not ffmpeg:
        import imageio_ffmpeg
        ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    path = os.path.join(out, 'trailer.json')
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(build(ffmpeg), f, indent=1)
    os.environ['TARTARUS_CINEMATIC'] = path
    print('engine exit', hs.run_engine([], out, None, 'Forest.json'), '->', os.path.join(out, 'trailer.mp4'))


if __name__ == '__main__':
    main()
