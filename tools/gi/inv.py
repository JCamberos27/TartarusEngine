"""House-wide Bounds hard-edge INVENTORY (fix-3 item 2).
    python -I inv.py <repo D:/ path> --md out.md [--csv out.csv]
Walls: box_edges.py rows (Bounds box edge on a wall face the light faces, within Range), then
  - merged: mesh/"Collider" duplicates and adjacent wall pieces with the same (light, face plane, cut, at) -> one edge;
  - visibility: a y cut inside a floor/ceiling slab sandwich (ceiling bottom .. floor top above it at that x,z), below the
    ground-floor top or at/above the top-storey ceiling is HIDDEN (exterior faces skip this: siding is continuous);
  - per-face fade (Bounds Fade +/-, ef469e82) is read per face: fade >= 0.3 m = SOFT, not a hard line.
Floors/ceilings (new): a vertical Bounds face (x or z plane) crossing a Floor_* top or Ceiling_* bottom the light faces
  (light above the floor / below the ceiling), inside the box's y range, within Range; visible share = part of the line
  not under a wall box; blk = walls between the light and the line's nearest point.
Strength: d/R (light distance / Range at the nearest point of the edge; engine falloff ~ (1-(d/R)^2)^2 style, so
  d/R < 0.5 strong, 0.5-0.8 visible, > 0.8 faint). Shadowed lights with blk > 0 = hidden by the shadow map; unshadowed with
  blk > 0 = the edge LEAKS through a wall (still visible)."""
import sys, os, math, collections
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
argv = list(sys.argv)
import bounds_audit as B  # parses argv[1] = repo

SOFT = 0.3
slabs = []  # (kind, lo, hi) of Floor_*/Ceiling_* box colliders
for e in B.S.ents:
    n = e.get("name", "")
    if not B.S.active(e) or not n.startswith(("Floor", "Ceiling")):
        continue
    b = B.S.box_collider(e)
    if b is None:
        continue
    lo, hi = b.aabb()
    slabs.append(("floor" if n.startswith("Floor") else "ceil", list(lo), list(hi), n))


def slabs_at(x, z, kind=None):
    return [s for s in slabs if (kind is None or s[0] == kind) and s[1][0] - 1e-3 <= x <= s[2][0] + 1e-3
            and s[1][2] - 1e-3 <= z <= s[2][2] + 1e-3]


def y_hidden(x, z, y):
    here_ = slabs_at(x, z)
    if not here_:
        return False
    for k, lo, hi, n in here_:
        if lo[1] - 0.01 <= y <= hi[1] + 0.01:
            return True
    ceils = [s for s in here_ if s[0] == "ceil"]
    floors = [s for s in here_ if s[0] == "floor"]
    for c in ceils:  # sandwich: ceiling bottom .. next floor top
        for f in floors:
            if c[1][1] - 0.01 <= y <= f[2][1] + 0.01 and 0 < f[2][1] - c[1][1] < 0.6:
                return True
    if floors and y <= min(f[2][1] for f in floors) + 0.01:
        return True
    if ceils and y >= max(c[1][1] for c in ceils) - 0.01 and not [f for f in floors if f[2][1] > y]:
        return True
    return False


CORNER = 0.005  # an edge within 5 mm of a wall box along its CUT axis sits in the junction (corner), not in the open:
# snapped faces land exactly on a wall's far plane and float noise put them a hair outside (29 false LEAK rows, 34ec488e)


def in_wall(p, pad=0.0, axis=None):
    """p inside a wall box; pad widens the box on every axis, or only on `axis` (the cut axis) when given"""
    pd = [pad if axis is None or i == axis else 0.0 for i in range(3)]
    return any(all(w["min"][i] - pd[i] <= p[i] <= w["max"][i] + pd[i] for i in range(3)) for w in B.WALLS)


SLABBOX = [dict(min=lo, max=hi) for k, lo, hi, n in slabs]


def crossed(p, q):
    n = 0
    for w in list(B.WALLS) + SLABBOX:
        t0, t1 = 0.0, 1.0
        for i in range(3):
            d = q[i] - p[i]
            if abs(d) < 1e-9:
                if not (w["min"][i] < p[i] < w["max"][i]):
                    break
            else:
                a, b = (w["min"][i] - p[i]) / d, (w["max"][i] - p[i]) / d
                t0, t1 = max(t0, min(a, b)), min(t1, max(a, b))
                if t0 > t1:
                    break
        else:
            if t1 - t0 > 1e-4 and t0 < 0.995:
                n += 1
    return n


