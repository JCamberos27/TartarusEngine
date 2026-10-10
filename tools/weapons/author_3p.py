# Authors the third-person arms clips (A_3P_*) from a weapon's first-person actions (A_FP_*), and exports them.
#
# Run inside Blender on a working COPY of the weapon's source .blend (never the original), e.g. through the Blender
# MCP:  exec(open(r"<repo>/tools/weapons/author_3p.py").read(), {"ARGS": {"out": r"<repo>/project/assets/Weapons/AKS74U/ThirdPerson"}})
# or:   blender --background "<copy>.blend" --python tools/weapons/author_3p.py -- <out dir> [save]
#
# Why: in third person the world body and the NPCs hold the gun where the first-person clips put it - under the
# eye, on the view's centreline, so it reads too high and too centred. The 3P clips are the same clips with the
# gun carried into the right shoulder pocket. The engine samples a state's 3P clip beside its 1P clip and moves
# the third-person gun and hands by the difference of their gun sockets (ik_hand_gun), so only CB_Gun matters
# here, and every procedural layer (ADS, recoil, sway) still comes from the first-person rig.
#
# CB_Gun drives the whole hold: ik_hand_gun copies it, the gun's root is CHILD_OF it, CB_ik_hand_r is CHILD_OF it
# and CB_ik_hand_l is CHILD_OF the gun's magazine (AK) / Shell (Remington). So each A_3P_x is A_FP_x with CB_Gun
# re-keyed every frame as  O * CB_Gun,  O a rigid offset in the rig's camera frame (right = -X, up = +Z,
# forward = -Y, cm) turned about the stock's butt. Everything else in the action is left as authored.
#
# The offsets were measured in the engine (STOCK_PROBE_POSE=1 --stock-probe ak: the world gun's butt against the
# body's right shoulder joint, right/up/forward cm): hip (-8.7, +3.6, +5.9), sights (-16.4, +6.2, -1.1). The
# shoulder pocket is about (-4.5, -1.5, +5.5) at the hip; on the sights the butt comes in to (-7, +1.5, +1.5),
# under the cheek. Tune them here and re-run; the engine needs no change.
import math
import os
import sys

import bpy
import mathutils as M

# --- offsets (right, up, forward cm; pitch degrees, + = muzzle down) -----------------------------------------
# A group either offsets the clip's own gun ("move", "pitch"), or - "carry" - replaces it: the "base" group's hold
# at Idle frame 0, turned about the butt by "pitch" (+ = muzzle down) and "yaw" (+ = muzzle inward, to the left)
# and moved by "move", with the clip's own sway about its average kept at "motion" (0..1). A weapon's "groups"
# override these.
# "damp": an action that takes the gun away from the hold and brings it back (inspect, mag check, melee). Its own
# motion is kept about the "base" group's hold at the clip's first frame, scaled to "damp" (0..1): the first-person
# clips bring the gun up to the camera - to the face, seen from outside - and swing it hard for the view. "away"
# (right, up, forward cm) moves it off the face as far as the clip has it out of the hold (0 at the hold, all of it at
# the clip's furthest).
GROUPS = {
    "hip":    {"move": (4.2, -5.1, -0.4), "pitch": 8.0},
    "aim":    {"move": (9.4, -4.7, 2.6), "pitch": 0.0},
    "sprint": {"move": (4.2, -5.1, -0.4), "pitch": 0.0},
    "inspect": {"damp": 0.75, "base": "hip", "away": (0.0, -6.0, 10.0)},
    "magcheck": {"damp": 0.8, "base": "hip", "away": (0.0, -4.0, 6.0)},
    "melee": {"damp": 0.6, "base": "hip", "away": (0.0, -4.0, 10.0)},
}

