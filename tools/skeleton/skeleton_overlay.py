# Side and front renders of the Skeleton race's parts over a see-through Quantum body, at rest: anything not sitting
# inside the body (a bone poking out, a gap at a joint) shows.
#
#   blender -b --factory-startup --python tools/skeleton/skeleton_overlay.py -- \
#       project/assets/Characters/Quantum project/assets/Characters/RealisticSkeleton <out prefix>
#
# Writes <out prefix>side.png and <out prefix>front.png.
import bpy, sys, math
from mathutils import Vector
qdir, sdir, out = sys.argv[sys.argv.index('--') + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)

def load(path, color, alpha):
    seen = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path)
    mat = bpy.data.materials.new('m'); mat.diffuse_color = (*color, alpha)
    for o in [o for o in bpy.data.objects if o not in seen]:
        if o.type == 'MESH':
            o.data.materials.clear(); o.data.materials.append(mat)
            for m in list(o.modifiers): o.modifiers.remove(m)  # rest shape
        elif o.type == 'ARMATURE':
            o.hide_render = True

for rel in ('Models/European/Quantum_Torso.fbx', 'Models/European/Quantum_Legs.fbx', 'Models/European/Quantum_Head.fbx',
            'FirstPerson/Quantum_Arms_FP.fbx', 'Models/European/Quantum_Feet.fbx'):
    load(qdir + '/' + rel, (0.9, 0.55, 0.4), 0.25)
for p in ('Torso', 'Arms', 'Legs', 'Feet', 'Pelvis', 'Head'):
    load(sdir + '/Skeleton_%s.fbx' % p, (0.95, 0.92, 0.8), 1.0)

meshes = [o for o in bpy.data.objects if o.type == 'MESH']
lo = Vector((1e9,) * 3); hi = Vector((-1e9,) * 3)
for o in meshes:
    for c in o.bound_box:
        w = o.matrix_world @ Vector(c)
        lo = Vector(map(min, lo, w)); hi = Vector(map(max, hi, w))
center = (lo + hi) / 2; size = max(hi - lo)
print('BOUNDS', lo, hi)

scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.display.shading.color_type = 'MATERIAL'
scene.display.shading.show_xray = False
scene.render.film_transparent = False
scene.render.resolution_x = 1400; scene.render.resolution_y = 1800
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
scene.collection.objects.link(cam); scene.camera = cam
cam.data.type = 'ORTHO'; cam.data.ortho_scale = size * 1.1
for name, d in (('side', Vector((1, 0, 0))), ('front', Vector((0, -1, 0)))):
    cam.location = center + d * size * 3
    cam.rotation_euler = (d * -1).to_track_quat('-Z', 'Y').to_euler()
    scene.render.filepath = out + name + '.png'
    bpy.ops.render.render(write_still=True)
print('DONE')
