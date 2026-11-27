import sys, json, re
sys.path.insert(0, sys.argv[1] + "/tools/gi")
import gi_audit as G

def load(repo):
    doc = json.load(open(repo + "/project/scenes/Forest.json", encoding="utf-8"))
    sc = G.Scene(doc)
    return doc, sc

WALLRE = re.compile(r"^(Wall_|Fireplace_withwall)")
def walls(sc):
    out = []
    for e in sc.ents:
        if not sc.active(e): continue
        b = sc.box_collider(e)
        if b is None: continue
        names = [e.get("name","")] + sc.ancestor_names(e)[:2]
        if not any(WALLRE.match(n) for n in names): continue
        mn, mx = b.aabb()
        out.append({"name": e.get("name",""), "id": e["id"], "min": mn, "max": mx})
    return out

def union_len(iv):
    iv = sorted(iv); tot = 0; cur = None
    for a, b in iv:
        if cur is None or a > cur[1]: 
            if cur: tot += cur[1]-cur[0]
            cur = [a, b]
        else: cur[1] = max(cur[1], b)
    if cur: tot += cur[1]-cur[0]
    return tot

def face_walls(vol, W, ylo_off=0.3, yhi_off=2.0, dmin=-0.6, dmax=1.5):
    mn, mx = vol["box"].aabb()
    y0, y1 = mn[1] + ylo_off, mn[1] + yhi_off
    res = {}
    for a in (0, 2):
        t = 2 - a
        for s in (1, -1):
            c = mx[a] if s > 0 else mn[a]
            hits = []
            for w in W:
                if w["max"][1] < y0 or w["min"][1] > y1: continue
                th = w["max"][a] - w["min"][a]
                if th > 0.45: continue
                lo, hi = max(w["min"][t], mn[t]), min(w["max"][t], mx[t])
                if hi - lo < 0.05: continue
                mid = 0.5 * (w["min"][a] + w["max"][a])
                d = s * (mid - c)
                if dmin <= d <= dmax:
                    inner = s * ((w["min"][a] if s > 0 else w["max"][a]) - c)
                    hits.append((d, inner, th, lo, hi, w["name"]))
            res[(a, s)] = hits
    return res
