"""Bakes Real Blood's headshot gore meshes into static, metre-scale FBX + texture sets the engine loads
(docs/BLOOD_FX.md, v2: headshot gore). Run once with Blender (5.x), after extracting the Knife packs:

  blender -b --factory-startup -P tools/knife_gore_bake.py -- "<ASSETS TO IMPORT dir>" "<project>/assets/Effects/Knife/Gore"

Writes (git-ignored, like the rest of the Knife data):
  exploded_head.fbx  - the stump left on the neck: origin at the neck's base, up = +Y, ~24 cm tall
  brain_part_<1-4>.fbx - chunks thrown out, each centred on its own middle
  gore_head_{BaseColor,Normal,Roughness}.png, gore_brain_{...}.png - the texture sets the model loader matches by name
The source meshes are skinned to their own Unity rigs; they're frozen here (no rig in the engine). The head's own
animation bursts it open over its first ~40 frames: the stump is its last frame, pivoted where frame 1's (whole) head
meets the neck.
"""
import os
import sys

import bpy
import numpy as np
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ASSETS = argv[0] if argv else r"C:\Users\jacob\OneDrive\Desktop\ASSETS TO IMPORT"
OUT = argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "project", "assets", "Effects", "Knife", "Gore")
RB = os.path.join(ASSETS, "Knife Real Blood", "Knife", "Real Blood", "Models")
os.makedirs(OUT, exist_ok=True)


def world_points(mesh):
    dg = bpy.context.evaluated_depsgraph_get()
    ev = mesh.evaluated_get(dg)
    m = ev.to_mesh()
    pts = [ev.matrix_world @ v.co for v in m.vertices]
    ev.to_mesh_clear()
    return pts


def bake_mesh(src, name, material, recentre, frame=None):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=src)
    mesh = next(o for o in bpy.context.scene.objects if o.type == "MESH")
    pivot = None
    if frame is not None:  # the pivot from the whole head, the shape from `frame`
        bpy.context.scene.frame_set(1)
        p1 = world_points(mesh)
        mn1 = Vector((min(p.x for p in p1), min(p.y for p in p1), min(p.z for p in p1)))
        mx1 = Vector((max(p.x for p in p1), max(p.y for p in p1), max(p.z for p in p1)))
        pivot = Vector(((mn1.x + mx1.x) * 0.5, (mn1.y + mx1.y) * 0.5, mn1.z))
        bpy.context.scene.frame_set(frame)
    # Freeze the deformed shape (as posed now) into a plain mesh in world space.
    dg = bpy.context.evaluated_depsgraph_get()
    frozen = bpy.data.meshes.new_from_object(mesh.evaluated_get(dg), depsgraph=dg)
    frozen.transform(mesh.matrix_world)
    obj = bpy.data.objects.new(name, frozen)
    for o in list(bpy.context.scene.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    bpy.context.scene.collection.objects.link(obj)
    mesh = obj
    bpy.context.view_layer.objects.active = mesh
    mesh.select_set(True)
    pts = [v.co for v in mesh.data.vertices]
    mn = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    mx = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    if pivot is None:
        pivot = (mn + mx) * 0.5
        if not recentre:
            pivot.z = mn.z  # its base on the origin
    for v in mesh.data.vertices:
        v.co -= pivot
    mesh.name = mesh.data.name = name
    mesh.data.materials.clear()
    mat = bpy.data.materials.new(material)
    mesh.data.materials.append(mat)
    bpy.ops.export_scene.fbx(filepath=os.path.join(OUT, name + ".fbx"), use_selection=True, apply_unit_scale=True,
                             axis_forward="-Z", axis_up="Y", bake_space_transform=True, add_leaf_bones=False,
                             object_types={"MESH"}, mesh_smooth_type="FACE")
    print(f"gore bake: {name}  {tuple(round(c, 3) for c in (mx - mn))} m")


def load_rgba(path):
    img = bpy.data.images.load(path)
    w, h = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)
    return img, px


def save(px, path):
    h, w = px.shape[:2]
    img = bpy.data.images.new(os.path.basename(path), w, h, alpha=True)
    img.pixels = px.reshape(-1).tolist()
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()


def texture_set(prefix, albedo, normal, specsmooth):
    import shutil
    shutil.copyfile(albedo, os.path.join(OUT, prefix + "_BaseColor.png"))
    shutil.copyfile(normal, os.path.join(OUT, prefix + "_Normal.png"))
    _, s = load_rgba(specsmooth)  # Unity Standard (Specular): smoothness in alpha
    rough = np.empty_like(s)
    rough[..., 0] = rough[..., 1] = rough[..., 2] = 1.0 - s[..., 3]
    rough[..., 3] = 1.0
    save(rough, os.path.join(OUT, prefix + "_Roughness.png"))


bake_mesh(os.path.join(RB, "Exploded Head", "new_exploded_head.fbx"), "exploded_head", "gore_head", recentre=False, frame=101)
for i in range(1, 5):
    bake_mesh(os.path.join(RB, "Dismemberment", f"brain_part_{i}.fbx"), f"brain_part_{i}", "gore_brain", recentre=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
eh = os.path.join(RB, "Exploded Head", "Textures")
texture_set("gore_head", os.path.join(eh, "low_exploded_head_new_AlbedoTransparency.png"), os.path.join(eh, "low_exploded_head_new_Normal.png"),
            os.path.join(eh, "low_exploded_head_new_SpecularSmoothness.png"))
dm = os.path.join(RB, "Dismemberment", "Textures")
texture_set("gore_brain", os.path.join(dm, "entrails_low2_entrails_AlbedoTransparency.png"), os.path.join(dm, "entrails_low2_entrails_Normal.png"),
            os.path.join(dm, "entrails_low2_entrails_SpecularSmoothness.png"))
print("gore bake: done ->", OUT)
