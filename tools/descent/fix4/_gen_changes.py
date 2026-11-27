"""Generator (author tool, not needed to re-apply): diff the Light properties of two Forest.json
revisions by id and print a CHANGES literal (name + position + parent name + old/new tokens)."""
import json, re, subprocess, sys
sys.path.insert(0, __file__.rsplit("\\", 1)[0] if "\\" in __file__ else __file__.rsplit("/", 1)[0])
from fix4_lib import *

REPO = sys.argv[1]; BASE = sys.argv[2]; TIP = sys.argv[3]
REVS = [BASE] + subprocess.check_output(["git", "-C", REPO, "rev-list", "--first-parent", "--reverse", BASE + ".." + TIP]).decode().split()

def load(rev):
    raw = subprocess.check_output(["git", "-C", REPO, "show", rev + ":project/scenes/Forest.json"])
    lines = raw.decode("utf-8").split("\n")
    return lines, load_entities(lines)

hist = [load(r) for r in REVS]
bl, be = hist[0]; tl, te = hist[-1]
hid = [{d["id"]: (s, e, d) for s, e, d in h[1]} for h in hist]
bid = {d["id"]: d for _, _, d in be}
tid = {d["id"]: (s, e, d) for s, e, d in te}
tok = re.compile(r'-?\d[\d.eE+-]*|true|false|"[^"]*"')
out = []
for s, e, d in be:
    if d["id"] not in tid:
        print("# REMOVED entity", d["id"], d["name"], file=sys.stderr); continue
    ts, tend, td = tid[d["id"]]
    other_b = {k: v for k, v in d.items() if k != "Light"}
    other_t = {k: v for k, v in td.items() if k != "Light"}
    if other_b != other_t:
        print("# NON-LIGHT DIFF", d["id"], d["name"], file=sys.stderr)
    lb, lt = d.get("Light", {}), td.get("Light", {})
    props = {}
    for k in sorted(set(lb) | set(lt)):
        if lb.get(k) == lt.get(k):
            continue
        def toks(lines, s_, e_, v):
            if v is None: return None
            loc = find_prop(lines, s_, e_, k)
            txt = "\n".join(lines[loc[0]:loc[1] + 1]).split(": ", 1)[1].rstrip(",")
            t = tok.findall(txt)
            return t if isinstance(v, list) else t[0]
        olds = []
        for (hl, he), hm in zip(hist[:-1], hid[:-1]):
            hs, hend, hd = hm[d["id"]]
            v = hd.get("Light", {}).get(k)
            t = toks(hl, hs, hend, v)
            if v != lt.get(k) and t not in olds:
                olds.append(t)
        props[k] = (olds, toks(tl, ts, tend, lt.get(k)))
    if props:
        par = bid.get(d.get("parentId"), {}).get("name")
        out.append(dict(name=d["name"], position=d["position"], parent=par, props=props, _id=d["id"]))
for t_id in tid:
    if t_id not in bid:
        print("# NEW entity", t_id, tid[t_id][2]["name"], file=sys.stderr)
import pprint
pprint.pprint(out, width=110, sort_dicts=False)
