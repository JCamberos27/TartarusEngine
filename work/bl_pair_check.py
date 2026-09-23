# READ-ONLY: which other arms clips were baked against the WRONG weapon action?
#
# Every arms clip A_FP_x should be baked while the weapon plays A_W_x (falling back to A_W_ADS
# when no weapon counterpart exists), because CB_ik_hand_l is CHILD_OF the weapon's `magazine`
# and therefore samples the weapon during the bake. If a clip was instead exported while the
# file still had the weapon on some unrelated action, its left hand is baked in the wrong place.
#
# For each arms action this evaluates the left/right arm paths twice - under the weapon action
# the .blend was last saved with, and under the CORRECT pairing - and reports the difference.
# Equal numbers => the shipped clip is fine; a large divergence => re-export it.
# Nothing is saved: actions / NLA / pose_position / frame are restored in memory, no .save().
#
#   blender.exe --background "<blend>" --python work/bl_pair_check.py
import bpy

ARMS, WEAP = "Armature", "AK"
BONES = ["lowerarm_l", "hand_l", "lowerarm_r", "hand_r"]


def action_fcurves(act):
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


def frame_list(fs, fe, n=24):
    step = max(1, (fe - fs) // n)
    fr = list(range(fs, fe + 1, step))
    if fr[-1] != fe:
        fr.append(fe)
    return fr


scn = bpy.context.scene
arm = bpy.data.objects[ARMS]
wep = bpy.data.objects[WEAP]

saved = {
    "arm_action": arm.animation_data.action if arm.animation_data else None,
    "wep_action": wep.animation_data.action if wep.animation_data else None,
    "arm_pose": arm.data.pose_position,
    "wep_pose": wep.data.pose_position,
    "frame": scn.frame_current, "fs": scn.frame_start, "fe": scn.frame_end,
}
saved_arm_nla = [(t, t.mute) for t in arm.animation_data.nla_tracks] if arm.animation_data else []
saved_wep_nla = [(t, t.mute) for t in wep.animation_data.nla_tracks] if wep.animation_data else []

NAIVE = saved["wep_action"].name if saved["wep_action"] else None
print("=== the weapon action the .blend was saved with (what a naive export bakes): %s ===" % NAIVE)


def correct_weapon(arms_action):
    if arms_action.startswith("A_FP_") and "A_W_" + arms_action[5:] in bpy.data.actions:
        return "A_W_" + arms_action[5:]
    return "A_W_ADS" if "A_W_ADS" in bpy.data.actions else None


def paths(arms_action, wep_action, fs, fe):
    arm.animation_data.action = bpy.data.actions[arms_action]
    wep.animation_data.action = bpy.data.actions[wep_action] if wep_action else None
    for t, _ in saved_arm_nla:
        t.mute = True
    for t, _ in saved_wep_nla:
        t.mute = True
    arm.data.pose_position = "POSE"
    wep.data.pose_position = "POSE"
    scn.frame_start, scn.frame_end = fs, fe
    out = {}
    prev, path = {}, {b: 0.0 for b in BONES}
    for f in frame_list(fs, fe):
        scn.frame_set(f)
        bpy.context.view_layer.update()
        for b in BONES:
            pb = arm.pose.bones.get(b)
            if pb is None:
                continue
            loc = (arm.matrix_world @ pb.matrix).translation.copy()
            if b in prev:
                path[b] += (loc - prev[b]).length
            prev[b] = loc
    out = dict(path)
    mb = wep.pose.bones.get("magazine")
    if mb is not None:
        prev = None
        p = 0.0
        for f in frame_list(fs, fe):
            scn.frame_set(f)
            bpy.context.view_layer.update()
            loc = (wep.matrix_world @ mb.matrix).translation.copy()
            if prev is not None:
                p += (loc - prev).length
            prev = loc
        out["magazine"] = p
    return out


print("\n%-18s %5s | %-34s | %-34s | %s" % (
    "arms action", "frames", "LEFT HAND path under saved pairing", "under CORRECT pairing", "verdict"))
print("-" * 130)
affected = []
for a in sorted(bpy.data.actions, key=lambda x: x.name):
    if not a.name.startswith("A_FP_"):
        continue
    fr = a.frame_range
    fs, fe = int(round(fr[0])), int(round(fr[1]))
    if fe <= fs:
        continue  # single-frame poses cannot drift
    correct = correct_weapon(a.name)
    bad = paths(a.name, NAIVE, fs, fe)
    good = paths(a.name, correct, fs, fe)
    dl, dr = bad["hand_l"], good["hand_l"]
    ratio = dl / good["hand_l"] if good["hand_l"] > 1e-9 else (999.0 if dl > 1e-9 else 1.0)
    verdict = "ok"
    if ratio > 1.5 or abs(dl - good["hand_l"]) > 5.0:
        verdict = "RE-EXPORT (%s vs saved %s)" % (correct, NAIVE)
        affected.append((a.name, correct, fs, fe, dl, good["hand_l"]))
    print("%-18s %3d..%-3d | L=%8.3f R=%8.3f mag=%8.3f | L=%8.3f R=%8.3f mag=%8.3f | %s" % (
        a.name, fs, fe, bad["hand_l"], bad["hand_r"], bad.get("magazine", -1),
        good["hand_l"], good["hand_r"], good.get("magazine", -1), verdict))

print("\n=== AFFECTED (%d) ===" % len(affected))
for name, w, fs, fe, dl, gl in affected:
    print("  %-22s -> %-14s frames=%d..%d   left-hand path %.3f vs %.3f (x%.1f)" % (
        name, w, fs, fe, dl, gl, dl / gl if gl > 1e-9 else 999.0))

arm.animation_data.action = saved["arm_action"]
wep.animation_data.action = saved["wep_action"]
arm.data.pose_position = saved["arm_pose"]
wep.data.pose_position = saved["wep_pose"]
for t, m in saved_arm_nla:
    t.mute = m
for t, m in saved_wep_nla:
    t.mute = m
scn.frame_start, scn.frame_end = saved["fs"], saved["fe"]
scn.frame_set(saved["frame"])
print("\nRESTORED arms=%s weapon=%s frame=%d frames=%d..%d" % (
    saved["arm_action"].name if saved["arm_action"] else None,
    saved["wep_action"].name if saved["wep_action"] else None,
    saved["frame"], saved["fs"], saved["fe"]))
print("DONE")
