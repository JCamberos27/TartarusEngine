import bpy, sys
arm = bpy.data.objects["Armature"]
pb = arm.pose.bones
db = arm.data.bones
print("POSE_BONES=%d DATA_BONES=%d DEFORM=%d" % (len(pb), len(db), sum(1 for b in db if b.use_deform)))
act = bpy.data.actions.get("A_FP_Idle")
n_bones_with_fc = set()
n_fc = 0
for layer in act.layers:
    for strip in layer.strips:
        for cb in strip.channelbags:
            for fc in cb.fcurves:
                n_fc += 1
                dp = fc.data_path
                if dp.startswith('pose.bones["'):
                    n_bones_with_fc.add(dp.split('"')[1])
print("A_FP_Idle fcurves=%d distinct_bones=%d" % (n_fc, len(n_bones_with_fc)))
act2 = bpy.data.actions.get("A_FP_ADS")
n2, b2 = 0, set()
for layer in act2.layers:
    for strip in layer.strips:
        for cb in strip.channelbags:
            for fc in cb.fcurves:
                n2 += 1
                dp = fc.data_path
                if dp.startswith('pose.bones["'):
                    b2.add(dp.split('"')[1])
print("A_FP_ADS fcurves=%d distinct_bones=%d" % (n2, len(b2)))
print("first_paths_idle=", [fc.data_path + "[%d]" % fc.array_index for layer in act.layers for strip in layer.strips for cb in strip.channelbags for fc in list(cb.fcurves)[:6]])
sys.stdout.flush()
