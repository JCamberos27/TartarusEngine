# Exports the whole .38 Special revolver set from its source .blend, without ever saving it.
#
#   blender --background "<38 Special 60fps.blend>" --python tools/weapons/export_revolver38.py -- \
#           <out dir: project/assets/Weapons/Revolver38> [only <Name>[,<Name>...]] [save3p <copy.blend>]
#
# save3p writes a separate working copy (save_as_mainfile copy=True; the source is never written) that
# carries the fixed actions, for tools/weapons/author_3p.py: its file name must contain "(3P)".
#
# The same rules as tools/weapons/export_remington.py (pinned FBX settings, both rigs playing their
# paired actions, weapon clips exported over the arms clip's range), for this file's rig:
#
#  - Arms rig `Armature` (the shared Manny/Quantum rig). Weapon rig `Armature.001`: Root > Hammer,
#    Trigger, Release, Crane > Cylinder > Ejector, plus Bullet1..6 (unparented, CHILD_OF Cylinder).
#    Weapon mesh `Revolver` (materials Revolver, Bullet). Root is CHILD_OF -> Armature:CB_Gun.
#    Both hand IK targets are CHILD_OF CB_Gun, so the arms never read the weapon rig.
#
# The source file's defects are fixed here, in memory (approved audit, 2026-11-09):
#  - A_FP_Reload never keys CB_ik_hand_r; every other take holds it at one constant. Keyed to it.
#  - A_W_Fire only flicked the hammer. Re-keyed as a double-action pull: trigger back, hammer
#    cocked and the cylinder indexing one chamber (60 deg, counter-clockwise from the rear) over the
#    first 2 frames, the hammer falling on frame 3, the trigger resetting by frame 18. The arms'
#    A_FP_Fire never moved the trigger finger; it now curls with the trigger.
#  - No ADS pose. Solved like the Remington's: A_FP_Idle frame 0 with CB_Gun moved so the rear
#    notch and front post lie on the AKS-74U's measured ADS sight line.
#  - Bullet1..6 are re-parented under Cylinder and their CHILD_OF motion baked, so the FBX
#    hierarchy carries no constraints. In the reload the six rounds that fall out of the cylinder are
#    the same bones the left hand later loads: each is scaled away (0.001) from the moment it clears
#    its chamber until the fingers pick it up, and the engine throws a physics case per chamber
#    instead (the eject timeline is written to the manifest).
#  - The weapon's Root CHILD_OF is muted for the weapon clips, so Root holds its bind pose; its
#    0.64 cm bind translation is then zeroed in the written FBXs (as the 870's Main was).
#  - Casing_38Special.fbx (spent case) and Round_38Special.fbx (live round) are cut from Bullet1.
#
# The source is never written: the only save is save3p's copy=True write to a separate path.
import bpy
import bmesh
import hashlib
import json
import math
import os
import sys

import mathutils as M

argv = sys.argv[sys.argv.index("--") + 1:]
OUT = os.path.abspath(argv[0])
ONLY = set(argv[argv.index("only") + 1].split(",")) if "only" in argv else None
SAVE3P = os.path.abspath(argv[argv.index("save3p") + 1]) if "save3p" in argv else None
if SAVE3P and ("(3P)" not in os.path.basename(SAVE3P) or os.path.normcase(SAVE3P) == os.path.normcase(bpy.data.filepath)):
    raise SystemExit("save3p needs a separate path whose name contains (3P)")
PREFIX = "Revolver38"
HERE = os.path.dirname(os.path.abspath(__file__))

# name -> (arms action, weapon action)
CLIPS = {
    "Idle": ("A_FP_Idle", "A_W_Idle"),
    "Walk": ("A_FP_Walk", "A_W_Idle"),
    "Sprint": ("A_FP_Sprint", "A_W_Idle"),
    "IdleToSprint": ("A_FP_Sprint_start", "A_W_Idle"),
    "SprintToIdle": ("A_FP_Sprint_end", "A_W_Idle"),
    "Fire": ("A_FP_Fire", "A_W_Fire"),
    "Regrip": ("A_FP_Regrip", "A_W_Idle"),
    "Inspect": ("A_FP_Inspect", "A_W_Inspect"),
    "Mag_Check": ("A_FP_Mag_Check", "A_W_Mag_Check"),
    "Melee": ("A_FP_Melee", "A_W_Idle"),
    "Reload": ("A_FP_Reload", "A_W_Reload"),
    "Draw": ("A_FP_Draw", "A_W_Idle"),
    "Holster": ("A_FP_Holster", "A_W_Idle"),
}
WEAPON_CLIPS = {"Fire", "Inspect", "Mag_Check", "Reload"}
BULLETS = ["Bullet%d" % i for i in range(1, 7)]