AK = {
    "arms": "Armature", "weapon": "AK", "prefix": "AKS-74U",
    # The support hand (as the Remington's, below): the first-person clips hold the handguard palm on its left side,
    # fingers hooked over the top - right from behind the sights, but from outside a hand beside the gun with its
    # fingers up. In 3P it rolls 45 degrees about the handguard (y -28..-16 gun space, centre z 0.7): palm under, thumb
    # up the left side, fingers up the right. No slide: the clip's arm is already near its full reach.
    "support": {
        "slide": 0.0, "roll": 45.0, "axis": (0.0, 0.7), "pole": (7.0, 0.0, -50.0),
        "clips": {"Idle": 1.0, "Walk": 1.0, "Fire": 1.0, "Aim": 1.0, "Regrip": 1.0, "Sprint": 1.0,
                  "IdleToSprint": 1.0, "SprintToIdle": 1.0, "Draw": (0.0, 1.0), "Holster": (1.0, 0.0)},
    },
    # name -> (arms action, weapon action, offset group, or (from group, to group) eased across the clip)
    "clips": {
        "Idle": ("A_FP_Idle", "A_W_ADS", "hip"),
        "Walk": ("A_FP_Walk", "A_W_ADS", "hip"),
        "Fire": ("A_FP_Fire", "A_W_Fire", "hip"),
        "Regrip": ("A_FP_Regrip", "A_W_ADS", "hip"),
        "Inspect": ("A_FP_Inspect", "A_W_Inspect", "inspect"),
        "Mag_Check": ("A_FP_Mag_Check", "A_W_Mag_Check", "magcheck"),
        "Melee": ("A_FP_Melee", "A_W_Melee", "melee"),
        "Tac_Reload": ("A_FP_Tac_Reload", "A_W_Tac_Reload", "hip"),
        "Empty_Reload": ("A_FP_Empty_Reload", "A_W_Empty_Reload", "hip"),
        "Sprint": ("A_FP_Sprint", "A_W_Sprint", "sprint"),
        "IdleToSprint": ("A_FP_IdleToSprint", "A_W_IdleToSprint", ("hip", "sprint")),
        "SprintToIdle": ("A_FP_SprintToIdle", "A_W_SprintToIdle", ("sprint", "hip")),
        # In the 3P hold by 45% of the draw / until 55% of the holster: eased across the whole clip, mid-draw the gun
        # was held out at arm's length in front of the face (where the first-person clip has it, rising into the view).
        "Draw": ("A_FP_Draw", "A_W_ADS", (None, "hip", 0.0, 0.45)),
        "Holster": ("A_FP_Holster", "A_W_Holster", ("hip", None, 0.55, 1.0)),
        # The engine's Aim clip is not in the .blend; its gun socket comes from the shipped 1P Aim FBX, as every clip's does.
        "Aim": ("A_FP_ADS", "A_W_ADS", "aim"),
    },
    # Third-person fixes by clip (rework_hand). The tac reload: the new magazine seated (frame 100), the hand stows the old
    # one at the pouch (124) and is back on the handguard by 142 - out and back in a third of a second, which the first
    # person never sees (the hand drops out of the view) and the third person saw as a flick behind the hip. Re-timed: it
    # sets off at once and takes twice as long out, a little longer back; still at the pouch at 124 (the magazine leaves
    # the hand there, on the clips' time).
    "fx": {
        "Tac_Reload": {"hand": [(100, 142, "retime", [(100, 100), (102, 113), (124, 124), (141, 138), (142, 142)])]},
        "Melee": {"smooth": 2.0},
    },
}

