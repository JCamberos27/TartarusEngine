import bpy

# Set the .blend's *saved* animation state to the neutral pairing:
#   Armature (arms) -> A_FP_Idle      (default state, 0..128)
#   AK     (weapon) -> A_W_ADS        (bind pose, the fallback the engine renders
#                                      for states with no weapon clip)
# This is the pairing export_clip.py already derives for Idle/Walk/Draw/Regrip, so an
# export that does NOT go through export_clip.py can no longer bake against
# A_W_Tac_Reload and silently corrupt an arms clip (FPS_ANIMATION_INVESTIGATION.md
# UPDATE 6). NLA tracks and pose_position are deliberately left untouched.
# Run as: blender -b "<blend>" --python work\set_neutral_action.py
ARMS_OBJECT = "Armature"
WEAPON_OBJECT = "AK"
ARMS_ACTION = "A_FP_Idle"
WEAPON_ACTION = "A_W_ADS"

pairs = ((ARMS_OBJECT, ARMS_ACTION), (WEAPON_OBJECT, WEAPON_ACTION))

print("=== before ===")
for obname, _ in pairs:
    ob = bpy.data.objects.get(obname)
    ad = ob.animation_data if ob else None
    print("  %-10s action=%s pose_position=%s" % (
        obname,
        ad.action.name if ad and ad.action else None,
        ob.data.pose_position if ob and ob.type == "ARMATURE" else "n/a"))

ok = True
for obname, actname in pairs:
    ob = bpy.data.objects.get(obname)
    act = bpy.data.actions.get(actname)
    if ob is None or ob.type != "ARMATURE":
        print("ERROR: object %r is not an armature" % obname)
        ok = False
        continue
    if act is None:
        print("ERROR: action %r does not exist" % actname)
        ok = False
        continue
    ad = ob.animation_data_create()
    ad.action = act
    # Blender 4.4+/5.x layered actions: make sure a slot is bound, else the action
    # is assigned but evaluates to nothing.
    if getattr(act, "slots", None) and ad.action_slot is None:
        ad.action_slot = act.slots[0]
    print("  set %-10s -> %s (slot=%s)" % (
        obname, ad.action.name if ad.action else None,
        ad.action_slot.name_display if ad.action_slot else None))

if not ok:
    print("ABORTING: nothing saved")
    raise SystemExit(1)

bpy.context.scene.frame_set(0)

print("=== after (in memory) ===")
for obname, _ in pairs:
    ob = bpy.data.objects.get(obname)
    ad = ob.animation_data
    nla = sum(1 for t in ad.nla_tracks)
    muted = sum(1 for t in ad.nla_tracks if t.mute)
    print("  %-10s action=%s slot=%s nla_tracks=%d (muted=%d) pose_position=%s" % (
        obname, ad.action.name if ad.action else None,
        ad.action_slot.name_display if ad.action_slot else None,
        nla, muted, ob.data.pose_position))

print("  frames current=%d start=%d end=%d" % (
    bpy.context.scene.frame_current,
    bpy.context.scene.frame_start,
    bpy.context.scene.frame_end))
print("  objects=%d meshes=%d armatures=%d actions=%d materials=%d" % (
    len(bpy.data.objects), len(bpy.data.meshes), len(bpy.data.armatures),
    len(bpy.data.actions), len(bpy.data.materials)))

# compress=True keeps the zstd container the file already uses; the default would
# rewrite ~305 MB uncompressed.
bpy.ops.wm.save_mainfile(compress=True)
print("SAVED:", bpy.data.filepath)
print("APPLY_DONE")