# The AKS-74U's ADS sight line in the shared arms rig's world space (Blender cm), as export_remington.py.
AK_EYE = M.Vector((1.1425, -9.5350, 158.9291))
AK_LOOK = M.Vector((-0.0073, -1.0, -0.0036)).normalized()
AK_UP = M.Vector((0.027, 0.0, 1.0))
# Eye to rear notch (cm). A handgun rides further out than a shouldered gun's 28 cm grip, but a
# first-person view keeps it close enough to read; tune here (and SIGHT_CORRECTION from Play).
EYE_TO_NOTCH = 34.0
# Measured in Play (Editor.log sight line, root space m): the eye sat at x -0.0137, y 0.0992 - 1.4 cm beside and
# 1.6 cm over the notch-post line - the scene view-model offset, as the 870 found. The gun moves by that much.
SIGHT_CORRECTION = M.Vector((-1.37, 1.56, 0.0))  # Root space, cm

# Double-action fire timing (frames at 60 fps) and the rig's own limits (its LIMIT_ROTATION values).
TRIGGER_PULL = 0.349   # rad about Trigger local X (+ = back)
HAMMER_COCK = -0.611   # rad about Hammer local X (- = back)
CYLINDER_STEP = math.radians(60.0)
# A 6-frame pull (0.1 s): the cylinder turns 10 deg a frame. At 2 frames it turned 30 a frame, the speed at which
# a six-chamber cylinder aliases (it reads as still or backwards). The round leaves when the hammer falls (the
# controller's Shot event, f7).
F_PULLED, F_FALL, F_HOLD, F_RESET = 6, 7, 10, 24

scn = bpy.context.scene
arm = bpy.data.objects["Armature"]
wep = bpy.data.objects["Armature.001"]
mesh = bpy.data.objects["Revolver"]
root_childof = wep.pose.bones["Root"].constraints[0]
assert root_childof.type == "CHILD_OF"

for o in (arm, wep):
    for t in o.animation_data.nla_tracks:
        t.mute = True
arm.data.pose_position = wep.data.pose_position = "POSE"
report = {"fixes": [], "warnings": []}


def bag(act):
    return act.layers[0].strips[0].channelbags[0]


def remove_curves(act, bones):
    b = bag(act)
    for fc in list(b.fcurves):
        if any('"%s"' % n in fc.data_path for n in bones):
            b.fcurves.remove(fc)


def copy_action(name, new):
    a = bpy.data.actions[name].copy()
    a.name = new
    return a


def assign(ob, act):
    ob.animation_data.action = act
    if act is not None and ob.animation_data.action_slot is None and len(act.slots):
        ob.animation_data.action_slot = act.slots[0]


def pair(arms_action, weapon_action):
    assign(arm, bpy.data.actions[arms_action] if isinstance(arms_action, str) else arms_action)
    assign(wep, bpy.data.actions[weapon_action] if isinstance(weapon_action, str) else weapon_action)


def world(ob, bone):
    return ob.matrix_world @ ob.pose.bones[bone].matrix


def key_pose(pb, frame):
    pb.keyframe_insert("location", frame=frame)
    pb.keyframe_insert("rotation_quaternion" if pb.rotation_mode == "QUATERNION" else "rotation_euler", frame=frame)
    pb.keyframe_insert("scale", frame=frame)


def set_rot(pb, q):
    if pb.rotation_mode == "QUATERNION":
        pb.rotation_quaternion = q
    else:
        pb.rotation_euler = q.to_euler(pb.rotation_mode)


def get_rot(pb):
    return pb.rotation_quaternion.copy() if pb.rotation_mode == "QUATERNION" else pb.rotation_euler.to_quaternion()


def frange(name):
    a = bpy.data.actions[name]
    return int(round(a.frame_range[0])), int(round(a.frame_range[1]))