def face_fades(e, L):
    """world (axis, sign) -> fade metres, honouring per-face Bounds Fade +/- (local box axes in the parent's space)."""
    base = float(L.get("Bounds Fade", 0.05))
    M = B.S.world(B.by[e["parentId"]])
    out = {}
    for a in range(3):
        col = [M[r][a] for r in range(3)]
        wa = max(range(3), key=lambda r: abs(col[r]))
        for s, key in ((1, "Bounds Fade +"), (-1, "Bounds Fade -")):
            v = L.get(key)
            f = float(v[a]) if v is not None and float(v[a]) >= 0 else base
            out[(wa, s if col[wa] > 0 else -s)] = f
    return out


lights = []
for e in B.doc["empties"]:
    L = e.get("Light")
    if L and L.get("Type") in ("Point", "Spot") and L.get("Bounds") and e.get("parentId") in B.by:
        W = B.S.world(e)
        lights.append((e, L, B.wbox(e), [W[r][3] for r in range(3)], float(L.get("Range", 0) or 0)))

CUTAX = {"lat-": None, "lat+": None, "y-": (1, -1), "y+": (1, 1)}
raw = []
for e, L, (lo, hi), p, R in lights:
    ff = face_fades(e, L)
    for w in B.WALLS:
        for ax in (0, 2):
            la = 2 - ax
            for sgn in (-1, 1):
                plane = w["max"][ax] if sgn > 0 else w["min"][ax]
                if not (lo[ax] < plane < hi[ax]) or (p[ax] - plane) * sgn <= 0.02:
                    continue
                a, b, c, d = w["min"][la], w["max"][la], w["min"][1], w["max"][1]
                oa, ob, oc, od = max(a, lo[la]), min(b, hi[la]), max(c, lo[1]), min(d, hi[1])
                if ob - oa < 0.05 or od - oc < 0.05:
                    continue
                cuts = []
                if lo[la] > a + 0.02: cuts.append(("lat-", lo[la], (la, -1)))
                if hi[la] < b - 0.02: cuts.append(("lat+", hi[la], (la, 1)))
                if lo[1] > c + 0.02: cuts.append(("y-", lo[1], (1, -1)))
                if hi[1] < d - 0.02: cuts.append(("y+", hi[1], (1, 1)))
                if not cuts:
                    continue
                ml, my = (oa + ob) / 2, (oc + od) / 2
                for kind, v, bf in cuts:
                    q = [0, 0, 0]; q[ax] = plane + sgn * 0.005
                    if kind.startswith("lat"):
                        q[la] = v; q[1] = min(max(p[1], oc), od); span = (oc, od)
                    else:
                        q[1] = v; q[la] = min(max(p[la], oa), ob); span = (oa, ob)
                    dist = math.dist(p, q)
                    if R and dist >= R:
                        continue
                    raw.append(dict(id=e["id"], name=e["name"][:26], sh="on" if L.get("Cast Shadows") else "off",
                                    wall=w.get("name", "?"), surf="wall", face=("+" if sgn > 0 else "-") + "xyz"[ax],
                                    ax=ax, sgn=sgn, la=la, plane=round(plane, 3), cut=kind, at=round(v, 3), span=span,
                                    q=q, ovl=(oa, ob, oc, od), width=b - a, dR=dist / R if R else 0, fade=ff[bf], blk=crossed(p, q), p=p, R=R))

import box_edges as BE  # noqa: E402  (reuse its exterior ray test; it prints its own table on import)
def is_ext(r):
    """exterior only if 3x3 rays across the cut wall rectangle all reach open air (window/door openings no longer count)"""
    ax, sgn, la = r["ax"], r["sgn"], r["la"]
    w = [x for x in B.WALLS if x.get("name", "?") == r["wall"]]
    lo_l, hi_l, lo_y, hi_y = r["ovl"]
    for fl in (0.15, 0.5, 0.85):
        for fy in (0.15, 0.5, 0.85):
            if BE.walls_on_ray(r["plane"] + sgn * 0.01, ax, sgn, lo_l + (hi_l - lo_l) * fl, lo_y + (hi_y - lo_y) * fy, 30.0):
                return False
    return True


for r in raw:
    r["ext"] = is_ext(r)

