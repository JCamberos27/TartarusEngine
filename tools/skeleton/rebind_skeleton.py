# Builds the wardrobe's Skeleton race (assets/Characters/RealisticSkeleton/Skeleton_*.fbx) from the Realistic Skeleton
# source pack (Drive: Raw\Characters\Realistic Skeleton\Models\Realistic_Skeleton.fbx, a UE4 mannequin rig): the
# skeleton rebound onto the Quantum UE5 armature, fitted inside the Quantum body as if it were its own, and cut into the
# body parts the wardrobe swaps (Torso, Arms, Hands, Legs, Feet, Pelvis, Head).
#
#   blender -b --factory-startup --python tools/skeleton/rebind_skeleton.py -- \
#       project/assets/Characters/Quantum/Models/European/Quantum_Torso.fbx <Realistic_Skeleton.fbx> <out dir>
#
# The Quantum_Head / _Feet / _Legs / _UnderPants beside Quantum_Torso give the outlines the bones are fitted into.
# The fit, in order:
#   - every bone moved onto its Quantum joint and sized to run joint to joint (twist bones ride their parent; spine joints
#     go by their share of the way up the spine, not by name); the pelvis sized to Quantum's hip joints
#   - the shoulder blades blended between clavicle (at the socket) and upper spine (toward the edge)
#   - the skull fitted inside the Quantum head with a little tissue, the neck vertebrae stretched up to its base
#   - the ribcage's front pulled inside the chest (the spine stays), the pelvis's wings narrowed inside the hips, the feet
#     fitted inside Quantum's bare feet
# Check a new build with tools/skeleton/skeleton_overlay.py, and against the Quantum soldier in `--npc-test pose`.
import bpy, sys
from mathutils import Matrix, Vector, Quaternion
from collections import Counter

qpath, spath, outdir = sys.argv[sys.argv.index('--') + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)

bpy.ops.import_scene.fbx(filepath=qpath)
qarm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
for o in [o for o in bpy.data.objects if o.type == 'MESH']:
    bpy.data.objects.remove(o)
before = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=spath)
sarm = next(o for o in bpy.data.objects if o not in before and o.type == 'ARMATURE')
meshes = [o for o in bpy.data.objects if o not in before and o.type == 'MESH']

QMAP = {'spine_02': 'spine_03', 'spine_03': 'spine_05'}
qb = qarm.data.bones
sb = sarm.data.bones
WS_inv = sarm.matrix_world.inverted()
toS = WS_inv @ qarm.matrix_world

# The spine joints go at the same share of the way up the Quantum spine (pelvis to neck) as on the skeleton's own:
# snapping the skeleton's spine_03 onto Quantum's spine_05 (at the base of the neck) put the ribcage a hand too high.
S_SPINE = ['pelvis', 'spine_01', 'spine_02', 'spine_03', 'neck_01']
Q_SPINE = ['pelvis', 'spine_01', 'spine_02', 'spine_03', 'spine_04', 'spine_05', 'neck_01']

def _arc(points):
    out = [0.0]
    for a, b in zip(points, points[1:]): out.append(out[-1] + (b - a).length)
    return out

_sp = [sb[n].head_local.copy() for n in S_SPINE]
_qp = [toS @ qb[n].head_local for n in Q_SPINE]
_sa, _qa = _arc(_sp), _arc(_qp)

def _along_q(f):
    d = f * _qa[-1]
    for i in range(len(_qp) - 1):
        if d <= _qa[i + 1] or i == len(_qp) - 2:
            t = (d - _qa[i]) / max(_qa[i + 1] - _qa[i], 1e-9)
            return _qp[i].lerp(_qp[i + 1], t)

SPINE_AT = {n: _along_q(_sa[i] / _sa[-1]) for i, n in enumerate(S_SPINE)}

def qhead(sname):
    # Twist bones keep their place on their parent: Quantum's sit elsewhere along the limb, and snapping to them slid
    # half of each long bone (the humerus was split between upperarm and its twist) down the limb.
    if 'twist' in sname: return None
    if sname in SPINE_AT: return SPINE_AT[sname].copy()
    q = QMAP.get(sname, sname)
    return (toS @ qb[q].head_local) if q in qb else None