# ---------------------------------------------------------------- arms reload: right-hand IK
def fix_reload():
    pb = arm.pose.bones["CB_ik_hand_r"]
    pair("A_FP_Idle", "A_W_Idle")
    scn.frame_set(0)
    loc, rot, scl = pb.location.copy(), get_rot(pb), pb.scale.copy()
    act = copy_action("A_FP_Reload", "A_FP_Reload_fixed")
    assign(arm, act)
    fs, fe = frange("A_FP_Reload")
    for f in (fs, fe):
        scn.frame_set(f)
        pb.location, pb.scale = loc, scl
        set_rot(pb, rot)
        key_pose(pb, f)
    report["fixes"].append("A_FP_Reload: CB_ik_hand_r keyed at the constant every other take holds "
                           "(loc %s)." % [round(v, 3) for v in loc])
    return act


# ---------------------------------------------------------------- weapon fire: double action
def fire_profile(f):
    """(trigger 0..1, hammer 0..1, cylinder step 1..0) at frame f."""
    def smooth(t):
        t = min(max(t, 0.0), 1.0)
        return t * t * (3.0 - 2.0 * t)
    trig = smooth(f / F_PULLED) if f <= F_HOLD else 1.0 - smooth((f - F_HOLD) / (F_RESET - F_HOLD))
    ham = smooth(f / F_PULLED) if f <= F_PULLED else (0.0 if f >= F_FALL else 1.0)
    cyl = 1.0 - smooth(f / F_PULLED)
    return trig, ham, cyl


def fix_weapon_fire(fe):
    act = copy_action("A_W_Fire", "A_W_Fire_fixed")
    remove_curves(act, ["Hammer", "Trigger", "Cylinder"])
    assign(wep, act)
    X, Y = M.Vector((1, 0, 0)), M.Vector((0, 1, 0))
    for f in range(0, fe + 1):
        scn.frame_set(f)
        t, h, c = fire_profile(f)
        for name, q in (("Trigger", M.Quaternion(X, TRIGGER_PULL * t)),
                        ("Hammer", M.Quaternion(X, HAMMER_COCK * h)),
                        # Starts one chamber back (+60) and turns to 0: the clip ends where Idle is,
                        # so a crossfade never spins the cylinder backwards.
                        ("Cylinder", M.Quaternion(Y, CYLINDER_STEP * c))):
            pb = wep.pose.bones[name]
            pb.location, pb.scale = M.Vector(), M.Vector((1, 1, 1))
            set_rot(pb, q)
            key_pose(pb, f)
    report["fixes"].append("A_W_Fire: double-action pull re-keyed (trigger %.0f deg, hammer %.0f deg, cylinder "
                           "60 deg CCW from the rear; pulled f%d, hammer falls f%d, trigger reset f%d), over the "
                           "arms' 0..%d." % (math.degrees(TRIGGER_PULL), math.degrees(-HAMMER_COCK),
                                             F_PULLED, F_FALL, F_RESET, fe))
    return act


# ---------------------------------------------------------------- arms fire: trigger finger
FINGER = [("CB_index_01_r", 0.30), ("CB_index_02_r", 0.40), ("CB_index_03_r", 0.30)]


def fix_arms_fire(w_fire):
    act = copy_action("A_FP_Fire", "A_FP_Fire_fixed")
    pair(act, w_fire)
    fs, fe = frange("A_FP_Fire")
    scn.frame_set(0)
    tip = lambda: world(arm, "index_03_r").translation + (world(arm, "index_03_r").to_3x3() @ M.Vector((0, arm.pose.bones["index_03_r"].length, 0)))
    trig_at = lambda f: (scn.frame_set(f), world(wep, "Trigger") @ M.Vector((0, wep.pose.bones["Trigger"].length * 0.6, 0)))[1]
    want = trig_at(F_PULLED) - trig_at(0)
    scn.frame_set(0)
    base = {n: get_rot(arm.pose.bones[n]) for n, _ in FINGER}
    tip0 = tip()

    def trial(axis, angle):
        for n, w in FINGER:
            set_rot(arm.pose.bones[n], base[n] @ M.Quaternion(axis, angle * w))
        bpy.context.view_layer.update()
        d = tip() - tip0
        for n, _ in FINGER:
            set_rot(arm.pose.bones[n], base[n])
        bpy.context.view_layer.update()
        return d

    best = None
    for axis in (M.Vector((1, 0, 0)), M.Vector((-1, 0, 0)), M.Vector((0, 0, 1)), M.Vector((0, 0, -1))):
        d = trial(axis, math.radians(10))
        score = d.normalized().dot(want.normalized()) if d.length > 1e-6 else -1
        if best is None or score > best[0]:
            best = (score, axis)
    axis = best[1]
    angle = 0.0
    for a in range(1, 61):
        if trial(axis, math.radians(a)).length >= want.length:
            angle = math.radians(a)
            break
    angle = angle or math.radians(25)
    # Per-frame base rotations (the take keys the fingers itself), then the curl on top.
    samples = {}
    for f in range(fs, F_RESET + 2):
        scn.frame_set(f)
        samples[f] = {n: get_rot(arm.pose.bones[n]) for n, _ in FINGER}
    for f in range(fs, F_RESET + 2):
        scn.frame_set(f)
        t = fire_profile(f)[0]
        for n, w in FINGER:
            pb = arm.pose.bones[n]
            set_rot(pb, samples[f][n] @ M.Quaternion(axis, angle * w * t))
            pb.keyframe_insert("rotation_quaternion" if pb.rotation_mode == "QUATERNION" else "rotation_euler", frame=f)
    report["fixes"].append("A_FP_Fire: index finger curls with the trigger (%.0f deg over three joints about local %s; "
                           "trigger face travels %.2f cm, alignment %.2f)." % (math.degrees(angle), tuple(axis),
                                                                                 want.length, best[0]))
    return act