REMINGTON = {
    "arms": "Armature", "weapon": "Armature.001", "prefix": "Remington870",
    # The Remington's own pocket offsets: with the AK's, its butt rode 3.6 cm over the shoulder joint and 9 cm ahead
    # of it on the sights (STOCK_PROBE_POSE=1 --stock-probe remington). Measured in the engine and aimed at the
    # pocket: sights (-6, -4.5, +3) - below the clavicle, in the pocket (at +0.5 the butt sat on the collarbone) -
    # and hip (-4.5, -2, +3.5), from the shoulder joint (right, up, forward cm).
    # Its sprint arms are the AK's keys, which with the longer gun carried it across the face: low ready instead.
    "groups": {
        "hip": {"move": (0.5, -6.3, -3.5), "pitch": 8.0},
        "aim": {"move": (7.8, -12.8, -3.2), "pitch": 0.0},
        # The sprint's body leans ~25 deg forward on top of this: 15 here (23 with the hip's) reads as low ready.
        "sprint": {"carry": True, "base": "hip", "pitch": 15.0, "yaw": 15.0, "move": (0.0, -2.0, 0.0), "motion": 0.4},
    },
    # The support hand in third person (the engine's world bodies take the 3P clips' grip, not only their gun): the
    # first-person clips hold the pump at its very front with the elbow out to the side, for the camera - seen from
    # outside, a straight arm stuck out at shoulder height. On the pump it slides `slide` cm back along the forend and
    # rolls `roll` degrees under it (about the forend's axis at `axis`, gun space x, z), and the elbow's pole goes to
    # `pole` (gun space cm: +x the gun's left, -y forward, +z up) so the elbow hangs under the gun. By clip, its weight
    # (or eased in / out across the clip); the reloads, inspect and melee keep the clips' own hand.
    "support": {
        "slide": 6.5, "roll": 25.0, "axis": (0.0, -0.35), "pole": (7.0, 0.0, -50.0),
        "clips": {"Idle": 1.0, "Walk": 1.0, "Fire": 1.0, "Aim": 1.0, "Pump": 1.0, "Regrip": 1.0, "Sprint": 1.0,
                  "IdleToSprint": 1.0, "SprintToIdle": 1.0, "Draw": (0.0, 1.0), "Holster": (1.0, 0.0)},
    },
    "clips": {
        "Idle": ("A_FP_Idle", "A_W_Idle", "hip"),
        "Walk": ("A_FP_Walk", "A_W_Idle", "hip"),
        "Fire": ("A_FP_Fire", "A_W_Idle", "hip"),
        "Pump": ("A_FP_Pump", "A_W_Pump", "hip"),
        "Regrip": ("A_FP_Regrip", "A_W_Idle", "hip"),
        "Inspect": ("A_FP_Inspect", "A_W_Inspect", "inspect"),
        "Mag_Check": ("A_FP_Mag_Check", "A_W_Mag_Check", "magcheck"),
        "Melee": ("A_FP_Melee", "A_W_Idle", "melee"),
        "Reload_Start": ("A_FP_reload_start", "A_W_Reload_start", "hip"),
        "Reload_Start_Empty": ("A_FP_Reload_start_empty", "A_W_Reload_start_empty", "hip"),
        "Reload_Loop": ("A_FP_Reload_loop", "A_W_Reload_loop", "hip"),
        "Reload_Loop_End": ("A_FP_Reload_loop_end", "A_W_Reload_loop_end", "hip"),
        "Reload_End": ("A_FP_Reload_end", "A_W_Reload_end", "hip"),
        "Sprint": ("A_FP_Sprint", "A_W_Idle", "sprint"),
        "IdleToSprint": ("A_FP_Sprint_start", "A_W_Idle", ("hip", "sprint")),
        "SprintToIdle": ("A_FP_Sprint_end", "A_W_Idle", ("sprint", "hip")),
        "Draw": ("A_FP_Draw", "A_W_Idle", (None, "hip", 0.0, 0.45)),
        "Holster": ("A_FP_Holster", "A_W_Idle", ("hip", None, 0.55, 1.0)),
        # The Remington has no aim action (export_remington.py solves it); the shipped 1P Aim FBX gives the socket.
        "Aim": ("A_FP_Idle", "A_W_Idle", "aim"),
    },
    # The reload's start: its last two frames snap the left hand 30 cm (back to a rest pose the loop never uses - the
    # loop starts where frame 28 is). Held at frame 28 instead.
    "fx": {
        "Reload_Start": {"hand": [(28, 30, "hold")]},
        "Melee": {"smooth": 2.0},
    },
}

# --- arguments ------------------------------------------------------------------------------------------------
_args = globals().get("ARGS")
if _args is None:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    _args = {"out": argv[0] if argv else None, "save": len(argv) > 1 and argv[1] == "save"}
OUT = os.path.abspath(_args["out"])
WEAPON_DIR = os.path.dirname(OUT)
ONLY = _args.get("only")

scn = bpy.context.scene
CFG = AK if bpy.data.objects.get("AK") else REMINGTON
arm = bpy.data.objects[CFG["arms"]]
wep = bpy.data.objects[CFG["weapon"]]
bpy.context.view_layer.objects.active = arm  # the FBX importer needs one
if os.path.basename(bpy.data.filepath).find("3P") < 0:
    raise SystemExit("run on the (3P) working copy of the .blend, not the original: %s" % bpy.data.filepath)


G = dict(GROUPS, **CFG.get("groups", {}))


def turned(g, butt_world):
    """A group's move and turn about the butt (armature space, cm): right = -X, up = +Z, forward = -Y."""
    r, u, f = g["move"]
    turn = M.Matrix.Rotation(math.radians(g.get("yaw", 0.0)), 4, "Z") @ M.Matrix.Rotation(math.radians(g["pitch"]), 4, "X")
    return M.Matrix.Translation(butt_world + M.Vector((-r, -f, u))) @ turn @ M.Matrix.Translation(-butt_world)


