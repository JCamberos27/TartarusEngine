# Exports the whole Remington 870 first-person set from its source .blend, without ever saving it.
#
#   blender --background "<Remington 870 60fps(Revised).blend>" --python tools/weapons/export_remington.py -- \
#           <out dir: project/assets/Weapons/Remington870> [only <Name>[,<Name>...]]
#
# The same rules as tools/weapons/export_clip.py (the AKS-74U's exporter), for this file's rig names:
#
#  - Arms rig `Armature` (the shared Manny/Quantum rig), weapon rig `Armature.001` (bones Main,
#    Trigger, Shell, LoadingPort, Pump), weapon mesh `Remington870`.
#  - Two constraints cross between the rigs: the weapon's `Main` is CHILD_OF -> Armature:CB_Gun,
#    and the arms' `CB_ik_hand_l` is CHILD_OF -> Armature.001:Shell. So every clip is baked with
#    BOTH rigs playing their paired actions: the left hand bakes against the weapon's Shell bone,
#    and the weapon's root bakes against the arms' gun bone. Arms actions without a weapon
#    counterpart pair with A_W_Idle (the weapon's 1-frame idle pose, which parks the loose shell).
#  - Arms clips are channels only (the arms model is the shared Quantum_Arms_FP.fbx). Weapon clips
#    are channels only, except A_W_Idle, which carries the mesh and is the weapon model.
#  - Both halves of a pair are exported over the ARMS clip's frame range, so a state's arms and
#    weapon clips are the same length and its events line up with both.
#  - Aim: the file has no ADS pose. It is solved here in memory: the A_FP_Idle frame-0 pose with
#    CB_Gun moved so the Remington's sight line (ghost-ring centre -> front post, Main space) lies
#    on the AKS-74U's measured ADS sight line (the eye, in the shared rig's space), with the grip
#    (Main) the same 28 cm down it. Both weapons then share the scene's view-model offset.
#
# Pinned FBX settings, as export_clip.py explains: bake, simplify 0, no leaf bones, no NLA, no
# all-actions, -Z forward / Y up, unit scale applied. No .save() call exists anywhere below.
import bpy
import json
import os
import sys

import mathutils as M

argv = sys.argv[sys.argv.index("--") + 1:]
OUT = os.path.abspath(argv[0])
ONLY = set(argv[2].split(",")) if len(argv) > 2 and argv[1] == "only" else None
PREFIX = "Remington870"

# name -> (arms action, weapon action). The weapon action also sets the weapon clip, if exported.
CLIPS = {
    "Idle": ("A_FP_Idle", "A_W_Idle"),
    "Walk": ("A_FP_Walk", "A_W_Idle"),
    "Sprint": ("A_FP_Sprint", "A_W_Idle"),
    "IdleToSprint": ("A_FP_Sprint_start", "A_W_Idle"),
    "SprintToIdle": ("A_FP_Sprint_end", "A_W_Idle"),
    "Fire": ("A_FP_Fire", "A_W_Idle"),
    "Pump": ("A_FP_Pump", "A_W_Pump"),
    "Regrip": ("A_FP_Regrip", "A_W_Idle"),
    "Inspect": ("A_FP_Inspect", "A_W_Inspect"),
    "Mag_Check": ("A_FP_Mag_Check", "A_W_Mag_Check"),
    "Melee": ("A_FP_Melee", "A_W_Idle"),
    "Reload_Start": ("A_FP_reload_start", "A_W_Reload_start"),
    "Reload_Start_Empty": ("A_FP_Reload_start_empty", "A_W_Reload_start_empty"),
    "Reload_Loop": ("A_FP_Reload_loop", "A_W_Reload_loop"),
    "Reload_Loop_End": ("A_FP_Reload_loop_end", "A_W_Reload_loop_end"),
    "Reload_End": ("A_FP_Reload_end", "A_W_Reload_end"),
    "Draw": ("A_FP_Draw", "A_W_Idle"),
    "Holster": ("A_FP_Holster", "A_W_Idle"),
}
# States whose weapon moves by itself get a weapon clip; the rest play A_W_Idle (the base model's
# own 1-frame take), never the bind pose - the rest pose leaves the loose shell in the port.
WEAPON_CLIPS = {"Pump", "Inspect", "Mag_Check", "Reload_Start", "Reload_Start_Empty", "Reload_Loop",
                "Reload_Loop_End", "Reload_End"}