# ---------------------------------------------------------------- ADS pose
def mesh_points_root():
    """Rear notch and front post tops, in Root's rest space (cm)."""
    root_rest = wep.data.bones["Root"].matrix_local
    inv = root_rest.inverted()
    vs = [mesh.matrix_world @ v.co for v in mesh.data.vertices]
    # Armature space: muzzle is -Y, up is +Z, the centre line is x = 0 (bones are all at x ~ 0).
    ymin = min(v.y for v in vs)
    front = max((v for v in vs if v.y < ymin + 1.2 and abs(v.x) < 0.12), key=lambda v: v.z)
    hammer_y = wep.data.bones["Hammer"].head_local.y
    cyl_y = wep.data.bones["Cylinder"].head_local.y
    strap = [v for v in vs if cyl_y - 0.5 < v.y < hammer_y - 0.15 and abs(v.x) < 0.05]
    notch = max(strap, key=lambda v: v.z) if strap else None
    if notch is None:
        raise SystemExit("no rear-sight vertices found on the centre line")
    return inv @ notch, inv @ front, notch, front


def solve_aim():
    pair("A_FP_Idle", "A_W_Idle")
    scn.frame_set(0)
    notch, post, notch_a, post_a = mesh_points_root()
    k = world(arm, "CB_Gun").inverted() @ world(wep, "Root")
    sight = (post - notch).normalized()
    # Root rotation R with R @ sight = look, and Root's up (its +Y, the rest pose's +Z) towards AK_UP.
    up_local = M.Vector((0, 1, 0))
    z_l = sight
    y_l = (up_local - up_local.dot(z_l) * z_l).normalized()
    x_l = y_l.cross(z_l)
    z_w = AK_LOOK
    y_w = (AK_UP - AK_UP.dot(z_w) * z_w).normalized()
    x_w = y_w.cross(z_w)
    R = M.Matrix((x_w, y_w, z_w)).transposed() @ M.Matrix((x_l, y_l, z_l))
    root = R.to_4x4()
    root.translation = AK_EYE + AK_LOOK * EYE_TO_NOTCH - R @ (notch - SIGHT_CORRECTION)
    act = bpy.data.actions["A_FP_Idle"].copy()
    act.name = "A_FP_Aim_solved"
    assign(arm, act)
    pb = arm.pose.bones["CB_Gun"]
    grip_before = world(arm, "CB_Gun").inverted() @ world(arm, "hand_r")
    for f in (0, 1):
        scn.frame_set(f)
        pb.matrix = arm.matrix_world.inverted() @ (root @ k.inverted())
        bpy.context.view_layer.update()
        key_pose(pb, f)
    scn.frame_set(0)
    got = world(wep, "Root")
    grip_after = world(arm, "CB_Gun").inverted() @ world(arm, "hand_r")
    reach = {s: (world(arm, "hand_" + s).translation - world(arm, "ik_hand_" + s).translation).length for s in ("l", "r")}
    rep = {"rootError": (got.translation - root.translation).length,
           "rightGripDrift": (grip_after.translation - grip_before.translation).length,
           "handReachError": reach,
           "notchRoot": list(notch), "postRoot": list(post),
           "eyeToNotch": EYE_TO_NOTCH,
           "notchWorld": list(got @ notch), "postWorld": list(got @ post)}
    print("AIM solve:", json.dumps(rep))
    if max(reach.values()) > 1.0:
        report["warnings"].append("Aim: a hand misses its IK target by %.1f cm (arm out of reach at EYE_TO_NOTCH=%.0f)."
                                  % (max(reach.values()), EYE_TO_NOTCH))
    report["fixes"].append("A_FP_Aim: solved (rear notch %.0f cm ahead of the AK eye, on its sight line)." % EYE_TO_NOTCH)
    return act, rep


