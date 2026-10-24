import bpy, sys

# Clean game export of the spider pack (Raw\Characters\Spider on the Drive): the mesh's own odd
# transform (rotated, 0.057 scale under the rig) baked into its vertices, the game size baked into the
# rig, clips renamed, unskinned IK helper bones left out.
#   blender -b --factory-startup --python clean_export.py -- "<Raw>/walk.fbx" <project>/assets/Characters/Spider/Spider.fbx
args = sys.argv[sys.argv.index("--") + 1:]
src, out = args
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=src)

arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
mesh = next(o for o in bpy.data.objects if o.type == "MESH")
arm.name = "Spider"
arm.data.name = "Spider"
mesh.name = "SpiderBody"

# Bake the mesh's local transform into its vertices (visual result unchanged).
bpy.ops.object.select_all(action="DESELECT")
mesh.select_set(True)
bpy.context.view_layer.objects.active = mesh
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

# The base pose's material slot that no face uses.
for i in reversed(range(len(mesh.data.materials))):
    if mesh.data.materials[i] and mesh.data.materials[i].name == "fur-mat":
        mesh.data.materials.pop(index=i)

# Every clip FBX of the pack carries all 11 actions; keep this file's copies, renamed.
names = {"end jump": "end_jump", "fast walk to attack": "lunge_attack", "idle": "idle", "idle  to jump": "idle_to_jump",
         "idle to attack": "attack", "idle2": "idle2", "in air": "in_air", "run": "run", "slow walk": "slow_walk",
         "start jump": "start_jump", "walk": "walk", "walk to jump": "walk_to_jump"}
for a in list(bpy.data.actions):
    short = a.name.split("|")[-1]
    if short in names:
        a.name = names[short]
        a.use_fake_user = True
    else:
        bpy.data.actions.remove(a)
print("actions", sorted(a.name for a in bpy.data.actions))

# The FBX import keyed the rig object's own transform into every clip: exported as is, each clip would put the rig
# back at the pack's scale and place, whatever it is given below. Only the bones' keys stay.
for a in bpy.data.actions:
    for layer in a.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                for fc in [fc for fc in bag.fcurves if not fc.data_path.startswith("pose.bones")]:
                    bag.fcurves.remove(fc)

# No action assigned and every bone at rest, so the rest pose exported is the bind pose, not a clip's frame.
if arm.animation_data:
    arm.animation_data.action = None
for pb in arm.pose.bones:
    pb.location = (0.0, 0.0, 0.0)
    pb.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    pb.rotation_euler = (0.0, 0.0, 0.0)
    pb.scale = (1.0, 1.0, 1.0)

# Game size baked into the rig's own transform, so the skin, bind pose and every clip's keys agree (an importer
# scale only reaches the bind pose). 0.40: the splayed rest pose is ~1.3 m across, the standing stance ~1.2 m - a
# medium dog.
from mathutils import Vector
arm.scale = (0.40, 0.40, 0.40)
bpy.context.view_layer.update()
pts = [mesh.matrix_world @ v.co for v in mesh.data.vertices]
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
arm.location -= Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z))   # feet on the floor, centred
bpy.context.view_layer.update()
print("size", [round(x, 3) for x in (hi - lo)])

bpy.ops.object.select_all(action="SELECT")
bpy.ops.export_scene.fbx(filepath=out, use_selection=True, object_types={"ARMATURE", "MESH"},
                         use_armature_deform_only=True, add_leaf_bones=False, bake_anim=True,
                         bake_anim_use_all_actions=True, bake_anim_use_nla_strips=False,
                         bake_anim_force_startend_keying=True, bake_anim_simplify_factor=0.0,
                         mesh_smooth_type="FACE", use_tspace=False, path_mode="STRIP",
                         axis_forward="-Z", axis_up="Y", apply_unit_scale=True, apply_scale_options="FBX_SCALE_NONE")
print("exported", out)

