# READ-ONLY: why does the LEFT arm move ~25 units during A_FP_Idle while the right is still?
#
# CB_ik_hand_l carries  CHILD_OF -> AK:magazine (influence 1.0), so the whole left arm is glued
# to the WEAPON armature's magazine bone. The weapon armature has its OWN action. If the arms
# export runs A_FP_Idle while the AK is still playing whatever action the file was saved on,
# the bake bakes a left hand chasing the wrong weapon animation.
#
# This evaluates A_FP_Idle (and A_FP_Sprint as a control) under several AK action assignments
# and reports the resulting left-arm motion, plus the magazine's own motion.
# Nothing is saved: every action / influence / frame is restored in memory, no .save().
#
#   blender.exe --background "<blend>" --python work/bl_idle_probe2.py
import bpy

ARMS = "Armature"
WEAP = "AK"
WATCH = ["clavicle_l", "upperarm_l", "lowerarm_l", "hand_l", "ik_hand_l",
         "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r", "ik_hand_r"]
MAG_BONES = ["magazine", "mag2", "root", "bolt", "trigger"]


def frame_list(fs, fe, n=16):
    step = max(1, (fe - fs) // n)
    fr = list(range(fs, fe + 1, step))
    if fr[-1] != fe:
        fr.append(fe)
    return fr


scn = bpy.context.scene
arm = bpy.data.objects[ARMS]
wep = bpy.data.objects[WEAP]

# ---- capture everything we might touch ---------------------------------------------------
saved = {
    "arm_action": arm.animation_data.action if arm.animation_data else None,
    "wep_action": wep.animation_data.action if wep.animation_data else None,
    "arm_pose": arm.data.pose_position,
    "wep_pose": wep.data.pose_position,
    "frame": scn.frame_current,
    "fs": scn.frame_start,
    "fe": scn.frame_end,
}
saved_wep_nla = [(t, t.mute) for t in wep.animation_data.nla_tracks] if wep.animation_data else []
saved_arm_nla = [(t, t.mute) for t in arm.animation_data.nla_tracks] if arm.animation_data else []

print("=== ACTIONS of interest ===")
for want in ("A_W_Idle", "A_W_Walk", "A_W_ADS", "A_W_Tac_Reload", "A_W_Sprint",
             "A_FP_Idle", "A_FP_Walk", "A_FP_Sprint", "A_FP_ADS"):
    a = bpy.data.actions.get(want)
    if a is None:
        print("  %-20s ABSENT" % want)
    else:
        fr = a.frame_range
        print("  %-20s range=(%d..%d)" % (want, int(round(fr[0])), int(round(fr[1]))))

print("\n=== current assignments (the file's saved state) ===")
print("  %s action = %s" % (ARMS, saved["arm_action"].name if saved["arm_action"] else None))
print("  %s action = %s" % (WEAP, saved["wep_action"].name if saved["wep_action"] else None))
for name, tracks in ((ARMS, saved_arm_nla), (WEAP, saved_wep_nla)):
    muted = sum(1 for _, m in tracks if m)
    print("  %s NLA tracks=%d muted=%d unmuted=%d" % (name, len(tracks), muted, len(tracks) - muted))
    for t, m in tracks:
        if not m:
            print("      UNMUTED track %-26s strips=%s" % (
                t.name, [getattr(s, "action", None).name if getattr(s, "action", None) else None
                         for s in t.strips]))

pb = arm.pose.bones.get("CB_ik_hand_l")
print("\n=== CB_ik_hand_l constraints ===")
for i, c in enumerate(pb.constraints):
    print("  [%d] %-12s target=%s sub=%s influence=%.3f mute=%s" % (
        i, c.type, c.target.name if c.target else "-", getattr(c, "subtarget", "") or "-",
        c.influence, c.mute))


def set_action(obj, action):
    if obj.animation_data is None:
        obj.animation_data_create()
    obj.animation_data.action = action


def nla_mute(obj, mute):
    if not obj.animation_data:
        return
    for t in obj.animation_data.nla_tracks:
        t.mute = mute


def world_path(obj, bone_name, frames):
    """Accumulated world-space path of a bone's origin over `frames` (Blender units)."""
    prev = None
    path = 0.0
    maxstep = 0.0
    first = last = None
    for f in frames:
        scn.frame_set(f)
        bpy.context.view_layer.update()
        pb = obj.pose.bones.get(bone_name)
        if pb is None:
            return None
        loc = (obj.matrix_world @ pb.matrix).translation.copy()
        if prev is not None:
            d = (loc - prev).length
            path += d
            maxstep = max(maxstep, d)
        else:
            first = loc.copy()
        prev = loc
        last = loc.copy()
    return path, maxstep, (first - last).length if first and last else 0.0


def evaluate(label, arms_action, wep_action, fs, fe):
    set_action(arm, bpy.data.actions[arms_action])
    set_action(wep, bpy.data.actions[wep_action] if wep_action else None)
    nla_mute(arm, True)
    nla_mute(wep, True)
    arm.data.pose_position = "POSE"
    wep.data.pose_position = "POSE"
    scn.frame_start, scn.frame_end = fs, fe
    frames = frame_list(fs, fe)
    print("\n--- %s   arms=%s  weapon=%s  frames=%d..%d (%d samples)" % (
        label, arms_action, wep_action or "NONE", fs, fe, len(frames)))
    for b in WATCH:
        r = world_path(arm, b, frames)
        if r:
            print("      arms:%-14s path=%8.3f  maxStep=%7.3f  end-vs-start=%7.3f" % (b, r[0], r[1], r[2]))
    for b in MAG_BONES:
        r = world_path(wep, b, frames)
        if r:
            print("      wep:%-15s path=%8.3f  maxStep=%7.3f  end-vs-start=%7.3f" % (b, r[0], r[1], r[2]))


A_IDLE_FR = (0, 128)
A_SPRINT_FR = (0, 44)

# What the file's saved state produces - this is what a naive re-export would bake.
evaluate("BASELINE (file as saved)", "A_FP_Idle", saved["wep_action"].name if saved["wep_action"] else None,
         *A_IDLE_FR)

# Weapon animation removed entirely: is the left arm motion coming from the weapon?
evaluate("weapon animation OFF", "A_FP_Idle", None, *A_IDLE_FR)

# The weapon pose the engine actually renders for Idle/Walk (they have no weapon clip, so the
# weapon sits on its bind pose, which is the ADS export).
if bpy.data.actions.get("A_W_ADS"):
    evaluate("weapon on A_W_ADS", "A_FP_Idle", "A_W_ADS", *A_IDLE_FR)

# Control: Sprint has a matching weapon action - does pairing them clean the left arm up?
evaluate("SPRINT paired correctly", "A_FP_Sprint", "A_W_Sprint", *A_SPRINT_FR)
evaluate("SPRINT with the file's weapon action", "A_FP_Sprint",
         saved["wep_action"].name if saved["wep_action"] else None, *A_SPRINT_FR)

# --- restore in memory; the .blend on disk is untouched ------------------------------------
set_action(arm, saved["arm_action"])
set_action(wep, saved["wep_action"])
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
