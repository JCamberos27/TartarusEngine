# Fitting things into the Forest house (forest_house.py) by their real shapes: every model's bounds as the engine
# measures them (forest_house_bounds.json, from --scene-bounds), oriented boxes in the pack's space, and the tests a
# placement has to pass - not through a wall, not into another piece, not in a doorway or a door's swing, resting on
# what is under it rather than floating or sunk.
#
# Regenerate the bounds after the pack or the clutter prefabs change:
#   python tools/scenes/house_fit.py <repo> --measure      (needs a built exe in <repo>/build/Release or <repo>/b/Release)
import json
import math
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BOUNDS_FILE = os.path.join(HERE, 'forest_house_bounds.json')


# --- quaternions ([x, y, z, w]) ---------------------------------------------------------------------------------
def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]


def qrot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return [vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx)]


def qinv(q):
    return [-q[0], -q[1], -q[2], q[3]]


def axis_q(axis, deg):
    h = math.radians(deg) / 2
    s = math.sin(h)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h)]


# --- oriented boxes -----------------------------------------------------------------------------------------------
class Box:
    """An oriented box: centre, three unit axes, half extents."""
    __slots__ = ('c', 'ax', 'h', 'tag')

    def __init__(self, c, ax, h, tag=None):
        self.c, self.ax, self.h, self.tag = c, ax, h, tag

    @staticmethod
    def of(pos, rot, scale, lo, hi, tag=None):
        """The box a local AABB (lo, hi) makes under position / rotation / scale."""
        sc = [abs(v) for v in scale]
        mid = [(lo[i] + hi[i]) * 0.5 * scale[i] for i in range(3)]
        c = qrot(rot, mid)
        c = [pos[i] + c[i] for i in range(3)]
        ax = [qrot(rot, a) for a in ((1, 0, 0), (0, 1, 0), (0, 0, 1))]
        h = [max((hi[i] - lo[i]) * 0.5 * sc[i], 1e-4) for i in range(3)]
        return Box(c, ax, h, tag)

    def corners(self):
        out = []
        for sx in (-1, 1):
            for sy in (-1, 1):
                for sz in (-1, 1):
                    out.append([self.c[i] + self.ax[0][i] * sx * self.h[0] + self.ax[1][i] * sy * self.h[1] +
                                self.ax[2][i] * sz * self.h[2] for i in range(3)])
        return out

    def ylo(self):
        return self.c[1] - sum(abs(self.ax[k][1]) * self.h[k] for k in range(3))

    def yhi(self):
        return self.c[1] + sum(abs(self.ax[k][1]) * self.h[k] for k in range(3))

    def xz(self):
        """The footprint's corners (convex hull of the projected corners)."""
        return hull([(p[0], p[2]) for p in self.corners()])

    def shrunk(self, m):
        return Box(self.c, self.ax, [max(v - m, 1e-4) for v in self.h], self.tag)


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def overlap(a, b, margin=0.0):
    """Do two oriented boxes overlap by more than `margin` (separating axis test)?"""
    d = [b.c[i] - a.c[i] for i in range(3)]
    axes = list(a.ax) + list(b.ax)
    for u in a.ax:
        for v in b.ax:
            c = _cross(u, v)
            n = math.sqrt(_dot(c, c))
            if n > 1e-6:
                axes.append([c[0] / n, c[1] / n, c[2] / n])
    for L in axes:
        ra = sum(abs(_dot(a.ax[k], L)) * a.h[k] for k in range(3))
        rb = sum(abs(_dot(b.ax[k], L)) * b.h[k] for k in range(3))
        if abs(_dot(d, L)) > ra + rb - margin:
            return False
    return True


def penetration(a, b):
    """How deep two boxes overlap along their least-overlapping axis (0 if apart)."""
    d = [b.c[i] - a.c[i] for i in range(3)]
    axes = list(a.ax) + list(b.ax)
    for u in a.ax:
        for v in b.ax:
            c = _cross(u, v)
            n = math.sqrt(_dot(c, c))
            if n > 1e-6:
                axes.append([c[0] / n, c[1] / n, c[2] / n])
    best = 1e9
    for L in axes:
        ra = sum(abs(_dot(a.ax[k], L)) * a.h[k] for k in range(3))
        rb = sum(abs(_dot(b.ax[k], L)) * b.h[k] for k in range(3))
        best = min(best, ra + rb - abs(_dot(d, L)))
        if best <= 0:
            return 0.0
    return best