def rigid_mix(a, b, w):
    """Lerp the translation, slerp the rotation."""
    q = a.to_quaternion().slerp(b.to_quaternion(), w)
    return M.Matrix.Translation(a.translation.lerp(b.translation, w)) @ q.to_matrix().to_4x4()


_carry = {}


def carry_frame(name):
    """A carry group's pose (its base group's hold at Idle frame 0, turned) and the 1P sprint's average socket."""
    if name not in _carry:
        g = G[name]
        idle = shipped_sockets("Idle", [0])[0]
        hold = turned(G[g["base"]], idle @ BUTT) @ idle
        sprint = bpy.data.actions[CFG["clips"]["Sprint"][0]]
        frames = list(range(int(round(sprint.frame_range[0])), int(round(sprint.frame_range[1])) + 1))
        socks = shipped_sockets("Sprint", frames)
        t = sum((s.translation for s in socks), M.Vector()) / len(socks)
        q0 = socks[0].to_quaternion()
        acc = [0.0, 0.0, 0.0, 0.0]  # w, x, y, z: the sockets' rotations summed on q0's side, then normalised
        for s in socks:
            q = s.to_quaternion()
            sign = -1.0 if q.dot(q0) < 0.0 else 1.0
            for i, c in enumerate((q.w, q.x, q.y, q.z)):
                acc[i] += sign * c
        mean = M.Matrix.Translation(t) @ M.Quaternion(acc).normalized().to_matrix().to_4x4()
        _carry[name] = (turned(g, hold @ BUTT) @ hold, mean)
    return _carry[name]


_clip = {}  # the clip being authored: its first frame's 1P socket ("m0") and how far its gun goes from it ("reach", cm)


def group_socket(group, m):
    """The 3P gun socket for a group, from the 1P one `m`."""
    if group is None:
        return m
    g = G[group]
    if g.get("damp") is not None:
        m0 = _clip["m0"]
        hold = turned(G[g["base"]], m0 @ BUTT) @ m0
        sock = hold @ rigid_mix(M.Matrix.Identity(4), m0.inverted() @ m, g["damp"])
        r, u, f = g.get("away", (0.0, 0.0, 0.0))
        out = (m.translation - m0.translation).length / max(_clip["reach"], 1e-3)
        e = min(out, 1.0)
        e = e * e * (3.0 - 2.0 * e)
        return M.Matrix.Translation(M.Vector((-r, -f, u)) * e) @ sock
    if not g.get("carry"):
        return turned(g, m @ BUTT) @ m
    pose, mean = carry_frame(group)
    sway = mean.inverted() @ m
    k = g.get("motion", 0.0)
    sway = rigid_mix(M.Matrix.Identity(4), sway, k)
    return pose @ sway


def socket_3p(spec, t, m):
    """The 3P socket at clip time t (0..1): one group, or eased between two (None = the 1P gun) - across the clip, or
    across (from, to) of it."""
    if not isinstance(spec, tuple):
        return group_socket(spec, m)
    a, b = spec[0], spec[1]
    t0, t1 = (spec[2], spec[3]) if len(spec) > 3 else (0.0, 1.0)
    t = min(max((t - t0) / max(t1 - t0, 1e-6), 0.0), 1.0)
    return rigid_mix(group_socket(a, m), group_socket(b, m), t * t * (3.0 - 2.0 * t))


def cb_gun_curves(action):
    bags = [bag for layer in action.layers for strip in layer.strips for bag in strip.channelbags]
    return [(bag, fc) for bag in bags for fc in bag.fcurves if fc.data_path.startswith('pose.bones["CB_Gun"]')]


def shipped_sockets(name, frames):
    """The engine's own 1P clip: its ik_hand_gun (armature space) at each frame, from the shipped FBX. The shipped
    clips were baked apart from this .blend and differ from its actions by up to ~1.5 cm / 2 deg on some frames;
    authored from them, the engine's 3P-minus-1P socket difference is the offset and nothing else."""
    path = os.path.join(WEAPON_DIR, "FirstPerson", "%s_A_FP_%s.fbx" % (CFG["prefix"], name))
    before = set(bpy.data.objects.keys())
    acts_before = set(bpy.data.actions.keys())
    bpy.ops.import_scene.fbx(filepath=path, use_anim=True)
    new = [bpy.data.objects[n] for n in set(bpy.data.objects.keys()) - before]
    imp = [o for o in new if o.type == "ARMATURE"][0]
    m = []
    for f in frames:
        scn.frame_set(f)
        m.append((arm.matrix_world.inverted() @ imp.matrix_world @ imp.pose.bones["ik_hand_gun"].matrix).copy())
    for o in new:
        data = o.data
        bpy.data.objects.remove(o)
        if isinstance(data, bpy.types.Armature):
            bpy.data.armatures.remove(data)
        elif isinstance(data, bpy.types.Mesh):
            bpy.data.meshes.remove(data)
    for a in set(bpy.data.actions.keys()) - acts_before:
        bpy.data.actions.remove(bpy.data.actions[a])
    bpy.context.view_layer.objects.active = arm  # the import made its own active; the next import needs one
    return m


