"""W2 verify slot: build tour_main.json (mirrors, kitchen sink, front/east facade) and tour_moon.json (251/253 poses)."""
import json
OUT = "D:/Jacob/tblocks/descent/gi-p2/argus/slot/"
sweep = json.load(open("D:/Jacob/tblocks/descent/house-polish/briareos/sweep/sweep_tour_open.json", encoding="utf-8-sig"))
mir = json.load(open("D:/Jacob/tblocks/descent/gi-p2/argus/check_ab/mirror_matte/tour_mirrors.json", encoding="utf-8-sig"))
by = {w["id"]: w for w in sweep["waypoints"]}
main_ids = ["jlook-kitchen-sink_basin", "jlook-kitchen-sink_wide", "out-door20760_near", "out-door20760_left", "out-door20760_far",
            "out-door20760_right", "out-door20754_near", "out-door20754_far"]
moon_ids = ["out-door20760_left", "out-door20760_far"]
base = {k: v for k, v in sweep.items() if k not in ("waypoints", "routes")}
tm = dict(base)
tm["waypoints"] = [w for w in mir["waypoints"]] + [by[i] for i in main_ids]
tn = dict(base)
tn["waypoints"] = [by[i] for i in moon_ids]
for name, t in (("tour_main.json", tm), ("tour_moon.json", tn)):
    json.dump(t, open(OUT + name, "w", encoding="utf-8"), indent=1)
    print(name, [w["id"] for w in t["waypoints"]])
