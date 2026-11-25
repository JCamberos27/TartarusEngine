#!/usr/bin/env python3
"""GI Phase 0 audit: an offline, read-only look at a scene's GI Volumes and its .gi bake.

Run with `python -I tools/gi/gi_audit.py --out <dir>` (CPU only, no engine, no GPU). It reads the scene JSON and the
.gi file(s) its GI Volumes name, and optionally a git revision of the scene to diff against. It writes REPORT.md and
CSVs into --out and nothing anywhere else.

Checks (docs: the GI Volume component, src/Renderer/GIData.h, GIVolumes.h, GIBaker.cpp, LightGather.cpp):
  * volumes   - every GI Volume's box, grid, spacing, fade and circuits; does the .gi still match (grid, box, layers)?
  * invalid   - per volume, the share of probes that look neighbour-filled (GI::FillInvalid), flagged above 20%
  * coverage  - the house's walkable floor (box floor colliders) sampled every 0.25 m at floor and eye height:
                every point inside no volume (it falls back to reflection-probe or sky ambient), grouped into areas
  * doorways  - where two volumes meet or overlap, the gap / overlap and the blend actually in effect
  * shadows   - per circuit, shadow-casting point and spot lamps against the scene's shadow budget, and which of them
                the bake shadows from each volume's camera (cause #4)
  * changes   - lights, circuits, volumes and geometry inside volumes changed since a git revision (the last bake)
  * margins   - (--check-margins) every place two volumes meet: across a wall, no weight leaks through it and the wall's
                own surfaces are fully covered; in a doorway or a seam inside one room, wSum never dips below 1. Also
                each volume's face probes that sit in open air no other volume shares (outdoors or an uncovered space).
                The exit status is 1 when a margin check fails.

The math mirrors the C++ (Box::FromTransform / Fade, GridDims, Deserialize, LightGather's slot score) so the numbers
match what the engine does; the pure parts are unit tested in test_gi_audit.py.
"""
from __future__ import annotations

import argparse
import collections
import csv
import datetime
import itertools
import json
import math
import os
import re
import struct
import subprocess
import sys

INVALID_FLAG_SHARE = 0.20
MAX_LAYERS = 4  # GIVolumes::kMaxLayers: layers past this are dropped at runtime
FLOOR_GROUPS = ("Floors_1st", "Floors_2nd", "Floors_basement", "Stairs")
WALL_GROUPS = ("Walls",)
DOOR_GROUPS = ("Doors",)

# ----------------------------------------------------------------------------------------------------------------
# Vector / matrix helpers (4x4 affine matrices as row-major nested lists; columns are the basis vectors)


def v_add(a, b):
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]


def v_sub(a, b):
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


def v_scale(a, s):
    return [a[0] * s, a[1] * s, a[2] * s]


def v_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def v_len(a):
    return math.sqrt(v_dot(a, a))


def quat_to_mat3(q):
    """Scene JSON quaternion [x, y, z, w] (JsonToQuat) -> 3x3 rotation, row-major. Normalised; zero -> identity."""
    x, y, z, w = (float(c) for c in q)
    n = math.sqrt(x * x + y * y + z * z + w * w)
    if n < 1e-12:
        return [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
    x, y, z, w = x / n, y / n, z / n, w / n
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]


def compose(pos, quat, scale):
    """Position * Rotation * Scale (World.h ComposeTransform)."""
    r = quat_to_mat3(quat)
    return [
        [r[0][0] * scale[0], r[0][1] * scale[1], r[0][2] * scale[2], float(pos[0])],
        [r[1][0] * scale[0], r[1][1] * scale[1], r[1][2] * scale[2], float(pos[1])],
        [r[2][0] * scale[0], r[2][1] * scale[1], r[2][2] * scale[2], float(pos[2])],
        [0.0, 0.0, 0.0, 1.0],
    ]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def mat_point(m, p):
    return [m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] for i in range(3)]


def translate(t):
    return [[1.0, 0.0, 0.0, t[0]], [0.0, 1.0, 0.0, t[1]], [0.0, 0.0, 1.0, t[2]], [0.0, 0.0, 0.0, 1.0]]


IDENTITY = translate((0.0, 0.0, 0.0))


class Box:
    """An oriented box, as GI::Box: centre, unit axes, half extents (world metres)."""

    def __init__(self, center, axes, half):
        self.center = list(center)
        self.axes = [list(a) for a in axes]
        self.half = list(half)

    @staticmethod
    def from_transform(world, size):
        """GI::Box::FromTransform: the transform's columns give the axes, their lengths scale the full Size."""
        center = [world[0][3], world[1][3], world[2][3]]
        axes, half = [], []
        for i in range(3):
            col = [world[0][i], world[1][i], world[2][i]]
            ln = v_len(col)
            axes.append(v_scale(col, 1.0 / ln) if ln > 1e-8 else [float(i == 0), float(i == 1), float(i == 2)])
            half.append(abs(size[i]) * ln * 0.5)
        return Box(center, axes, half)

    def to_local(self, p):
        d = v_sub(p, self.center)
        return [v_dot(self.axes[0], d), v_dot(self.axes[1], d), v_dot(self.axes[2], d)]

    def to_world(self, q):
        out = list(self.center)
        for i in range(3):
            out = v_add(out, v_scale(self.axes[i], q[i]))
        return out

    def inside_margin(self, p):
        """Metres p is inside the box (negative: outside along the worst axis)."""
        q = self.to_local(p)
        return min(self.half[i] - abs(q[i]) for i in range(3))

    def fade(self, p, fade):
        """GI::Box::Fade / the shader's per-volume weight: 1 well inside, to 0 over `fade` metres at the faces."""
        m = self.inside_margin(p)
        if fade <= 0.0:
            return 1.0 if m >= 0.0 else 0.0
        return min(max(m / fade, 0.0), 1.0)

    def fade_faces(self, p, lo, hi):
        """GI::Box::FadeFaces: a fade per face, `lo` for the -x/-y/-z faces, `hi` for +x/+y/+z (box-local axes), 0 = hard."""
        q = self.to_local(p)
        f = 1.0
        for i in range(3):
            d = self.half[i] - abs(q[i])
            fd = lo[i] if q[i] < 0.0 else hi[i]
            f = min(f, (1.0 if d >= 0.0 else 0.0) if fd <= 0.0 else min(max(d / fd, 0.0), 1.0))
        return f

    def contains(self, p, pad=0.0):
        return self.inside_margin(p) >= -pad

    def aabb(self):
        ext = [sum(abs(self.axes[a][i]) * self.half[a] for a in range(3)) for i in range(3)]
        return [self.center[i] - ext[i] for i in range(3)], [self.center[i] + ext[i] for i in range(3)]

    def matches(self, other, pos_tol=1e-3, axis_tol=1e-3):
        if any(abs(self.center[i] - other.center[i]) > pos_tol for i in range(3)):
            return False
        if any(abs(self.half[i] - other.half[i]) > pos_tol for i in range(3)):
            return False
        return all(abs(self.axes[a][i] - other.axes[a][i]) <= axis_tol for a in range(3) for i in range(3))


def grid_dims(half, spacing):
    """GI::GridDims: probes per axis, one on each face and about `spacing` apart, clamped to [2, 256]."""
    s = max(spacing, 0.05)
    return tuple(min(max(int(math.ceil(2.0 * half[i] / s - 1e-4)) + 1, 2), 256) for i in range(3))


def probe_index(i, j, k, dims):
    return i + dims[0] * (j + dims[1] * k)


# ----------------------------------------------------------------------------------------------------------------
# .gi files (TGI1, GIData.cpp Serialize / Deserialize)

GI_MAGIC = b"TGI1"


def parse_gi(data):
    """Bytes of a .gi file -> list of volume dicts {name, box, dims, layers: [(circuit, probes)]}. A probe is a tuple of
    12 floats (R a,bx,by,bz, G ..., B ...). Raises ValueError on a malformed file, like Deserialize."""
    off = 0

    def take(fmt):
        nonlocal off
        size = struct.calcsize(fmt)
        if off + size > len(data):
            raise ValueError("truncated")
        vals = struct.unpack_from(fmt, data, off)
        off += size
        return vals

    def take_str():
        (n,) = take("<I")
        if n > 4096 or off + n > len(data):
            raise ValueError("bad string")
        s = data[off:off + n].decode("utf-8", errors="replace")
        return s, n

    if data[:4] != GI_MAGIC:
        raise ValueError("not a .gi file")
    off = 4
    (version,) = take("<I")
    if version != 1:
        raise ValueError("unsupported .gi version %d" % version)
    (count,) = take("<I")
    if count > 4096:
        raise ValueError("bad volume count")
    vols = []
    for _ in range(count):
        name, n = take_str()
        off += n
        center = take("<3f")
        axes = [take("<3f") for _ in range(3)]
        half = take("<3f")
        dims = take("<3i")
        if any(d < 2 or d > 256 for d in dims):
            raise ValueError("bad grid size")
        (nl,) = take("<I")
        if nl > 64:
            raise ValueError("bad layer count")
        np_ = dims[0] * dims[1] * dims[2]
        layers = []
        for _ in range(nl):
            circuit, n = take_str()
            off += n
            flat = take("<%de" % (np_ * 12))
            layers.append((circuit, [tuple(flat[p * 12:(p + 1) * 12]) for p in range(np_)]))
        vols.append({"name": name, "box": Box(center, axes, half), "dims": tuple(dims), "layers": layers})
    return vols


def serialize_gi(vols):
    """The inverse of parse_gi (tests build small files with it)."""
    out = bytearray(GI_MAGIC)
    out += struct.pack("<II", 1, len(vols))
    for v in vols:
        nb = v["name"].encode("utf-8")
        out += struct.pack("<I", len(nb)) + nb
        b = v["box"]
        out += struct.pack("<3f", *b.center)
        for a in b.axes:
            out += struct.pack("<3f", *a)
        out += struct.pack("<3f", *b.half)
        out += struct.pack("<3i", *v["dims"])
        out += struct.pack("<I", len(v["layers"]))
        for circuit, probes in v["layers"]:
            cb = circuit.encode("utf-8")
            out += struct.pack("<I", len(cb)) + cb
            for p in probes:
                out += struct.pack("<12e", *p)
    return bytes(out)


def split_circuits(s):
    """GI::SplitCircuits: ';' or ',' separated, trimmed, empties and repeats dropped, order kept."""
    out = []
    for part in re.split(r"[;,]", s or ""):
        t = part.strip(" \t")
        if t and t not in out:
            out.append(t)
    return out


# ----------------------------------------------------------------------------------------------------------------
# Invalid (neighbour-filled) probes. The .gi doesn't store validity: GI::FillInvalid replaced every probe inside a wall
# or furniture with the plain average of its already-done 6-neighbours (valid ones, or ones filled in an earlier ring).
# The valid mask is geometric, so the SAME neighbour subset averages to the probe in every layer. A probe counts as
# filled when some subset reproduces it in all 12 coefficients of all layers to within half-float rounding, and the
# subset isn't simply copies of it (uniform regions, e.g. an all-black layer, are ambiguous and counted apart).