def butt_in_gun():
    """The stock's butt in CB_Gun's space: the rearmost gun vertices near the bore."""
    dg = bpy.context.evaluated_depsgraph_get()
    mesh_ob = [o for o in wep.children if o.type == "MESH"][0].evaluated_get(dg)
    gun = arm.matrix_world @ arm.pose.bones["CB_Gun"].matrix
    mw = mesh_ob.matrix_world
    pts = [mw @ v.co for v in mesh_ob.data.vertices]
    near = [p for p in pts if abs(p.z - gun.translation.z) < 14.0 and abs(p.x - gun.translation.x) < 8.0]
    back = max(p.y for p in near)
    rear = [p for p in near if p.y > back - 1.0]
    centre = sum(rear, M.Vector()) / len(rear)
    return gun.inverted() @ centre


def use_actions(arms_action, weapon_action):
    arm.animation_data.action = bpy.data.actions[arms_action]
    if wep.animation_data is None:
        wep.animation_data_create()
    for t in wep.animation_data.nla_tracks:
        t.mute = True
    wep.animation_data.action = bpy.data.actions[weapon_action]


def author(name, arms_action, weapon_action, spec):
    src = bpy.data.actions[arms_action]
    out_name = "A_3P_" + name
    if out_name in bpy.data.actions:
        bpy.data.actions.remove(bpy.data.actions[out_name])
    fs, fe = (int(round(v)) for v in src.frame_range)
    if name == "Aim":
        fe = fs + 1  # one pose, held over two frames (a zero-length take won't import)
    # 1. The first-person gun, frame by frame (armature space): the shipped clip's (CB_Gun = ik_hand_gun).
    frames = list(range(fs, fe + 1))
    src_m = shipped_sockets(name, frames)
    if name == "Aim":
        src_m = [src_m[0]] * len(frames)
    use_actions(arms_action, weapon_action)
    pb = arm.pose.bones["CB_Gun"]
    fx = CFG.get("fx", {}).get(name, {})
    hand_l, pole_l = arm.pose.bones["CB_ik_hand_l"], arm.pose.bones["CB_pole_elbow_l"]
    first = []  # the first-person hold: (CB_Gun, the left hand's target, its elbow's pole, the hand's CHILD_OF influence)
    hand_parent(hand_l).influence = 1.0  # unkeyed, the hand is on its parent (as export() has it); keyed, the keys win
    for f in frames:
        scn.frame_set(f)
        first.append((pb.matrix.copy(), hand_l.matrix.copy(), pole_l.matrix.copy(), hand_parent(hand_l).influence))
    # 2. The 3P sockets, frame by frame (and, for a clip that asks, smoothed in time).
    _clip["m0"] = src_m[0]
    _clip["reach"] = max((m.translation - src_m[0].translation).length for m in src_m)
    span = max(fe - fs, 1)
    socks = [socket_3p(spec, (f - fs) / span, m) for f, m in zip(frames, src_m)]
    if fx.get("smooth"):
        socks = smooth_sockets(socks, fx["smooth"])
    # 3. The copy, its CB_Gun curves replaced by the offset hold.
    dst = src.copy()
    dst.name = out_name
    dst.use_fake_user = True
    for bag, fc in cb_gun_curves(dst):
        bag.fcurves.remove(fc)
    arm.animation_data.action = dst
    prev = None
    for f, sock in zip(frames, socks):
        scn.frame_set(f)
        pb.matrix = sock
        bpy.context.view_layer.update()
        basis = pb.matrix_basis.copy()
        pb.location = basis.translation
        e = basis.to_euler("XYZ", prev) if prev is not None else basis.to_euler("XYZ")
        pb.rotation_euler = e
        prev = e
        pb.keyframe_insert("location", frame=f, group="CB_Gun")
        pb.keyframe_insert("rotation_euler", frame=f, group="CB_Gun")
    support = CFG.get("support")
    if support and name in support["clips"]:
        rework_support(dst, name, frames, support)
    elif any(inf < 0.999 for _, _, _, inf in first):
        rework_free_hand(dst, frames, first)
    if fx.get("hand"):
        rework_hand(dst, frames, fx["hand"])
    return dst, fs, fe