def main_child(b):
    best, score = None, -1
    for c in b.children:
        if c.name.startswith('ik_') or 'twist' in c.name or qhead(c.name) is None:
            continue
        s = 2 if c.name.startswith('middle_01') else 1 if c.name.split('_')[0] in ('upperarm', 'lowerarm', 'hand', 'neck', 'head', 'spine', 'calf', 'foot', 'ball') else 0
        if s > score:
            best, score = c, s
    return best

def swing_to(a, b):
    a, b = a.normalized(), b.normalized()
    return a.rotation_difference(b)

# The shoulder pieces are split between clavicle and upper spine in the source. Each bone is its own mesh island; the two
# mostly on a clavicle (the collarbone and the shoulder blade) are reweighted below.
import bmesh
_body = next(o for o in meshes if 'Body' in o.name)
_bm = bmesh.new(); _bm.from_mesh(_body.data); _bm.verts.ensure_lookup_table()
_seen_v = set(); _islands = []
for v0 in _bm.verts:
    if v0.index in _seen_v: continue
    stack = [v0]; isl = []; _seen_v.add(v0.index)
    while stack:
        v = stack.pop(); isl.append(v.index)
        for e in v.link_edges:
            w = e.other_vert(v)
            if w.index not in _seen_v: _seen_v.add(w.index); stack.append(w)
    _islands.append(isl)
_bm.free()
_W = sarm.matrix_world
for side in ('_l', '_r'):
    sh = _W @ sb['upperarm' + side].head_local
    cl = _W @ sb['clavicle' + side].head_local
    cg = _body.vertex_groups['clavicle' + side]
    for isl in _islands:
        pts = [_body.matrix_world @ _body.data.vertices[i].co for i in isl]
        c = sum(pts, Vector()) / len(pts)
        tops = Counter(_body.vertex_groups[max(_body.data.vertices[i].groups, key=lambda g: g.weight).group].name
                       for i in isl if _body.data.vertices[i].groups)
        if not (tops and tops.most_common(1)[0][0] == 'clavicle' + side and len(tops) > 1): continue
        sg = _body.vertex_groups['spine_03']  # the source's upper spine (Quantum's spine_05 after the rename)
        for g in list(_body.vertex_groups):
            g.remove(isl)
        zext = max(p.z for p in pts) - min(p.z for p in pts); xext = max(p.x for p in pts) - min(p.x for p in pts)
        if zext > xext:  # the shoulder blade is tall; the collarbone runs across
            # The shoulder blade (behind the clavicle's root): it follows the clavicle at its socket, so it stays on the
            # arm, and the upper spine toward its spine edge and lower tip, so it stays on the back of the ribcage. All
            # on the clavicle, it swung off the back like a wing whenever the clavicle turned (aiming).
            far = max((p - sh).length for p in pts)
            for i, p in zip(isl, pts):
                t = min(1.0, max(0.0, ((p - sh).length - 0.04) / max(far - 0.04, 1e-6)))
                w = 1.0 - 0.75 * t  # 1 at the socket, 0.25 at the far edge
                cg.add([i], w, 'REPLACE')
                sg.add([i], 1.0 - w, 'REPLACE')
            print('SCAPULA', side, len(isl), 'verts: clavicle at the socket, spine toward the edge', dict(tops))
        else:
            # The collarbone itself, split between clavicle and spine: wholly on the clavicle, so it doesn't tear.
            cg.add(isl, 1.0, 'REPLACE')
            print('CLAVICLE', side, len(isl), 'verts onto', cg.name, dict(tops))

# The Quantum head's outline (world): the vertices that follow its head bone, so the neck is left out. The skull is
# fitted inside it after the rebind (below), not sized at its joint.
_seen = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=__import__('os').path.join(__import__('os').path.dirname(qpath), 'Quantum_Head.fbx'))
_qh = [o for o in bpy.data.objects if o not in _seen]
_pts = []
for o in _qh:
    if o.type != 'MESH' or 'head' not in o.vertex_groups: continue
    hi = o.vertex_groups['head'].index
    for v in o.data.vertices:
        if v.groups and max(v.groups, key=lambda g: g.weight).group == hi:
            _pts.append(o.matrix_world @ v.co)