# merge duplicates / adjacent pieces
groups = collections.OrderedDict()
for r in raw:
    k = (r["id"], r["face"], r["plane"], r["cut"], r["at"])
    g = groups.get(k)
    if g is None:
        groups[k] = dict(r, walls={r["wall"].split(" Collider")[0]}, spans=[r["span"]])
    else:
        g["walls"].add(r["wall"].split(" Collider")[0]); g["spans"].append(r["span"])
        g["width"] = max(g["width"], r["width"])
        if r["dR"] < g["dR"]:
            g.update(q=r["q"], dR=r["dR"], blk=r["blk"])
        g["ext"] = g["ext"] or r["ext"]

rows = []
for g in groups.values():
    q = g["q"]
    if g["cut"].startswith("y") and not g["ext"]:
        n = [0, 0, 0]; n[g["ax"]] = g["sgn"] * 0.1
        vis = not y_hidden(q[0] + n[0], q[2] + n[2], g["at"])
    else:
        vis = True
    if vis:  # share of the cut line (across the merged spans) not buried inside another wall box (junctions)
        pts = []
        for lo_, hi_ in g["spans"]:
            for i in range(12):
                t = lo_ + (hi_ - lo_) * (i + 0.5) / 12
                pp = list(q)
                if g["cut"].startswith("lat"):
                    pp[1] = t
                else:
                    pp[g["la"]] = t
                pts.append(pp)
        cax = g["la"] if g["cut"].startswith("lat") else 1
        free = [pp for pp in pts if not in_wall(pp, CORNER, cax)]
        vis = round(len(free) / len(pts), 2) if pts else 1.0
        if not g["ext"] and free:  # y cuts at 'at'; lat cuts per point height (strips inside a slab band are hidden)
            n = [0, 0, 0]; n[g["ax"]] = g["sgn"] * 0.1
            free = [pp for pp in free if not y_hidden(pp[0] + n[0], pp[2] + n[2], g["at"] if g["cut"].startswith("y") else pp[1])]
            vis = round(len(free) / len(pts), 2)
    g["vis"] = vis
    rows.append(g)

# floors / ceilings
for e, L, (lo, hi), p, R in lights:
    ff = face_fades(e, L)
    for kind, slo, shi, sname in slabs:
        yv = shi[1] if kind == "floor" else slo[1]
        if not (lo[1] < yv < hi[1]):
            continue
        if (kind == "floor" and p[1] <= yv + 0.02) or (kind == "ceil" and p[1] >= yv - 0.02):
            continue
        for ax in (0, 2):
            la = 2 - ax
            for sgn, v in ((-1, lo[ax]), (1, hi[ax])):
                if not (slo[ax] + 0.02 < v < shi[ax] - 0.02):
                    continue
                a, b = max(slo[la], lo[la]), min(shi[la], hi[la])
                if b - a < 0.05:
                    continue
                pts = [a + (b - a) * (i + 0.5) / 20 for i in range(20)]
                off = 0.01 if kind == "floor" else -0.01
                visible = [t for t in pts if not in_wall([v if ax == 0 else t, yv + off, t if ax == 0 else v], CORNER, ax)]
                if not visible:
                    continue
                best = None
                cands = []
                for t in visible:
                    qq = [0, 0, 0]; qq[ax] = v; qq[la] = t; qq[1] = yv + off
                    cands.append((math.dist(p, qq), qq, crossed(p, qq)))
                # junction sliver: a sample lit through a wall while within 5 cm of a wall box sits in the corner the
                # blocking wall makes with the line (doorway threshold beside a jamb); it is not the edge's nearest point
                open_ = [c for c in cands if not (c[2] > 0 and in_wall(c[1], 0.05))] or cands
                best = min(open_, key=lambda c: c[0])
                if R and best[0] >= R:
                    continue
                thr = min(abs(v - slo[ax]), abs(v - shi[ax])) < 0.12
                rows.append(dict(id=e["id"], name=e["name"][:26], sh="on" if L.get("Cast Shadows") else "off", thr=thr,
                                 walls={sname}, surf=kind, face=("+" if sgn > 0 else "-") + "xyz"[ax], plane=round(yv, 3),
                                 cut=("+" if sgn > 0 else "-") + "xyz"[ax] + " box face", at=round(v, 3),
                                 dR=best[0] / R if R else 0, fade=ff[(ax, sgn)], blk=best[2], ext=False,
                                 vis=len(visible) / 20.0, q=best[1]))