def rework_free_hand(dst, frames, first):
    """The left hand where the first-person clip has it off its parent (the magazine / shell) - working the charging
    handle, seating a magazine, at the pouch: its keys are in the camera's space, so the 3P gun's offset left it behind -
    reaching for the charging handle it grabbed the air past the handguard, and where the clip parents it again it
    jumped by the whole offset. It now rides the 3P gun all through the clip, as the parented hand does: the first-person
    path relative to the gun is kept. Off the gun the engine's hand anchor holds it to the body."""
    hand, pole = arm.pose.bones["CB_ik_hand_l"], arm.pose.bones["CB_pole_elbow_l"]
    gun = arm.pose.bones["CB_Gun"]
    parent = hand_parent(hand)
    third = []
    for f in frames:
        scn.frame_set(f)
        third.append(gun.matrix.copy())
    for bag, fc in bone_curves(dst, "CB_ik_hand_l") + bone_curves(dst, "CB_pole_elbow_l"):
        bag.fcurves.remove(fc)
    prev_h = prev_p = None
    for f, (g1, h1, p1, _), g3 in zip(frames, first, third):
        carry = g3 @ g1.inverted()
        scn.frame_set(f)
        parent.influence = 0.0
        parent.keyframe_insert("influence", frame=f)
        prev_h = key_matrix(hand, carry @ h1, prev_h, "CB_ik_hand_l")
        prev_p = key_matrix(pole, carry @ p1, prev_p, "CB_pole_elbow_l")


def smooth_sockets(socks, sigma):
    """A Gaussian in time (sigma frames) over the sockets: a first-person melee's one-frame snap becomes a fast move.
    The first and last frames are kept (the clip still starts and ends on the hold)."""
    n, r = len(socks), int(math.ceil(sigma * 3.0))
    out = []
    for i in range(n):
        acc_t, acc_q, wsum = M.Vector(), [0.0, 0.0, 0.0, 0.0], 0.0
        q0 = socks[i].to_quaternion()
        for j in range(max(0, i - r), min(n, i + r + 1)):
            w = math.exp(-0.5 * ((j - i) / sigma) ** 2)
            acc_t += socks[j].translation * w
            q = socks[j].to_quaternion()
            sign = -1.0 if q.dot(q0) < 0.0 else 1.0
            for k, c in enumerate((q.w, q.x, q.y, q.z)):
                acc_q[k] += sign * c * w
            wsum += w
        sm = M.Matrix.Translation(acc_t / wsum) @ M.Quaternion(acc_q).normalized().to_matrix().to_4x4()
        edge = min(i, n - 1 - i) / 3.0  # all of it 3 frames in from either end
        out.append(rigid_mix(socks[i], sm, min(edge, 1.0)))
    return out


