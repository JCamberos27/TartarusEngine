"""Light Bounds vs walls (W1 Atlas; Jacob #1 + Briareos sweep #4: Bounds faces short of the wall leave the wall unlit).
    python -I bounds_audit.py <repo> [--md out.md] [--json out.json]
For every house light (descendant of 35585) with Bounds, each horizontal face (-x, +x, -z, +z) is checked against the walls
the light faces: rays from the light's own coordinate along the face normal, at lateral samples across the box (0.25 m step)
and 3 heights on the light's floor, hit the nearest active wall box collider (geo.walls). The room wall for the face = the
most common near plane (0.03 m bins); rays through doorways hit something farther and are ignored.
Bounds Fade is applied INWARD (LightBuffer.glsl BoundsAtten: weight = inside distance / fade, 0 at the face), so the face
must sit at least `fade` past the wall's near plane to light the wall surface fully, and must not pass the wall's far plane
(that lights the neighbour's side). Target = near + fade (+0.005). Status per face:
  OK     near + fade - 0.01 <= face <= far
  SHORT  face < near + fade - 0.01           (unlit strip at the wall; Jacob #1)
  OVER   face > far                           (lights past the wall)
  OPEN   no room wall found (face in open air / mostly doorway): listed, needs a look
Y faces (floor/ceiling) are not checked here (see fixture_audit / the upper-floor note)."""
import sys, json, os, collections
ARGV = list(sys.argv)
repo = sys.argv[1]
sys.argv = [sys.argv[0], repo]
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)  # geo.py lives beside this script
sys.path.insert(0, repo + "/tools/gi")
import gi_audit as G
import geo

HOUSE = 35585

# LISTED EXCEPTIONS (gate = "listed exceptions only": every non-OK face must match (id, face, status)). The list is DATA in the
# repo under test: <repo>/tools/gi/bounds_exceptions.json, committed with the scene change it excuses, so a gate result is
# reproducible from a sha.
_EXC = os.path.join(repo, "tools", "gi", "bounds_exceptions.json")
LISTED = {(r["id"], r["face"], r["status"]): r["reason"] for r in json.load(open(_EXC, encoding="utf-8"))["exceptions"]}     if os.path.exists(_EXC) else {}

doc =json.load(open(repo + "/project/scenes/Forest.json", encoding="utf-8"))
S = G.Scene(doc)
_, sc = geo.load(repo)
WALLS = geo.walls(sc)
by = {e["id"]: e for e in doc["empties"]}


def in_house(e):
    while e:
        if e["id"] == HOUSE:
            return True
        e = by.get(e.get("parentId"))
    return False


def wbox(e):
    """world AABB of a light's Bounds (house is rotated by multiples of 90 deg about Y)."""
    L = e["Light"]; M = S.world(by[e["parentId"]]); c = L["Bounds Center"]; s = L["Bounds Size"]
    cs = [[c[0] + a * s[0] / 2, c[1] + b * s[1] / 2, c[2] + g * s[2] / 2] for a in (-1, 1) for b in (-1, 1) for g in (-1, 1)]
    w = [[sum(M[r][k] * p[k] for k in range(3)) + M[r][3] for r in range(3)] for p in cs]
    return [min(p[r] for p in w) for r in range(3)], [max(p[r] for p in w) for r in range(3)]


def first_wall(o, ax, sgn, lat, y, umax, with_wall=False):
    """nearest wall along +/-ax from o: returns (near, far) coordinates (world) [+ the wall dict with with_wall], or None."""
    la = 2 - ax  # lateral axis (x<->z)
    best = None
    for w in WALLS:
        if not (w["min"][1] <= y <= w["max"][1] and w["min"][la] - 1e-3 <= lat <= w["max"][la] + 1e-3):
            continue
        near, far = (w["min"][ax], w["max"][ax]) if sgn > 0 else (w["max"][ax], w["min"][ax])
        u = (near - o) * sgn
        if u < -0.02 or u > umax:
            continue
        if (far - o) * sgn <= 0.02:  # wall behind / around the origin
            continue
        if best is None or u < best[0]:
            best = (u, near, far, w)
    return None if best is None else (best[1:] if with_wall else best[1:3])