# ---------------------------------------------------------------- bullets: re-parent, bake, hide
def sample_bullets(arms_act, wep_act, fs, fe, hide):
    pair(arms_act, wep_act)
    scn.frame_set(fs)
    cyl0 = wep.pose.bones["Cylinder"].matrix.inverted()
    home = {b: (cyl0 @ wep.pose.bones[b].matrix).translation.copy() for b in BULLETS}
    out = {b: [] for b in BULLETS}
    for f in range(fs, fe + 1):
        scn.frame_set(f)
        cyl = wep.pose.bones["Cylinder"].matrix.inverted()
        tips = [world(arm, n).translation + world(arm, n).to_3x3() @ M.Vector((0, arm.pose.bones[n].length, 0))
                for n in ("thumb_03_l", "index_03_l", "middle_03_l")]
        root = wep.pose.bones["Root"].matrix.inverted()
        for b in BULLETS:
            m = wep.pose.bones[b].matrix.copy()
            off = ((cyl @ m).translation - home[b]).length
            p = (wep.matrix_world @ m).translation
            near = min((p - t).length for t in tips)
            out[b].append([m, off, near, hide and off > 3.0 and near > 4.0, (root @ m).translation])
    # Ignore blips: hidden or shown runs shorter than 3 frames take their neighbours' state.
    for b in BULLETS:
        s = out[b]
        for i in range(1, len(s) - 1):
            run = [j for j in range(max(0, i - 1), min(len(s), i + 2))]
            if s[i - 1][3] == s[i + 1][3] != s[i][3]:
                s[i][3] = s[i - 1][3]
    return out


def reparent_bullets():
    bpy.context.view_layer.objects.active = wep
    for o in bpy.context.view_layer.objects:
        o.select_set(o == wep)
    bpy.ops.object.mode_set(mode="EDIT")
    eb = wep.data.edit_bones
    for b in BULLETS:
        eb[b].use_connect = False
        eb[b].parent = eb["Cylinder"]
    bpy.ops.object.mode_set(mode="OBJECT")
    for b in BULLETS:
        pb = wep.pose.bones[b]
        for c in list(pb.constraints):
            pb.constraints.remove(c)


def write_bullets(wep_act_name, arms_act, samples, fs):
    act = copy_action(wep_act_name, wep_act_name + "_baked")
    remove_curves(act, BULLETS)
    pair(arms_act, act)
    n = len(next(iter(samples.values())))
    for i in range(n):
        f = fs + i
        scn.frame_set(f)
        for b in BULLETS:
            pb = wep.pose.bones[b]
            m, _, _, hidden, _ = samples[b][i]
            pb.matrix = m
            bpy.context.view_layer.update()
            if hidden:
                pb.scale = M.Vector((0.001, 0.001, 0.001))
            key_pose(pb, f)
    return act


def eject_timeline(samples, fs):
    """Per chamber: the frame it leaves (first hidden frame), its Root-space position and velocity."""
    rows = []
    for b in BULLETS:
        s = samples[b]
        i = next((i for i, r in enumerate(s) if r[3]), None)
        if i is None:
            continue
        j = max(i - 2, 0)
        # Engine root space: Root's axes (+X right, +Y up, +Z to the muzzle), metres, origin at the
        # armature origin - Root's bind translation is zeroed in the written FBXs.
        rest = wep.data.bones["Root"].matrix_local
        pr = lambda r: rest.to_3x3().inverted() @ (rest @ r[4]) * 0.01
        vel = (pr(s[i]) - pr(s[j])) * (scn.render.fps / (i - j))  # m/s
        load = next((k for k in range(i, len(s)) if not s[k][3] and s[k][1] < 0.3), None)
        rows.append({"bullet": b, "ejectFrame": fs + i, "loadedFrame": (fs + load) if load is not None else None,
                     "origin": [round(v, 4) for v in pr(s[j])],
                     "velocity": [round(v, 3) for v in vel]})
    return rows


