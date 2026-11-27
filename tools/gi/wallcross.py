"""Interim GI coverage gate (until gi_audit --check-margins sees uneven faces; review of #1205).

Usage:  python -I wallcross.py <repo_root> [--scene project/scenes/Forest.json] [--tol 0.0]
        (needs <repo_root>/tools/gi/gi_audit.py; read-only; exit 1 = a leak was found)

Every GI volume against every thin wall collider ("Walls" group, <= 0.45 m thick, longer than thick) it crosses,
not just adjacent pairs. A volume on one side of a wall whose box reaches past the wall's FAR surface, where the air
just beyond (0.03 m) is covered by a volume of a DIFFERENT room (gi_audit.room_of), is a LEAK. Its face probes sit in
the next room's air and bake that room's light, and the volume's own wall-side samples blend them in.
Skipped: the far side is more wall (corners, T-junctions, staggered walls); the volume reaches > SPAN m past the far
surface (it spans the wall: a stub or wall end inside it, or one space by design); weight 0 there (face short of it).
Rows: weight_in_far_air (the volume's fade weight 0.03 m past the far surface), past_far_m (how far the face sits
beyond the far surface), the volume, the axis, the wall's mid-plane and thickness, how much of the wall it overlaps,
the far room(s), the far sample point. Fix a row by pulling that face back to <= the wall's mid-plane + 0.055 m.
"""
import argparse, json, os, sys

SPAN = 0.45  # a volume reaching further than this past the far surface spans the wall (not a leak face)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("repo")
    ap.add_argument("--scene", default="project/scenes/Forest.json")
    ap.add_argument("--tol", type=float, default=0.0, help="ignore faces at most this far past the far surface (m)")
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(a.repo, "tools", "gi"))
    import gi_audit as ga

    scene = ga.Scene(json.load(open(os.path.join(a.repo, a.scene), encoding="utf-8")))
    vols = scene.volumes()
    walls = [b for b in (scene.box_collider(e) for e in scene.descendants_of_group(ga.WALL_GROUPS)) if b is not None]
    rows = []
    for v in vols:
        vmn, vmx = v["box"].aabb()
        vc = [(vmn[i] + vmx[i]) / 2 for i in range(3)]
        for w in walls:
            wmn, wmx = w.aabb()
            for axis in (0, 2):
                t = 2 - axis
                thick, length = wmx[axis] - wmn[axis], wmx[t] - wmn[t]
                if thick > 0.45 or thick >= length:
                    continue
                ov_t = min(vmx[t], wmx[t]) - max(vmn[t], wmn[t])
                ov_y = min(vmx[1], wmx[1]) - max(vmn[1], wmn[1])
                if ov_t < 0.1 or ov_y < 0.5:
                    continue
                mid = (wmn[axis] + wmx[axis]) / 2
                low_side = vc[axis] < mid
                if low_side:
                    if vmx[axis] <= wmn[axis]:
                        continue
                    past_far = vmx[axis] - wmx[axis]
                else:
                    if vmn[axis] >= wmx[axis]:
                        continue
                    past_far = wmn[axis] - vmn[axis]
                if past_far <= a.tol or past_far > SPAN:
                    continue
                tc = (max(vmn[t], wmn[t]) + min(vmx[t], wmx[t])) / 2
                yc = (max(vmn[1], wmn[1]) + min(vmx[1], wmx[1])) / 2
                far = [0.0, yc, 0.0]
                far[t] = tc
                far[axis] = (wmx[axis] + 0.03) if low_side else (wmn[axis] - 0.03)
                if any(o.contains(far) for o in walls):
                    continue
                wv = ga.volume_fade(v, far) if hasattr(ga, "volume_fade") else v["box"].fade(far, v["fade"])  # per-face fades (Fade Min / Max) when the audit knows them
                others = [o["name"] for o in vols if o is not v and o["box"].contains(far)]
                other_rooms = sorted({ga.room_of(n) for n in others} - {ga.room_of(v["name"])})
                if wv > 1e-6 and other_rooms:
                    rows.append((round(wv, 3), round(past_far, 3), v["name"], "xyz"[axis], round(mid, 3), round(thick, 3),
                                 round(ov_t, 2), ",".join(other_rooms), [round(x, 2) for x in far]))
    rows.sort(reverse=True)
    print("weight_in_far_air | past_far_m | volume | axis | wall_mid | wall_thick | overlap_along | far rooms | far point")
    for r in rows:
        print(" | ".join(str(x) for x in r))
    print("%d leak row(s), %d volume(s); %d walls, %d volumes checked"
          % (len(rows), len({r[2] for r in rows}), len(walls), len(vols)))
    return 1 if rows else 0


if __name__ == "__main__":
    sys.exit(main())
