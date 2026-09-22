import bpy, gzip, zlib

# Read-only preflight: what Blender version wrote this file, and is the rig saved in
# POSE or REST position? Never saves.
# Run as: blender -b "<blend>" --python work\preflight_neutral.py
path = bpy.data.filepath
with open(path, "rb") as f:
    magic = f.read(4)

version = "unknown"
if magic[:7] == b"BLEND" or magic[:6] == b"BLENDER":
    mode = "plain (uncompressed)"
    with open(path, "rb") as f:
        hdr = f.read(12)
    version = hdr.decode("ascii", "replace")[9:12]
elif magic == b"\x1f\x8b":
    mode = "gzip compressed"
    try:
        with gzip.open(path, "rb") as g:
            hdr = g.read(12)
        version = hdr.decode("ascii", "replace")[9:12]
    except Exception as e:
        version = "gzip-open failed: %s" % e
elif magic == b"\x28\xb5\x2f\xfd":
    mode = "zstd compressed"
    try:  # python 3.14+; Blender 5.1 ships older, so this may not exist
        from compression import zstd
        with open(path, "rb") as f:
            hdr = zstd.decompress(f.read(65536))[:12]
        version = hdr.decode("ascii", "replace")[9:12]
    except Exception as e:
        version = "zstd (header not readable from stdlib: %s)" % type(e).__name__
else:
    mode = "unrecognised magic %r" % (magic,)

print("COMPRESSION:", mode)
print("SAVED_AS_VERSION:", version, " (e.g. 501 = Blender 5.01; this is", bpy.app.version_string, ")")
print("PY_VERSION:", bpy.app.version)

print("--- rig pose_position (REST would bake a T-pose through a naive export) ---")
for name in ("Armature", "AK"):
    ob = bpy.data.objects.get(name)
    if ob is None or ob.type != "ARMATURE":
        print("  %-10s MISSING or not an armature" % name)
        continue
    print("  %-10s pose_position=%s  action=%s" % (
        name, ob.data.pose_position,
        ob.animation_data.action.name if ob.animation_data and ob.animation_data.action else None))

print("--- all %d actions ---" % len(bpy.data.actions))
for act in sorted(bpy.data.actions, key=lambda a: a.name):
    print("  %-26s %.0f..%.0f" % (act.name, act.frame_range[0], act.frame_range[1]))

print("--- fingerprint (compare before/after the save) ---")
sc = bpy.context.scene
print("  objects=%d meshes=%d armatures=%d actions=%d materials=%d" % (
    len(bpy.data.objects), len(bpy.data.meshes), len(bpy.data.armatures),
    len(bpy.data.actions), len(bpy.data.materials)))
print("  frames current=%d start=%d end=%d fps=%d" % (
    sc.frame_current, sc.frame_start, sc.frame_end, sc.render.fps))
print("  PREFLIGHT_DONE")