NEIGHBOURS = ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1))


# Black floor: a probe whose every coefficient (all layers) is under this, with all its neighbours under it too, is
# 'uniform', never 'filled'. In an unlit room (SH ~1e-5) any neighbour average lands within _close's absolute term of
# the probe, so the subset test can't tell a real probe from a fill there (the storeroom and box room read 98-100%
# "filled" on final-1 while the baker logged 2% / 0% inner invalid). Absolute, not relative to the house: 1e-4 is
# ~1/100 of the darkest lit room's DC term and well under the 0.002 luma bar, so no lit probe is affected.
BLACK_FLOOR = 1e-4


def _is_black(probes, idx):
    return all(abs(x) < BLACK_FLOOR for l in range(len(probes)) for x in probes[l][idx])


def _close(a, b):
    return abs(a - b) <= abs(b) * 2.0 ** -9 + 1e-6


BAKE_LOG_LINE = re.compile(r"\[GI bake\] (GI .+?): (\d+) probes, (\d+) inside geometry")


def parse_bake_log(text):
    """--bake-gi stdout -> {volume: (probes, invalid)} from the baker's '[GI bake] <volume>: N probes, M inside geometry
    (filled)' lines. The baker knows exactly which probes it filled; the classifier only guesses from the .gi."""
    return {m.group(1): (int(m.group(2)), int(m.group(3))) for m in BAKE_LOG_LINE.finditer(text)}


def bake_inner_invalid(probes, invalid, dims):
    """-> (shell, inner, inner_invalid) for one volume from the baker's counts (Atlas's method, house-polish atlas.md
    gap 2). The grid's outer shell sits in the walls by design, so every shell probe is taken as invalid and the rest
    of the count as inner: a lower bound on the inner probes the room's air samples."""
    inner = max(0, dims[0] - 2) * max(0, dims[1] - 2) * max(0, dims[2] - 2)
    shell = probes - inner
    return shell, inner, max(0, invalid - shell)


