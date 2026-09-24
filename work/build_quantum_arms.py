# Builds the Quantum first-person arms: the Quantum body's arms (M_Quantum_Arms faces) skinned to
# the UE5 Manny arms skeleton the AK clips were authored on. The two rigs share every core bone at
# an identical bind pose; Quantum's extra corrective/detail bones have their weights folded into
# the nearest Manny ancestor, so the AK's clips, IK bones and camera bone work unchanged.
#   blender --background --factory-startup --python work/build_quantum_arms.py -- <manny_arms.fbx> <quantum_body_full.fbx> <out.fbx>
import bpy, bmesh, sys
from collections import defaultdict
manny_path, quantum_path, out_path = sys.argv[sys.argv.index('--') + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)

def load(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path, ignore_leaf_bones=False)
    new = [o for o in bpy.data.objects if o not in before]
    return [o for o in new if o.type == 'ARMATURE'][0], [o for o in new if o.type == 'MESH']

M, m_meshes = load(manny_path)
Q, q_meshes = load(quantum_path)
bpy.context.view_layer.update()
for o in m_meshes: bpy.data.objects.remove(o)
body = [o for o in q_meshes if o.name.startswith('Quantum_Body_Full')][0]
for o in q_meshes:
    if o is not body: bpy.data.objects.remove(o)

# Keep only the arms faces.
arms_slot = [i for i, m in enumerate(body.data.materials) if m and m.name.startswith('M_Quantum_Arms')][0]
bm = bmesh.new(); bm.from_mesh(body.data)
bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index != arms_slot], context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
for f in bm.faces: f.material_index = 0
bm.to_mesh(body.data); bm.free()
arms_mat = body.data.materials[arms_slot]
body.data.materials.clear(); body.data.materials.append(arms_mat)
arms_mat.name = 'M_Quantum_Arms'
# The FBX's own texture links point at the vendor's bake folders; the engine material
# (assets/quantum/Materials/M_Quantum_Arms.mat, via the .fpsanim's armsMaterials) supplies them.
if arms_mat.node_tree:
    for n in [n for n in arms_mat.node_tree.nodes if n.type == 'TEX_IMAGE']: arms_mat.node_tree.nodes.remove(n)

# Fold every Quantum-only bone's weights into its nearest ancestor that Manny has.
manny_bones = set(b.name for b in M.data.bones)
def target(name):
    b = Q.data.bones.get(name)
    while b is not None and b.name not in manny_bones: b = b.parent
    return b.name if b else None
groups = {g.index: g.name for g in body.vertex_groups}
acc = defaultdict(lambda: defaultdict(float))
unmapped = set()
for v in body.data.vertices:
    for g in v.groups:
        t = target(groups[g.group])
        if t is None: unmapped.add(groups[g.group]); continue
        acc[v.index][t] += g.weight
for g in list(body.vertex_groups): body.vertex_groups.remove(g)
out_groups = {}
for vi, ws in acc.items():
    top = sorted(ws.items(), key=lambda kv: -kv[1])[:4]
    s = sum(w for _, w in top) or 1.0
    for name, w in top:
        if name not in out_groups: out_groups[name] = body.vertex_groups.new(name=name)
        out_groups[name].add([vi], w / s, 'REPLACE')
print('unmapped groups:', sorted(unmapped))
print('deform bones used:', len(out_groups))

# Re-home the mesh onto Manny's armature, keeping its world-space bind shape.
mw = body.matrix_world.copy()
body.parent = None
body.data.transform(mw)
body.matrix_world.identity()
body.data.transform(M.matrix_world.inverted())
body.parent = M
body.matrix_parent_inverse.identity()
body.matrix_world = M.matrix_world
for mod in list(body.modifiers): body.modifiers.remove(mod)
mod = body.modifiers.new('Armature', 'ARMATURE'); mod.object = M
body.name = body.data.name = 'SK_Quantum_Arms'
bpy.data.objects.remove(Q)
bpy.context.view_layer.update()
print('verts', len(body.data.vertices), 'faces', len(body.data.polygons), 'dims', tuple(round(d, 3) for d in body.dimensions))

bpy.ops.export_scene.fbx(filepath=out_path, use_selection=False, object_types={'ARMATURE', 'MESH'},
                         add_leaf_bones=False, bake_anim=False, use_armature_deform_only=False,
                         primary_bone_axis='Y', secondary_bone_axis='X', mesh_smooth_type='FACE',
                         apply_unit_scale=True, global_scale=1.0, use_tspace=False)
print('wrote', out_path)