for r in rows:
    hidden_by_shadow = r["sh"] == "on" and r["blk"] > 0
    soft = r["fade"] >= SOFT
    if r["vis"] is False or r["vis"] == 0:
        r["cls"] = "HIDDEN (in slab/wall junction)"
    elif hidden_by_shadow:
        r["cls"] = "hidden (shadowed, wall between)"
    elif soft:
        r["cls"] = "soft (fade %.2f)" % r["fade"]
    elif r["dR"] >= 0.8:
        r["cls"] = "faint"
    elif r["blk"] > 0:
        r["cls"] = "LEAK through wall"
    elif r["dR"] < 0.5:
        r["cls"] = "STRONG"
    else:
        r["cls"] = "visible"
for r in rows:
    if r["cls"] in ("STRONG", "visible", "LEAK through wall", "faint"):
        if r["surf"] == "wall" and r.get("width", 9) <= 0.3:
            r["cls"] += " jamb/end"
        elif r["surf"] != "wall" and r.get("thr"):
            r["cls"] += " threshold"
order = {"STRONG": 0, "LEAK through wall": 1, "visible": 2, "faint": 3}
rows.sort(key=lambda r: (order.get(r["cls"], order.get(r["cls"].rsplit(" ", 1)[0], 8) + 0.5), r["dR"]))
cnt = collections.Counter((r["surf"] if r["surf"] == "wall" else "floor/ceil", r["cls"].split(" (")[0]) for r in rows)

out = ["# Bounds hard-edge inventory (inv.py on %s)" % B.repo if hasattr(B, "repo") else "# Bounds hard-edge inventory", "",
       "%d merged edges from %d lights (raw box_edges rows %d). Counts by class:" % (len(rows), len({r['id'] for r in rows}), len(raw)), ""]
out += ["- %s %s: %d" % (k[0], k[1], v) for k, v in sorted(cnt.items())]
out += ["", "| class | id | light | sh | surface | face | plane | cut | at | d/R | fade | blk | ext | vis | walls/slab |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
for r in rows:
    if r["cls"].startswith(("HIDDEN", "hidden")) and "--hidden" not in argv:
        continue
    out.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %.2f | %.2f | %s | %s | %s | %s |" % (
        r["cls"], r["id"], r["name"], r["sh"], r["surf"], r["face"], r["plane"], r["cut"], r["at"], r["dR"], r["fade"],
        r["blk"], "EXT" if r["ext"] else "", r["vis"] if isinstance(r["vis"], float) else ("" if r["vis"] else "no"),
        ", ".join(sorted(r["walls"]))[:60]))
txt = "\n".join(out) + "\n"
open(argv[argv.index("--md") + 1], "w", encoding="utf-8").write(txt)
print("\n".join(out[:12]))
if "--dbg" in argv:
    hid = sorted([r for r in rows if r["cls"].startswith("HIDDEN") and r["surf"] == "wall" and r["blk"] == 0], key=lambda r: r["dR"])
    for r in hid[:25]:
        q = r["q"]
        inw = [w.get("name", "?") for w in B.WALLS if all(w["min"][i] <= q[i] <= w["max"][i] for i in range(3))]
        print(r["id"], r["face"], r["plane"], r["cut"], r["at"], "dR %.2f" % r["dR"], sorted(r["walls"])[:2], "q", [round(x, 3) for x in q], "in", inw[:3])
if "--why" in argv:
    ids = {int(x) for x in argv[argv.index("--why") + 1].split(",")}
    for r in rows:
        if r["id"] in ids and r["cls"].startswith(("LEAK", "STRONG", "visible", "soft")):
            p = [L for L in lights if L[0]["id"] == r["id"]][0][3]; q = r["q"]
            names = []
            for w in list(B.WALLS) + [dict(s, name=n) for s, n in zip(SLABBOX, [x[3] for x in slabs])]:
                t0, t1 = 0.0, 1.0; ok = True
                for i in range(3):
                    d = q[i] - p[i]
                    if abs(d) < 1e-9:
                        if not (w["min"][i] < p[i] < w["max"][i]): ok = False; break
                    else:
                        a, b = (w["min"][i] - p[i]) / d, (w["max"][i] - p[i]) / d
                        t0, t1 = max(t0, min(a, b)), min(t1, max(a, b))
                        if t0 > t1: ok = False; break
                if ok and t1 - t0 > 1e-4 and t0 < 0.995:
                    names.append("%s x%.2f..%.2f z%.2f..%.2f y%.2f..%.2f" % (w.get("name", "?"), w["min"][0], w["max"][0], w["min"][2], w["max"][2], w["min"][1], w["max"][1]))
            print(r["cls"], r["id"], r["surf"], r["face"], r["plane"], r["cut"], r["at"], "q", [round(x, 2) for x in q], "p", [round(x, 2) for x in p], "| via", names)