# The AKS-74U's ADS sight line (the eye and its look direction), in the shared arms rig's world
# space (Blender cm): the AK .fpsanim's measured gameplay.sightLine taken through A_W_ADS's root.
AK_EYE = M.Vector((1.1425, -9.5350, 158.9291))
AK_LOOK = M.Vector((-0.0073, -1.0, -0.0036)).normalized()
AK_UP = M.Vector((0.027, 0.0, 1.0))
# The Remington's sight line in Main space (cm; Main's -Z is the bore, +Y up): the ghost ring's
# centre and the front post's top both sit ~6.6 cm over Main's axis. The grip (Main) rides 28 cm
# ahead of the eye, as the AK's root does (sightLine.origin x = 0.28 m).
SIGHT_HEIGHT = 6.6
EYE_BEHIND_MAIN = 28.0
# Measured in Play with the first solve (the engine's logged sight line, Main space, m): the eye sat
# at (0.0138, 0.0810) - 1.4 cm right of and 1.5 cm over the sights - since the AK line above is the
# engine eye only up to the scene's view-model offset. The gun moves by that much, in its own frame.
SIGHT_CORRECTION = M.Vector((1.38, 1.50, 0.0))

scn = bpy.context.scene
arm = bpy.data.objects["Armature"]
wep = bpy.data.objects["Armature.001"]
mesh = bpy.data.objects["Remington870"]

state = {
    "arm_action": arm.animation_data.action,
    "wep_action": wep.animation_data.action,
    "arm_pose": arm.data.pose_position,
    "wep_pose": wep.data.pose_position,
    "frames": (scn.frame_start, scn.frame_end),
    "nla": [(t, t.mute) for o in (arm, wep) for t in o.animation_data.nla_tracks],
}
for t, _ in state["nla"]:
    t.mute = True
arm.data.pose_position = wep.data.pose_position = "POSE"


def pair(arms_action, weapon_action):
    arm.animation_data.action = bpy.data.actions[arms_action]
    wep.animation_data.action = bpy.data.actions[weapon_action]


def export(path, objects, fs, fe, with_mesh):
    for ob in bpy.data.objects:
        ob.select_set(False)
    for ob in objects:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    scn.frame_start, scn.frame_end = fs, fe
    scn.frame_set(fs)
    ok = bpy.ops.export_scene.fbx(
        filepath=path,
        use_selection=True,
        object_types={"ARMATURE", "MESH"} if with_mesh else {"ARMATURE"},
        use_mesh_modifiers=False,
        add_leaf_bones=False,
        bake_anim=True,
        bake_anim_use_all_bones=True,
        bake_anim_use_nla_strips=False,
        bake_anim_use_all_actions=False,
        bake_anim_force_startend_keying=True,
        bake_anim_step=1.0,
        bake_anim_simplify_factor=0.0,
        path_mode="AUTO",
        axis_forward="-Z",
        axis_up="Y",
        global_scale=1.0,
        apply_unit_scale=True,
    )
    if ok != {"FINISHED"}:
        raise SystemExit("export failed: %s %r" % (path, ok))
    return os.path.getsize(path)


def world(ob, bone):
    return ob.matrix_world @ ob.pose.bones[bone].matrix


def solve_aim_action():
    """A_FP_Idle's first frames with CB_Gun keyed so the sights sit on the AK's ADS sight line."""
    pair("A_FP_Idle", "A_W_Idle")
    scn.frame_set(0)
    # CB_Gun -> Main is constant (CHILD_OF); keep it while moving the gun.
    k = world(arm, "CB_Gun").inverted() @ world(wep, "Main")
    z = -AK_LOOK
    y = (AK_UP - AK_UP.dot(z) * z).normalized()
    x = y.cross(z)
    rot = M.Matrix((x, y, z)).transposed()
    main = rot.to_4x4()
    main.translation = AK_EYE - rot @ (M.Vector((0.0, SIGHT_HEIGHT, EYE_BEHIND_MAIN)) - SIGHT_CORRECTION)
    grip_before = world(arm, "CB_Gun").inverted() @ world(arm, "hand_r")
    act = bpy.data.actions["A_FP_Idle"].copy()
    act.name = "A_FP_Aim_solved"
    arm.animation_data.action = act
    scn.frame_set(0)
    pb = arm.pose.bones["CB_Gun"]
    for f in (0, 1):
        scn.frame_set(f)
        pb.matrix = arm.matrix_world.inverted() @ (main @ k.inverted())
        bpy.context.view_layer.update()
        for path in ("location", "rotation_euler", "scale"):
            pb.keyframe_insert(path, frame=f)
    scn.frame_set(0)
    got = world(wep, "Main")
    grip_after = world(arm, "CB_Gun").inverted() @ world(arm, "hand_r")
    hand_l = world(wep, "Shell").inverted() @ world(arm, "hand_l")
    report = {
        "mainError": (got.translation - main.translation).length,
        "rightGripDrift": (grip_after.translation - grip_before.translation).length,
        "leftHandFromShell": hand_l.translation.length,
        "eye": list(AK_EYE),
        "sightPoint": list(got @ M.Vector((0.0, SIGHT_HEIGHT, EYE_BEHIND_MAIN))),
    }
    print("AIM solve: Main off target %.4f cm, right grip drift %.4f cm, left hand %.2f cm from Shell"
          % (report["mainError"], report["rightGripDrift"], report["leftHandFromShell"]))
    return act, report