def rework_hand(dst, frames, windows):
    """The left hand's trips the first-person clips hide out of the view, re-made for third person, in the gun's frame:
    "bridge" goes straight (smoothstepped, a little low of the line) from where the hand is at the window's start to
    where it is at its end; "hold" keeps it where it is at the start. The hand's CHILD_OF is keyed off for the whole
    clip and its motion baked, as rework_support does."""
    hand, pole, gun = arm.pose.bones["CB_ik_hand_l"], arm.pose.bones["CB_pole_elbow_l"], arm.pose.bones["CB_Gun"]
    parent = hand_parent(hand)
    parent.influence = 1.0  # as export() has an unkeyed influence (rework_free_hand, if it ran, keyed it)
    rec = {}
    for f in frames:
        scn.frame_set(f)
        rec[f] = (hand.matrix.copy(), pole.matrix.copy(), gun.matrix.copy())
    for bag, fc in bone_curves(dst, "CB_ik_hand_l") + bone_curves(dst, "CB_pole_elbow_l"):
        bag.fcurves.remove(fc)
    want = dict((f, (rec[f][0], rec[f][1])) for f in frames)
    def at(src):
        """The hand and pole on the gun (gun space) at a fractional clip frame."""
        f0 = int(math.floor(src))
        f1 = min(f0 + 1, frames[-1])
        k = src - f0
        (h0, p0, g0), (h1, p1, g1) = rec[f0], rec[f1]
        return rigid_mix(g0.inverted() @ h0, g1.inverted() @ h1, k), rigid_mix(g0.inverted() @ p0, g1.inverted() @ p1, k)

    for window in windows:
        fa, fb, mode = window[:3]
        if mode == "retime":
            knots = window[3]
            for f in range(fa, fb + 1):
                for (o0, s0), (o1, s1) in zip(knots, knots[1:]):
                    if o0 <= f <= o1:
                        src = s0 + (s1 - s0) * (f - o0) / max(o1 - o0, 1)
                        break
                h, p_ = at(src)
                gf = rec[f][2]
                want[f] = (gf @ h, gf @ p_)
            continue
        ha, pa, ga = rec[fa]
        hb, pb_, gb = rec[fb]
        a_h, b_h = ga.inverted() @ ha, gb.inverted() @ hb
        a_p, b_p = ga.inverted() @ pa, gb.inverted() @ pb_
        for f in range(fa, fb + 1):
            gf = rec[f][2]
            if mode == "hold":
                want[f] = (gf @ a_h, gf @ a_p)
                continue
            t = (f - fa) / max(fb - fa, 1)
            s = t * t * (3.0 - 2.0 * t)
            h = gf @ rigid_mix(a_h, b_h, s)
            h = M.Matrix.Translation(M.Vector((0.0, 0.0, -3.0 * math.sin(math.pi * t)))) @ h  # a little low of the line
            want[f] = (h, gf @ rigid_mix(a_p, b_p, s))
    prev_h = prev_p = None
    for f in frames:
        scn.frame_set(f)
        parent.influence = 0.0
        parent.keyframe_insert("influence", frame=f)
        prev_h = key_matrix(hand, want[f][0], prev_h, "CB_ik_hand_l")
        prev_p = key_matrix(pole, want[f][1], prev_p, "CB_pole_elbow_l")


def bone_curves(action, bone):
    bags = [bag for layer in action.layers for strip in layer.strips for bag in strip.channelbags]
    return [(bag, fc) for bag in bags for fc in bag.fcurves if fc.data_path.startswith('pose.bones["%s"]' % bone)]


def key_matrix(pb, m, prev, group):
    """Sets pose bone `pb` to armature-space `m` (its constraints off) and keys it; returns its rotation for continuity."""
    pb.matrix = m
    bpy.context.view_layer.update()
    basis = pb.matrix_basis.copy()
    pb.location = basis.translation
    pb.keyframe_insert("location", group=group)
    if pb.rotation_mode == "QUATERNION":
        q = basis.to_quaternion()
        if prev is not None and q.dot(prev) < 0.0:
            q.negate()
        pb.rotation_quaternion = q
        pb.keyframe_insert("rotation_quaternion", group=group)
        return q
    e = basis.to_euler(pb.rotation_mode, prev) if prev is not None else basis.to_euler(pb.rotation_mode)
    pb.rotation_euler = e
    pb.keyframe_insert("rotation_euler", group=group)
    return e


def hand_parent(hand):
    return next(c for c in hand.constraints if c.type == "CHILD_OF")