def classify_probes(layers, dims):
    """-> list per probe: 'filled', 'uniform' (indistinguishable from its neighbours) or 'valid'."""
    n = dims[0] * dims[1] * dims[2]
    if not layers:
        return ["valid"] * n
    probes = [layer[1] for layer in layers]
    out = []
    for k in range(dims[2]):
        for j in range(dims[1]):
            for i in range(dims[0]):
                idx = probe_index(i, j, k, dims)
                nbs = []
                for dx, dy, dz in NEIGHBOURS:
                    a, b, c = i + dx, j + dy, k + dz
                    if 0 <= a < dims[0] and 0 <= b < dims[1] and 0 <= c < dims[2]:
                        nbs.append(probe_index(a, b, c, dims))
                out.append(_classify_one(probes, idx, nbs))
    # Copies: a group of bit-identical neighbours has one source. If any member is an averaged fill, the group was
    # copied out from it (all filled); otherwise the member nearest the box centre is taken as the real probe.
    seen = set()
    mid = [(d - 1) * 0.5 for d in dims]
    for start in range(n):
        if out[start] != "copy" or start in seen:
            continue
        comp, stack = [], [start]
        seen.add(start)
        while stack:
            c = stack.pop()
            comp.append(c)
            i, j, k = c % dims[0], (c // dims[0]) % dims[1], c // (dims[0] * dims[1])
            for dx, dy, dz in NEIGHBOURS:
                a, b, cc = i + dx, j + dy, k + dz
                if 0 <= a < dims[0] and 0 <= b < dims[1] and 0 <= cc < dims[2]:
                    m = probe_index(a, b, cc, dims)
                    if m not in seen and all(probes[l][m] == probes[l][c] for l in range(len(probes))):
                        seen.add(m)
                        stack.append(m)
        if any(out[c] == "filled" for c in comp):
            keep = None
        else:
            def dist(c):
                i, j, k = c % dims[0], (c // dims[0]) % dims[1], c // (dims[0] * dims[1])
                return (i - mid[0]) ** 2 + (j - mid[1]) ** 2 + (k - mid[2]) ** 2
            keep = min(comp, key=dist)
        for c in comp:
            out[c] = "valid" if c == keep else "filled"
    return out


def _classify_one(probes, idx, nbs):
    # An exact copy of a neighbour in every layer is a fill from one done neighbour (a face probe in a wall copies the
    # one inside it) unless the probe is all black, where copies are just as likely to be real.
    dark = _is_black(probes, idx)
    if dark and all(_is_black(probes, j) for j in nbs):
        return "uniform"  # below the black floor: can't be told from a fill (see BLACK_FLOOR)
    same =[all(probes[l][j] == probes[l][idx] for l in range(len(probes))) for j in nbs]
    # Quick scalar screen on the layer-0 red DC term, then the full check.
    target = probes[0][idx][0]
    found_uniform = False
    for r in range(1, len(nbs) + 1):
        for sub in itertools.combinations(range(len(nbs)), r):
            if not _close(target, sum(probes[0][nbs[s]][0] for s in sub) / r):
                continue
            ok = True
            for l in range(len(probes)):
                p = probes[l][idx]
                for c in range(12):
                    if not _close(p[c], sum(probes[l][nbs[s]][c] for s in sub) / r):
                        ok = False
                        break
                if not ok:
                    break
            if not ok:
                continue
            if all(same[s] for s in sub):
                found_uniform = True
                continue
            return "filled"
    if not found_uniform:
        return "valid"
    return "uniform" if dark else "copy"  # copies are resolved per group in classify_probes


# ----------------------------------------------------------------------------------------------------------------
# Scene


class Scene:
    def __init__(self, doc):
        self.doc = doc
        self.ents = []
        for key in ("empties", "models", "boxes"):
            for e in doc.get(key, []):
                e = dict(e)
                e["_kind"] = key
                self.ents.append(e)
        self.by_id = {e["id"]: e for e in self.ents if "id" in e}
        self._world = {}
        self.max_point_shadows = int(doc.get("maxPointShadows", 2))
        self.max_spot_shadows = int(doc.get("maxSpotShadows", 4))

    def parent(self, e):
        return self.by_id.get(e.get("parentId", -1))

    def ancestors(self, e, cap=256):
        out = []
        p = self.parent(e)
        while p is not None and len(out) < cap:
            out.append(p)
            p = self.parent(p)
        return out

    def ancestor_names(self, e):
        return [a.get("name", "") for a in self.ancestors(e)]

    def active(self, e):
        return e.get("active", True) is not False and all(a.get("active", True) is not False for a in self.ancestors(e))

    def world(self, e):
        eid = e.get("id")
        if eid in self._world:
            return self._world[eid]
        local = compose(e.get("position", (0, 0, 0)), e.get("rotation", (0, 0, 0, 1)), e.get("scale", (1, 1, 1)))
        p = self.parent(e)
        w = mat_mul(self.world(p), local) if p is not None and p is not e else local
        self._world[eid] = w
        return w

    def position(self, e):
        w = self.world(e)
        return [w[0][3], w[1][3], w[2][3]]

    def box_collider(self, e):
        """A non-trigger Box collider as a world OBB, or None."""
        c = e.get("collider")
        if not c or c.get("isTrigger") or c.get("shape", 0) != 0 or "halfExtents" not in c:
            return None
        he = c["halfExtents"]
        if min(he) <= 0:
            return None
        return Box.from_transform(mat_mul(self.world(e), translate(c.get("center", (0, 0, 0)))), [2 * h for h in he])

    def volumes(self):
        out = []
        for e in self.ents:
            g = e.get("GI Volume")
            if g is None:
                continue
            box = Box.from_transform(self.world(e), g.get("Size", (4, 3, 4)))
            spacing = float(g.get("Spacing", 0.5))
            out.append({
                "id": e["id"], "name": e.get("name", ""), "order": e.get("order", 0), "box": box,
                "spacing": spacing, "fade": float(g.get("Fade", 0.05)), "intensity": float(g.get("Intensity", 1.0)),
                "fade_lo": [float(x) if x > 0 else float(g.get("Fade", 0.05)) for x in g.get("Fade Min", (0, 0, 0))],
                "fade_hi": [float(x) if x > 0 else float(g.get("Fade", 0.05)) for x in g.get("Fade Max", (0, 0, 0))],
                "circuits": split_circuits(g.get("Circuits", "")), "outside": bool(g.get("Outside", False)),
                "bake_file": g.get("Bake File", ""), "dims": grid_dims(box.half, spacing), "active": self.active(e),
            })
        out.sort(key=lambda v: v["order"])
        return out

    def lights(self):
        out = []
        for e in self.ents:
            lc = e.get("Light")
            if lc is None:
                continue
            circ = (e.get("Light Circuit") or {}).get("Circuit")
            color = lc.get("Color", (1, 1, 1))
            name = e.get("name", "")
            par = self.parent(e)
            if par is not None and (name.lower().endswith("light") or name.lower() in ("light", "point light", "spot light")):
                name = "%s/%s" % (par.get("name", ""), name)  # many lamps are "<fitting>/Lamp Light"
            out.append({
                "id": e["id"], "name": name, "type": lc.get("Type", "Point"), "pos": self.position(e),
                "circuit": circ, "shadow": bool(lc.get("Cast Shadows", False)),
                "intensity": float(lc.get("Intensity", 1.0)), "range": float(lc.get("Range", 10.0)),
                "color": list(color), "active": self.active(e),
                "lit": float(lc.get("Intensity", 1.0)) > 0 and v_dot(color, color) > 0,
            })
        return out

    def switches(self):
        return [{"id": e["id"], "name": e.get("name", ""), "circuit": e["Light Switch"].get("Circuit", ""),
                 "start_on": bool(e["Light Switch"].get("Start On", True)), "pos": self.position(e)}
                for e in self.ents if "Light Switch" in e]

    def descendants_of_group(self, names):
        return [e for e in self.ents if any(n in names for n in self.ancestor_names(e))]

    def eye_height(self, default=1.7):
        for e in self.ents:
            fpc = e.get("First Person Controller")
            if fpc and "Eye Height" in fpc:
                return float(fpc["Eye Height"])
        return default


# ----------------------------------------------------------------------------------------------------------------
# Shadow slots (LightGather::AssignShadowSlots, the camera-relative score)


def distance_fade(rng, dist, tan_half_fov):
    """LightBuffer::DistanceFade."""
    if dist <= rng or rng <= 0.0:
        return 1.0
    px = rng / dist * (540.0 / max(tan_half_fov, 1e-3))
    t = min(max((px - 10.0) / (24.0 - 10.0), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def shadow_score(light, cam):
    rng = max(light["range"], 0.2)
    gap = max(v_len(v_sub(light["pos"], cam)) - rng, 0.0)
    return light["intensity"] * rng * rng / (gap * gap + 1.0)


def pick_shadowed(lights, cam, budget, prev=(), tan_half_fov=1.0):
    """The `budget` best-scoring lights from camera `cam` (stable sort, last frame's holders x1.25). -> list of ids."""
    cands = []
    for l in lights:
        d = v_len(v_sub(l["pos"], cam))
        if distance_fade(l["range"], d, tan_half_fov) <= 0.0:
            continue
        s = shadow_score(l, cam)
        if l["id"] in prev:
            s *= 1.25
        cands.append((s, l["id"]))
    cands.sort(key=lambda t: -t[0])  # Python's sort is stable, like std::stable_sort
    return [i for _, i in cands[:max(budget, 0)]]


def shadow_candidates(lights):
    """The lights LightGather scores at all: point/spot, Cast Shadows, lit, active."""
    return [l for l in lights if l["type"] in ("Point", "Spot") and l["shadow"] and l["lit"] and l["active"]]


def reaches(light, box):
    """Does the light's range sphere touch the box? (A lamp that can't reach a volume can't change its bake.)"""
    return -box.inside_margin(light["pos"]) < light["range"]


# ----------------------------------------------------------------------------------------------------------------
# Coverage


def sample_top_face(box, step):
    """World points on the face of `box` that points most upward, every `step` metres (cell centres)."""
    best, best_up, sign = 0, -2.0, 1.0
    for a in range(3):
        for s in (1.0, -1.0):
            up = box.axes[a][1] * s
            if up > best_up:
                best, best_up, sign = a, up, s
    u, v = [a for a in range(3) if a != best]
    nu = max(int(math.floor(2 * box.half[u] / step)), 1)
    nv = max(int(math.floor(2 * box.half[v] / step)), 1)
    du, dv = 2 * box.half[u] / nu, 2 * box.half[v] / nv
    pts = []
    for iu in range(nu):
        for iv in range(nv):
            q = [0.0, 0.0, 0.0]
            q[best] = sign * box.half[best]
            q[u] = -box.half[u] + (iu + 0.5) * du
            q[v] = -box.half[v] + (iv + 0.5) * dv
            pts.append(box.to_world(q))
    return pts


def volume_fade(v, p):
    """The shader's per-volume weight at p, with the volume's per-face fades (Fade Min / Fade Max) when it has them."""
    if "fade_lo" in v:
        return v["box"].fade_faces(p, v["fade_lo"], v["fade_hi"])
    return v["box"].fade(p, v["fade"])


def coverage_at(p, volumes, normal=None):
    """(sum of the volumes' fades at p, [(name, fade)] for the volumes that reach p): the shader's wSum. With a surface
    `normal`, each volume tests p pushed off the surface by its normal bias (0.25 x Spacing, GIVolumes.cpp), as the
    shader does."""
    hits = []
    for v in volumes:
        q = p if normal is None else v_add(p, v_scale(normal, 0.25 * v.get("spacing", 0.0)))
        hits.append((v["name"], volume_fade(v, q)))
    hits = [h for h in hits if h[1] > 0.0]
    return sum(f for _, f in hits), hits


def cluster_points(keys, step):
    """Connected components (8-neighbour on the x/z grid, same level) of grid keys (ix, level, iz). -> list of lists."""
    keyset = set(keys)
    seen, out = set(), []
    for k in keys:
        if k in seen:
            continue
        comp, stack = [], [k]
        seen.add(k)
        while stack:
            c = stack.pop()
            comp.append(c)
            for dx in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    n = (c[0] + dx, c[1], c[2] + dz)
                    if n in keyset and n not in seen:
                        seen.add(n)
                        stack.append(n)
        out.append(comp)
    out.sort(key=len, reverse=True)
    return out


def room_of(name):
    """'GI living room 3' -> 'living room'."""
    n = re.sub(r"^GI\s+", "", name)
    return re.sub(r"\s+\d+$", "", n)


def nearest_volume(p, volumes):
    """The volume p is nearest to (metres outside its box), preferring volumes on p's storey (their height range holds
    p), so an upstairs gap isn't put down to the room below."""
    storey = [v for v in volumes if v["box"].aabb()[0][1] - 0.3 <= p[1] <= v["box"].aabb()[1][1] + 0.3]
    best, bd = None, 1e30
    for v in storey or volumes:
        d = -v["box"].inside_margin(p)
        if d < bd:
            best, bd = v, d
    return best, bd


# ----------------------------------------------------------------------------------------------------------------
# Doorways: volumes whose boxes meet (axis-aligned world AABBs; every Forest volume is unrotated)


def adjacent_pairs(volumes, touch=0.30, min_shared=0.30):
    """Pairs that overlap or sit within `touch` m of each other on one axis while sharing at least `min_shared` m of face
    on the other two. -> [(a, b, axis, gap)] with gap < 0 = overlap depth."""
    out = []
    boxes = [v["box"].aabb() for v in volumes]
    for i in range(len(volumes)):
        for j in range(i + 1, len(volumes)):
            (amin, amax), (bmin, bmax) = boxes[i], boxes[j]
            sep = [max(amin[a], bmin[a]) - min(amax[a], bmax[a]) for a in range(3)]  # >0 gap, <0 overlap
            # The crossing axis is the one with the least overlap among the horizontal axes (doorways are walked
            # through); the other two must share face.
            for axis in (0, 2, 1):
                others = [a for a in range(3) if a != axis]
                if sep[axis] <= touch and all(-sep[o] >= min_shared for o in others):
                    if axis != 1 and -sep[axis] > min(-sep[o] for o in others):
                        continue  # deeper overlap than the shared face: not a meeting edge on this axis
                    out.append((volumes[i], volumes[j], axis, sep[axis]))
                    break
    return out


def blend_note(gap, fade_a, fade_b):
    if gap > 1e-4:
        return "gap %.2f m: GI falls to the fallback (reflection-probe / sky ambient) between them" % gap
    ov = -gap
    if ov <= max(fade_a, fade_b) + 1e-4:
        return "touching (overlap %.3f m): GI dips through ~%.2f m of fade (wSum < 1)" % (ov, fade_a + fade_b - ov)
    return ("overlap %.2f m: step A->avg over %.2f m, avg plateau %.2f m, avg->B over %.2f m"
            % (ov, fade_b, max(ov - fade_a - fade_b, 0.0), fade_a))


def _air_beyond(own, x, axis, p, walls, volumes, skip=None):
    """Whose air the point p with p[axis] = x is in, seen from volume `own`: None (wall, but `skip`: a corner or a T;
    or air only volumes of own's room cover, as past a wall stub or column inside one room: harmless), "room" (a
    volume of another room covers it) or "open" (no other volume covers it: outdoors, a window, unvolumed air)."""
    q = list(p)
    q[axis] = x
    if any(c is not skip and c.contains(q) for c in walls):
        return None
    room = room_of(own["name"])
    others = [o for o in volumes if o is not own and o["box"].contains(q)]
    if not others:
        return "open"
    return "room" if any(room_of(o["name"]) != room for o in others) else None


def check_margins(volumes, walls, step=0.05, heights=(1.0, 1.7), deep=0.5):
    """--check-margins: every side-by-side meeting of two volumes (adjacent_pairs), sampled along the shared face every
    `step` m at `heights` above the higher of the two floors.

    A sample is WALLED when a wall collider running along the meeting (thin across it) lies anywhere between the two
    faces, not only at their midpoint: with uneven faces (one inside the wall, one past it) the midpoint can land in
    room air. Each of that wall's two surfaces is tested as the shader tests a surface: pushed off it along its normal
    by each volume's normal bias. The volume on the far side must weigh 0 there (`leak`: light through the wall), and
    all volumes together must cover the surface (`cover`: no fallback on the wall itself). A face that reaches past
    the wall's far surface into air (not into more wall: a corner) is `through-wall` even when the normal bias hides
    it from the leak test: its face probes bake the next room's light. Meetings that overlap by more than `deep` m are
    one space by design (a stairwell through a room): they get no wall test.
    Any other sample is OPEN (a doorway, or a seam inside one room): wSum is walked across the meeting, skipping points
    inside walls, and must never dip (`open_min`).
    "Cover" and "never dip" are judged against ONE box spanning both volumes along the crossing axis (and their shared
    span across it), faded the same way: the pair must do at least as well as a single merged volume would. That
    leaves a volume's outer faces (an outside door, a window) alone and flags only what the meeting itself loses.
    open_min and cover_min are reported as wSum minus that reference (>= 0 passes).
    -> one row per pair: sample counts, open_min, leak_max, cover_min, through_max and a verdict (ok / seam / leak /
    wall-gap / through-wall)."""
    rows = []
    for a, b, axis, gap in adjacent_pairs(volumes):
        if axis == 1:
            continue
        t = 2 - axis
        (amin, amax), (bmin, bmax) = a["box"].aabb(), b["box"].aabb()
        if (amin[axis] + amax[axis]) > (bmin[axis] + bmax[axis]):
            a, b = b, a
            (amin, amax), (bmin, bmax) = (bmin, bmax), (amin, amax)
        plane = 0.5 * (amax[axis] + bmin[axis])  # a is on the low side, b on the high side
        lo_t, hi_t = max(amin[t], bmin[t]), min(amax[t], bmax[t])
        floor, top = max(amin[1], bmin[1]), min(amax[1], bmax[1])
        n_open = n_wall = 0
        open_min, leak_max, cover_min, through_max = 2.0, 0.0, 2.0, 0.0
        two_rooms = room_of(a["name"]) != room_of(b["name"])
        fade = max(a["fade"], b["fade"])
        u_lo, u_hi = [0.0] * 3, [0.0] * 3
        u_lo[axis], u_hi[axis] = min(amin[axis], bmin[axis]), max(amax[axis], bmax[axis])
        u_lo[t], u_hi[t] = lo_t, hi_t
        u_lo[1], u_hi[1] = floor, top
        merged = Box([(u_lo[i] + u_hi[i]) * 0.5 for i in range(3)], [[1, 0, 0], [0, 1, 0], [0, 0, 1]],
                     [(u_hi[i] - u_lo[i]) * 0.5 for i in range(3)])
        lo_a, hi_a = sorted((amax[axis], bmin[axis]))
        band = (max(lo_a - 2 * fade, u_lo[axis]), min(hi_a + 2 * fade, u_hi[axis]))
        k = max(int((hi_t - lo_t) / step), 1)
        for i in range(k):
            tt = lo_t + (i + 0.5) * (hi_t - lo_t) / k
            for h in heights:
                y = floor + h
                if y >= top:
                    continue
                p = [0.0, y, 0.0]
                p[axis], p[t] = plane, tt
                w = None
                if -gap <= deep:
                    for c in walls:
                        lo, hi = c.aabb()
                        if not (hi[axis] - lo[axis] <= 0.45 and hi[axis] - lo[axis] < hi[t] - lo[t]):
                            continue
                        # the wall's stretch between the two faces; inside one room (a stub, a column) only at the
                        # midpoint, as light can't leak between two volumes of the same room
                        if two_rooms:
                            s0, s1 = max(lo[axis], lo_a), min(hi[axis], hi_a)
                        else:
                            s0 = s1 = plane
                        if s1 < s0:
                            continue
                        q = list(p)
                        q[axis] = 0.5 * (s0 + s1)
                        if c.contains(q):
                            w = c
                            break
                if w is None:
                    if any(c.contains(p) for c in walls):
                        continue  # a wall end or corner across the meeting: neither open nor a wall along it
                    n_open += 1
                    nstep = max(int(round((band[1] - band[0]) / 0.005)), 1)
                    for j in range(nstep + 1):
                        q = list(p)
                        q[axis] = band[0] + (band[1] - band[0]) * j / nstep
                        if any(c.contains(q) for c in walls):
                            continue
                        open_min = min(open_min, min(coverage_at(q, volumes)[0], 1.0) - merged.fade(q, fade))
                    continue
                n_wall += 1
                wlo, whi = w.aabb()[0][axis], w.aabb()[1][axis]
                # a's +axis face past the high surface, b's -axis face past the low one: probes in the far room's air
                for past, surf, sign, own in ((amax[axis] - whi, whi, 1.0, a), (wlo - bmin[axis], wlo, -1.0, b)):
                    if past > 1e-4 and _air_beyond(own, surf + sign * min(0.01, past * 0.5), axis, p, walls,
                                                   volumes, skip=w):
                        through_max = max(through_max, past)
                for surf, sign, far in ((wlo, -1.0, b), (whi, 1.0, a)):
                    q = list(p)
                    q[axis] = surf
                    nrm = [0.0, 0.0, 0.0]
                    nrm[axis] = sign
                    if any(c is not w and c.contains(v_add(q, v_scale(nrm, 0.01))) for c in walls):
                        continue  # a corner: this bit of surface is buried in the wall across it, never seen
                    qb = v_add(q, v_scale(nrm, 0.25 * min(a.get("spacing", 0.0), b.get("spacing", 0.0))))
                    cover_min = min(cover_min, min(coverage_at(q, volumes, nrm)[0], 1.0) - merged.fade(qb, fade))
                    qf = v_add(q, v_scale(nrm, 0.25 * far.get("spacing", 0.0)))
                    leak_max = max(leak_max, volume_fade(far, qf))
        verdict = []
        if n_open and open_min < -1e-6:
            verdict.append("seam")
        if n_wall and leak_max > 1e-6:
            verdict.append("leak")
        if n_wall and cover_min < -1e-6:
            verdict.append("wall-gap")
        if n_wall and through_max > 1e-4:
            verdict.append("through-wall")
        rows.append({"a": a["name"], "b": b["name"], "axis": "xyz"[axis], "plane": round(plane, 3),
                     "span": "%.2f..%.2f" % (lo_t, hi_t), "open_samples": n_open,
                     "open_min_wsum": round(open_min, 4) if n_open else "",
                     "walled_samples": n_wall, "wall_leak_max": round(leak_max, 4) if n_wall else "",
                     "wall_cover_min": round(cover_min, 4) if n_wall else "",
                     "through_wall_max_m": round(through_max, 4) if n_wall else "",
                     "verdict": "+".join(verdict) or "ok"})
    return rows


def face_probe_exposure(volumes, walls, step=0.25, heights=(1.0, 1.7)):
    """Where each volume's side-face probes sit, sampled every `step` m at `heights` above its floor: in a wall collider
    (the bake marks them invalid and fills them from inside: no leak), in air another volume also covers (a doorway or
    a seam: shared light), THROUGH-WALL (in another room's air, with a wall running along the face starting within
    `reach` m inside it: the probe bakes the light of the room beyond that wall, a leak; fails the check),
    THROUGH-WALL-OPEN (the same, but in air no other volume covers: a face just past a window's glass or frame, or an
    outside door's jambs; reported, not failed), or EXPOSED (air no other volume covers: outdoors or an uncovered
    space, so the room's wall-side samples blend some of that light in). -> one row per volume."""
    reach = 0.25  # the normal bias of a 1 m grid: a wall starting farther in is the room's own (a stub, a jamb end)
    rows = []
    for v in volumes:
        mn, mx = v["box"].aabb()
        cnt = collections.Counter()
        for axis in (0, 2):
            t = 2 - axis
            thin = []  # walls running along this axis's faces (thin across them), as AABBs
            for w in walls:
                lo, hi = w.aabb()
                if hi[axis] - lo[axis] <= 0.45 and hi[axis] - lo[axis] < hi[t] - lo[t]:
                    thin.append((lo, hi))
            k = max(int((mx[t] - mn[t]) / step), 1)
            for c, inward in ((mn[axis], 1.0), (mx[axis], -1.0)):
                walk = min(reach, mx[axis] - mn[axis])
                s0, s1 = sorted((c, c + inward * walk))
                for i in range(k + 1):
                    for h in heights:
                        y = mn[1] + h
                        if y >= mx[1]:
                            continue
                        p = [0.0, y, 0.0]
                        p[axis], p[t] = c, mn[t] + (mx[t] - mn[t]) * i / k
                        if any(w.contains(p) for w in walls):
                            cnt["wall"] += 1
                            continue
                        crossed = any(lo[t] <= p[t] <= hi[t] and lo[1] <= y <= hi[1]
                                      and lo[axis] < s1 and hi[axis] > s0 for lo, hi in thin)
                        beyond = _air_beyond(v, c, axis, p, walls, volumes) if crossed else None
                        if beyond == "room":
                            cnt["through_wall"] += 1
                        elif beyond == "open":
                            cnt["through_wall_open"] += 1
                        elif any(o is not v and o["box"].contains(p) for o in volumes):
                            cnt["shared"] += 1
                        else:
                            cnt["exposed"] += 1
        n = sum(cnt.values())
        rows.append({"volume": v["name"], "samples": n, "in_wall": cnt["wall"], "shared": cnt["shared"],
                     "through_wall": cnt["through_wall"], "through_wall_open": cnt["through_wall_open"],
                     "exposed": cnt["exposed"],
                     "exposed_share": round(cnt["exposed"] / n, 3) if n else 0.0})
    return rows


# ----------------------------------------------------------------------------------------------------------------
# Diff against a git revision


def git_show(repo, rev, path):
    flags = 0x08000000 if os.name == "nt" else 0  # CREATE_NO_WINDOW: no console pops up on Jacob's screen
    r = subprocess.run(["git", "-C", repo, "show", "%s:%s" % (rev, path)], capture_output=True, creationflags=flags)
    if r.returncode != 0:
        raise RuntimeError("git show %s:%s failed: %s" % (rev, path, r.stderr.decode(errors="replace").strip()))
    return r.stdout


def git_out(repo, *args):
    flags = 0x08000000 if os.name == "nt" else 0
    r = subprocess.run(["git", "-C", repo] + list(args), capture_output=True, creationflags=flags)
    return r.stdout.decode(errors="replace").strip() if r.returncode == 0 else ""


def match_entities(old, new, key, pos):
    """Pairs old and new records that are the same object even when an editor save renumbered the ids: same id first,
    then within each key (e.g. name + asset), nearest position first. -> (pairs [(o, n)], added [n], removed [o])."""
    by_id = {key_id: o for key_id, o in ((o.get("id"), o) for o in old) if key_id is not None}
    pairs, used_o, used_n = [], set(), set()
    for j, n in enumerate(new):
        o = by_id.get(n.get("id"))
        if o is not None and key(o) == key(n):
            pairs.append((o, n))
            used_o.add(id(o))
            used_n.add(j)
    groups_o = collections.defaultdict(list)
    for o in old:
        if id(o) not in used_o:
            groups_o[key(o)].append(o)
    rest_n = collections.defaultdict(list)
    for j, n in enumerate(new):
        if j not in used_n:
            rest_n[key(n)].append(n)
    added, removed = [], []
    for k in set(groups_o) | set(rest_n):
        os_, ns = list(groups_o.get(k, [])), list(rest_n.get(k, []))
        cand = sorted(((v_len(v_sub(pos(o), pos(n))), a, b) for a, o in enumerate(os_) for b, n in enumerate(ns)))
        ta, tb = set(), set()
        for _, a, b in cand:
            if a not in ta and b not in tb:
                ta.add(a)
                tb.add(b)
                pairs.append((os_[a], ns[b]))
        removed += [o for a, o in enumerate(os_) if a not in ta]
        added += [n for b, n in enumerate(ns) if b not in tb]
    return pairs, added, removed


def diff_lights(old, new, move_tol=0.01):
    pairs, added, removed = match_entities(old, new, lambda l: (l["name"], l["type"]), lambda l: l["pos"])
    rows = [("added", b["name"], b["circuit"], "") for b in added]
    rows += [("removed", a["name"], a["circuit"], "") for a in removed]
    for a, b in pairs:
        ch = []
        d = v_len(v_sub(a["pos"], b["pos"]))
        if d > move_tol:
            ch.append("moved %.2f m" % d)
        for k in ("type", "circuit", "shadow", "intensity", "range", "active", "name"):
            av, bv = a[k], b[k]
            if isinstance(av, float):
                if abs(av - bv) > 1e-4:
                    ch.append("%s %.3g->%.3g" % (k, av, bv))
            elif av != bv:
                ch.append("%s %r->%r" % (k, av, bv))
        if [round(c, 4) for c in a["color"]] != [round(c, 4) for c in b["color"]]:
            ch.append("color")
        if ch:
            rows.append(("changed", b["name"], b["circuit"], "; ".join(ch)))
    return rows


def geometry_near(scene, volumes, margin=2.0, pad=0.05):
    """Models and boxes whose origin lies within `margin` m of the volumes' joint AABB, as records
    {id, name, asset, pos, vols (the volumes holding the origin), active}."""
    lo = [min(v["box"].aabb()[0][i] for v in volumes) - margin for i in range(3)]
    hi = [max(v["box"].aabb()[1][i] for v in volumes) + margin for i in range(3)]
    out = []
    for e in scene.ents:
        if e["_kind"] == "empties" or "Light" in e or "GI Volume" in e:
            continue
        p = scene.position(e)
        if not all(lo[i] <= p[i] <= hi[i] for i in range(3)):
            continue
        out.append({"id": e.get("id"), "name": e.get("name", ""), "asset": e.get("path", e["_kind"]), "pos": p,
                    "vols": [v["name"] for v in volumes if v["box"].contains(p, pad)], "active": scene.active(e)})
    return out


def diff_geometry(old_recs, new_recs, move_tol=0.01):
    """-> (added, removed, moved) record lists, only for objects inside a volume before or after."""
    pairs, added, removed = match_entities(old_recs, new_recs, lambda r: (r["name"], r["asset"]), lambda r: r["pos"])
    moved = []
    for o, n in pairs:
        if not (o["vols"] or n["vols"]):
            continue
        d = v_len(v_sub(o["pos"], n["pos"]))
        if d > move_tol or o["active"] != n["active"]:
            moved.append(dict(n, moved=d, was_active=o["active"], vols=n["vols"] or o["vols"]))
    return [r for r in added if r["vols"]], [r for r in removed if r["vols"]], moved


# ----------------------------------------------------------------------------------------------------------------
# Report


def fmt3(v, nd=2):
    return "(%s)" % ", ".join("%.*f" % (nd, c) for c in v)


def run(args):
    repo = os.path.abspath(args.repo)
    scene_rel = args.scene.replace("\\", "/")
    scene_path = os.path.join(repo, scene_rel)
    with open(scene_path, "r", encoding="utf-8") as f:
        scene = Scene(json.load(f))
    vols = scene.volumes()
    lights = scene.lights()
    eye = scene.eye_height()
    os.makedirs(args.out, exist_ok=True)
    project = os.path.join(repo, "project")

    # ---- the bake files
    bakes, bake_err = {}, {}
    for bf in sorted({v["bake_file"] for v in vols if v["bake_file"]}):
        path = os.path.join(project, bf)
        try:
            with open(path, "rb") as f:
                bakes[bf] = {g["name"]: g for g in parse_gi(f.read())}
        except (OSError, ValueError) as ex:
            bake_err[bf] = str(ex)
    bake_log = {}
    if getattr(args, "bake_log", ""):
        with open(args.bake_log, "r", encoding="utf-8", errors="replace") as f:
            bake_log = parse_bake_log(f.read())

    # ---- per volume: match + invalid share
    vol_rows = []
    for v in vols:
        g = bakes.get(v["bake_file"], {}).get(v["name"])
        row = {"volume": v["name"], "center": fmt3(v["box"].center), "size": fmt3([2 * h for h in v["box"].half]),
               "spacing": v["spacing"], "fade": v["fade"], "intensity": v["intensity"],
               "circuits": ";".join(v["circuits"]), "outside": v["outside"],
               "dims": "x".join(map(str, v["dims"])), "probes": v["dims"][0] * v["dims"][1] * v["dims"][2]}
        issues = []
        if not v["bake_file"]:
            issues.append("no Bake File")
        elif v["bake_file"] in bake_err:
            issues.append("bake unreadable: " + bake_err[v["bake_file"]])
        elif g is None:
            issues.append("MISSING from bake")
        if g is not None:
            row["baked_dims"] = "x".join(map(str, g["dims"]))
            if tuple(g["dims"]) != tuple(v["dims"]):
                issues.append("STALE grid (baked %s, now %s): runtime drops this volume" % (row["baked_dims"], row["dims"]))
            if not g["box"].matches(v["box"]):
                dc = v_len(v_sub(g["box"].center, v["box"].center))
                dh = max(abs(g["box"].half[i] - v["box"].half[i]) for i in range(3))
                issues.append("box moved/resized since bake (centre %.3f m, half %.3f m)" % (dc, dh))
            baked_layers = [c for c, _ in g["layers"]]
            want = ([""] if v["outside"] else []) + v["circuits"]
            missing = [c or "Outside" for c in want if c not in baked_layers]
            extra = [c or "Outside" for c in baked_layers if c not in want]
            if missing:
                issues.append("layers not baked: " + ", ".join(missing))
            if extra:
                issues.append("baked layers no longer wanted: " + ", ".join(extra))
            if len(baked_layers) > MAX_LAYERS:
                issues.append("%d layers > kMaxLayers %d (extra dropped at runtime)" % (len(baked_layers), MAX_LAYERS))
            row["baked_layers"] = ";".join(c or "Outside" for c in baked_layers)
            cls = classify_probes(g["layers"], g["dims"])
            nf, nu = cls.count("filled"), cls.count("uniform")
            d = g["dims"]
            inner = [cls[probe_index(i, j, k, d)] for k in range(1, d[2] - 1) for j in range(1, d[1] - 1)
                     for i in range(1, d[0] - 1)]
            row["filled"] = nf
            row["dark_ambiguous"] = nu
            row["invalid_share"] = round(nf / len(cls), 4)
            # Face probes sit in the walls by design (GIBaker's own check counts inner probes only), so the flag is on
            # the inner probes: those are the ones the room's air actually samples.
            row["classifier_inner_invalid_share"] = round(inner.count("filled") / len(inner), 4) if inner else None
            row["inner_invalid_share"] = row["classifier_inner_invalid_share"]
            row["inner_invalid_source"] = "classifier"
            # The baker's own count wins when its log is given: it knows which probes it filled.
            if v["name"] in bake_log:
                probes_n, invalid_n = bake_log[v["name"]]
                shell, n_inner, ii = bake_inner_invalid(probes_n, invalid_n, d)
                row["bake_invalid"] = invalid_n
                row["bake_shell"] = shell
                row["bake_inner_invalid"] = ii
                row["inner_invalid_share"] = round(ii / n_inner, 4) if n_inner else None
                row["inner_invalid_source"] = "bake log"
            if (row["inner_invalid_share"] or 0) > INVALID_FLAG_SHARE:
                issues.append("inner invalid share %.0f%% > 20%% (%s)" % (100 * row["inner_invalid_share"],
                                                                          row["inner_invalid_source"]))
        if len(([""] if v["outside"] else []) + v["circuits"]) > MAX_LAYERS:
            issues.append("wants more than %d layers" % MAX_LAYERS)
        row["issues"] = " | ".join(issues)
        vol_rows.append(row)
    orphan_bakes = sorted(set().union(*[set(b) for b in bakes.values()]) - {v["name"] for v in vols}) if bakes else []

    # ---- coverage
    house = [e for e in scene.descendants_of_group(FLOOR_GROUPS)]
    vol_top = max((v["box"].aabb()[1][1] for v in vols), default=1e30)
    floors, skipped_slabs = [], []
    for e in house:
        b = scene.box_collider(e)
        if b is None or not scene.active(e):
            continue
        top = max(p[1] for p in sample_top_face(b, 10.0))
        # Ceilings hang under the floor pieces in the hierarchy; a slab above every GI box is a roof/ceiling cap.
        if "ceiling" in e.get("name", "").lower() or top > vol_top:
            skipped_slabs.append(e.get("name", ""))
            continue
        grp = next((n for n in scene.ancestor_names(e) if n in FLOOR_GROUPS), "?")
        floors.append((e, b, grp))
    walls = [b for b in (scene.box_collider(e) for e in scene.descendants_of_group(WALL_GROUPS)) if b is not None]
    mesh_floors = [e for e in house if (e.get("collider") or {}).get("shape") in (3, 4)]
    step = args.step
    samples = {}  # key -> (point on floor, group)
    for e, b, grp in floors:
        for p in sample_top_face(b, step):
            key = (round(p[0] / step), round(p[1] * 10), round(p[2] / step))
            if key in samples:
                continue
            probe = [p[0], p[1] + 0.3, p[2]]
            if any(w.contains(probe) for w in walls):
                continue
            samples[key] = (p, grp)
    cov_rows, gap_keys = [], {"floor": [], "eye": []}
    counts = collections.Counter()
    for key, (p, grp) in samples.items():
        for label, dy, nrm in (("floor", 0.0, (0.0, 1.0, 0.0)), ("eye", eye, None)):
            q = [p[0], p[1] + dy, p[2]]
            w, hits = coverage_at(q, vols, nrm)
            state = "gap" if w <= 0.0 else ("partial" if w < 1.0 else "covered")
            counts[(label, state)] += 1
            if state != "covered":
                gap_keys[label].append(key) if state == "gap" else None
                cov_rows.append({"height": label, "x": round(q[0], 3), "y": round(q[1], 3), "z": round(q[2], 3),
                                 "floor_group": grp, "state": state, "wsum": round(w, 3),
                                 "volumes": ";".join("%s=%.2f" % h for h in hits)})
    gap_rows = []
    for label in ("eye", "floor"):
        for comp in cluster_points(gap_keys[label], step):
            pts = [samples[k][0] for k in comp]
            cx = sum(p[0] for p in pts) / len(pts)
            cz = sum(p[2] for p in pts) / len(pts)
            cy = sum(p[1] for p in pts) / len(pts) + (eye if label == "eye" else 0.0)
            nv, nd = nearest_volume([cx, cy, cz], vols)
            gap_rows.append({"height": label, "points": len(comp), "area_m2": round(len(comp) * step * step, 2),
                             "centroid": fmt3([cx, cy, cz]), "floor_group": samples[comp[0]][1],
                             "x_range": "%.2f..%.2f" % (min(p[0] for p in pts), max(p[0] for p in pts)),
                             "z_range": "%.2f..%.2f" % (min(p[2] for p in pts), max(p[2] for p in pts)),
                             "nearest_volume": nv["name"] if nv else "", "nearest_room": room_of(nv["name"]) if nv else "",
                             "dist_to_nearest_m": round(nd, 2)})

    # ---- doorways
    doors = [scene.position(e) for e in scene.descendants_of_group(DOOR_GROUPS) if e["_kind"] != "empties"]
    door_rows = []
    for a, b, axis, gap in adjacent_pairs(vols):
        amin, amax = a["box"].aabb()
        bmin, bmax = b["box"].aabb()
        lo = [max(amin[i], bmin[i]) for i in range(3)]
        hi = [min(amax[i], bmax[i]) for i in range(3)]
        mid = [(lo[i] + hi[i]) * 0.5 for i in range(3)]
        near_door = any(all(lo[i] - 0.6 <= d[i] <= hi[i] + 0.6 for i in (0, 2)) and abs(d[1] - lo[1]) < 3.0 for d in doors)
        if axis == 1:
            kind = "floor/ceiling"
        elif gap > 1e-4:
            kind = "gap"
        elif -gap <= max(a["fade"], b["fade"]) + 1e-4:
            kind = "touch"
        else:
            kind = "overlap"
        door_rows.append({"a": a["name"], "b": b["name"], "kind": kind, "axis": "xyz"[axis], "gap_m": round(gap, 3),
                          "same_room": room_of(a["name"]) == room_of(b["name"]), "door_entity_near": near_door,
                          "fade_a": a["fade"], "fade_b": b["fade"], "at": fmt3(mid),
                          "blend": blend_note(gap, a["fade"], b["fade"])})

    # ---- margins (--check-margins)
    margin_rows = face_rows = None
    if getattr(args, "check_margins", False):
        margin_rows = check_margins(vols, walls)
        face_rows = face_probe_exposure(vols, walls)
    args.margin_rows, args.face_rows = margin_rows, face_rows

    # ---- circuits and the shadow budget
    by_circ = collections.defaultdict(list)
    for l in lights:
        if l["type"] in ("Point", "Spot"):
            by_circ[l["circuit"] or "(none)"].append(l)
    vol_circs = collections.Counter(c for v in vols for c in v["circuits"])
    sw = scene.switches()
    sw_circs = collections.defaultdict(list)
    for s in sw:
        sw_circs[s["circuit"]].append(s)
    circ_rows = []
    sim_rows = []
    tan_half = math.tan(math.radians(args.bake_fov) * 0.5)
    for c in sorted(set(by_circ) | set(vol_circs) | set(sw_circs)):
        ls = by_circ.get(c, [])
        cand = shadow_candidates(ls)
        pts = [l for l in cand if l["type"] == "Point"]
        sps = [l for l in cand if l["type"] == "Spot"]
        row = {"circuit": c, "lamps": len(ls), "active_lit": sum(1 for l in ls if l["active"] and l["lit"]),
               "point_shadow": len(pts), "spot_shadow": len(sps),
               "point_budget": scene.max_point_shadows, "spot_budget": scene.max_spot_shadows,
               "volumes_baking_it": vol_circs.get(c, 0), "switches": len(sw_circs.get(c, [])),
               "start_on": ";".join(str(s["start_on"]) for s in sw_circs.get(c, []))}
        flags = []
        baking = [v for v in vols if (v["outside"] if c == "(none)" else c in v["circuits"])]
        reach_p = max((sum(1 for l in pts if reaches(l, v["box"])) for v in baking), default=0)
        reach_s = max((sum(1 for l in sps if reaches(l, v["box"])) for v in baking), default=0)
        row["max_point_reaching_a_volume"] = reach_p
        row["max_spot_reaching_a_volume"] = reach_s
        if reach_p > scene.max_point_shadows:
            flags.append("POINT OVER BUDGET (%d reach one volume > %d)" % (reach_p, scene.max_point_shadows))
        if reach_s > scene.max_spot_shadows:
            flags.append("SPOT OVER BUDGET (%d reach one volume > %d)" % (reach_s, scene.max_spot_shadows))
        if (len(pts) > scene.max_point_shadows or len(sps) > scene.max_spot_shadows) and not flags:
            flags.append("more shadowed lamps than slots, but none of the extra reach a baking volume")
        unshadowed = [l for l in ls if l["active"] and l["lit"] and not l["shadow"]]
        if unshadowed:
            flags.append("%d lamp(s) with Cast Shadows off (light through walls in bake and game)" % len(unshadowed))
        if c != "(none)" and vol_circs.get(c, 0) == 0 and ls and any(l["active"] and l["lit"] for l in ls):
            flags.append("no volume bakes this circuit (no bounce)")
        if c != "(none)" and vol_circs.get(c, 0) and not ls and not sw_circs.get(c):
            flags.append("volumes bake it but no light or switch has it")
        # The bake: one camera per volume (GIBaker::Step moves it to the volume centre; a frame's batch ends at the
        # volume's end), so the shadowed set is fixed inside a volume and can change between volumes.
        # Lights on no circuit shine only in the Outside pass (GIBaker::ApplyLighting), baked for Outside volumes.
        if c == "(none)":
            row["volumes_baking_it"] = sum(1 for v in vols if v["outside"])
        sets, short = set(), []
        prev_pts, prev_sps = [], []
        for v in vols:
            if not (v["outside"] if c == "(none)" else c in v["circuits"]):
                continue
            sp = pick_shadowed(pts, v["box"].center, scene.max_point_shadows, prev_pts, tan_half)
            ss = pick_shadowed(sps, v["box"].center, scene.max_spot_shadows, prev_sps, tan_half)
            prev_pts, prev_sps = sp, ss
            # Only lamps that reach this volume matter to its probes.
            rel = {l["id"] for l in pts + sps if reaches(l, v["box"])}
            sets.add((tuple(sorted(set(sp) & rel)), tuple(sorted(set(ss) & rel)), tuple(sorted(rel))))
            dropped = [l["name"] for l in pts + sps if l["id"] in rel and l["id"] not in sp and l["id"] not in ss]
            sim_rows.append({"circuit": c, "volume": v["name"],
                             "shadowed": ";".join(l["name"] for l in pts + sps if l["id"] in sp or l["id"] in ss),
                             "unshadowed_in_bake": ";".join(dropped)})
            if dropped:
                short.append(v["name"])
        row["bake_volumes_short_of_slots"] = len(short)
        if short:
            flags.append("bakes %d volume(s) with a reaching shadow-caster left unshadowed (cause #4): %s"
                         % (len(short), ", ".join(short)))
        row["flags"] = " | ".join(flags)
        circ_rows.append(row)

    # In Play every circuit is on together: the whole house competes for the same slots, scored from the player's eye.
    # Camera at each volume's centre, in scene order (a crude walk), flashlight on (it takes one spot slot).
    all_c = shadow_candidates(lights)
    play_pts = [l for l in all_c if l["type"] == "Point"]
    play_sps = [l for l in all_c if l["type"] == "Spot"]
    play_sets, prev_pts, prev_sps, play_short, max_reach = set(), [], [], [], 0
    for v in vols:
        sp = pick_shadowed(play_pts, v["box"].center, scene.max_point_shadows, prev_pts, tan_half)
        ss = pick_shadowed(play_sps, v["box"].center, max(scene.max_spot_shadows - 1, 0), prev_sps, tan_half)
        prev_pts, prev_sps = sp, ss
        rel = [l for l in all_c if reaches(l, v["box"])]
        max_reach = max(max_reach, sum(1 for l in rel if l["type"] == "Point"))
        play_sets.add(tuple(sorted(l["id"] for l in rel if l["id"] in sp or l["id"] in ss)))
        dropped = [l["name"] for l in rel if l["id"] not in sp and l["id"] not in ss]
        if dropped:
            play_short.append(v["name"])
        sim_rows.append({"circuit": "(play: all on, flashlight)", "volume": v["name"],
                         "shadowed": ";".join(l["name"] for l in rel if l["id"] in sp or l["id"] in ss),
                         "unshadowed_in_bake": ";".join(dropped)})
    args.play = {"points": len(play_pts), "spots": len(play_sps), "sets": len(play_sets), "short": play_short,
                 "max_reach": max_reach,
                 "house_points": sum(1 for l in play_pts if any(reaches(l, v["box"]) for v in vols)),
                 "house_spots": sum(1 for l in play_sps if any(reaches(l, v["box"]) for v in vols)),
                 "no_shadow": sum(1 for l in lights if l["type"] in ("Point", "Spot") and l["active"] and l["lit"]
                                  and not l["shadow"]),
                 "lit": sum(1 for l in lights if l["type"] in ("Point", "Spot") and l["active"] and l["lit"])}

    # ---- changes since the bake revision
    since = args.since
    change = {"rev": since, "lights": [], "circuits": None, "volumes": [], "geo_added": [], "geo_removed": [],
              "geo_moved": [], "error": ""}
    if since:
        try:
            old = Scene(json.loads(git_show(repo, since, scene_rel)))
            change["lights"] = diff_lights(old.lights(), lights)
            oc = {l["circuit"] for l in old.lights() if l["circuit"]} | {s["circuit"] for s in old.switches()}
            nc = {l["circuit"] for l in lights if l["circuit"]} | {s["circuit"] for s in sw}
            change["circuits"] = (sorted(nc - oc), sorted(oc - nc))
            sp_, sa_, sr_ = match_entities(old.switches(), sw, lambda s: s["name"], lambda s: s["pos"])
            change["switches"] = [(n["name"], n["circuit"], "Start On %s->%s" % (o["start_on"], n["start_on"]))
                                  for o, n in sp_ if o["start_on"] != n["start_on"]]
            change["switches"] += [(o["name"], o["circuit"], "circuit %r->%r" % (o["circuit"], n["circuit"]))
                                   for o, n in sp_ if o["circuit"] != n["circuit"]]
            change["switches"] += [(n["name"], n["circuit"], "added") for n in sa_]
            change["switches"] += [(o["name"], o["circuit"], "removed") for o in sr_]
            ov = {v["name"]: v for v in old.volumes()}
            for v in vols:
                o = ov.get(v["name"])
                if o is None:
                    change["volumes"].append((v["name"], "added"))
                elif not o["box"].matches(v["box"]) or o["spacing"] != v["spacing"] or o["circuits"] != v["circuits"] \
                        or o["outside"] != v["outside"]:
                    change["volumes"].append((v["name"], "changed"))
            for n in ov:
                if n not in {v["name"] for v in vols}:
                    change["volumes"].append((n, "removed"))
            allv = vols + [ov[n] for n in ov if n not in {v["name"] for v in vols}]
            change["geo_added"], change["geo_removed"], change["geo_moved"] = diff_geometry(
                geometry_near(old, allv), geometry_near(scene, allv))
            change["id_overlap"] = len({e.get("id") for e in old.ents} & {e.get("id") for e in scene.ents})
            change["old_count"] = len(old.ents)
        except (RuntimeError, ValueError) as ex:
            change["error"] = str(ex)

    # ---- tour cross-check
    tour_rows, route_rows = [], []
    state_of = lambda w: "gap" if w <= 0 else ("partial" if w < 1 else "covered")  # noqa: E731
    if args.tour:
        with open(args.tour, "r", encoding="utf-8") as f:
            tdoc = json.load(f)
        shaped = tour_waypoints(tdoc)
        if shaped:
            t_eye, wps, routes = shaped
            for wid, room, feet in wps:
                e = [feet[0], feet[1] + t_eye, feet[2]]
                we, he = coverage_at(e, vols)
                wf, _ = coverage_at(feet, vols, (0.0, 1.0, 0.0))
                tour_rows.append({"waypoint": wid, "room": room, "eye": fmt3(e), "wsum": round(we, 3),
                                  "state": state_of(we), "floor_state": state_of(wf),
                                  "volumes": ";".join("%s=%.2f" % h for h in he)})
            for rid, feet_list in routes:
                pts = route_samples(feet_list, args.step)
                states = [state_of(coverage_at([p[0], p[1] + t_eye, p[2]], vols)[0]) for p in pts]
                # Runs of gap/partial along the walk, in metres.
                runs, cur = [], 0
                for s in states:
                    if s != "covered":
                        cur += 1
                    elif cur:
                        runs.append(cur)
                        cur = 0
                if cur:
                    runs.append(cur)
                route_rows.append({"route": rid, "length_m": round(args.step * max(len(pts) - 1, 0), 1),
                                   "gap_m": round(args.step * states.count("gap"), 2),
                                   "partial_m": round(args.step * states.count("partial"), 2),
                                   "uncovered_stretches": len(runs),
                                   "longest_stretch_m": round(args.step * max(runs, default=0), 2)})
        else:
            for name, p in tour_points(tdoc):
                w, hits = coverage_at(p, vols)
                tour_rows.append({"waypoint": name, "eye": fmt3(p), "wsum": round(w, 3), "state": state_of(w),
                                  "volumes": ";".join("%s=%.2f" % h for h in hits)})
    args.route_rows = route_rows

    # ---- write
    write_csv(os.path.join(args.out, "gi_audit_volumes.csv"), vol_rows)
    write_csv(os.path.join(args.out, "gi_audit_gaps.csv"), gap_rows)
    write_csv(os.path.join(args.out, "gi_audit_coverage_points.csv"), cov_rows)
    write_csv(os.path.join(args.out, "gi_audit_doorways.csv"), door_rows)
    write_csv(os.path.join(args.out, "gi_audit_circuits.csv"), circ_rows)
    write_csv(os.path.join(args.out, "gi_audit_bake_shadows.csv"), sim_rows)
    if tour_rows:
        write_csv(os.path.join(args.out, "gi_audit_tour.csv"), tour_rows)
    if margin_rows is not None:
        write_csv(os.path.join(args.out, "gi_audit_margins.csv"), margin_rows)
        write_csv(os.path.join(args.out, "gi_audit_face_probes.csv"), face_rows)
    args.skipped_slabs = skipped_slabs
    report = render_report(args, repo, scene, vols, vol_rows, orphan_bakes, samples, counts, gap_rows, floors,
                           mesh_floors, door_rows, circ_rows, change, tour_rows, eye)
    with open(os.path.join(args.out, "REPORT.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(report)
    if margin_rows is not None and (any(r["verdict"] != "ok" for r in margin_rows)
                                    or any(r["through_wall"] for r in face_rows)):
        return 1
    return 0


def tour_waypoints(doc):
    """W3's tools/gi/forest_house_tour.json: {player: {eyeHeight}, waypoints: [{id, room, feet}], routes: [{id, points}]}.
    -> (eye height, [(id, room, feet)], [(route id, [feet...])]). None when the file isn't in that shape."""
    wps = doc.get("waypoints") if isinstance(doc, dict) else None
    if not isinstance(wps, list) or not all(isinstance(w, dict) and "feet" in w for w in wps):
        return None
    eye = float((doc.get("player") or {}).get("eyeHeight", 1.7))
    pts = [(str(w.get("id", i)), str(w.get("room", "")), [float(c) for c in w["feet"]]) for i, w in enumerate(wps)]
    by = {p[0]: p[2] for p in pts}
    routes = [(str(r.get("id", r.get("name", ""))), [by[i] for i in r.get("points", []) if i in by])
              for r in doc.get("routes", []) if isinstance(r, dict)]
    return eye, pts, routes


def route_samples(feet_list, step):
    """Points every `step` m along the polyline through the waypoints' feet (the walk between them)."""
    out = []
    for a, b in zip(feet_list, feet_list[1:]):
        d = v_len(v_sub(b, a))
        n = max(int(math.ceil(d / step)), 1)
        out += [v_add(a, v_scale(v_sub(b, a), k / n)) for k in range(n)]
    if feet_list:
        out.append(list(feet_list[-1]))
    return out


def tour_points(doc):
    """Waypoints from a tour JSON of unknown exact shape: any object with a 3-number position/pos/p (+ name/id)."""
    out = []

    def walk(o, path):
        if isinstance(o, dict):
            for k in ("position", "pos", "p", "eye"):
                v = o.get(k)
                if isinstance(v, list) and len(v) == 3 and all(isinstance(c, (int, float)) for c in v):
                    out.append((str(o.get("name", o.get("id", path))), [float(c) for c in v]))
                    return
            for k, v in o.items():
                walk(v, path + "/" + str(k))
        elif isinstance(o, list):
            for i, v in enumerate(o):
                walk(v, "%s[%d]" % (path, i))

    walk(doc, "")
    return out


def write_csv(path, rows):
    keys = []
    for r in rows:
        for k in r:
            if k not in keys:
                keys.append(k)
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys or ["empty"])
        w.writeheader()
        for r in rows:
            w.writerow(r)


def md_table(rows, cols):
    if not rows:
        return "_none_\n"
    out = "| " + " | ".join(cols) + " |\n|" + "---|" * len(cols) + "\n"
    for r in rows:
        out += "| " + " | ".join(str(r.get(c, "")).replace("|", "/") for c in cols) + " |\n"
    return out


def render_report(args, repo, scene, vols, vol_rows, orphan_bakes, samples, counts, gap_rows, floors, mesh_floors,
                  door_rows, circ_rows, change, tour_rows, eye):
    head = git_out(repo, "rev-parse", "--short=8", "HEAD")
    gi_rev = git_out(repo, "log", "-1", "--format=%h %cs %s", "--", "project/" + (vols[0]["bake_file"] if vols else ""))
    L = []
    L.append("# GI Phase 0: coverage and bake audit (W1 Atlas)\n")
    L.append("Generated %s UTC by `tools/gi/gi_audit.py` (offline, read-only, CPU only) at worktree HEAD `%s`.  "
             % (datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M"), head))
    L.append("Scene `%s`; bake last committed: `%s`; changes diffed against `%s`.\n" % (args.scene, gi_rev, args.since))
    stale = [r for r in vol_rows if "STALE" in r.get("issues", "") or "MISSING" in r.get("issues", "")]
    moved = [r for r in vol_rows if "moved/resized" in r.get("issues", "")]
    hi_inv = [r for r in vol_rows if (r.get("inner_invalid_share") or 0) > INVALID_FLAG_SHARE]
    over = [r for r in circ_rows if "OVER BUDGET" in r["flags"]]
    multi = [r for r in circ_rows if r.get("bake_volumes_short_of_slots", 0) > 0]
    unsh = [r for r in circ_rows if "Cast Shadows off" in r["flags"]]
    eye_tot = counts[("eye", "gap")] + counts[("eye", "partial")] + counts[("eye", "covered")]
    fl_tot = counts[("floor", "gap")] + counts[("floor", "partial")] + counts[("floor", "covered")]
    gaps_by_room = collections.Counter()
    for g in gap_rows:
        if g["height"] == "eye":
            gaps_by_room[g["nearest_room"]] += g["area_m2"]

    L.append("## Summary\n")
    L.append("- **Volumes:** %d GI Volumes; %d stale or missing in the bake, %d with the box moved/resized since the bake"
             "%s." % (len(vols), len(stale), len(moved),
                      ("; bake holds %d volume(s) the scene no longer has (%s)" % (len(orphan_bakes), ", ".join(orphan_bakes)))
                      if orphan_bakes else ""))
    shares = [r["invalid_share"] for r in vol_rows if "invalid_share" in r]
    L.append("- **Invalid probes:** %d volume(s) with more than 20%% of their INNER probes neighbour-filled%s. Counting "
             "the face probes too (they sit in the walls by design) the share is %.0f-%.0f%% (median %.0f%%)." % (
                 len(hi_inv), (": " + ", ".join("%s %.0f%%" % (r["volume"], 100 * r["inner_invalid_share"]) for r in hi_inv))
                 if hi_inv else "", 100 * min(shares or [0]), 100 * max(shares or [0]),
                 100 * sorted(shares or [0])[len(shares or [0]) // 2]))
    if eye_tot:
        L.append("- **Coverage (walkable floor, %.2f m grid, %d points):** at eye height (%.2f m) %.1f%% in no volume, "
                 "%.1f%% in a fade band; at floor height %.1f%% / %.1f%%. Largest eye-height gaps by nearest room: %s."
                 % (args.step, len(samples), eye, 100.0 * counts[("eye", "gap")] / eye_tot,
                    100.0 * counts[("eye", "partial")] / eye_tot, 100.0 * counts[("floor", "gap")] / max(fl_tot, 1),
                    100.0 * counts[("floor", "partial")] / max(fl_tot, 1),
                    ", ".join("%s %.1f m2" % (k, a) for k, a in gaps_by_room.most_common(6)) or "none"))
    kinds = collections.Counter(d["kind"] for d in door_rows)
    cross = [d for d in door_rows if d["kind"] != "floor/ceiling"]
    L.append("- **Doorways / seams:** %d side-by-side meetings between volumes (%d between rooms, %d inside one room): "
             "%d with a gap (GI drops to fallback), %d touching (GI dips through the fade), %d overlapping; plus %d "
             "floor/ceiling stackings. Every volume's fade is %s m, so every seam is a <= 0.05 m step." % (
                 len(cross), sum(1 for d in cross if not d["same_room"]), sum(1 for d in cross if d["same_room"]),
                 kinds["gap"], kinds["touch"], kinds["overlap"], kinds["floor/ceiling"],
                 "/".join(sorted({"%.2f" % v["fade"] for v in vols}))))
    L.append("- **Bake shadow budget (point %d / spot %d, each circuit baked alone):** %d circuit(s) with more "
             "shadow-casters reaching one volume than slots%s; %d circuit(s) bake a volume with a reaching shadow-caster "
             "left unshadowed (cause #4)%s; %d circuit(s) have lamps with Cast Shadows off." % (
                 scene.max_point_shadows, scene.max_spot_shadows, len(over),
                 (" (" + ", ".join(r["circuit"] for r in over) + ")") if over else "", len(multi),
                 (" (" + ", ".join(r["circuit"] for r in multi) + ")") if multi else "", len(unsh)))
    pl = getattr(args, "play", None)
    if pl:
        L.append("- **In Play (all circuits on):** %d of %d lit point/spot lamps have Cast Shadows OFF (they light "
                 "through walls, in the bake and the game). Shadowed lamps that reach the house: %d point (for %d "
                 "point slots) and %d spot (for %d spot slots, flashlight on); up to %d shadowed point lamps reach one "
                 "volume. With the camera at each of the %d volume centres the shadowed set takes %d different forms, "
                 "and %d volume(s) have a reaching shadow-caster left unshadowed (cause #1)." % (
                     pl["no_shadow"], pl["lit"], pl["house_points"], scene.max_point_shadows, pl["house_spots"],
                     max(scene.max_spot_shadows - 1, 0), pl["max_reach"], len(vols), pl["sets"], len(pl["short"])))
    if change["error"]:
        L.append("- **Changes since %s:** could not diff: %s" % (change["rev"], change["error"]))
    elif change["rev"]:
        added = sum(1 for r in change["lights"] if r[0] == "added")
        removed = sum(1 for r in change["lights"] if r[0] == "removed")
        changed = sum(1 for r in change["lights"] if r[0] == "changed")
        L.append("- **Changes since %s:** lights %d added, %d removed, %d changed; circuits +%d / -%d; volumes %d changed; "
                 "geometry inside volumes %d added, %d removed, %d moved or toggled." % (
                     change["rev"], added, removed, changed, len(change["circuits"][0]), len(change["circuits"][1]),
                     len(change["volumes"]), len(change["geo_added"]), len(change["geo_removed"]), len(change["geo_moved"])))
    L.append("")
    if getattr(args, "notes", ""):
        with open(args.notes, "r", encoding="utf-8") as f:
            L.append(f.read().rstrip() + "\n")

    L.append("## Method and limits\n")
    L.append("- Read-only: the scene JSON and .gi from the worktree, plus `git show %s:%s`. Writes only into `--out`."
             % (args.since, args.scene))
    L.append("- Boxes, grid dims, fade and the shadow score are ported from GIData.cpp, the GIVolumes.glsl blend and "
             "LightGather.cpp; unit tested against hand-made cases (`tools/gi/test_gi_audit.py`).")
    L.append("- **Invalid probes** are not stored in the .gi. A probe counts as neighbour-filled when some subset of its "
             "6-neighbours averages to it in every coefficient of every layer within half-float rounding (what "
             "GI::FillInvalid leaves). A probe with one done neighbour is filled as a bit-exact copy of it: a group of "
             "identical neighbours counts all-but-one as filled (all of them when one member is an averaged fill). "
             "All-black probes are ambiguous and counted apart (`dark_ambiguous`), and so is a probe that is black "
             "(every coefficient under %g) with all its neighbours black too: in an unlit room any neighbour average "
             "matches it within rounding, so it can't be told from a fill. The bake's own log line "
             "`[GI bake] <volume>: N probes, X inside geometry (filled)` is the exact number: pass the bake's stdout "
             "with `--bake-log` and `inner_invalid_share` comes from it (shell = N - (x-2)(y-2)(z-2) taken as all "
             "invalid, a lower bound; `inner_invalid_source` says which count was used)." % BLACK_FLOOR)
    L.append("- **Walkable floor** = the top faces of the box floor colliders under %s (%d slabs), less points inside a "
             "wall collider 0.3 m up. %d floor piece(s) have mesh colliders and are NOT sampled (%s): the stair runs, "
             "for one. Talos's tour waypoints cover those (see the cross-check). Furniture is not subtracted. "
             "Skipped as not walkable: %d ceiling slab(s) (they hang under the floor pieces) and slabs above every GI "
             "box (%s). Floor-height points sit on the slab top with an up normal, so each volume tests them lifted by "
             "its normal bias (0.25 x Spacing), as the shader does; eye-height points are tested as they are."
             % ("/".join(FLOOR_GROUPS), len(floors), len(mesh_floors),
                ", ".join(sorted({e.get("name", "") for e in mesh_floors})) or "none",
                sum(1 for n in args.skipped_slabs if "ceiling" in n.lower()),
                ", ".join(sorted({n for n in args.skipped_slabs if "ceiling" not in n.lower()})) or "none"))
    L.append("- A point is covered when the shader's summed volume fade `wSum` reaches 1; `partial` is a fade band "
             "(0 < wSum < 1, part fallback).")
    L.append("- **Bake shadow sets** simulate LightGather's slot score with the camera at each volume's centre, in scene "
             "order, with the 25%% holder bonus carried between volumes; bake camera FOV assumed %.0f deg (it only "
             "matters through DistanceFade, which is 1 at house distances). Kronos's bake trace is the measurement; "
             "this is the prediction." % args.bake_fov)
    L.append("")

    L.append("## 1. Volumes and the bake\n")
    L.append(md_table(vol_rows, ["volume", "size", "dims", "baked_dims", "spacing", "fade", "circuits", "outside",
                                 "invalid_share", "classifier_inner_invalid_share", "bake_inner_invalid",
                                 "inner_invalid_share", "inner_invalid_source", "issues"]))
    L.append("Full table: `gi_audit_volumes.csv`.\n")

    L.append("## 2. Coverage gaps (eye height first)\n")
    big = [g for g in gap_rows if g["points"] >= 2][:40]
    L.append(md_table(big, ["height", "nearest_room", "nearest_volume", "dist_to_nearest_m", "area_m2", "points",
                            "floor_group", "centroid", "x_range", "z_range"]))
    L.append("%d gap area(s) in all (single points included): `gi_audit_gaps.csv`; every uncovered or partial point: "
             "`gi_audit_coverage_points.csv`.\n" % len(gap_rows))

    L.append("## 3. Doorways and volume seams\n")
    L.append("Between rooms (the walked-through seams):\n")
    L.append(md_table([d for d in door_rows if d["kind"] != "floor/ceiling" and not d["same_room"]],
                      ["a", "b", "kind", "axis", "gap_m", "door_entity_near", "blend"]))
    L.append("Inside one room (sub-boxes of an L-shaped room) and floor/ceiling stackings: `gi_audit_doorways.csv`.\n")

    L.append("## 4. Lamps per circuit vs the shadow budget (cause #4)\n")
    L.append("The baker renders each circuit alone (LightSwitch::SetAll off, then that circuit on) and points the view "
             "camera at each volume's centre in turn (GIBaker.cpp:218, a probe batch ends at the volume's end :238), so "
             "a circuit with more shadow-casting lamps than slots bakes some of them unshadowed, and WHICH ones depends "
             "on the volume. Unshadowed lamps light through walls into neighbouring volumes' probes.\n")
    L.append(md_table(circ_rows, ["circuit", "lamps", "active_lit", "point_shadow", "spot_shadow", "volumes_baking_it",
                                  "max_point_reaching_a_volume", "bake_volumes_short_of_slots", "flags"]))
    L.append("Per volume, which lamps the bake shadows: `gi_audit_bake_shadows.csv`.\n")

    L.append("## 5. Changes since the bake (%s)\n" % change["rev"])
    if change["error"]:
        L.append("Diff failed: %s\n" % change["error"])
    else:
        L.append("**Lights:**\n")
        L.append(md_table([{"what": a, "light": b, "circuit": c, "detail": d} for a, b, c, d in change["lights"]],
                          ["what", "light", "circuit", "detail"]))
        if change["circuits"] is not None:
            L.append("**Circuits:** added %s; removed %s.\n" % (", ".join(change["circuits"][0]) or "none",
                                                                ", ".join(change["circuits"][1]) or "none"))
        L.append("**Light switches:**\n")
        L.append(md_table([{"switch": a, "circuit": b, "change": c} for a, b, c in change.get("switches", [])],
                          ["switch", "circuit", "change"]))
        L.append("**Volumes changed:** %s\n" % (", ".join("%s (%s)" % v for v in change["volumes"]) or "none"))
        L.append("Objects are matched by name + asset and nearest position, not by id: %d of the %d entities at %s keep "
                 "their id today (editor saves renumber).\n" % (change.get("id_overlap", 0), change.get("old_count", 0),
                                                                 change["rev"]))
        for label, rows in (("added", change["geo_added"]), ("removed", change["geo_removed"])):
            per = collections.Counter(v for r in rows for v in r["vols"])
            L.append("**Geometry %s inside volumes:** %d object(s). By volume: %s.\n" % (
                label, len(rows), ", ".join("%s %d" % kv for kv in per.most_common()) or "none"))
            if rows:
                L.append(md_table([{"object": r["name"], "asset": os.path.basename(str(r["asset"])), "volume": r["vols"][0],
                                    "pos": fmt3(r["pos"])} for r in rows[:60]], ["object", "asset", "volume", "pos"]))
        per = collections.Counter(v for r in change["geo_moved"] for v in r["vols"])
        L.append("**Geometry moved or toggled inside volumes:** %d object(s). By volume: %s.\n" % (
            len(change["geo_moved"]), ", ".join("%s %d" % kv for kv in per.most_common()) or "none"))
        if change["geo_moved"]:
            mv = sorted(change["geo_moved"], key=lambda r: -r["moved"])
            L.append(md_table([{"object": r["name"], "volume": r["vols"][0], "moved_m": round(r["moved"], 3),
                                "active": "%s->%s" % (r["was_active"], r["active"])} for r in mv[:60]],
                              ["object", "volume", "moved_m", "active"]))

    L.append("## 6. Tour cross-check\n")
    if tour_rows:
        bad = [r for r in tour_rows if r["state"] != "covered" or r.get("floor_state", "covered") != "covered"]
        L.append("%d waypoint(s) from `%s`; %d not fully covered at the eye or under the feet.\n"
                 % (len(tour_rows), args.tour, len(bad)))
        L.append(md_table(bad, ["waypoint", "room", "eye", "state", "floor_state", "wsum", "volumes"]))
        if getattr(args, "route_rows", None):
            L.append("**Routes**, walked at eye height every %.2f m between their waypoints (straight lines):\n" % args.step)
            L.append(md_table(args.route_rows, ["route", "length_m", "gap_m", "partial_m", "uncovered_stretches",
                                                "longest_stretch_m"]))
        L.append("Every waypoint: `gi_audit_tour.csv`.\n")
    else:
        L.append("Pending: Talos's `tools/gi/forest_house_tour.json` isn't in yet. Re-run with `--tour <file>`.\n")
    if getattr(args, "margin_rows", None) is not None:
        mr, fr = args.margin_rows, args.face_rows
        bad = [r for r in mr if r["verdict"] != "ok"]
        n_open = sum(1 for r in mr if r["open_samples"])
        n_wall = sum(1 for r in mr if r["walled_samples"])
        L.append("## 7. Margin check (--check-margins)\n")
        L.append("%d side-by-side meetings: %d with an open band (a doorway, or a seam inside one room), %d across a "
                 "wall (a meeting can be both). **%d fail.** seam = wSum dips crossing an open band; leak = a volume "
                 "weighs > 0 on the far surface of a wall; wall-gap = a wall surface isn't fully covered; "
                 "through-wall = a face reaches past the wall's far surface into air. Both dips are "
                 "measured against one merged box over the pair (open_min_wsum / wall_cover_min = wSum minus that; "
                 ">= 0 passes).\n"
                 % (len(mr), n_open, n_wall, len(bad)))
        if bad:
            L.append(md_table(bad, ["a", "b", "axis", "plane", "span", "open_min_wsum", "wall_leak_max",
                                    "wall_cover_min", "through_wall_max_m", "verdict"]))
        thr = [r for r in fr if r["through_wall"]]
        L.append("**Face probes through a wall into another room's air** (they bake the next room's light; each one "
                 "fails the check): %d of %d volumes.\n" % (len(thr), len(fr)))
        if thr:
            L.append(md_table(sorted(thr, key=lambda r: -r["through_wall"]),
                              ["volume", "samples", "in_wall", "through_wall", "shared", "exposed"]))
        tho = [r for r in fr if r["through_wall_open"]]
        L.append("**Face probes just past a wall, window or door frame into air no volume covers** (outdoors, or a "
                 "window plane; reported, not failed): %d of %d volumes.\n" % (len(tho), len(fr)))
        if tho:
            L.append(md_table(sorted(tho, key=lambda r: -r["through_wall_open"]),
                              ["volume", "samples", "in_wall", "through_wall_open", "exposed"]))
        opens = [r for r in mr if r["open_samples"]]
        if opens:
            L.append("Open bands, lowest wSum crossing each:\n")
            L.append(md_table(sorted(opens, key=lambda r: r["open_min_wsum"]),
                              ["a", "b", "axis", "plane", "span", "open_samples", "open_min_wsum", "verdict"]))
        exp = [r for r in fr if r["exposed"]]
        L.append("**Face probes in open air no other volume shares** (outdoors, or a space no volume covers; the room's "
                 "wall-side samples blend some of that light in): %d of %d volumes.\n" % (len(exp), len(fr)))
        if exp:
            L.append(md_table(sorted(exp, key=lambda r: -r["exposed_share"]),
                              ["volume", "samples", "in_wall", "shared", "exposed", "exposed_share"]))
        L.append("Every pair: `gi_audit_margins.csv`; every volume: `gi_audit_face_probes.csv`.\n")
    return "\n".join(L) + "\n"


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", default=os.path.normpath(os.path.join(here, "..", "..")), help="repository root")
    ap.add_argument("--scene", default="project/scenes/Forest.json", help="scene, repo-relative")
    ap.add_argument("--out", required=True, help="output directory (REPORT.md + CSVs)")
    ap.add_argument("--since", default="378a1e74", help="git revision of the last bake to diff the scene against ('' = skip)")
    ap.add_argument("--tour", default="", help="optional tour JSON to cross-check against coverage")
    ap.add_argument("--step", type=float, default=0.25, help="floor sampling step, metres")
    ap.add_argument("--bake-fov", type=float, default=90.0, help="bake camera vertical FOV for DistanceFade")
    ap.add_argument("--notes", default="", help="optional markdown file inserted after the summary (hand-written findings)")
    ap.add_argument("--bake-log", default="",
                    help="the --bake-gi stdout of this bake: the baker's 'N probes, M inside geometry' lines give the "
                         "inner-invalid share instead of the classifier's guess")
    ap.add_argument("--check-margins", action="store_true",
                    help="check every place two volumes meet: no leak through walls, no wSum dip across doorways and "
                         "seams; exit status 1 on a failure")
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
