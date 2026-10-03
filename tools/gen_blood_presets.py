"""Regenerates src/Game/Combat/BloodFxPresets.inc from the KriptoFX "Volumetric Blood Fluids" prefabs.

The package itself is not in git (docs/BLOOD_FX.md); this script reads its Prefabs/*.prefab (Unity YAML)
and writes the numbers the game needs - each prefab's sprays (sim, transform, playback) and floor decals
(decal set, box, height response, reveal curve) - converted to this engine's right-handed axes (z negated).

    python tools/gen_blood_presets.py "<path to VolumetricBloodFX>"
"""
import glob, os, re, sys

SCRIPTS = {"36b8a788ff17aab40b283e664f0304da": "BloodSettings", "49633916cd5f89f4b9ce3e918ec8509e": "ManualAnim",
           "e19be2280f3477b4ab19c882db70e553": "ShaderProps", "ece88c1f58dc84b4da85e14370eb3148": "DecalSettings"}
# Unity sim folder -> imported sim name (BloodFxImport::ImportPackage).
SIM_OF_DIR = {"Blood1": "blood1", "Blood2/Left": "blood2_left", "Blood2/Right": "blood2_right", "Blood2/Vertical": "blood2_vertical"}
for _i in range(3, 10):
    SIM_OF_DIR["blood%d" % _i] = "blood%d" % _i
# blood13.prefab's third sub-spray points at a mesh the package doesn't ship; its material is blood7's.
MISSING_MESH_FIX = {"8b293307e3aec0141845b9f675b25b47": "BloodResources/blood7/blood_mesh.fbx"}


def num(s, key):
    m = re.search(r"^\s*" + key + r": (-?[\d.eE+-]+)\s*$", s, re.M)
    return float(m.group(1)) if m else None


def vec(s, key):
    m = re.search(key + r": \{([^}]*)\}", s)
    return {k.strip(): float(v) for k, v in (p.split(":") for p in m.group(1).split(","))} if m else None


def curve(s, key):
    m = re.search(r"^  " + key + r":\n(.*?)(?=^  \w|\Z)", s, re.S | re.M)
    keys = []
    conv = lambda x: 1e30 if x == "Infinity" else (-1e30 if x == "-Infinity" else float(x))
    pat = r"time: (-?[\d.eE+-]+)\s+value: (-?[\d.eE+-]+)\s+inSlope: (-?[\w.eE+-]+)\s+outSlope: (-?[\w.eE+-]+)"
    for k in re.finditer(pat, m.group(1)):
        keys.append((float(k.group(1)), float(k.group(2)), conv(k.group(3)), conv(k.group(4))))
    return keys


def f(x):
    s = "%.6g" % x
    return s + ("f" if ("." in s or "e" in s) else ".0f")


def curve_cpp(keys):
    return "{%d, {%s}}" % (len(keys), ", ".join("{%s, %s, %s, %s}" % tuple(f(v) for v in k) for k in keys))


def trs(t):
    x, y, z, w = t["r"]
    sx, sy, sz = t["s"]
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[r[i][0] * sx, r[i][1] * sy, r[i][2] * sz, t["p"][i]] for i in range(3)] + [[0, 0, 0, 1]]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def mirror(m):
    """S M S with S = diag(1, 1, -1, 1): a Unity (left-handed) transform in this engine's axes."""
    sg = [1, 1, -1, 1]
    return [[m[i][j] * sg[i] * sg[j] for j in range(4)] for i in range(4)]


