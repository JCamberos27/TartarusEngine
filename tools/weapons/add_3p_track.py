# Adds the third-person arms track ("arms3p") to a weapon's controller: every motion on the "arms" track gets the
# same clip's third-person version (FirstPerson/<P>_A_FP_<X>.fbx -> ThirdPerson/<P>_A_3P_<X>.fbx, from
# tools/weapons/author_3p.py), in every layer, blend tree children included. Re-runnable: it rewrites the track.
#
#   python tools/weapons/add_3p_track.py project/assets/Weapons/AKS74U/AKS74U.controller \
#                                        project/assets/Weapons/Remington870/Remington870.controller
#
# Run it again after make_remington_controller.py (which rebuilds the Remington's from the AK's), and after the
# engine has written the new clips' .meta files, so the motions carry their GUIDs.
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROJECT = os.path.join(REPO, "project")


def third_person(ref):
    folder, name = ref.rsplit("/", 1)
    if not folder.endswith("/FirstPerson") or "_A_FP_" not in name:
        raise SystemExit("not a first-person arms clip: %s" % ref)
    path = folder[: -len("FirstPerson")] + "ThirdPerson/" + name.replace("_A_FP_", "_A_3P_")
    if not os.path.exists(os.path.join(PROJECT, path)):
        raise SystemExit("no third-person clip %s (run tools/weapons/author_3p.py first)" % path)
    out = {"clip": path}
    meta = os.path.join(PROJECT, path + ".meta")
    if os.path.exists(meta):
        out["clipGuid"] = json.load(open(meta, encoding="utf-8"))["guid"]
    return out


def convert(motion):
    out = third_person(motion["clip"]) if motion.get("clip") else {}
    for k, v in motion.items():
        if k == "children":
            out["children"] = [dict(c, **third_person(c["clip"])) if c.get("clip") else dict(c) for c in v]
        elif k not in ("clip", "clipGuid"):
            out[k] = v
    return out


for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as f:
        c = json.load(f)
    if "arms3p" not in c["tracks"]:
        c["tracks"].append("arms3p")
    n = 0
    for layer in c["layers"]:
        for s in layer["states"]:
            motions = s.get("motions", {})
            motions.pop("arms3p", None)
            if "arms" in motions:
                motions["arms3p"] = convert(motions["arms"])
                n += 1
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(c, f, indent=2)  # the file's own key order
        f.write("\n")
    print("%s: arms3p on %d states" % (os.path.basename(path), n))