Q_HEAD_LO = Vector((min(p.x for p in _pts), min(p.y for p in _pts), min(p.z for p in _pts)))
Q_HEAD_HI = Vector((max(p.x for p in _pts), max(p.y for p in _pts), max(p.z for p in _pts)))
for o in _qh: bpy.data.objects.remove(o)
HIP_SCALE = (qhead('thigh_l') - qhead('thigh_r')).length / max((sb['thigh_l'].head_local - sb['thigh_r'].head_local).length, 1e-6)

# Disconnect, and let every bone take its own size, so each can be moved and fitted on its own.
bpy.context.view_layer.objects.active = sarm
bpy.ops.object.mode_set(mode='EDIT')
for e in sarm.data.edit_bones:
    e.use_connect = False
    e.inherit_scale = 'NONE'
bpy.ops.object.mode_set(mode='POSE')
scale = {}

swing = {}
target = {}
order = [b for b in sb]  # bones are listed parents-first
for b in order:
    if b.name.startswith('ik_'):
        continue
    rest = b.matrix_local.copy()  # armature space
    p = b.parent
    s = swing.get(p.name, Quaternion()) if p else Quaternion()
    head = qhead(b.name)
    c = main_child(b)
    # Each bone sized to run joint to joint on Quantum (its main child's joint), the pelvis to Quantum's hips; the skull
    # is fitted after the rebind; a bone with nothing to measure (a fingertip, a twist) keeps its parent's size.
    k = scale.get(p.name, 1.0) if p else 1.0
    if b.name == 'pelvis': k = HIP_SCALE
    elif b.name == 'head': k = 1.0
    if head is not None and c is not None:
        d_rest = c.head_local - b.head_local
        d_tgt = qhead(c.name) - head
        if d_rest.length > 1e-6 and d_tgt.length > 1e-6 and b.name != 'pelvis':
            k = d_tgt.length / d_rest.length
        if d_rest.length > 1e-6 and d_tgt.length > 1e-6:
            frm = s @ d_rest
            s = swing_to(frm, d_tgt) @ s
            # a hand also rolls about its length until its palm (index to pinky knuckle) lies as Quantum's
            if b.name.startswith('hand_'):
                side = b.name[-2:]
                bi, bp = 'index_01' + side, 'pinky_01' + side
                if bi in sb and bp in sb and qhead(bi) and qhead(bp):
                    ax = d_tgt.normalized()
                    ba = s @ (sb[bp].head_local - sb[bi].head_local)
                    ra = qhead(bp) - qhead(bi)
                    ba -= ax * ba.dot(ax); ra -= ax * ra.dot(ax)
                    if ba.length > 1e-6 and ra.length > 1e-6:
                        s = ba.normalized().rotation_difference(ra.normalized()) @ s
    k = min(max(k, 0.6), 1.6)
    scale[b.name] = k
    swing[b.name] = s
    rot = s.to_matrix().to_4x4() @ Matrix(rest.to_3x3()).to_4x4() @ Matrix.Scale(k, 4)
    if head is None:
        # unmatched (none expected besides the skull's jaw): ride the parent rigidly
        pm = target[p.name]
        local = p.matrix_local.inverted() @ rest
        m = pm @ local
    else:
        m = Matrix.Translation(head) @ rot
    target[b.name] = m
    sarm.pose.bones[b.name].matrix = m
    bpy.context.view_layer.update()

bpy.ops.object.mode_set(mode='OBJECT')
# Bake the pose into the meshes, rename the groups to the Quantum bones, bind them to the Quantum armature.
for o in meshes:
    bpy.context.view_layer.objects.active = o
    for o2 in bpy.data.objects: o2.select_set(o2 == o)
    mod = next(m for m in o.modifiers if m.type == 'ARMATURE')
    bpy.ops.object.modifier_apply(modifier=mod.name)
    for old, new in (('spine_03', 'spine_05'), ('spine_02', 'spine_03'), ('jaw', 'head')):
        g = o.vertex_groups.get(old)
        if g is None: continue
        if new in o.vertex_groups:  # jaw onto an existing head group: merge
            gi, ni = g.index, o.vertex_groups[new].index
            for v in o.data.vertices:
                w = next((x.weight for x in v.groups if x.group == gi), 0.0)
                if w > 0: o.vertex_groups[new].add([v.index], w, 'ADD')
            o.vertex_groups.remove(g)
        else:
            g.name = new
    mw = o.matrix_world.copy()
    o.parent = qarm
    o.matrix_world = mw
    m = o.modifiers.new('Armature', 'ARMATURE'); m.object = qarm