# ---------------------------------------------------------------- casing and round meshes
def cut_round(spent):
    gi = mesh.vertex_groups["Bullet1"].index
    inv = wep.data.bones["Bullet1"].matrix_local.inverted()
    me = mesh.data
    bm = bmesh.new()
    vmap = {}
    keep = {v.index for v in me.vertices if any(g.group == gi and g.weight > 0.5 for g in v.groups)}
    for p in me.polygons:
        if p.material_index != list(mesh.data.materials).index(bpy.data.materials["Bullet"]) or not all(v in keep for v in p.vertices):
            continue
        vs = []
        for v in p.vertices:
            if v not in vmap:
                vmap[v] = bm.verts.new(inv @ me.vertices[v].co)
            vs.append(vmap[v])
        try:
            bm.faces.new(vs)
        except ValueError:
            pass
    bm.verts.ensure_lookup_table()
    ys = [v.co.y for v in bm.verts]
    y0, y1 = min(ys), max(ys)
    if spent:
        # The case: everything behind the mouth (case length / overall length of a .38 Special ~ 0.75).
        cut = y0 + 0.745 * (y1 - y0)
        bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], plane_co=(0, cut, 0),
                               plane_no=(0, 1, 0), clear_outer=True)
        y1 = cut
    # Engine casing convention (FirstPersonPresentation::Eject): long axis +Y in the FBX, base at +Y.
    # Blender +Z exports as FBX +Y, so the base (y0) goes to +Z, centred on the middle.
    mid = 0.5 * (y0 + y1)
    for v in bm.verts:
        x, y, z = v.co
        v.co = M.Vector((x, z, -(y - mid)))
    name = "Casing_38Special" if spent else "Round_38Special"
    md = bpy.data.meshes.new(name)
    bm.to_mesh(md)
    bm.free()
    md.materials.append(bpy.data.materials["Bullet"])
    ob = bpy.data.objects.new(name, md)
    scn.collection.objects.link(ob)
    # Match the shipped cases' budget (100 tris); the live round keeps a little more for the hand.
    target = 120 if spent else 220
    tris = sum(len(p.vertices) - 2 for p in md.polygons)
    if tris > target:
        dec = ob.modifiers.new("dec", "DECIMATE")
        dec.ratio = target / tris
    return ob, tris, y1 - y0


# ---------------------------------------------------------------- export
def export(path, objects, fs, fe, with_mesh, modifiers=False):
    for ob in bpy.data.objects:
        ob.select_set(False)
    for ob in objects:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    scn.frame_start, scn.frame_end = fs, fe
    scn.frame_set(fs)
    ok = bpy.ops.export_scene.fbx(
        filepath=path, use_selection=True,
        object_types={"ARMATURE", "MESH"} if with_mesh else {"ARMATURE"},
        use_mesh_modifiers=modifiers, add_leaf_bones=False,
        bake_anim=fe > fs or not with_mesh or objects[0].type == "ARMATURE",
        bake_anim_use_all_bones=True, bake_anim_use_nla_strips=False, bake_anim_use_all_actions=False,
        bake_anim_force_startend_keying=True, bake_anim_step=1.0, bake_anim_simplify_factor=0.0,
        path_mode="AUTO", axis_forward="-Z", axis_up="Y", global_scale=1.0, apply_unit_scale=True)
    if ok != {"FINISHED"}:
        raise SystemExit("export failed: %s %r" % (path, ok))
    return os.path.getsize(path)