def action_range(name):
    a = bpy.data.actions[name]
    return int(a.frame_range[0]), int(a.frame_range[1])


os.makedirs(os.path.join(OUT, "FirstPerson"), exist_ok=True)
os.makedirs(os.path.join(OUT, "Weapon"), exist_ok=True)
manifest = {
    "source": bpy.data.filepath,
    "fps": scn.render.fps,
    "armsSkeleton": "Armature",
    "weaponSkeleton": "Armature.001",
    "weaponMesh": "Remington870",
    "exports": [],
    "errors": [],
}


def want(name):
    return ONLY is None or name in ONLY


try:
    # The weapon model: A_W_Idle with the mesh (bind = rest; its one-frame take parks the shell).
    if want("Weapon"):
        pair("A_FP_Idle", "A_W_Idle")
        f = "%s_A_W_Idle.fbx" % PREFIX
        n = export(os.path.join(OUT, "Weapon", f), [wep, mesh], 0, 1, True)
        manifest["exports"].append({"file": "Weapon/" + f, "action": "A_W_Idle", "pairedWith": "A_FP_Idle",
                                    "frameStart": 0, "frameEnd": 1, "skeleton": "Armature.001",
                                    "meshes": ["Remington870"], "bytes": n,
                                    "note": "Weapon model and the weapon clip of every state whose gun doesn't move by itself."})
        print("EXPORTED", f)

    for name, (fp, wp) in CLIPS.items():
        if not want(name):
            continue
        fs, fe = action_range(fp)
        pair(fp, wp)
        f = "%s_A_FP_%s.fbx" % (PREFIX, name)
        n = export(os.path.join(OUT, "FirstPerson", f), [arm], fs, fe, False)
        manifest["exports"].append({"file": "FirstPerson/" + f, "action": fp, "pairedWith": wp,
                                    "frameStart": fs, "frameEnd": fe, "skeleton": "Armature",
                                    "meshes": [], "bytes": n})
        print("EXPORTED", f, fs, fe, "paired", wp)
        if name in WEAPON_CLIPS:
            pair(fp, wp)
            f = "%s_A_W_%s.fbx" % (PREFIX, name)
            n = export(os.path.join(OUT, "Weapon", f), [wep], fs, fe, False)
            manifest["exports"].append({"file": "Weapon/" + f, "action": wp, "pairedWith": fp,
                                        "frameStart": fs, "frameEnd": fe, "skeleton": "Armature.001",
                                        "meshes": [], "bytes": n,
                                        "note": "Exported over the arms clip's range (the weapon action holds its last key)."})
            print("EXPORTED", f, fs, fe)

    if want("Aim"):
        act, rep = solve_aim_action()
        wep.animation_data.action = bpy.data.actions["A_W_Idle"]
        f = "%s_A_FP_Aim.fbx" % PREFIX
        n = export(os.path.join(OUT, "FirstPerson", f), [arm], 0, 1, False)
        manifest["exports"].append({"file": "FirstPerson/" + f, "action": "A_FP_Idle (frame 0) + solved CB_Gun",
                                    "pairedWith": "A_W_Idle", "frameStart": 0, "frameEnd": 1,
                                    "skeleton": "Armature", "meshes": [], "bytes": n, "solve": rep,
                                    "note": "The source file has no ADS action. Solved in memory (never saved): A_FP_Idle's "
                                            "frame-0 pose with CB_Gun moved so the ghost ring and front post (6.6 cm over "
                                            "Main's axis) lie on the AKS-74U's measured ADS sight line, the grip 28 cm down it."})
        print("EXPORTED", f)
        bpy.data.actions.remove(act)
finally:
    # Restore in memory; the .blend on disk is untouched.
    arm.animation_data.action = state["arm_action"]
    wep.animation_data.action = state["wep_action"]
    arm.data.pose_position = state["arm_pose"]
    wep.data.pose_position = state["wep_pose"]
    scn.frame_start, scn.frame_end = state["frames"]
    for t, m in state["nla"]:
        t.mute = m

if ONLY is None:
    with open(os.path.join(OUT, "export_manifest.json"), "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2)
print("DONE %d files" % len(manifest["exports"]))
sys.stdout.flush()