bpy.data.objects.remove(sarm)

# The skull fitted inside the Quantum head with a little tissue all round: crown and chin set its height, face and back
# of the head its depth; its width keeps the skull's own proportions (the head's width counts the ears).
_skull = next(o for o in meshes if 'Skull' in o.name)
_sp = [_skull.matrix_world @ v.co for v in _skull.data.vertices]
s_lo = Vector((min(p.x for p in _sp), min(p.y for p in _sp), min(p.z for p in _sp)))
s_hi = Vector((max(p.x for p in _sp), max(p.y for p in _sp), max(p.z for p in _sp)))
SCALP, CHIN, FACE, BACK = 0.006, 0.008, 0.007, 0.008
t_lo = Vector((Q_HEAD_LO.x, Q_HEAD_LO.y + FACE, Q_HEAD_LO.z + CHIN))
t_hi = Vector((Q_HEAD_HI.x, Q_HEAD_HI.y - BACK, Q_HEAD_HI.z - SCALP))
sz = (t_hi.z - t_lo.z) / (s_hi.z - s_lo.z)
sy = (t_hi.y - t_lo.y) / (s_hi.y - s_lo.y)
sx = (sy + sz) / 2
s_c, t_c = (s_lo + s_hi) / 2, (t_lo + t_hi) / 2
_inv = _skull.matrix_world.inverted()
for v, p in zip(_skull.data.vertices, _sp):
    d = p - s_c
    v.co = _inv @ (t_c + Vector((d.x * sx, d.y * sy, d.z * sz)))
print('SKULLFIT scale x %.3f y %.3f z %.3f, moved %.1f cm' % (sx, sy, sz, (t_c - s_c).length * 100))

# The neck vertebrae run from where they leave the ribcage up to the skull's base (Quantum's head joint): the skull
# moved with its fit, and the top vertebra has to meet it.
_body = next(o for o in meshes if 'Body' in o.name)
_ni = _body.vertex_groups['neck_01'].index
_nv = [v for v in _body.data.vertices if v.groups and max(v.groups, key=lambda g: g.weight).group == _ni]
_np = [_body.matrix_world @ v.co for v in _nv]
n_bot = min(p.z for p in _np); n_top = max(p.z for p in _np)
n_top_c = sum((p for p in _np if p.z > n_top - 0.01), Vector()) / max(1, sum(1 for p in _np if p.z > n_top - 0.01))
# Where the top vertebra touched the skull before the fit, and where the fit took that spot.
_contact = min(_sp, key=lambda p: (p - n_top_c).length)
_d = _contact - s_c
q_joint = t_c + Vector((_d.x * sx, _d.y * sy, _d.z * sz))
_binv = _body.matrix_world.inverted()
for v, p in zip(_nv, _np):
    t = (p.z - n_bot) / max(n_top - n_bot, 1e-6)  # 0 at the ribcage, 1 at the top vertebra
    goal = Vector((q_joint.x - n_top_c.x, q_joint.y - n_top_c.y, q_joint.z - n_top)) * t
    v.co = _binv @ (p + goal)
print('NECK top %.1f cm -> head joint, moved its top %.1f cm' % (n_top * 100, (q_joint - n_top_c).length * 100))

# The ribcage inside the Quantum chest: at the ribs' heights, the chest's front and back (and sides) less muscle and
# skin. Only ever made smaller, and its height is kept.
_seen = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=qpath)
_qt = [o for o in bpy.data.objects if o not in _seen]
_rg = {_body.vertex_groups[n].index for n in ('spine_03', 'spine_05') if n in _body.vertex_groups}
_rv = [v for v in _body.data.vertices if v.groups and max(v.groups, key=lambda g: g.weight).group in _rg]
_rp = [_body.matrix_world @ v.co for v in _rv]
r_lo = Vector((min(p.x for p in _rp), min(p.y for p in _rp), min(p.z for p in _rp)))
r_hi = Vector((max(p.x for p in _rp), max(p.y for p in _rp), max(p.z for p in _rp)))
_cp = [o.matrix_world @ v.co for o in _qt if o.type == 'MESH' for v in o.data.vertices]
_cp = [p for p in _cp if r_lo.z <= p.z <= r_hi.z]
for o in _qt: bpy.data.objects.remove(o)
c_lo = Vector((min(p.x for p in _cp), min(p.y for p in _cp), 0)); c_hi = Vector((max(p.x for p in _cp), max(p.y for p in _cp), 0))
FRONT, SIDE = 0.015, 0.02
# The spine (the ribs' back, +y) stays put; the front of the ribs comes in to sit behind the chest (front is -y).
ry = min(1.0, (r_hi.y - (c_lo.y + FRONT)) / (r_hi.y - r_lo.y))
rx = min(1.0, ((c_hi.x - c_lo.x) - 2 * SIDE) / (r_hi.x - r_lo.x))
r_c = (r_lo + r_hi) / 2
for v, p in zip(_rv, _rp):
    v.co = _binv @ Vector((r_c.x + (p.x - r_c.x) * rx, r_hi.y + (p.y - r_hi.y) * ry, p.z))
