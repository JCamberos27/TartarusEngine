"""House light -> fixture audit (W1 Atlas, Jacob's lamp-1 look, rules from ISSUES.md / SM).
    python -I fixture_audit.py <repo> [--md out.md]
RULES (each a column, FAIL = must fix before the re-bake):
  OFFSET     light within 5 cm of its fixture's emitter point (table TARGET; wall lamps: within 5 cm of the mesh box)
  NOFIX      every light has a fixture mesh (fixture box within 0.6 m of the light)
  EMISSIVE   the fixture has a Light Circuit on the LIGHT's circuit, Glow > 0, and a material that glows
             (assigned .mat or the FBX meta's materialRemap .mat: EmissiveStrength > 0 and an emissive map or colour)
  OWNSHADOW  a shadow-casting light's fixture must not cast shadows ("castShadows": 0 on the fixture model)
Fixture boxes = the pack's Lamp_* prefab colliders (engine space, metres; FBX checked by hand for Lamp_bedroom/ceiling_circle).
"""
import sys, json, glob, os, math
repo = sys.argv[1]
sys.path.insert(0, repo + "/tools/gi")
import gi_audit as G
doc = json.load(open(repo + "/project/scenes/Forest.json", encoding="utf-8"))
sc = G.Scene(doc)
PACK = repo + "/project/assets/Environments/SuburbNeighborhoodHousePack/"
LOCAL = {}
for f in glob.glob(PACK + "Prefabs/Props_*/Lamp_*.prefab"):
    d = json.load(open(f))
    lo, hi = [9.0] * 3, [-9.0] * 3
    for c in ("models", "empties"):
        for e in d.get(c, []):
            col = e.get("collider")
            if not col or "halfExtents" not in col: continue
            ctr = col.get("center", [0, 0, 0]); h = col["halfExtents"]
            for k in range(3):
                lo[k] = min(lo[k], e["position"][k] + ctr[k] - h[k]); hi[k] = max(hi[k], e["position"][k] + ctr[k] + h[k])
    if lo[0] < 9: LOCAL[os.path.basename(f)[:-7]] = (lo, hi)
LOCAL["Lamp_wall_desk@Lamp_wall_desk_lamp"] = LOCAL["Lamp_wall_desk"]
# emitter point per fixture type, model-local (x, y, z); None = wall lamp: within 5 cm of its box, on the room side
TARGET = {
    "Lamp_bedroom": (0.0, -0.204, 0.0),        # flush dome: 2 cm under the bowl (bottom -0.184)
    "Lamp_ceiling_circle": (0.0, -0.05, 0.0),  # recessed disc: 3 cm under the lens (bottom -0.020)
    "Lamp_chandelier": (0.027, -0.85, 0.0),    # 2.5 cm under the shade cluster (-0.825..-0.575), its x centre
    "Lamp_kitchen_A": (0.0, -0.75, 0.0),       # in the shade (-0.85..-0.65)
    "Lamp_kitchen_B": (0.0, -0.42, 0.0),       # in the shade (-0.52..-0.32)
    "Lamp_stand": (0.0, 1.20, 0.0),            # bulb low in the shade (1.16..1.36)
    "Lamp_table_A": (0.0, 0.256, 0.0), "Lamp_table_B": (0.0, 0.144, 0.0), "Lamp_floor": (0.0, 0.198, 0.0),  # pack bulbs in the shade
}
TOL, NEAR = 0.05, 0.6

def kind(m): return os.path.basename(m.get("path", "")).rsplit(".", 1)[0]
def inv_apply(M, p):
    d = [p[i] - M[i][3] for i in range(3)]
    return [sum(M[i][j] * d[i] for i in range(3)) for j in range(3)]
def apply(M, p): return [sum(M[i][j] * p[j] for j in range(3)) + M[i][3] for i in range(3)]
def boxdist(q, lo, hi): return math.sqrt(sum(max(lo[k] - q[k], 0, q[k] - hi[k]) ** 2 for k in range(3)))

_mat = {}
def glows(path):
    if path not in _mat:
        try:
            j = json.load(open(repo + "/project/" + path, encoding="utf-8"))
            p = j.get("properties", j)
            s = p.get("_EmissiveStrength", 0); mp = p.get("_EmissiveMap", ""); c = p.get("_EmissiveColor", [0, 0, 0])
            _mat[path] = bool(s and s > 0 and (mp or any(x > 0 for x in c[:3])))
        except Exception:
            _mat[path] = None
    return _mat[path]
def remap_glows(m):
    try:
        meta = json.load(open(repo + "/project/" + m["path"] + ".meta", encoding="utf-8"))
        return any(glows(p) for p in meta.get("materialRemap", {}).values())
    except Exception:
        return False

