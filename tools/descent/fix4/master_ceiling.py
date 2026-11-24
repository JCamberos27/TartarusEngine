"""fix4 re-apply: master-bedroom ceiling diagonal fix (PM-approved design; HANDOFF_W1 item 1). Run AFTER jambs.py.

Owner: W1 Atlas. Light: 'Lamp_stand Lamp_stand 17' (old id 35611) under 'House Lights'.
Cause: its hard point-shadow terminator from the opening's west corner draws a diagonal on the master ceiling,
and the room behind Wall_Interior_3x (4) is lit only through the shadow map.
  TRIM : world +z face 31.66 -> 26.077 (Wall_Interior_3x (4) far plane), fade F-[0] = 0.19.
  COPY : 'Lamp_stand Lamp_stand 17 (master)', UNSHADOWED exact copy (same colour, intensity 0.5, range, circuit),
         box world x -11.222 (door-wall far plane) .. -6.663 (the lamp's +x after jambs.py), z 25.887 .. 31.66,
         fade F+[0] = 0.19 at z 25.887 = crossfade with the trim (sum exactly 1). id 36181, order 268775.
CPU check (old ids, fix35611/cmp_404.png + cmp_138.png): no terminator step, box room 0.0002 vs 0.026.
Map (parent House Lights): world x = local z + 1, world z = -local x - 7; +z = F-[0], -z = F+[0], +x = F+[2], -x = F-[2].
Idempotent: a second run changes nothing. Fails loudly if the light is missing/ambiguous or its box is not what
jambs.py leaves (x -13.02..-6.663, z 20.16|20.30..31.66).

Usage:  python -I master_ceiling.py <repo dir | path/to/Forest.json> [--write]     (dry run without --write)
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
SCRIPT = "master_ceiling.py"
NAME, PARENT = "Lamp_stand Lamp_stand 17", "House Lights"
POS = [-32.61000061035156, 4.763999938964844, -12.754400253295898]     # local position on main db92576f
COPY_NAME, COPY_ID, COPY_ORDER = NAME + " (master)", 36181, 268775
Z_CUT, F, X_DOOR = 26.077, 0.19, -11.222

def lz(x): return x - 1.0       # world x -> local z
def lx(z): return -z - 7.0      # world z -> local x

def world(L):
    c, s = L["Bounds Center"], L["Bounds Size"]
    return (c[2] - s[2] / 2 + 1.0, c[2] + s[2] / 2 + 1.0, -(c[0] + s[0] / 2) - 7.0, -(c[0] - s[0] / 2) - 7.0)

def near(a, b): return abs(a - b) < 1e-3

def main():
    a = sys.argv[1:]
    if not a or a[0].startswith("-"): die(__doc__)
    path = scene_path(a[0]); write = "--write" in a
    scene = load(path); allent, byid = index(scene)
    o = find_one(allent, byid, NAME, PARENT, POS, "master light")
    Lo = o.get("Light") or die("master light has no Light")
    if Lo.get("Cast Shadows") is not True: die(f"{NAME} is expected to cast shadows, has {Lo.get('Cast Shadows')}")
    if not close(Lo.get("Intensity"), 0.5): die(f"{NAME} Intensity is {Lo.get('Intensity')}, the approved design assumes 0.5")
    xlo, xhi, zlo, zhi = world(Lo)
    if not (near(xlo, -13.02) and near(xhi, -6.663) and (near(zlo, 20.16) or near(zlo, 20.30))):
        die(f"{NAME} box is world x {xlo:.4f}..{xhi:.4f} z {zlo:.4f}..{zhi:.4f}; expected x -13.02..-6.663, "
            f"z 20.16|20.30..: run jambs.py first")
        
    changed = 0
    fm = fade(Lo, "Bounds Fade -")
    if near(zhi, Z_CUT) and close(fm[0], F):
        print(f"  already  trim {o['id']} {NAME!r}")
    elif near(zhi, 31.66) and close(fm[0], -1):
        c, s = Lo["Bounds Center"], Lo["Bounds Size"]
        Lo["Bounds Center"] = [round((lx(zlo) + lx(Z_CUT)) / 2, 4), c[1], c[2]]
        Lo["Bounds Size"] = [round(Z_CUT - zlo, 4), s[1], s[2]]
        Lo["Bounds Fade -"] = [F, fm[1], fm[2]]
        changed += 1
        print(f"  TRIM     {o['id']} {NAME!r} world +z 31.66 -> {Z_CUT}, F-[0] {F}: C {Lo['Bounds Center']} S {Lo['Bounds Size']}")
    else:
        die(f"{NAME}: unexpected state, world +z {zhi:.4f}, F-[0] {fm[0]} (expected 31.66/-1 or {Z_CUT}/{F})")

    c, s = Lo["Bounds Center"], Lo["Bounds Size"]
    z0, z1 = Z_CUT - F, 31.66
    want = {"Bounds Center": [round((lx(z0) + lx(z1)) / 2, 4), c[1], round((lz(X_DOOR) + lz(xhi)) / 2, 4)],
            "Bounds Size": [round(z1 - z0, 4), s[1], round(xhi - X_DOOR, 4)],
            "Bounds Fade +": [F, -1.0, -1.0], "Bounds Fade -": [-1.0, -1.0, -1.0]}
    ex = [e for e in allent if e.get("name") == COPY_NAME]
    if ex:
        if len(ex) != 1 or ex[0]["id"] != COPY_ID: die(f"{COPY_NAME!r} already present as {[e['id'] for e in ex]}, expected one with id {COPY_ID}")
        Lc = ex[0].get("Light", {})
        for k, v in want.items():
            cur = fade(Lc, k) if k.startswith("Bounds Fade ") else Lc.get(k)
            if cur is None or not all(close(x, y) for x, y in zip(cur, v)): die(f"{COPY_NAME!r}: {k} = {cur}, expected {v}")
        if Lc.get("Cast Shadows") is not False: die(f"{COPY_NAME!r} must not cast shadows")
        print(f"  already  copy {COPY_ID} {COPY_NAME!r}")
    else:
        check_new_id(byid, allent, COPY_ID, COPY_ORDER, "master copy")
        cp = copy.deepcopy(o)
        cp["id"], cp["order"], cp["name"] = COPY_ID, COPY_ORDER, COPY_NAME
        cp["Light"]["Cast Shadows"] = False
        cp["Light"].update(want)
        for k in CATS:
            if o in scene[k]: scene[k].insert(scene[k].index(o) + 1, cp); break
        changed += 1
        print(f"  ADDED    {COPY_ID} order {COPY_ORDER} {COPY_NAME!r} unshadowed, C {want['Bounds Center']} S {want['Bounds Size']} F+ {want['Bounds Fade +']}")
    for e in [o] + [x for k in CATS for x in scene[k] if x.get("id") == COPY_ID]:
        L = e["Light"]; w = world(L)
        print(f"    {e['id']} {e['name']!r} shadows {L['Cast Shadows']} I {L['Intensity']} world x {w[0]:.4f}..{w[1]:.4f} z {w[2]:.4f}..{w[3]:.4f} "
              f"F+ {fade(L, 'Bounds Fade +')} F- {fade(L, 'Bounds Fade -')}")
    print(f"{SCRIPT}: {changed} change(s)")
    if not write:
        print("DRY RUN (nothing written). Add --write to save " + path); return
    if changed:
        with open(path, "wb") as f: f.write(dumps(scene).encode("utf-8"))
        print("written " + path)
    else:
        print("nothing to do; file untouched")

if __name__ == "__main__":
    main()
