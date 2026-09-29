# Read-only: per frame of each reload clip, the left hand / Shell relative to Main (gun) and the
# distance from the hand to the gun mesh's box (Main space). Never saves.
import bpy, mathutils as M
arm = bpy.data.objects["Armature"]; wep = bpy.data.objects["Armature.001"]; mesh = bpy.data.objects["Remington870"]
scn = bpy.context.scene
for o in (arm, wep):
    for t in o.animation_data.nla_tracks: t.mute = True
def W(ob, b): return ob.matrix_world @ ob.pose.bones[b].matrix
arm.animation_data.action = bpy.data.actions["A_FP_Idle"]; wep.animation_data.action = bpy.data.actions["A_W_Idle"]
scn.frame_set(0)
dg = bpy.context.evaluated_depsgraph_get()
me = mesh.evaluated_get(dg).to_mesh()
mi = W(wep, "Main").inverted() @ mesh.matrix_world
pts = [mi @ v.co for v in me.vertices]
lo = M.Vector([min(p[i] for p in pts) for i in range(3)]); hi = M.Vector([max(p[i] for p in pts) for i in range(3)])
print("gun box (Main cm)", tuple(round(x,1) for x in lo), tuple(round(x,1) for x in hi))
def boxdist(p):
    d = M.Vector([max(lo[i]-p[i], 0, p[i]-hi[i]) for i in range(3)]); return d.length
for fp, wp in [("A_FP_reload_start","A_W_Reload_start"),("A_FP_Reload_start_empty","A_W_Reload_start_empty"),
               ("A_FP_Reload_loop","A_W_Reload_loop"),("A_FP_Reload_loop_end","A_W_Reload_loop_end"),("A_FP_Reload_end","A_W_Reload_end")]:
    arm.animation_data.action = bpy.data.actions[fp]; wep.animation_data.action = bpy.data.actions[wp]
    a = bpy.data.actions[fp]; fs, fe = int(a.frame_range[0]), int(a.frame_range[1])
    print("==", fp, fs, fe)
    for f in range(fs, fe+1, 4):
        scn.frame_set(f)
        main = W(wep, "Main"); mi = main.inverted()
        h = mi @ W(arm, "hand_l").translation; s = mi @ W(wep, "Shell").translation
        head = W(arm, "head").translation
        print("f%3d hand(Main) %6.1f %6.1f %6.1f boxd %5.1f | shell(Main) %6.1f %6.1f %6.1f boxd %5.1f | main-from-head %6.1f %6.1f %6.1f"
              % (f, *h, boxdist(h), *s, boxdist(s), *(main.translation - head)))
