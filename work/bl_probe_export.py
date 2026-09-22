# READ-ONLY. Reports the FBX exporter's actual operator properties (Blender 5 renamed/added a
# few, so we never guess kwargs) plus the armature/action state we need to reproduce the
# existing clip export recipe exactly. Never writes, never saves the .blend.
import bpy, sys

props = bpy.ops.export_scene.fbx.get_rna_type().properties
print("=== FBX OPERATOR PROPS ===")
for p in props:
    if p.identifier == "rna_type":
        continue
    if p.type == "ENUM":
        items = [i.identifier for i in p.enum_items]
        print("  %-34s enum   default=%r  %s" % (p.identifier, getattr(p, "default", None), items))
    else:
        print("  %-34s %-6s default=%r" % (p.identifier, p.type, getattr(p, "default", None)))

arm = bpy.data.objects.get("Armature")
print("=== ARMATURE ===")
if arm:
    ad = arm.animation_data
    print("  pose_position =", arm.data.pose_position)
    print("  current action =", ad.action.name if (ad and ad.action) else None)
    print("  nla strips =", [s.name for s in ad.nla_tracks] if ad else None)
else:
    print("  MISSING")

print("=== ACTIONS ===")
for a in sorted(bpy.data.actions, key=lambda x: x.name):
    fs, fe = a.frame_range
    fcurves = 0
    try:
        for layer in a.layers:
            for strip in layer.strips:
                for cb in strip.channelbags:
                    fcurves += len(cb.fcurves)
    except Exception as e:
        fcurves = -1
    if a.name.startswith("A_FP_ADS") or a.name in ("A_FP_Idle", "A_FP_Walk"):
        print("  %-24s range=(%s,%s) fcurves=%s" % (a.name, fs, fe, fcurves))

print("=== SCENE ===")
sc = bpy.context.scene
print("  frame_start/end =", sc.frame_start, sc.frame_end, " use_frame_range =", getattr(sc, "use_frame_range", None))
print("  fps =", sc.render.fps, "/", sc.render.fps_base)
print("  unit scale =", sc.unit_settings.scale_length)

print("=== SELECTABLE OBJECTS ===")
want = ["Armature", "SK_Manny_Simple", "SK_Manny_Arms",
        "Quantum_Body_Full", "Quantum_Body_Full.001", "Quantum_Body_Full.002"]
for n in want:
    ob = bpy.data.objects.get(n)
    print("  %-24s %s type=%s visible=%s" % (n, "OK" if ob else "MISSING",
                                             ob.type if ob else "-",
                                             (not ob.hide_viewport) if ob else "-"))
print("=== OTHER ARMATURE-LIKE ===")
for ob in bpy.data.objects:
    if ob.type == "ARMATURE":
        print("  ", ob.name)
sys.stdout.flush()
