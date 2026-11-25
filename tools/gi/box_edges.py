"""Hard-edge inventory (W1 Atlas, fix-3): where a light's Bounds box edge lands on a WALL face that the light faces.
    python -I box_edges.py <repo> [--md out.md] [--all]   (default: exterior faces only = #1 siding)
For every light with Bounds (house and outside) and every vertical face of every active wall box collider (geo.walls):
the face plane must lie inside the box along its normal, the light must be on the face's front side (N.L > 0) and the box
must cut the face rectangle (box side/top/bottom strictly inside it) -> a rectangle edge on that wall face.
Exterior = nothing within 30 m in front of the face (open air). d/R = distance light -> nearest point of the cut / Range
(>= 1: never reached). 'blk' = walls crossed between the light and the cut point (shadowed lights: hidden; unshadowed: leak)."""
import sys, os, math
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import bounds_audit as B

def walls_on_ray(o, ax, sgn, lat, y, umax):
    la = 2 - ax; out = []
    for w in B.WALLS:
        if not (w["min"][1] <= y <= w["max"][1] and w["min"][la] - 1e-3 <= lat <= w["max"][la] + 1e-3):
            continue
        near, far = (w["min"][ax], w["max"][ax]) if sgn > 0 else (w["max"][ax], w["min"][ax])
        u = (near - o) * sgn
        if 0.02 < u <= umax and (far - o) * sgn > 0.02:
            out.append((u, near, far))
    return sorted(out)

ALL = "--all" in B.ARGV
lights = []
for e in B.doc["empties"]:
    L = e.get("Light")
    if L and L.get("Type") in ("Point", "Spot") and L.get("Bounds") and e.get("parentId") in B.by:
        W = B.S.world(e)
        lights.append((e, L, B.wbox(e), [W[r][3] for r in range(3)], float(L.get("Range", 0) or 0)))

def crossed(p, q):
    """walls whose box the segment p->q passes through (slab test), excluding boxes containing q's end within 2 cm."""
    n = 0
    for w in B.WALLS:
        t0, t1 = 0.0, 1.0
        for i in range(3):
            d = q[i] - p[i]
            if abs(d) < 1e-9:
                if not (w["min"][i] < p[i] < w["max"][i]): break
            else:
                a, b = (w["min"][i] - p[i]) / d, (w["max"][i] - p[i]) / d
                t0, t1 = max(t0, min(a, b)), min(t1, max(a, b))
                if t0 > t1: break
        else:
            if t1 - t0 > 1e-4 and t0 < 0.995:
                n += 1
    return n

rows = []
for e, L, (lo, hi), p, R in lights:
    fade = float(L.get("Bounds Fade", 0.05))
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
                if lo[la] > a + 0.02: cuts.append(("lat-", lo[la]))
                if hi[la] < b - 0.02: cuts.append(("lat+", hi[la]))
                if lo[1] > c + 0.02: cuts.append(("y-", lo[1]))
                if hi[1] < d - 0.02: cuts.append(("y+", hi[1]))
                if not cuts:
                    continue
                ml, my = (oa + ob) / 2, (oc + od) / 2
                ext = not [h for h in walls_on_ray(plane + sgn * 0.01, ax, sgn, ml, my, 30.0)]
                if not (ext or ALL):
                    continue
                for kind, v in cuts:
                    q = [0, 0, 0]; q[ax] = plane + sgn * 0.005
                    if kind.startswith("lat"):
                        q[la] = v; q[1] = min(max(p[1], oc), od)
                    else:
                        q[1] = v; q[la] = min(max(p[la], oa), ob)
                    dist = math.dist(p, q)
                    if R and dist >= R:
                        continue
                    rows.append(dict(id=e["id"], name=e["name"][:28], sh="on" if L.get("Cast Shadows") else "off",
                                     wall=w.get("name", "?")[:30], face=("+" if sgn > 0 else "-") + "xyz"[ax],
                                     plane=round(plane, 3), cut=kind, at=round(v, 3), dR=round(dist / R, 2) if R else "-",
                                     fade=fade, blk=crossed(p, q), ext=ext))
rows.sort(key=lambda r: (r["sh"] == "on" and r["blk"] > 0, r["blk"] > 0, r["dR"]))
out = ["# Bounds box edges on wall faces (box_edges.py%s)" % (" --all" if ALL else ", exterior only"), "",
       "%d edges from %d lights (%d with no wall between light and edge)." % (
           len(rows), len({r["id"] for r in rows}), sum(r["blk"] == 0 for r in rows)), "",
       "| id | light | sh | wall | face | plane | cut | at | d/R | fade | blk | ext |", "|---|---|---|---|---|---|---|---|---|---|---|---|"]
out += ["| %(id)s | %(name)s | %(sh)s | %(wall)s | %(face)s | %(plane)s | %(cut)s | %(at)s | %(dR)s | %(fade)s | %(blk)s | %(ext)s |" % r for r in rows]
txt = "\n".join(out) + "\n"
if "--md" in B.ARGV:
    open(B.ARGV[B.ARGV.index("--md") + 1], "w", encoding="utf-8").write(txt)
print("\n".join(out[:6 + min(len(rows), 40)]))