print('RIBS scale x %.3f y %.3f' % (rx, ry))

# The pelvis's wings: sized to put its sockets on Quantum's hip joints, the whole pelvis came out 13% bigger and the
# crests sat a few millimetres under the skin. Above the sockets (which stay on the thighs) the wings narrow, up to the
# crests, until they sit 1.5 cm inside Quantum's hips.
_seen = set(bpy.data.objects)
for rel in ('Quantum_Torso.fbx', 'Quantum_Legs.fbx', 'Quantum_UnderPants.fbx'):
    bpy.ops.import_scene.fbx(filepath=__import__('os').path.join(__import__('os').path.dirname(qpath), rel))
_qo = [o for o in bpy.data.objects if o not in _seen]
_hb = [o.matrix_world @ v.co for o in _qo if o.type == 'MESH' for v in o.data.vertices]
for o in _qo: bpy.data.objects.remove(o)
_pg = _body.vertex_groups['pelvis'].index
_pv = [v for v in _body.data.vertices if v.groups and max(v.groups, key=lambda g: g.weight).group == _pg]
_pp = [_body.matrix_world @ v.co for v in _pv]
hip_z = (qarm.matrix_world @ qb['thigh_l'].head_local).z
crest_z = max(p.z for p in _pp)
cx = sum(p.x for p in _pp) / len(_pp)
_band = [p for p in _pp if p.z > crest_z - 0.04]
_bband = [p for p in _hb if crest_z - 0.04 < p.z <= crest_z]
half_bone = max(abs(p.x - cx) for p in _band)
half_body = min(max(p.x for p in _bband) - cx, cx - min(p.x for p in _bband))
kx_top = min(1.0, (half_body - 0.015) / half_bone)
for v, p in zip(_pv, _pp):
    t = min(1.0, max(0.0, (p.z - hip_z) / max(crest_z - hip_z, 1e-6)))
    kx = 1.0 - (1.0 - kx_top) * t
    v.co = _binv @ Vector((cx + (p.x - cx) * kx, p.y, p.z))
print('PELVIS wings narrowed to %.3f at the crest (crest %.1f cm out, hips %.1f cm)' % (kx_top, half_bone * 100, half_body * 100))

