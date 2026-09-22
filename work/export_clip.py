# Exports ONE action from the AKS-74U rig as an FBX clip, without ever saving the .blend.
#
#   blender --background "<blend>" --python work/export_clip.py -- \
#           <action> <out.fbx> <frame_start> <frame_end> <meshes 0|1> [weapon <A_W_x>|none|auto]
#
# THE WEAPON ACTION MATTERS. CB_ik_hand_l carries CHILD_OF -> AK:magazine at influence 1.0, so
# the entire left arm is glued to the WEAPON armature's magazine bone and the bake samples the
# weapon as much as the arms. Exporting A_FP_* while the AK still plays whatever action the file
# happened to be saved on bakes a left hand chasing the wrong weapon animation: that is exactly
# how the shipped AKS-74U_A_FP_Idle.fbx got a left arm travelling 25 cm while the right arm was
# dead still (A_W_Tac_Reload was the file's saved weapon action, and its magazine starts moving
# around frame 44 - right in the middle of the 0..128 idle range).
#
# So the weapon action is DERIVED from the arms action: A_FP_x -> A_W_x, falling back to A_W_ADS
# when no weapon counterpart exists. Idle and Walk have no A_W_Idle / A_W_Walk, and the engine
# renders those states with the weapon sitting on its bind pose - which IS the A_W_ADS export -
# so ADS is the pose the left hand has to be baked against.
#
# Settings are pinned explicitly rather than left to operator defaults: Blender's defaults
# (add_leaf_bones=True, bake_anim_use_all_actions=True, bake_anim_use_nla_strips=True,
# bake_anim_simplify_factor=1.0) would each change the output - extra bones, extra takes, NLA
# strips folded in, or keys decimated. bake_anim_simplify_factor=0.0 is what keeps a re-export
# bit-comparable in shape with the shipped clip FBXs.
#
# The file is never written back: the action assignment, pose_position and scene frame range are
# restored in memory afterwards, and no .save() call exists anywhere below.
import bpy
import sys

argv = sys.argv[sys.argv.index("--") + 1:]
ACTION, OUT = argv[0], argv[1]
FS, FE = int(argv[2]), int(argv[3])
WITH_MESHES = argv[4] == "1"
# Optional 6th argument: an explicit weapon action, "none", or "auto" (the default).
WEAPON_ARG = argv[5] if len(argv) > 5 else "auto"

# Exactly the meshes export_manifest.json records for the shipped arm clips. Kept so that a
# meshed export is a drop-in match; the ADS clip uses meshes=0 (armature only) so it brings no
# materials along and therefore no extra "Texture: failed to load" lines in the smoke test.
MESH_NAMES = ["SK_Manny_Simple", "SK_Manny_Arms",
              "Quantum_Body_Full", "Quantum_Body_Full.001", "Quantum_Body_Full.002"]

scn = bpy.context.scene
arm = bpy.data.objects.get("Armature")
if arm is None:
    raise SystemExit("no 'Armature' object in the file")

if ACTION not in bpy.data.actions:
    raise SystemExit("no action named %r (have %d)" % (ACTION, len(bpy.data.actions)))

# --- which weapon action must the left hand be baked against? ------------------------------
wep = bpy.data.objects.get("AK")


def derived_weapon_action():
    if ACTION.startswith("A_FP_") and "A_W_" + ACTION[5:] in bpy.data.actions:
        return "A_W_" + ACTION[5:]
    if "A_W_ADS" in bpy.data.actions:
        return "A_W_ADS"
    return None


if WEAPON_ARG == "auto":
    WEAPON_ACTION = derived_weapon_action()
elif WEAPON_ARG == "none":
    WEAPON_ACTION = None
else:
    if WEAPON_ARG not in bpy.data.actions:
        raise SystemExit("no weapon action named %r" % WEAPON_ARG)
    WEAPON_ACTION = WEAPON_ARG
if wep is None and WEAPON_ACTION:
    raise SystemExit("weapon armature 'AK' missing but weapon action %r was requested" % WEAPON_ACTION)

