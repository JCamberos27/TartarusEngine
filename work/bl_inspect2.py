import bpy
from mathutils import Vector

deps = bpy.context.evaluated_depsgraph_get()

def world_bbox(ob_eval, mw):
    lo = [1e30] * 3
    hi = [-1e30] * 3
    for v in ob_eval.data.vertices:
        w = mw @ v.co
        for i in range(3):
            lo[i] = min(lo[i], w[i])
            hi[i] = max(hi[i], w[i])
    return lo, hi

print("FRAME", bpy.context.scene.frame_current)

for name in ("SK_Manny_Arms", "aks74u", "SK_Manny_Simple"):
    ob = bpy.data.objects.get(name)
    if not ob:
        print("MISSING", name)
        continue
    ev = ob.evaluated_get(deps)
    lo, hi = world_bbox(ev, ev.matrix_world)
    print("EVAL", name,
          "| lo=", [round(x, 4) for x in lo],
          "| hi=", [round(x, 4) for x in hi],
          "| size=", [round(hi[i] - lo[i], 4) for i in range(3)],
          "| center=", [round((hi[i] + lo[i]) / 2, 4) for i in range(3)],
          "| mw_trans=", [round(x, 4) for x in ob.matrix_world.translation])

# hand / grip landmarks
for arm_name in ("Armature", "AK"):
    arm_ob = bpy.data.objects.get(arm_name)
    if not arm_ob or arm_ob.type != 'ARMATURE':
        continue
    pb = arm_ob.pose.bones
    print("BONES", arm_name, "count=", len(pb))
    for b in pb:
        low = b.name.lower()
        if any(k in low for k in ("hand", "grip", "weapon", "socket", "head", "eye", "root", "ik")):
            w = arm_ob.matrix_world @ b.head
            print("  BONE", arm_name, b.name, "head_world=", [round(x, 4) for x in w])

# armature object transforms / rest vs pose
for arm_name in ("Armature", "AK"):
    arm_ob = bpy.data.objects.get(arm_name)
    if not arm_ob:
        continue
    print("ARM", arm_name,
          "mw_trans=", [round(x, 4) for x in arm_ob.matrix_world.translation],
          "mw_euler=", [round(x, 4) for x in arm_ob.matrix_world.to_euler()],
          "mode=", arm_ob.mode,
          "posed_bones=", sum(1 for b in arm_ob.pose.bones
                              if b.matrix_basis.to_translation().length > 1e-6
                              or abs(b.matrix_basis.to_euler().x) > 1e-6
                              or abs(b.matrix_basis.to_euler().y) > 1e-6
                              or abs(b.matrix_basis.to_euler().z) > 1e-6))