# The feet inside Quantum's bare feet: sized by the ankle-to-ball spacing they came out 14% too big (toes 4 cm past
# Quantum's). Each is fitted heel to toe and sole to ankle with a little tissue (front is -y); the ankle stays on the shin.
_seen = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=__import__('os').path.join(__import__('os').path.dirname(qpath), 'Quantum_Feet.fbx'))
_qf = [o for o in bpy.data.objects if o not in _seen]
_qfp = [o.matrix_world @ v.co for o in _qf if o.type == 'MESH' for v in o.data.vertices]
for o in _qf: bpy.data.objects.remove(o)
for side in ('_l', '_r'):
    ankle = qarm.matrix_world @ qb['foot' + side].head_local
    sgn = 1 if ankle.x > 0 else -1
    q_toe = min(p.y for p in _qfp if sgn * p.x > 0)
    fg = {_body.vertex_groups[n].index for n in ('foot' + side, 'ball' + side) if n in _body.vertex_groups}
    fv = [v for v in _body.data.vertices if v.groups and max(v.groups, key=lambda g: g.weight).group in fg]
    fp = [_body.matrix_world @ v.co for v in fv]
    qs = [p for p in _qfp if sgn * p.x > 0]
    q_heel = max(p.y for p in qs); q_sole = min(p.z for p in qs)
    TOE, HEEL, SOLE = 0.008, 0.010, 0.008
    s_toe = min(p.y for p in fp); s_heel = max(p.y for p in fp); s_sole = min(p.z for p in fp)
    # Length from toe to heel, height from the sole up to the ankle joint; width follows the two.
    ky = ((q_heel - HEEL) - (q_toe + TOE)) / max(s_heel - s_toe, 1e-6)
    kz = (ankle.z - (q_sole + SOLE)) / max(ankle.z - s_sole, 1e-6)
    kx = (ky + kz) / 2
    # The ankle joint stays put in height and across; along the foot, the heel and toe land inside Quantum's.
    ty = (q_toe + TOE) - (s_toe - ankle.y) * ky - ankle.y
    nfp = [Vector((ankle.x + (p.x - ankle.x) * kx, ankle.y + ty + (p.y - ankle.y) * ky, ankle.z + (p.z - ankle.z) * kz)) for p in fp]
    for v, n in zip(fv, nfp):
        v.co = _binv @ n
    top = min(fp, key=lambda p: (p - ankle).length)
    top_n = nfp[fp.index(top)]
    print('FEET %s scale x %.3f y %.3f z %.3f, slid %.1f cm; ankle bone %.1f cm from the joint (was %.1f)' % (
        side, kx, ky, kz, ty * 100, (top_n - ankle).length * 100, (top - ankle).length * 100))

body = next(o for o in meshes if 'Body' in o.name)
skull = next(o for o in meshes if 'Skull' in o.name)
names = {g.index: g.name for g in body.vertex_groups}

def part_of(n):
    if n.startswith(('hand_', 'index_', 'middle_', 'ring_', 'pinky_', 'thumb_')): return 'Hands'
    if n.startswith(('upperarm', 'lowerarm')): return 'Arms'
    if n.startswith(('thigh', 'calf')): return 'Legs'
    if n.startswith(('foot', 'ball')): return 'Feet'
    if n.startswith(('neck', 'head')): return 'Neck'
    if n.startswith('pelvis'): return 'Pelvis'
    return 'Torso'

def cut(src, keep, name):
    """A copy of src holding only the vertices whose heaviest group's part is in `keep`."""
    o = src.copy(); o.data = src.data.copy(); o.name = name
    bpy.context.collection.objects.link(o)
    for o2 in bpy.data.objects: o2.select_set(o2 == o)
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='DESELECT'); bpy.ops.object.mode_set(mode='OBJECT')
    n = 0
    for v in o.data.vertices:
        top = max(v.groups, key=lambda x: x.weight) if v.groups else None
        v.select = top is None or part_of(names[top.group]) not in keep
        n += 0 if v.select else 1
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.delete(type='VERT'); bpy.ops.object.mode_set(mode='OBJECT')
    print('PART', name, n)
    return o

parts = {
    'Skeleton_Torso': cut(body, {'Torso'}, 'Skeleton_Torso'),
    'Skeleton_Arms': cut(body, {'Arms', 'Hands'}, 'Skeleton_Arms'),
    'Skeleton_Hands': cut(body, {'Hands'}, 'Skeleton_Hands'),
    'Skeleton_Legs': cut(body, {'Legs'}, 'Skeleton_Legs'),
    'Skeleton_Feet': cut(body, {'Feet'}, 'Skeleton_Feet'),
    'Skeleton_Pelvis': cut(body, {'Pelvis'}, 'Skeleton_Pelvis'),
}
# The head: the skull with the neck vertebrae, so it never floats above an open collar.
neck = cut(body, {'Neck'}, 'Skeleton_Neck')
for o2 in bpy.data.objects: o2.select_set(o2 in (skull, neck))
bpy.context.view_layer.objects.active = skull
bpy.ops.object.join()
skull.name = 'Skeleton_Head'
parts['Skeleton_Head'] = skull
bpy.data.objects.remove(body)

for fname, o in parts.items():
    for o2 in bpy.data.objects: o2.select_set(o2 in (qarm, o))
    bpy.ops.export_scene.fbx(filepath=f'{outdir}/{fname}.fbx', use_selection=True, add_leaf_bones=False, bake_anim=False,
                             mesh_smooth_type='FACE', apply_unit_scale=True, use_armature_deform_only=False)
print('DONE')