def main(root):
    guid2path = {}
    for m in glob.glob(root + "/**/*.meta", recursive=True):
        g = re.search(r"^guid: (\w+)", open(m, encoding="latin-1").read(), re.M)
        if g:
            guid2path[g.group(1)] = os.path.relpath(m[:-5], root).replace("\\", "/")
    guid2path.update(MISSING_MESH_FIX)

    out = []
    for pf in sorted(glob.glob(root + "/Prefabs/*.prefab"), key=lambda p: os.path.basename(p).lower()):
        name = os.path.splitext(os.path.basename(pf))[0]
        if name == "AttachedBloodDecal":
            continue
        txt = open(pf, encoding="latin-1").read()
        docs = {m.group(2): (int(m.group(1)), m.group(3))
                for m in re.finditer(r"--- !u!(\d+) &(-?\d+)[^\n]*\n(.*?)(?=\n--- |\Z)", txt, re.S)}
        tr, items = {}, {}
        for fid, (cls, body) in docs.items():
            gm = re.search(r"m_GameObject: \{fileID: (-?\d+)\}", body)
            if not gm:
                continue
            go = gm.group(1)
            it = items.setdefault(go, {})
            if cls == 4:
                r = vec(body, "m_LocalRotation")
                p = vec(body, "m_LocalPosition")
                sc = vec(body, "m_LocalScale")
                tr[go] = dict(fid=fid, father=re.search(r"m_Father: \{fileID: (-?\d+)\}", body).group(1),
                              r=(r["x"], r["y"], r["z"], r["w"]), p=(p["x"], p["y"], p["z"]), s=(sc["x"], sc["y"], sc["z"]))
            elif cls == 23:
                mg = re.search(r"m_Materials:\s*\n\s*- \{fileID: \d+, guid: (\w+)", body)
                it["mat"] = guid2path.get(mg.group(1)) if mg else None
            elif cls == 114:
                kind = SCRIPTS.get(re.search(r"m_Script: \{fileID: \d+, guid: (\w+)", body).group(1))
                if kind == "BloodSettings":
                    it[kind] = dict(speed=num(body, "AnimationSpeed"))
                elif kind == "ManualAnim":
                    it[kind] = dict(frames=num(body, "FramesCount"), limit=num(body, "TimeLimit"),
                                    offset=num(body, "OffsetFrames"), curve=curve(body, "AnimationSpeed"))
                elif kind == "ShaderProps":
                    it[kind] = dict(graph=num(body, "GraphTimeMultiplier"), curve=curve(body, "FloatCurve"))
                elif kind == "DecalSettings":
                    it[kind] = dict(hmax=num(body, "TimeHeightMax"), hmin=num(body, "TimeHeightMin"),
                                    smax=vec(body, "TimeScaleMax"), smin=vec(body, "TimeScaleMin"),
                                    omax=vec(body, "TimeOffsetMax"), omin=vec(body, "TimeOffsetMin"),
                                    curve=curve(body, "TimeByHeight"))
        root_go = next(g for g, t in tr.items() if t["father"] == "0")
        fid2go = {t["fid"]: g for g, t in tr.items()}

        def world(go):  # prefab space: the root's own transform included (it only scales)
            m = trs(tr[go])
            fa = tr[go]["father"]
            while fa != "0":
                g = fid2go[fa]
                m = mul(trs(tr[g]), m)
                fa = tr[g]["father"]
            return m
        rs = tr[root_go]["s"][0]
        speed = items[root_go]["BloodSettings"]["speed"]
        sprays, decals = [], []
        for go, it in items.items():
            mat = it.get("mat")
            if not mat or go == root_go:
                continue
            t = tr[go]
            parent = mirror(world(fid2go[t["father"]]))
            whole = mirror(world(go))
            folder = os.path.dirname(mat).replace("BloodResources/", "")
            x, y, z, w = t["r"]
            qe = (-x, -y, z, w)  # S R S with S = diag(1, 1, -1)
            pe = (t["p"][0], t["p"][1], -t["p"][2])
            if "ManualAnim" in it:
                a = it["ManualAnim"]
                lin = len(a["curve"]) == 2 and abs(a["curve"][0][1]) < 1e-6 and abs(a["curve"][1][1] - 1) < 1e-6
                assert lin and a["offset"] == 0, name + ": non-linear playback"
                sprays.append("{\"%s\", {%s}, %s, %s}" % (
                    SIM_OF_DIR[folder], ", ".join(f(whole[i][j]) for i in range(3) for j in range(4)), f(a["limit"]), f(a["frames"])))
            elif "DecalSettings" in it:
                d = it["DecalSettings"]
                sp = it["ShaderProps"]
                dset = "char" if os.path.basename(mat) == "DecalPoint.mat" else folder.replace("Blood2/", "blood2_").lower()
                o = lambda v: (v["x"], v["y"], -v["z"])
                vals = ((dset, ", ".join(f(parent[i][j]) for i in range(3) for j in range(4))) + tuple(map(f, qe)) + tuple(map(f, pe)) + tuple(map(f, t["s"])) + (f(d["hmax"]), f(d["hmin"]))
                        + tuple(map(f, (d["smin"]["x"], d["smin"]["y"], d["smin"]["z"])))
                        + tuple(map(f, (d["smax"]["x"], d["smax"]["y"], d["smax"]["z"])))
                        + tuple(map(f, o(d["omin"]))) + tuple(map(f, o(d["omax"])))
                        + (curve_cpp(d["curve"]), f(sp["graph"]), curve_cpp(sp["curve"])))
                decals.append("{\"%s\", {%s}, {%s, %s, %s, %s}, {%s, %s, %s}, {%s, %s, %s}, %s, %s, {%s, %s, %s}, {%s, %s, %s}, "
                              "{%s, %s, %s}, {%s, %s, %s}, %s, %s, %s}" % vals)
        out.append("    {\"%s\", %s, %s,\n     {%s},\n     {%s}}," % (
            name.lower(), f(rs), f(speed), ",\n      ".join(sprays), ",\n      ".join(decals)))
    src = ("// Generated by tools/gen_blood_presets.py from the KriptoFX \"Volumetric Blood Fluids\" prefabs - do not edit.\n"
           "// Engine axes (Unity's z negated). Matrices are 3x4 row-major, into the prefab's frame (its root's scale included).\n"
           "// Preset: name, root scale, AnimationSpeed, sprays, floor decals. Spray: sim, sim -> prefab matrix, TimeLimit, FramesCount.\n"
           "// Decal: set, parent -> prefab matrix, local rotation xyzw, position, scale, TimeHeightMax, TimeHeightMin, TimeScaleMin,\n"
           "//        TimeScaleMax, TimeOffsetMin, TimeOffsetMax, TimeByHeight, GraphTimeMultiplier, FloatCurve (the reveal / dry-up).\n"
           + "\n".join(out) + "\n")
    dst = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "Game", "Combat", "BloodFxPresets.inc")
    open(dst, "w", newline="\n").write(src)
    print("wrote", os.path.normpath(dst), "-", len(out), "presets")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else r"C:/Users/jacob/OneDrive/Desktop/ASSETS TO IMPORT/VolumetricBloodFX")
