"""fix4 OPTION (b), only if Jacob picks it: soften the master bedroom's pure red so its baked bounce keeps some luminance.

Owner: W1 Atlas (SM 16:3x MST). NOT part of the default fix4 run. Needs a GI re-bake to take effect in the bounce.
Why: #1216 made the 3 'Ceiling Master bedroom' lights pure red [1, 0, 0]. Red bounce carries only the red channel,
so the first bake with them (fix4) made the whole red room deep red and about 5x darker than the stale warm bake.
Change: those 3 lights' Color [1, 0, 0] -> [1, 0.25, 0.2]. Intensity 0.6, ColorTempK 0, shadows etc. unchanged.
Matching: name + parent 'House Lights' + local position (ids are not stable). Per component:
current == new -> already; current == old -> set; anything else -> FAIL. Idempotent. Text-safe: only these
values change; untouched numbers are written back byte-for-byte; CRLF/LF kept.

Usage:  python -I master_red_tint.py <repo dir | path/to/Forest.json> [--write]     (dry run without --write)
"""
# ---------------------------------------------------------------- shared helpers (kept inline: scripts run with python -I)
import sys, json, copy, json.encoder as _je
CATS = ("empties", "models", "boxes")
NL = "\n"
TOL = 2e-5                     # editor saves float32: 7.717 comes back as 7.716999816894531
DEF_FADE = [-1, -1, -1]        # a missing 'Bounds Fade +/-' == [-1,-1,-1] (the editor writes the default on save)
ID_BLOCK, ORD_BLOCK = range(36175, 36190), range(268769, 268784)   # Atlas's SM block (TODAY_6PM_PLAN.md)

class _RawFloat(float):
    """float that remembers its source text, so untouched numbers are written back byte-for-byte"""
    __slots__ = ("raw",)
    def __new__(cls, s):
        o = float.__new__(cls, s); o.raw = s; return o

def _floatstr(o, _repr=float.__repr__):
    if isinstance(o, _RawFloat): return o.raw
    if o != o or o in (float("inf"), float("-inf")): raise ValueError("non-finite float")
    return _repr(o)

def load(path):
    with open(path, "rb") as f: t = f.read().decode("utf-8")
    global NL
    NL = "\r\n" if "\r\n" in t else "\n"   # autocrlf checkouts are CRLF: keep whatever the file has
    return json.loads(t, parse_float=_RawFloat)

def dumps(d):
    enc = json.JSONEncoder(indent=2, sort_keys=True, ensure_ascii=False)
    it = _je._make_iterencode({}, enc.default, _je.py_encode_basestring, "  ", _floatstr,
                              enc.key_separator, enc.item_separator, True, False, True)
    return "".join(it(d, 0)).replace("\n", NL)

def die(msg):
    print("FAIL: " + msg, file=sys.stderr); sys.exit(1)

def close(a, b):
    return a is not None and b is not None and abs(float(a) - float(b)) <= TOL

def same_pos(p, q):
    return all(close(x, y) for x, y in zip(p, q))

def index(scene):
    allent = [e for k in CATS for e in scene[k]]
    byid = {e["id"]: e for e in allent}
    return allent, byid

def find_one(allent, byid, name, parent, position, what):
    """exactly one entity with this name, parent name and local position (ids were renumbered by #1216)"""
    hits = [e for e in allent if e.get("name") == name and same_pos(e.get("position", []), position)
            and byid.get(e.get("parentId"), {}).get("name") == parent]
    if len(hits) != 1:
        byname = [e["id"] for e in allent if e.get("name") == name]
        die(f"{what}: {len(hits)} matches for name={name!r} parent={parent!r} pos={position} "
            f"(entities with that name: {byname[:10]})")
    return hits[0]

def fade(L, k):
    v = L.get(k)
    return list(v) if v is not None else list(DEF_FADE) if k in ("Bounds Fade +", "Bounds Fade -") else None

def check_new_id(byid, allent, nid, nord, what):
    if nid not in ID_BLOCK or nord not in ORD_BLOCK: die(f"{what}: id {nid}/order {nord} outside Atlas's block")
    if nid in byid: die(f"{what}: id {nid} already used by {byid[nid].get('name')!r}")
    if any(e.get("order") == nord for e in allent): die(f"{what}: order {nord} already used")

def scene_path(arg):
    return arg if arg.lower().endswith(".json") else arg.rstrip("/\\") + "/project/scenes/Forest.json"

# ---------------------------------------------------------------- apply
SCRIPT = "master_red_tint.py"
NAME, PARENT = "Ceiling Master bedroom", "House Lights"
POSITIONS = [[-35.8861198425293, 6.25499963760376, -9.783419609069824],
             [-34.80495071411133, 6.25499963760376, -9.783419609069824],
             [-33.596920013427734, 6.25499963760376, -9.783419609069824]]
OLD, NEW = [1.0, 0.0, 0.0], [1.0, 0.25, 0.2]

def main():
    a = sys.argv[1:]
    if not a or a[0].startswith("-"): die(__doc__)
    path = scene_path(a[0]); write = "--write" in a
    scene = load(path); allent, byid = index(scene)
    n_all = sum(1 for e in allent if e.get("name") == NAME and "Light" in e)
    if n_all != len(POSITIONS): die(f"expected {len(POSITIONS)} lights named {NAME!r}, found {n_all}")
    nset = nalready = 0
    for p in POSITIONS:
        e = find_one(allent, byid, NAME, PARENT, p, NAME)
        L = e.get("Light") or die(f"{NAME} {e['id']} has no Light")
        col = list(L.get("Color") or die(f"{NAME} {e['id']} has no Color"))
        for j in range(3):
            if close(col[j], NEW[j]): nalready += 1; continue
            if not close(col[j], OLD[j]):
                die(f"{NAME} {e['id']}: Color[{j}] is {col[j]}, expected {OLD[j]} (or {NEW[j]}): resolve with the SM")
            print(f"  set      {e['id']} {NAME!r} Color[{j}] {col[j]} -> {NEW[j]}")
            col[j] = NEW[j]; nset += 1
        L["Color"] = col
    print(f"{SCRIPT}: {nset} values set, {nalready} already applied")
    if not write:
        print("DRY RUN (nothing written). Add --write to save " + path); return
    if nset:
        with open(path, "wb") as f: f.write(dumps(scene).encode("utf-8"))
        print("written " + path)
    else:
        print("nothing to do; file untouched")

if __name__ == "__main__":
    main()