fix = [m for m in doc["models"] if kind(m) in LOCAL and kind(m) != "Lamp_wall_desk"]
HOUSE = lambda p: -16 < p[0] < 16 and 18 < p[2] < 34
rows, paired = [], {}
for l in sorted(sc.lights(), key=lambda l: (str(l["circuit"]), l["id"])):
    if not l["circuit"] or not HOUSE(l["pos"]): continue
    best = None
    for m in fix:
        w = sc.world(m); q = inv_apply(w, l["pos"]); lo, hi = LOCAL[kind(m)]
        dd = boxdist(q, lo, hi)
        if best is None or dd < best[0]: best = (dd, m, w, q)
    dd, m, w, q = best
    r = {"l": l, "m": None, "fails": []}
    if dd > NEAR:
        r["fails"].append("NOFIX")
        r["note"] = "nearest fixture %d %s %.2f m away" % (m["id"], kind(m), dd)
    else:
        k = kind(m); r["m"] = m; paired.setdefault(m["id"], []).append(l["id"])
        t = TARGET.get(k)
        if t is None:
            off = dd; r["target"] = None
        else:
            off = math.dist(q, t); r["target"] = apply(w, t)
        r["off"] = off
        if off > TOL: r["fails"].append("OFFSET")
        lc = m.get("Light Circuit") or {}
        mats = [x.get("path", "") for x in m.get("materials", [])]
        r["glow"] = lc.get("Glow"); r["fcirc"] = lc.get("Circuit"); r["mats"] = [os.path.basename(x) for x in mats]
        mat_ok = any(glows(x) for x in mats) or (not mats and remap_glows(m)) or remap_glows(m)
        if not (lc and lc.get("Circuit") == l["circuit"] and (lc.get("Glow") or 0) > 0 and mat_ok):
            r["fails"].append("EMISSIVE")
        r["fshadow"] = m.get("castShadows", 1)
        if l["shadow"] and r["fshadow"] != 0: r["fails"].append("OWNSHADOW")
    rows.append(r)

out = []
P = out.append
P("# House light -> fixture audit (%s)" % os.path.basename(os.path.abspath(repo)))
P("")
P("Rules: OFFSET (> %d cm from the fixture's emitter point), NOFIX (no fixture mesh within %.1f m), EMISSIVE (fixture Light Circuit = the light's circuit, Glow > 0, glowing material), OWNSHADOW (shadow-casting light whose fixture still casts shadows). Script: house-polish/atlas_fixtures/fixture_audit.py." % (TOL * 100, NEAR))
P("")
P("| light | name | circuit | type | light shadow | fixture | fixture type | offset m | target (world) | fixture circuit / Glow | fixture castShadows | FAILS |")
P("|---|---|---|---|---|---|---|---|---|---|---|---|")
nf = 0
for r in rows:
    l, m = r["l"], r["m"]
    nf += bool(r["fails"])
    if m is None:
        P("| %d | %s | %s | %s | %s | - | - | - | %s | - | - | **%s** |" % (l["id"], l["name"], l["circuit"], l["type"], "ON" if l["shadow"] else "off", r["note"], ", ".join(r["fails"])))
        continue
    t = r["target"]
    P("| %d | %s | %s | %s | %s | %d | %s | %.3f | %s | %s / %s | %s | %s |" % (
        l["id"], l["name"], l["circuit"], l["type"], "ON" if l["shadow"] else "off", m["id"], kind(m), r["off"],
        "(%.3f, %.3f, %.3f)" % tuple(t) if t else "wall lamp (box)", r["fcirc"], r["glow"], r["fshadow"],
        "**" + ", ".join(r["fails"]) + "**" if r["fails"] else "pass"))
P("")
P("Lights: %d, failing: %d." % (len(rows), nf))
P("")
P("## Fixtures with no light of their own (info: they still need an emissive on a circuit, or stay dead on purpose)")
P("")
P("| fixture | type | world pos | Light Circuit / Glow | castShadows |")
P("|---|---|---|---|---|")
for m in fix:
    w = sc.world(m); p = [w[i][3] for i in range(3)]
    if HOUSE(p) and m["id"] not in paired:
        lc = m.get("Light Circuit") or {}
        P("| %d | %s | (%.2f, %.2f, %.2f) | %s / %s | %s |" % (m["id"], kind(m), *p, lc.get("Circuit"), lc.get("Glow"), m.get("castShadows", 1)))
s = "\n".join(out)
print(s)
if "--md" in sys.argv:
    open(sys.argv[sys.argv.index("--md") + 1], "w", encoding="utf-8").write(s + "\n")