def audit():
    rows = []
    for e in doc["empties"]:
        L = e.get("Light")
        if not L or L.get("Type") not in ("Point", "Spot") or not L.get("Bounds") or not in_house(e):
            continue
        lo, hi = wbox(e); fade = float(L.get("Bounds Fade", 0.05))
        W = S.world(e); p = [W[r][3] for r in range(3)]
        ys = [y for y in (lo[1] + 0.4, lo[1] + 1.2, min(hi[1] - 0.25, lo[1] + 2.3)) if lo[1] < y < hi[1]]
        for ax in (0, 2):
            la = 2 - ax
            lats = []
            t = lo[la] + 0.15
            while t <= hi[la] - 0.15 + 1e-6:
                lats.append(t); t += 0.25
            if not lats:
                lats = [(lo[la] + hi[la]) / 2]
            for sgn in (-1, 1):
                face = hi[ax] if sgn > 0 else lo[ax]
                o = min(max(p[ax], lo[ax] + 0.01), hi[ax] - 0.01)
                umax = (face - o) * sgn + 1.5
                hits = []
                for y in ys:
                    for lt in lats:
                        h = first_wall(o, ax, sgn, lt, y, umax, True)
                        if h:
                            hits.append(h)
                n = len(ys) * len(lats)
                bins = collections.Counter(round(h[0] / 0.03) for h in hits)
                name = ("+" if sgn > 0 else "-") + "xyz"[ax]
                row = {"id": e["id"], "name": e["name"], "type": L["Type"], "shadows": bool(L.get("Cast Shadows")),
                       "face": name, "value": round(face, 3), "fade": fade, "samples": n, "hits": len(hits)}
                if not bins or bins.most_common(1)[0][1] < max(2, 0.3 * n):
                    row.update(status="OPEN", near=None, far=None, target=None, delta=None)
                else:
                    b = bins.most_common(1)[0][0]
                    sel = [h for h in hits if round(h[0] / 0.03) == b]
                    near = sum(h[0] for h in sel) / len(sel)
                    far = sorted(h[1] for h in sel)[len(sel) // 2]
                    target = near + sgn * (fade + 0.005)
                    fu, nu, ffu = face * sgn, near * sgn, far * sgn
                    if fu < nu + fade - 0.01:
                        st = "SHORT"
                    elif fu > ffu + 1e-3:
                        st = "OVER"
                    else:
                        st = "OK"
                    if st != "OK" and (target * sgn) > ffu - 0.005:
                        st += "-THIN"  # wall thinner than the fade: needs a look
                    row.update(status=st, near=round(near, 3), far=round(far, 3), target=round(target, 3),
                               delta=round((face - target) * sgn, 3), share=round(len(sel) / n, 2))
                    # share<0.5 blind spot (W1 step 5, 2026-11-24): the mode wall alone decides the status, so a face
                    # spanning two walls (L-room, offset wall, stub) is judged on one and blind to the other, and the
                    # label can flip when the lateral lattice moves. Judge every other wall bin with >= 15% of the
                    # samples too; half-height walls (< 1.5 m tall: counters) are skipped. NOT gated: report only.
                    sec = []
                    for b2, c2 in bins.most_common():
                        if b2 == b or c2 < max(2, 0.15 * n):
                            continue
                        s2 = [h for h in hits if round(h[0] / 0.03) == b2]
                        ws = sorted(s2, key=lambda h: h[2]["max"][1] - h[2]["min"][1])[len(s2) // 2][2]
                        if ws["max"][1] - ws["min"][1] < 1.5:
                            continue
                        n2 = sum(h[0] for h in s2) / len(s2); f2 = sorted(h[1] for h in s2)[len(s2) // 2]
                        st2 = "SHORT" if fu < n2 * sgn + fade - 0.01 else ("OVER" if fu > f2 * sgn + 1e-3 else "OK")
                        sec.append(dict(status=st2, near=round(n2, 3), far=round(f2, 3), share=round(c2 / n, 2),
                                        wall=str(ws.get("name", ""))[:32]))
                    row["second"] = sec
                rows.append(row)
    return rows


if __name__ == "__main__":
    rows = audit()
    bad = [r for r in rows if r["status"] != "OK"]
    seen = {(r["id"], r["face"], r["status"]) for r in bad}
    unl = [r for r in bad if (r["id"], r["face"], r["status"]) not in LISTED]
    stale = sorted(k for k in LISTED if k not in seen)
    gate = ("Listed exceptions matched: %d; UNLISTED: %d; listed but not seen: %d -> GATE %s." % (
        len(bad) - len(unl), len(unl), len(stale), "PASS" if not unl else "FAIL"))
    out = ["# Light Bounds vs walls (bounds_audit.py)", "",
           "Faces checked: %d on %d lights; not OK: %d (SHORT %d, OVER %d, OPEN %d)." % (
               len(rows), len({r["id"] for r in rows}), len(bad), sum(r["status"].startswith("SHORT") for r in rows),
               sum(r["status"].startswith("OVER") for r in rows), sum(r["status"] == "OPEN" for r in rows)), "",
           "| id | light | type | sh | face | value | wall near | wall far | target | face - target | share | status |",
           "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in sorted(rows, key=lambda r: (r["status"] == "OK", r["id"], r["face"])):
        out.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["id"], r["name"], r["type"], "on" if r["shadows"] else "off", r["face"], r["value"], r.get("near"), r.get("far"),
            r.get("target"), r.get("delta"), r.get("share", ""), r["status"]))
    # secondary walls (share<0.5 blind spot): informational, between the table and the gate block
    sec_rows = [(r, s) for r in rows for s in r.get("second", []) if s["status"] != "OK" and s["status"] != r["status"].split("-")[0]]
    sec_out = ["", "## Secondary walls (a face judged on its mode wall only; other walls with >= 15%% of samples disagree). NOT gated: %d" % len(sec_rows), "",
               "| id | light | sh | face | value | mode status (share) | 2nd wall | near | far | share | 2nd status |", "|---|---|---|---|---|---|---|---|---|---|---|"]
    sec_out += ["| %s | %s | %s | %s | %s | %s (%s) | %s | %s | %s | %s | %s |" % (r["id"], r["name"], "on" if r["shadows"] else "off", r["face"],
                r["value"], r["status"], r.get("share", ""), s["wall"], s["near"], s["far"], s["share"], s["status"]) for r, s in sec_rows]
    out += sec_out
    # gate block AFTER the table, so the table part stays line-for-line diffable against older gate reports
    tail = ["", "## Gate (listed exceptions)", "", gate]
    tail += ["- UNLISTED: %s %s %s (%s)" % (r["id"], r["face"], r["status"], r["name"]) for r in unl]
    tail += ["- listed, not seen (fixed or changed; prune): %s %s %s" % k for k in stale]
    tail += ["- listed %s %s %s: %s" % (k + (LISTED[k],)) for k in sorted(seen) if k in LISTED]
    out += tail
    txt = "\n".join(out) + "\n"
    if "--md" in ARGV:
        open(ARGV[ARGV.index("--md") + 1], "w", encoding="utf-8").write(txt)
    if "--json" in ARGV:
        json.dump(rows, open(ARGV[ARGV.index("--json") + 1], "w"), indent=1)
    print(out[2])
    for l in out[4:len(out) - len(tail) - len(sec_out)]:
        if not l.endswith("| OK |"):
            print(l)
    for l in sec_out:
        print(l)
    print(gate)
    for l in tail[4:]:
        if not l.startswith("- listed "):
            print(l)
    sys.exit(1 if unl else 0)
