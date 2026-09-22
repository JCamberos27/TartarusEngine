# READ-ONLY: is the left arm's wild motion during A_FP_Idle present in the .blend itself?
#
# Evaluates the armature under A_FP_Idle across its range and reports, left vs right:
#   * which pose bones the action actually keys (and how many keyframes)
#   * constraints on the arm chains, their targets, and the target's OWN action
#   * per-frame world motion of the arm bones, plus the local LOCATION of each
#     (an animated bone location = the bone stretches/slides, which reads as a broken hand)
#   * NLA tracks stacked on the armature
# Nothing is saved: the action / pose_position / frame are restored in memory, no .save().
#
#   blender.exe --background "<blend>" --python work/bl_idle_probe.py
import bpy
import re

WATCH = [
    "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l", "ik_hand_l",
    "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r", "ik_hand_r",
    "ik_hand_gun", "ik_hand_root", "root", "pelvis",
    "CB_ik_hand_l", "CB_ik_hand_r", "CB_pole_elbow_l", "CB_pole_elbow_r", "CB_Gun",
    "index_01_l", "index_01_r",
]
WATCH_SET = set(WATCH)


def action_fcurves(act):
    """Blender 5.x slotted actions: fcurves live in layer->strip->channelbag."""
    if act is None:
        return []
    try:
        legacy = list(act.fcurves)
        if legacy:
            return legacy
    except AttributeError:
        pass
    fcs = []
    for layer in getattr(act, "layers", []):
        for strip in getattr(layer, "strips", []):
            for cb in getattr(strip, "channelbags", []):
                fcs.extend(cb.fcurves)
    return fcs


PAT = re.compile(r'pose\.bones\["([^"]+)"\]')
ARM_NAME = "Armature"
scn = bpy.context.scene
arm = bpy.data.objects.get(ARM_NAME)
if arm is None:
    raise SystemExit("no Armature object")

print("=== FILE STATE ===")
print("  scene frames %d..%d  fps=%g  frame_current=%d" % (
    scn.frame_start, scn.frame_end, scn.render.fps, scn.frame_current))
print("  armature action=%s  pose_position=%s" % (
    arm.animation_data.action.name if arm.animation_data and arm.animation_data.action else None,
    arm.data.pose_position))
if arm.animation_data:
    for t in arm.animation_data.nla_tracks:
        print("  NLA track %s  mute=%s strips=%s" % (
            t.name, t.mute, [(s.name, getattr(s, "action", None).name if getattr(s, "action", None) else None) for s in t.strips]))
    if not arm.animation_data.nla_tracks:
        print("  NLA tracks: none")

ACT = bpy.data.actions.get("A_FP_Idle")
if ACT is None:
    raise SystemExit("no A_FP_Idle action")
fr = ACT.frame_range
FS, FE = int(round(fr[0])), int(round(fr[1]))
print("\n=== A_FP_Idle  range=(%d..%d)  fcurves=%d ===" % (FS, FE, len(action_fcurves(ACT))))

keyed = {}
objpaths = []
for fc in action_fcurves(ACT):
    m = PAT.search(fc.data_path)
    if m:
        keyed.setdefault(m.group(1), []).append((fc.data_path, fc.array_index, len(fc.keyframe_points)))
    else:
        objpaths.append(fc.data_path)
print("  keyed bones: %d" % len(keyed))
for bn in WATCH:
    if bn in keyed:
        parts = []
        for dp, ai, nk in keyed[bn]:
            prop = dp.split('"')[2].split(".")[-1] if '"' in dp else dp
            parts.append("%s[%d]x%d" % (prop, ai, nk))
        print("    %-22s %s" % (bn, ", ".join(parts)))
unkeyed = [b for b in WATCH if b not in keyed]
print("  NOT keyed in this action: %s" % ", ".join(unkeyed))
if objpaths:
    print("  NON-POSE fcurves: %s" % sorted(set(objpaths)))

# A control bone keyed 0 times but present is different from absent - report CB_ neighbours too.
print("\n  all keyed bones (%d): %s" % (len(keyed), ", ".join(sorted(keyed))))

print("\n=== CONSTRAINTS on the arm chains ===")
targets_seen = []
for bn in WATCH:
    pb = arm.pose.bones.get(bn)
    if pb is None:
        print("  %-22s MISSING" % bn)
        continue
    if not pb.constraints:
        continue
    for c in pb.constraints:
        tgt = c.target.name if c.target else "-"
        sub = getattr(c, "subtarget", "") or "-"
        print("  %-22s %-22s target=%-16s sub=%-20s infl=%.3f mute=%s" % (
            bn, c.type, tgt, sub, c.influence, c.mute))
        if c.target:
            targets_seen.append(c.target)

