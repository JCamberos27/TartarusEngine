import bpy

def bbox(ob):
    mw = ob.matrix_world
    lo = [1e30] * 3
    hi = [-1e30] * 3
    for v in ob.data.vertices:
        w = mw @ v.co
        for i in range(3):
            lo[i] = min(lo[i], w[i])
            hi[i] = max(hi[i], w[i])
    return lo, hi

print("FRAME", bpy.context.scene.frame_current)
for ob in bpy.data.objects:
    if ob.type == 'ARMATURE':
        mw = ob.matrix_world
        print("ARMATURE", ob.name,
              "mw_trans=", [round(x, 4) for x in mw.translation],
              "euler=", [round(x, 4) for x in mw.to_euler()],
              "scale=", [round(x, 4) for x in mw.to_scale()],
              "children=", [c.name for c in ob.children])

for ob in bpy.data.objects:
    if ob.type != 'MESH':
        continue
    lo, hi = bbox(ob)
    print("MESH", ob.name,
          "| parent=", ob.parent.name if ob.parent else None,
          "| mw_trans=", [round(x, 4) for x in ob.matrix_world.translation],
          "| mw_euler=", [round(x, 4) for x in ob.matrix_world.to_euler()],
          "| mw_scale=", [round(x, 4) for x in ob.matrix_world.to_scale()],
          "| size=", [round(hi[i] - lo[i], 4) for i in range(3)],
          "| lo=", [round(x, 4) for x in lo],
          "| hi=", [round(x, 4) for x in hi],
          "| mods=", [m.type for m in ob.modifiers],
          "| verts=", len(ob.data.vertices))