def digest(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def zero_root(paths):
    """Zero Root's bind/animation translation in written weapon FBXs (fbx_export_edit, as the 870)."""
    import addon_utils
    fbx_dir = next(os.path.dirname(m.__file__) for m in addon_utils.modules() if m.__name__ == "io_scene_fbx")
    os.environ.setdefault("TARTARUS_BLENDER_FBX_ADDON", fbx_dir)
    sys.path.insert(0, HERE)
    import fbx_export_edit as fe
    for p in paths:
        root, version = fe.load(p)
        fe.zero_translation(root, b"Root")
        fe.write(p, root, version)


def want(name):
    return ONLY is None or name in ONLY


os.makedirs(os.path.join(OUT, "FirstPerson"), exist_ok=True)
os.makedirs(os.path.join(OUT, "Weapon"), exist_ok=True)
manifest = {"source": bpy.data.filepath, "fps": scn.render.fps, "armsSkeleton": "Armature",
            "weaponSkeleton": "Armature.001", "weaponMesh": "Revolver", "exports": []}

# The hip shot's kick, kept to this share of the source take's (user, 2026-11-09: the barrel went way too high - the
# muzzle flipped ~28 deg). CB_Gun carries the gun and both hands: its motion off frame 0, scaled.
HIP_FIRE_KICK = 0.6


def damp_fire_gun(act, w_act, k):
    pair(act, w_act)
    pb = arm.pose.bones["CB_Gun"]
    fs, fe = int(round(act.frame_range[0])), int(round(act.frame_range[1]))
    frames = {}
    for f in range(fs, fe + 1):
        scn.frame_set(f)
        frames[f] = (pb.location.copy(), get_rot(pb))
    l0, q0 = frames[fs]
    for f in range(fs, fe + 1):
        l, q = frames[f]
        if q0.dot(q) < 0.0:
            q = -q
        pb.location = l0.lerp(l, k)
        set_rot(pb, q0.slerp(q, k))
        pb.keyframe_insert("location", frame=f)
        pb.keyframe_insert("rotation_quaternion" if pb.rotation_mode == "QUATERNION" else "rotation_euler", frame=f)
    report["fixes"].append("A_FP_Fire: the gun's kick (CB_Gun off frame 0) kept to x%.2f of the take's." % k)
    return act


# Fixed / derived actions (all in memory).
arms_override = {"Reload": fix_reload()}
fire_fe = frange("A_FP_Fire")[1]
w_fire = fix_weapon_fire(fire_fe)
arms_override["Fire"] = damp_fire_gun(fix_arms_fire(w_fire), w_fire, HIP_FIRE_KICK)
aim_act, aim_rep = solve_aim()

# Bullets: sample with the original constraints live, then re-parent and bake.
weapon_actions = {"A_W_Idle": ("A_FP_Idle", 0, 1, False),
                  "A_W_Fire_fixed": (arms_override["Fire"], 0, fire_fe, False),
                  "A_W_Inspect": ("A_FP_Inspect",) + frange("A_FP_Inspect") + (False,),
                  "A_W_Mag_Check": ("A_FP_Mag_Check",) + frange("A_FP_Mag_Check") + (False,),
                  "A_W_Reload": (arms_override["Reload"],) + frange("A_FP_Reload") + (True,)}
samples = {w: sample_bullets(a, w, fs, fe, hide) for w, (a, fs, fe, hide) in weapon_actions.items()}
timeline = eject_timeline(samples["A_W_Reload"], frange("A_FP_Reload")[0])
reparent_bullets()
baked = {w: write_bullets(w, a, samples[w], fs) for w, (a, fs, fe, _) in weapon_actions.items()}
weapon_for = {"A_W_Idle": baked["A_W_Idle"], "A_W_Fire": baked["A_W_Fire_fixed"],
              "A_W_Inspect": baked["A_W_Inspect"], "A_W_Mag_Check": baked["A_W_Mag_Check"],
              "A_W_Reload": baked["A_W_Reload"]}
for r in timeline:
    print("EJECT", json.dumps(r))
report["fixes"].append("Bullet1..6 re-parented under Cylinder, CHILD_OF baked; reload rounds hidden between "
                       "leaving the cylinder and the left hand picking them up.")


def render_checks(folder):
    """Workbench stills of every fix, through the scene's head camera (REVOLVER38_RENDER=<dir>)."""
    os.makedirs(folder, exist_ok=True)
    scn.render.engine = "BLENDER_WORKBENCH"
    scn.display.shading.light, scn.display.shading.color_type = "STUDIO", "MATERIAL"
    if hasattr(scn.render.image_settings, "media_type"):
        scn.render.image_settings.media_type = "IMAGE"
    scn.render.image_settings.file_format = "PNG"
    scn.render.resolution_x, scn.render.resolution_y = 960, 540
    for o in bpy.data.objects:
        if o.name.startswith(("Cube", "Motion_Ref", "_", "Light")) or o.type == "FONT":
            o.hide_render = True
    shots = [("aim", aim_act, weapon_for["A_W_Idle"], [0])]
    shots.append(("fire", arms_override["Fire"], weapon_for["A_W_Fire"], [0, 1, 2, 3, 6, 12, 18]))
    shots.append(("reload", arms_override["Reload"], weapon_for["A_W_Reload"], [50, 56, 58, 62, 70, 94, 100, 112, 124, 132, 200, 290]))
    for tag, a, w, frames in shots:
        pair(a, w)
        for f in frames:
            scn.frame_set(f)
            scn.render.filepath = os.path.join(folder, "%s_%03d.png" % (tag, f))
            bpy.ops.render.render(write_still=True)


if os.environ.get("REVOLVER38_RENDER"):
    render_checks(os.environ["REVOLVER38_RENDER"])

root_childof.mute = True  # weapon clips: Root holds its bind pose
weapon_files = []
try:
    if want("Weapon"):
        pair("A_FP_Idle", weapon_for["A_W_Idle"])
        f = "%s_A_W_Idle.fbx" % PREFIX
        p = os.path.join(OUT, "Weapon", f)
        export(p, [wep, mesh], 0, 1, True)
        weapon_files.append(p)
        manifest["exports"].append({"file": "Weapon/" + f, "action": "A_W_Idle", "pairedWith": "A_FP_Idle",
                                    "frameStart": 0, "frameEnd": 1, "skeleton": "Armature.001", "meshes": ["Revolver"],
                                    "note": "Weapon model and the weapon clip of every state whose gun doesn't move by itself."})
    for name, (fp, wp) in CLIPS.items():
        if not want(name):
            continue
        fs, fe = frange(fp)
        arms_act = arms_override.get(name, fp)
        pair(arms_act, weapon_for[wp])
        f = "%s_A_FP_%s.fbx" % (PREFIX, name)
        export(os.path.join(OUT, "FirstPerson", f), [arm], fs, fe, False)
        manifest["exports"].append({"file": "FirstPerson/" + f, "action": arms_act if isinstance(arms_act, str) else arms_act.name,
                                    "pairedWith": wp, "frameStart": fs, "frameEnd": fe, "skeleton": "Armature"})
        print("EXPORTED", f, fs, fe)
        if name in WEAPON_CLIPS:
            f = "%s_A_W_%s.fbx" % (PREFIX, name)
            p = os.path.join(OUT, "Weapon", f)
            export(p, [wep], fs, fe, False)
            weapon_files.append(p)
            manifest["exports"].append({"file": "Weapon/" + f, "action": weapon_for[wp].name, "pairedWith": fp,
                                        "frameStart": fs, "frameEnd": fe, "skeleton": "Armature.001",
                                        "note": "Exported over the arms clip's range (the weapon action holds its last key)."})
            print("EXPORTED", f, fs, fe)
    if want("Aim"):
        pair(aim_act, weapon_for["A_W_Idle"])
        f = "%s_A_FP_Aim.fbx" % PREFIX
        export(os.path.join(OUT, "FirstPerson", f), [arm], 0, 1, False)
        manifest["exports"].append({"file": "FirstPerson/" + f, "action": "A_FP_Idle (frame 0) + solved CB_Gun",
                                    "pairedWith": "A_W_Idle", "frameStart": 0, "frameEnd": 1, "skeleton": "Armature",
                                    "solve": aim_rep})
        print("EXPORTED", f)
    if want("Casing"):
        for spent in (True, False):
            ob, tris, length = cut_round(spent)
            f = ob.name + ".fbx"
            export(os.path.join(OUT, f), [ob], 0, 0, True, modifiers=True)
            manifest["exports"].append({"file": f, "meshes": [ob.name], "sourceTris": tris, "lengthCm": round(length, 3),
                                        "note": "Cut from the Revolver mesh's Bullet1 (%s), long axis +Y, base at +Y."
                                                % ("case only" if spent else "whole round")})
            print("EXPORTED", f, tris, round(length, 3))
finally:
    root_childof.mute = False

if weapon_files:
    zero_root(weapon_files)
for e in manifest["exports"]:
    p = os.path.join(OUT, e["file"])
    e["bytes"], e["sha256"] = os.path.getsize(p), digest(p)
manifest["ejectTimeline"] = timeline
manifest["fixes"] = report["fixes"]
manifest["warnings"] = report["warnings"]
if ONLY is None:
    with open(os.path.join(OUT, "export_manifest.json"), "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2)
if SAVE3P:
    for a in list(arms_override.values()) + [aim_act] + list(baked.values()):
        a.use_fake_user = True
    for ob in [o for o in bpy.data.objects if o.name in ("Casing_38Special", "Round_38Special")]:
        bpy.data.objects.remove(ob)
    pair("A_FP_Idle", weapon_for["A_W_Idle"])
    bpy.ops.wm.save_as_mainfile(filepath=SAVE3P, copy=True)
    print("SAVED 3P working copy", SAVE3P)
print("DONE %d files; warnings: %s" % (len(manifest["exports"]), report["warnings"]))
sys.stdout.flush()