# --- 2D convex polygons (plan view, (x, z)) -------------------------------------------------------------------------
def hull(pts):
    pts = sorted(set((round(p[0], 5), round(p[1], 5)) for p in pts))
    if len(pts) < 3:
        return pts

    def cr(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lo, up = [], []
    for p in pts:
        while len(lo) >= 2 and cr(lo[-2], lo[-1], p) <= 0:
            lo.pop()
        lo.append(p)
    for p in reversed(pts):
        while len(up) >= 2 and cr(up[-2], up[-1], p) <= 0:
            up.pop()
        up.append(p)
    return lo[:-1] + up[:-1]


def poly_overlap(p, q):
    """Do two convex polygons overlap (touching does not count)?"""
    for poly in (p, q):
        n = len(poly)
        for i in range(n):
            a, b = poly[i], poly[(i + 1) % n]
            ax, az = -(b[1] - a[1]), b[0] - a[0]
            pa = [ax * v[0] + az * v[1] for v in p]
            pb = [ax * v[0] + az * v[1] for v in q]
            if max(pa) <= min(pb) + 1e-6 or max(pb) <= min(pa) + 1e-6:
                return False
    return True


class Zone:
    """A plan-view keep-out (a doorway and its swing) over a height band."""
    def __init__(self, poly, y0, y1, name):
        self.poly, self.y0, self.y1, self.name = poly, y0, y1, name

    def hits(self, box):
        if box.yhi() < self.y0 or box.ylo() > self.y1:
            return False
        return poly_overlap(self.poly, box.xz())


# --- the measured bounds -------------------------------------------------------------------------------------------
_BOUNDS = None


def bounds():
    """model or prefab path -> (lo, hi) in its own space."""
    global _BOUNDS
    if _BOUNDS is None:
        with open(BOUNDS_FILE, encoding='utf-8') as f:
            _BOUNDS = {k: (v[0], v[1]) for k, v in json.load(f).items()}
    return _BOUNDS


def rest_lift(lo, hi, rot, scale=(1.0, 1.0, 1.0)):
    """Where the pivot goes above the surface for a piece at `rot` to sit on it: minus its lowest point."""
    b = Box.of((0.0, 0.0, 0.0), rot, scale, lo, hi)
    return -b.ylo()


# Prefab folders from outside the house pack that the house is dressed with (the store room, the squatter's corner).
EXTRA_PREFAB_DIRS = ['assets/Environments/ModularCityAlleyPack/Prefabs/Prefabs_Props', 'assets/Furniture/Bedroom/Models']
EXTRA_PREFABS = ('Bed_Mattress.prefab', 'Bed_Pillow_A.prefab')     # of those, the ones it uses


def measure(repo, exe=None):
    """Run the engine's --scene-bounds over every model in the house pack and every prefab the house dresses with."""
    exe = exe or next((p for p in (os.path.join(repo, 'build', 'Release', 'TartarusEngine.exe'),
                                   os.path.join(repo, 'b', 'Release', 'TartarusEngine.exe')) if os.path.exists(p)), None)
    if not exe:
        sys.exit('house_fit: no built exe')
    with open(os.path.join(HERE, 'forest_house_pack.json'), encoding='utf-8') as f:
        ents = json.load(f)['entities']
    paths = {}
    for e in ents:
        if e['kind'] == 'models' and e.get('path'):
            paths.setdefault(e['path'], e.get('pathGuid'))
    tmp = tempfile.mkdtemp(prefix='house_fit_')
    models = []
    for i, (p, g) in enumerate(sorted(paths.items())):
        m = {'id': i + 1, 'order': i + 1, 'parentId': -1, 'name': f'm{i}', 'path': p,
             'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}
        if g:
            m['pathGuid'] = g
        models.append(m)
    scene = os.path.join(tmp, 'house_models.json')
    with open(scene, 'w', encoding='utf-8') as f:
        json.dump({'formatVersion': 4, 'boxes': [], 'empties': [], 'models': models}, f)
    pack = 'assets/Environments/SuburbNeighborhoodHousePack/Prefabs/'
    folders = [pack + d for d in ('Props_interior', 'Props_exterior', 'Ground')] + EXTRA_PREFAB_DIRS
    dirs = [os.path.join(repo, 'project', d) for d in folders]
    out = os.path.join(tmp, 'out')
    env = dict(os.environ, TARTARUS_HIDDEN='1', TARTARUS_MUTE='1')
    r = subprocess.run([exe, '--project', os.path.join(repo, 'project'), '--scene-bounds', scene] + dirs + ['--out', out],
                       env=env, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=1800)
    if r.returncode:
        sys.exit('house_fit: --scene-bounds failed:\n' + r.stdout[-3000:] + r.stderr[-2000:])
    result = {}
    with open(os.path.join(out, 'scenes', 'house_models.json'), encoding='utf-8') as f:
        for e in json.load(f)['entities']:
            if e.get('min'):
                i = int(e['path'].split('/')[-1][1:])
                result[models[i]['path']] = [[round(v, 4) for v in e['min']], [round(v, 4) for v in e['max']]]
    for d in folders:
        with open(os.path.join(out, 'prefabs', os.path.basename(d) + '.json'), encoding='utf-8') as f:
            data = json.load(f)
        for name, pre in (data.get('prefabs') or {}).items():
            u = pre.get('union') or {}
            if u.get('min') and (d not in EXTRA_PREFAB_DIRS or os.path.basename(name) in EXTRA_PREFABS):
                key = d + '/' + os.path.basename(name)
                result[key] = [[round(v, 4) for v in u['min']], [round(v, 4) for v in u['max']]]
    with open(BOUNDS_FILE, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(dict(sorted(result.items())), f, indent=0, separators=(',', ':'))
        f.write('\n')
    print(f'house_fit: {len(result)} bounds -> {BOUNDS_FILE}')


if __name__ == '__main__':
    if '--measure' in sys.argv:
        measure(os.path.abspath(sys.argv[1]))