def rework_support(dst, name, frames, sup):
    """The support hand slid back along the forend and rolled under it, and its elbow's pole under the gun (gun space),
    by the clip's weight. The hand's CHILD_OF (to the weapon's shell / magazine) is keyed off in this clip: its keys
    then carry the whole of its motion, as the clip had it with the gun at its 3P hold."""
    hand, pole, gun = arm.pose.bones["CB_ik_hand_l"], arm.pose.bones["CB_pole_elbow_l"], arm.pose.bones["CB_Gun"]
    parent = hand_parent(hand)
    # 1. As the clip has them now.
    parent.influence = 1.0
    rec = []
    for f in frames:
        scn.frame_set(f)
        rec.append((hand.matrix.copy(), pole.matrix.copy(), gun.matrix.copy()))
    # 2. Re-keyed.
    for bag, fc in bone_curves(dst, "CB_ik_hand_l") + bone_curves(dst, "CB_pole_elbow_l"):
        bag.fcurves.remove(fc)
    spec = sup["clips"][name]
    fs, span = frames[0], max(frames[-1] - frames[0], 1)
    pivot = M.Vector((sup["axis"][0], 0.0, sup["axis"][1]))
    prev_h = prev_p = None
    for f, (hm, pm, gm) in zip(frames, rec):
        if isinstance(spec, tuple):
            t = (f - fs) / span
            w = spec[0] + (spec[1] - spec[0]) * t * t * (3.0 - 2.0 * t)
        else:
            w = spec
        off = (M.Matrix.Translation(M.Vector((0.0, sup["slide"] * w, 0.0))) @ M.Matrix.Translation(pivot) @
               M.Matrix.Rotation(math.radians(sup["roll"] * w), 4, "Y") @ M.Matrix.Translation(-pivot))
        scn.frame_set(f)
        parent.influence = 0.0
        parent.keyframe_insert("influence", frame=f)
        prev_h = key_matrix(hand, gm @ off @ gm.inverted() @ hm, prev_h, "CB_ik_hand_l")
        want = (gm @ M.Matrix.Translation(M.Vector(sup["pole"]))).translation
        prev_p = key_matrix(pole, M.Matrix.Translation(pm.translation.lerp(want, w)) @ pm.to_3x3().to_4x4(), prev_p, "CB_pole_elbow_l")


def export(action, weapon_action, fs, fe, path):
    for ob in bpy.context.view_layer.objects:
        ob.select_set(False)
    arm.hide_set(False)  # a hidden armature can't be selected, and the export would write nothing
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    arm.data.pose_position = "POSE"
    arm.animation_data.action = action
    wep.animation_data.action = bpy.data.actions[weapon_action]
    # The support hand's parent on, unless this clip keys it off (rework_support): an unkeyed influence keeps the last
    # value set.
    hand_parent(arm.pose.bones["CB_ik_hand_l"]).influence = 1.0
    scn.frame_start, scn.frame_end = fs, fe
    scn.frame_set(fs)
    # Pinned exactly as tools/weapons/export_clip.py (see there for why each setting matters).
    ok = bpy.ops.export_scene.fbx(
        filepath=path, use_selection=True, object_types={"ARMATURE"}, use_mesh_modifiers=False, add_leaf_bones=False,
        bake_anim=True, bake_anim_use_all_bones=True, bake_anim_use_nla_strips=False, bake_anim_use_all_actions=False,
        bake_anim_force_startend_keying=True, bake_anim_step=1.0, bake_anim_simplify_factor=0.0, path_mode="AUTO",
        axis_forward="-Z", axis_up="Y", global_scale=1.0, apply_unit_scale=True)
    if ok != {"FINISHED"}:
        raise SystemExit("export failed for %s" % path)


# By name: re-authoring replaces the A_3P_ actions, so a reference to one would be stale.
prev_arm_action = arm.animation_data.action.name if arm.animation_data.action else None
prev_wep_action = wep.animation_data.action.name if wep.animation_data and wep.animation_data.action else None
prev_range = (scn.frame_start, scn.frame_end)
os.makedirs(OUT, exist_ok=True)

use_actions(CFG["clips"]["Idle"][0], CFG["clips"]["Idle"][1])
scn.frame_set(0)
BUTT = butt_in_gun()
for group, g in G.items():
    if g.get("carry"):
        carry_frame(group)  # up front: it imports the shipped clips, which mustn't happen mid-clip
report = []
for name, (arms_action, weapon_action, spec) in CFG["clips"].items():
    if ONLY and name not in ONLY:
        continue
    action, fs, fe = author(name, arms_action, weapon_action, spec)
    path = os.path.join(OUT, "%s_A_3P_%s.fbx" % (CFG["prefix"], name))
    export(action, weapon_action, fs, fe, path)
    report.append("%s %d..%d -> %s" % (action.name, fs, fe, os.path.basename(path)))

arm.animation_data.action = bpy.data.actions.get(prev_arm_action) if prev_arm_action else None
hand_parent(arm.pose.bones["CB_ik_hand_l"]).influence = 1.0  # the first-person actions keep their hand on the weapon
if wep.animation_data:
    wep.animation_data.action = bpy.data.actions.get(prev_wep_action) if prev_wep_action else None
scn.frame_start, scn.frame_end = prev_range
if _args.get("save"):
    bpy.ops.wm.save_mainfile()
print("\n".join(report))
result = {"butt_in_gun": [round(v, 3) for v in BUTT], "clips": report}