# --- capture state we must put back ---------------------------------------------------------
prev_action = arm.animation_data.action if (arm.animation_data and arm.animation_data.action) else None
prev_pose = arm.data.pose_position
prev_fs, prev_fe = scn.frame_start, scn.frame_end
prev_wep_action = wep.animation_data.action if wep and wep.animation_data and wep.animation_data.action else None
prev_wep_pose = wep.data.pose_position if wep else None
# The AK carries its own NLA stashes; they are muted in the saved file, but pinning them makes
# an export reproducible regardless of how the file was last left, and restores them either way.
prev_wep_nla = [(t, t.mute) for t in wep.animation_data.nla_tracks] if wep and wep.animation_data else []

# --- select just the arms rig (never 'AK', the weapon armature) -----------------------------
targets = [arm]
if WITH_MESHES:
    for n in MESH_NAMES:
        ob = bpy.data.objects.get(n)
        if ob is None:
            raise SystemExit("missing mesh %r" % n)
        targets.append(ob)
for ob in bpy.data.objects:
    ob.select_set(False)
for ob in targets:
    ob.select_set(True)
bpy.context.view_layer.objects.active = arm

# The shipped clips bake the evaluated (constraint-driven) pose, so POSE - not REST - and the
# scene's frame range is narrowed to the clip's own range (the FBX exporter bakes scene
# frame_start..frame_end; there is no range argument on the operator).
arm.data.pose_position = "POSE"
if arm.animation_data is None:
    arm.animation_data_create()
arm.animation_data.action = bpy.data.actions[ACTION]
scn.frame_start, scn.frame_end = FS, FE
scn.frame_set(FS)

# Pair the weapon with the arms clip. The left arm's CHILD_OF constraint reads the weapon
# armature live, so this is what decides where the baked left hand ends up.
if wep is not None:
    if wep.animation_data is None:
        wep.animation_data_create()
    for t, _ in prev_wep_nla:
        t.mute = True
    wep.data.pose_position = "POSE"
    wep.animation_data.action = bpy.data.actions[WEAPON_ACTION] if WEAPON_ACTION else None

ok = bpy.ops.export_scene.fbx(
    filepath=OUT,
    use_selection=True,
    object_types={"ARMATURE", "MESH"} if WITH_MESHES else {"ARMATURE"},
    use_mesh_modifiers=False,
    add_leaf_bones=False,
    # Baked: the actions key only the CB_* control bones (A_FP_ADS touches 27 of them), so a raw
    # fcurve export writes no channel for any deform bone - Blender 5 emits nothing at all with
    # bake_anim=False. Baking samples the evaluated pose instead, which is where the constraint
    # driven deform bones actually get their values.
    #
    # This yields one channel per bone (263 + root) where the shipped clips carry 89 - exactly
    # the rig's deform-bone count. That difference is invisible: only nodes with a vertex group
    # get a BoneId and enter the skin palette, the CB_* control bones are never drawn, and a
    # crossfade against a clip that does not mention a bone falls back to BindTRS.
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
    raise SystemExit("export operator failed: %r" % (ok,))

# --- restore in memory; the .blend on disk is untouched ------------------------------------
arm.animation_data.action = prev_action
arm.data.pose_position = prev_pose
scn.frame_start, scn.frame_end = prev_fs, prev_fe
if wep is not None:
    wep.animation_data.action = prev_wep_action
    wep.data.pose_position = prev_wep_pose
    for t, m in prev_wep_nla:
        t.mute = m

print("EXPORTED action=%s frames=%d..%d meshes=%s -> %s" %
      (ACTION, FS, FE, WITH_MESHES, OUT))
print("  weapon action paired = %s" % (WEAPON_ACTION or "none"))
print("RESTORED action=%s pose_position=%s frames=%d..%d weapon=%s" %
      (prev_action.name if prev_action else None, prev_pose, prev_fs, prev_fe,
       prev_wep_action.name if prev_wep_action else None))
sys.stdout.flush()
