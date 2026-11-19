"""Approximate the Quantum wardrobe's catalog (Wardrobe::Classify) and show which rules match what now.
python wardrobe_audit.py <repo>"""
import json, os, sys, glob, collections
repo = sys.argv[1]
P = os.path.join(repo, "project")
w = json.load(open(os.path.join(P, "assets/Characters/Quantum/Quantum.wardrobe"), encoding="utf-8"))
root = w["root"]
over = {i["path"]: i for i in w["items"]}
items = []
for meta in glob.glob(os.path.join(P, root, "Models/Clothing/**/*.fbx.meta"), recursive=True):
    rel = os.path.relpath(meta[:-5], os.path.join(P, root)).replace("\\", "/")
    parts = rel.split("/")
    if any(p in w["excludeFolders"] for p in parts[:-1]): continue
    o = over.get(rel, {})
    if o.get("hidden"): continue
    stem = parts[-1][:-4]
    slot = o.get("slot")
    if not slot:
        for d in reversed(parts[:-1]):
            for s in w["slots"]:
                if d in s["folders"] and (not s.get("nameSuffix") or stem.endswith(s["nameSuffix"])):
                    slot = s["id"]; break
            if slot: break
    gender = "Female" if ("Female" in parts or stem.startswith("SKM_F_") or stem.startswith("SM_F_")) else "Male"
    remap = json.load(open(meta, encoding="utf-8")).get("materialRemap", {})
    mats = " ".join(os.path.basename(v) for v in remap.values())
    items.append(dict(rel=rel, stem=stem, slot=slot, gender=gender, mats=mats, tags=set()))

def low(s): return s.lower()
def match(r, it, prefix=""):
    if "slot" in r and r["slot"] != it["slot"]: return False
    if "gender" in r and r["gender"] != it["gender"]: return False
    if "nameHasAny" in r and not any(low(x) in low(it["stem"]) for x in r["nameHasAny"]): return False
    if "nameLacksAll" in r and any(low(x) in low(it["stem"]) for x in r["nameLacksAll"]): return False
    if "materialHasAny" in r and not any(low(x) in low(it["mats"]) for x in r["materialHasAny"]): return False
    if "tags" in r and not all(t in it["tags"] for t in r["tags"]): return False
    if "lacksTags" in r and any(t in it["tags"] for t in r["lacksTags"]): return False
    return True

for r in w["tags"]:
    for it in items:
        if match(r, it): it["tags"] |= set(r["add"])

print("== items per slot/gender")
c = collections.Counter((it["slot"], it["gender"]) for it in items)
for k in sorted(c, key=str): print(k, c[k])
print("\n== unslotted:", [it["rel"] for it in items if not it["slot"]])

def report(name, rules, sub=None):
    print(f"\n== {name}: rules matching nothing")
    for r in rules:
        rr = sub(r) if sub else [r]
        for x in rr:
            if x is None: continue
            n = sum(match(x, it) for it in items)
            if n == 0: print("  0 :", json.dumps(x)[:160])
report("tags", w["tags"])
report("covers", w["covers"], lambda r: [{k: v for k, v in r.items() if k in ("slot", "gender", "nameHasAny", "nameLacksAll")}])
report("clears", w["clears"])
report("excludes", w["excludes"], lambda r: [r["a"], r["b"]])
report("layers", w["layers"])

styles = [s["name"] for s in w["styles"]]
print("\n== per slot: items with no style tag (go in every style)")
for s in w["slots"]:
    none = [it["stem"] for it in items if it["slot"] == s["id"] and not (it["tags"] & set(styles))]
    if none: print(f"  {s['id']}: {len(none)}  e.g. {none[:6]}")

for slot in ("Hair", "FacialHair", "Headwear", "Headphones", "Balaclava"):
    print(f"\n== {slot}")
    for it in sorted(items, key=lambda i: i["stem"]):
        if it["slot"] == slot: print(f"  {it['gender'][0]} {it['stem']:45s} {sorted(it['tags'])}")

# Long-sleeve covers: male Tops/Outerwear NOT turned into sleeve hands, and female has none.
sleeve = [r for r in w["covers"] if r.get("with") == "SleeveHands"]
print("\n== male tops/outerwear that keep full arms (not SleeveHands):")
for it in items:
    if it["gender"] == "Male" and it["slot"] in ("Top", "Outerwear") and not any(match(r, it) for r in sleeve):
        print("  ", it["stem"])
