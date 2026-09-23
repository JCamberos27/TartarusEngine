import bpy

# Read-only inspection of the .blend's saved animation state.
# Never saves. Run as: blender -b "<blend>" --python work\inspect_blend.py
path = bpy.data.filepath
print("BLEND_FILE:", path)

try:
    with open(path, "rb") as f:
        hdr = f.read(12)
    # "BLENDER" + ptr-size('_' 32bit / 'v' 64bit) + endian('0' LE / '1' BE) + 3-char version
    print("HEADER:", hdr.decode("ascii", "replace"))
except Exception as e:
    print("HEADER_READ_FAILED:", e)

print("BLENDER_VERSION:", bpy.app.version_string)
sc = bpy.context.scene
print("FRAMES: current=%d start=%d end=%d fps=%d" % (sc.frame_current, sc.frame_start, sc.frame_end, sc.render.fps))

print("--- objects carrying animation data ---")
for ob in bpy.data.objects:
    ad = ob.animation_data
    if ad is None:
        if ob.type == "ARMATURE":
            print("ARMATURE %-14s (no animation_data)" % ob.name)
        continue
    if ob.type not in {"ARMATURE", "MESH"} and ad.action is None and len(ad.nla_tracks) == 0:
        continue
    act = ad.action.name if ad.action else None
    slot = None
    try:
        slot = ad.action_slot.name_display if ad.action_slot else None
    except Exception:
        pass
    tracks = []
    for t in ad.nla_tracks:
        strips = ",".join(
            "%s%s" % (s.action.name if s.action else "<none>", "[MUTE]" if s.mute else "")
            for s in t.strips
        )
        tracks.append("%s%s{%s}%s" % (t.name, "[MUTE]" if t.mute else "", strips,
                                      "[SOLO]" if t.is_solo else ""))
    print("OBJ %-14s type=%-7s action=%s slot=%s nla=%s" % (
        ob.name, ob.type, act, slot, tracks if tracks else "none"))

print("--- candidate neutral actions present in file ---")
for name in ("A_FP_Idle", "A_FP_Aim", "A_FP_Tac_Reload", "A_W_ADS", "A_W_Tac_Reload", "A_W_Mag_Check"):
    act = bpy.data.actions.get(name)
    if act is None:
        print("  %-20s MISSING" % name)
        continue
    fr = ("%.0f..%.0f" % (act.frame_range[0], act.frame_range[1]))
    slots = [s.name_display for s in getattr(act, "slots", [])]
    print("  %-20s range=%-10s layers=%d slots=%s" % (name, fr, len(act.layers), slots))

print("--- counts ---")
print("actions=%d objects=%d" % (len(bpy.data.actions), len(bpy.data.objects)))
print("INSPECT_DONE")