print("\n=== TARGET OBJECTS' own animation ===")
for ob in sorted(set(targets_seen), key=lambda o: o.name):
    ad = ob.animation_data
    print("  %-20s type=%-8s action=%-22s nla=%d" % (
        ob.name, ob.type,
        ad.action.name if ad and ad.action else "-",
        len(ad.nla_tracks) if ad else 0))

# --- evaluate the action in memory ---------------------------------------------------------
prev_action = arm.animation_data.action if arm.animation_data and arm.animation_data.action else None
prev_pose = arm.data.pose_position
prev_frame = scn.frame_current

if arm.animation_data is None:
    arm.animation_data_create()
arm.data.pose_position = "POSE"
arm.animation_data.action = ACT

print("\n=== evaluated motion under A_FP_Idle (Blender units; file is Z-up) ===")
frames = list(range(FS, FE + 1, max(1, (FE - FS) // 16)))
if frames[-1] != FE:
    frames.append(FE)

def stats(bn):
    return {"path": 0.0, "maxrot": 0.0, "maxpos": 0.0,
            "locmin": [1e9] * 3, "locmax": [-1e9] * 3,
            "rotmin": [1e9] * 3, "rotmax": [-1e9] * 3}

S = {bn: stats(bn) for bn in WATCH}
prev_m = {}
first_m = {}
last_m = {}
for f in frames:
    scn.frame_set(f)
    bpy.context.view_layer.update()
    for bn in WATCH:
        pb = arm.pose.bones.get(bn)
        if pb is None:
            continue
        mw = arm.matrix_world @ pb.matrix
        loc = mw.translation
        q = mw.to_quaternion()
        if bn in prev_m:
            pl, pq = prev_m[bn]
            d = loc - pl
            S[bn]["path"] += d.length
            S[bn]["maxpos"] = max(S[bn]["maxpos"], d.length)
            S[bn]["maxrot"] = max(S[bn]["maxrot"], pq.rotation_difference(q).angle)
        else:
            first_m[bn] = (loc.copy(), q.copy())
        prev_m[bn] = (loc.copy(), q.copy())
        last_m[bn] = (loc.copy(), q.copy())
        for i, v in enumerate((pb.location.x, pb.location.y, pb.location.z)):
            S[bn]["locmin"][i] = min(S[bn]["locmin"][i], v)
            S[bn]["locmax"][i] = max(S[bn]["locmax"][i], v)
        e = q.to_euler("XYZ")
        for i, v in enumerate((e.x, e.y, e.z)):
            S[bn]["rotmin"][i] = min(S[bn]["rotmin"][i], v)
            S[bn]["rotmax"][i] = max(S[bn]["rotmax"][i], v)

print("  sampled %d frames: %s" % (len(frames), frames))
print("  %-20s %10s %10s %10s   local location min->max (spread)" % (
    "bone", "path", "maxStep", "seam"))
for bn in WATCH:
    if bn not in S or bn not in first_m:
        continue
    seam = first_m[bn][0].rotation_difference(last_m[bn][0]).angle
    spread = max(S[bn]["locmax"][i] - S[bn]["locmin"][i] for i in range(3))
    print("  %-20s %10.4f %10.4f %10.4f   spread=%.5f  loc=(%.5f,%.5f,%.5f)->(%.5f,%.5f,%.5f)" % (
        bn, S[bn]["path"], S[bn]["maxpos"], seam, spread,
        S[bn]["locmin"][0], S[bn]["locmin"][1], S[bn]["locmin"][2],
        S[bn]["locmax"][0], S[bn]["locmax"][1], S[bn]["locmax"][2]))

print("\n  per-frame world position of hand_l vs hand_r (first 8 samples):")
prev_l = prev_r = None
for i, f in enumerate(frames[:8]):
    scn.frame_set(f)
    bpy.context.view_layer.update()
    out = ["  f%-4d" % f]
    for bn in ("hand_l", "hand_r", "lowerarm_l", "lowerarm_r"):
        pb = arm.pose.bones.get(bn)
        if pb is None:
            continue
        loc = (arm.matrix_world @ pb.matrix).translation
        delta = ""
        if bn.endswith("_l") and prev_l is not None and bn == "hand_l":
            delta = "  d=%.5f" % (loc - prev_l).length
        if bn.endswith("_r") and prev_r is not None and bn == "hand_r":
            delta = "  d=%.5f" % (loc - prev_r).length
        out.append("%s=(%.4f,%.4f,%.4f)%s" % (bn, loc.x, loc.y, loc.z, delta))
        if bn == "hand_l":
            prev_l = loc.copy()
        if bn == "hand_r":
            prev_r = loc.copy()
    print(" ".join(out))

# --- restore in memory; the .blend on disk is untouched ------------------------------------
arm.animation_data.action = prev_action
arm.data.pose_position = prev_pose
scn.frame_set(prev_frame)
print("\nRESTORED action=%s pose_position=%s frame=%d" % (
    prev_action.name if prev_action else None, prev_pose, prev_frame))
print("DONE")
