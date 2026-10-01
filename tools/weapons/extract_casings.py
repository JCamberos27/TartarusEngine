"""Extract the ejected-casing meshes from the weapon FBXs and decimate them.

    blender -b --factory-startup -P tools/weapons/extract_casings.py

AKS-74U: the magazine's top rounds in the weapon mesh are separate loose parts - a 284-tri case and a
174-tri bullet (material cartridge_545x39mm). Only the case is kept: a spent casing has no bullet.
Remington 870: the loading shell is the 316-tri loose part on the Shell bone (Remington atlas).

Each part is moved to the origin in metres with its long axis on Blender +Z (engine +Y after the FBX
axis conversion), decimated to at most MAX_TRIS triangles (the UVs survive a collapse decimate) and
exported next to the weapon. The .fpsanim "eject" block names the result and its material.
"""
import os

import bmesh
import bpy
from mathutils import Matrix, Vector

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WEAPONS = os.path.join(ROOT, "project", "assets", "Weapons")
MAX_TRIS = 100

JOBS = [
    # (source fbx, material slot name, triangles of the part to keep, output fbx)
    (os.path.join(WEAPONS, "AKS74U", "Weapon", "AKS-74U_A_W_Fire.fbx"), "cartridge_545x39mm", 284,
     os.path.join(WEAPONS, "AKS74U", "Casing_545x39.fbx")),
    (os.path.join(WEAPONS, "Remington870", "Weapon", "Remington870_A_W_Idle.fbx"), "02___Default", 316,
     os.path.join(WEAPONS, "Remington870", "Shell_12ga.fbx")),
]


def tri_count(faces):
    return sum(len(f.verts) - 2 for f in faces)


def islands(bm):
    bm.faces.ensure_lookup_table()
    seen, out = set(), []
    for f in bm.faces:
        if f.index in seen:
            continue
        stack, comp = [f], []
        while stack:
            x = stack.pop()
            if x.index in seen:
                continue
            seen.add(x.index)
            comp.append(x)
            for e in x.edges:
                stack.extend(e.link_faces)
        out.append(comp)
    return out


def extract(src, material, tris, out):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=src)
    obj = next(o for o in bpy.context.scene.objects if o.type == "MESH")
    slot = [m.name if m else "" for m in obj.data.materials].index(material)

    bm = bmesh.new()
    bm.from_mesh(obj.data)
    comps = islands(bm)
    keep = next(c for c in comps if tri_count(c) == tris and all(f.material_index == slot for f in c))
    keep_set = set(keep)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f not in keep_set], context="FACES")
    for f in bm.faces:
        f.material_index = 0
    # World space (the imported object carries the FBX unit scale), so the result is in metres.
    bm.transform(obj.matrix_world)
    lo = Vector((min(v.co[i] for v in bm.verts) for i in range(3)))
    hi = Vector((max(v.co[i] for v in bm.verts) for i in range(3)))
    bm.transform(Matrix.Translation(-(lo + hi) * 0.5))
    size = hi - lo
    axis = max(range(3), key=lambda i: size[i])
    if axis == 0:
        bm.transform(Matrix.Rotation(-1.5707963, 4, "Y"))
    elif axis == 1:
        bm.transform(Matrix.Rotation(1.5707963, 4, "X"))

    mesh = bpy.data.meshes.new(os.path.splitext(os.path.basename(out))[0])
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(obj.data.materials[slot])
    part = bpy.data.objects.new(mesh.name, mesh)
    bpy.context.scene.collection.objects.link(part)
    for o in list(bpy.context.scene.objects):
        if o is not part:
            bpy.data.objects.remove(o, do_unlink=True)

    before = sum(len(p.vertices) - 2 for p in mesh.polygons)
    bpy.context.view_layer.objects.active = part
    part.select_set(True)
    ratio = min(1.0, MAX_TRIS / before)
    while True:
        mod = part.modifiers.new("Decimate", "DECIMATE")
        mod.ratio = ratio
        dg = bpy.context.evaluated_depsgraph_get()
        ev = part.evaluated_get(dg).to_mesh()
        n = sum(len(p.vertices) - 2 for p in ev.polygons)
        part.evaluated_get(dg).to_mesh_clear()
        if n <= MAX_TRIS:
            break
        part.modifiers.remove(mod)
        ratio *= 0.95
    bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.ops.object.shade_smooth_by_angle(angle=0.7)
    tri = part.modifiers.new("Triangulate", "TRIANGULATE")
    bpy.ops.object.modifier_apply(modifier=tri.name)
    after = len(part.data.polygons)

    bpy.ops.export_scene.fbx(filepath=out, use_selection=True, object_types={"MESH"},
                             use_mesh_modifiers=True, add_leaf_bones=False, bake_anim=False,
                             mesh_smooth_type="FACE")
    print("[extract_casings] %s: %d -> %d tris, %.1f x %.1f mm" %
          (os.path.basename(out), before, after, size[axis] * 1000.0, sorted(size)[1] * 1000.0))


for job in JOBS:
    extract(*job)
