#include "ComponentRegistryInternal.h"

#include "Components.h"
#include "ReflectAttributes.h" // vInspector attributes (Attr)

#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <random>

namespace ComponentRegistry {

namespace {
// The four root-motion fields (RootMotionOptions) starting at `first`: stable keys, the mode's
// labels, one collapsible group, and the options hidden while it's Off. The bone is drawn by a
// custom picker (the model's bones), so it's EditorHidden.
void SetupRootMotionFields(ReflectComponent& m, size_t first) {
    static const char* kModeLabels = "Off\0Apply\0In Place\0";
    static const char* kKeys[] = {"rootMotion", "rootMotionBone", "rootMotionRotation", "rootMotionVertical"};
    for (size_t i = 0; i < 4; ++i) {
        ReflectField& f = m.Fields[first + i];
        f.Key = kKeys[i];
        f.Group = "Root Motion";
        if (i > 0) { f.VisibleIfField = "Root Motion"; f.VisibleIfValue = 0; f.VisibleIfNot = true; }
    }
    m.Fields[first].EnumLabels = kModeLabels;
    m.Fields[first].EnumCount = 3;
    m.Fields[first + 1].EditorHidden = true;
}
} // namespace

// Animation (the legacy Animation component, the Animator Controller, IK rigs, the procedural Animator), the camera, the first-person controller and body, outfits.
void RegisterCharacterComponents() {
    using T = ReflectFieldType;

    // Migrated from hand-coded serialization/Inspector code onto reflection (#184) — the third
    // such migration. Unlike Camera/Audio/Collider (all ruled out: they carry multi-select
    // and/or custom widgets the generic path doesn't support), Animator was plain fields only,
    // so this is a pure move with no functional loss besides the sub-heading grouping the old
    // hand-written section had (see the field-naming note on AnimatorComponent in Components.h).
    // Runtime-only scratch fields (Initialized, Base*, BaseColor, Elapsed) are not reflected.
    // #175 / #113 — skeletal clip playback. Clip is drawn by a custom combo (the model's clip
    // names) in DrawReflectedComponentExtra, so it's EditorHidden here but still serialized.
    {
        static const char* kWrapLabels = "Once\0Loop\0Ping Pong\0Clamp Forever\0";
        ReflectComponent m;
        m.Name = "Animation"; m.Icon = ICON_FA_FILM; m.Category = "Rendering";
        m.Tooltip = "Plays one of this object's model animation clips in Play mode. Game code can switch the clip "
                    "(it crossfades) or stop / start it.";
        // No Key: this component serializes generically under its Name ("Animation"). A Key of
        // "animation" here made the prefab override helpers (which look components up by
        // ReflectComponentKey) search for a block the serializer never writes.
        m.Fields = {
            { "Clip", T::String, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, Clip), 0.0f,
              "Which clip to play (empty = the model's first clip)." },
            { "Play Automatically", T::Bool, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, PlayAutomatically), 0.0f,
              "Start playing as soon as Play mode begins." },
            { "Wrap Mode", T::Enum, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, WrapMode), 0.0f,
              "Once: play to the end, then stop. Loop: repeat. Ping Pong: forwards then backwards.\n"
              "Clamp Forever: play to the end and hold the last frame." },
            { "Speed", T::Float, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, Speed), 0.01f,
              "Playback rate (1 = authored speed; negative plays backwards).", -10.0f, 10.0f },
            { "Cross Fade", T::Float, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, CrossFade), 0.01f,
              "Seconds to blend when the clip changes while playing.", 0.0f, 5.0f },
            { "Root Motion", T::Enum, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, RootMotion.Mode), 0.0f,
              "What happens to the travel the clips author on the root bone.\n"
              "Off: the clips play as authored (a walk drags the mesh away and snaps back each loop).\n"
              "Apply: the travel moves this object instead, and the pose stays in place.\n"
              "In Place: the pose stays in place and the object doesn't move - game code reads the\n"
              "motion and moves it (e.g. through a character controller)." },
            { "Root Motion Bone", T::String, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, RootMotion.Bone), 0.0f,
              "The bone whose travel is the character's (empty = auto: \"root\", else the hips / pelvis)." },
            { "Root Motion Rotation", T::Bool, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, RootMotion.Rotation), 0.0f,
              "Turns in the clips turn the object. Off: the turn stays in the pose." },
            { "Root Motion Vertical", T::Bool, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, RootMotion.Vertical), 0.0f,
              "Height changes in the clips move the object up and down. Off (usual): jumps and crouches\n"
              "stay in the pose, and the object keeps to the ground." },
        };
        m.Fields[0].EditorHidden = true;
        m.Fields[2].EnumLabels = kWrapLabels;
        m.Fields[2].EnumCount = 4;
        SetupRootMotionFields(m, 5);
        Register<SkeletalAnimationComponent>(std::move(m));
    }

    // #175 Part B - the Animator Controller state machine. Controller is an EditorHidden string:
    // the Inspector draws a picker of the project's .controller files plus the controller's
    // states / parameters / transitions editor (EditorLayer_Inspector.cpp, keyed "Animator Controller").
    {
        ReflectComponent m;
        m.Name = "Animator Controller"; m.Icon = ICON_FA_DIAGRAM_PROJECT; m.Category = "Rendering";
        m.Tooltip = "Plays this object's model clips from a state machine (a .controller file): states, and "
                    "crossfaded transitions on parameters that game code sets.";
        m.Fields = {
            { "Controller", T::String, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, Controller), 0.0f,
              "The .controller file this object runs." },
            { "Speed", T::Float, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, Speed), 0.01f,
              "Multiplies every state's playback speed.", 0.0f, 10.0f },
            { "Track", T::String, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, Track), 0.0f,
              "Which of the controller's clip tracks this object plays (empty = the first).\n"
              "A controller can carry several clip sets per state, e.g. \"arms\" and \"weapon\"." },
            { "Root Motion", T::Enum, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, RootMotion.Mode), 0.0f,
              "What happens to the travel the clips author on the root bone.\n"
              "Off: the clips play as authored (a walk drags the mesh away and snaps back each loop).\n"
              "Apply: the travel moves this object instead, and the pose stays in place.\n"
              "In Place: the pose stays in place and the object doesn't move - game code reads the\n"
              "motion and moves it (e.g. through a character controller)." },
            { "Root Motion Bone", T::String, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, RootMotion.Bone), 0.0f,
              "The bone whose travel is the character's (empty = auto: \"root\", else the hips / pelvis)." },
            { "Root Motion Rotation", T::Bool, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, RootMotion.Rotation), 0.0f,
              "Turns in the clips turn the object. Off: the turn stays in the pose." },
            { "Root Motion Vertical", T::Bool, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, RootMotion.Vertical), 0.0f,
              "Height changes in the clips move the object up and down. Off (usual): jumps and crouches\n"
              "stay in the pose, and the object keeps to the ground." },
        };
        m.Fields[0].EditorHidden = true;
        SetupRootMotionFields(m, 3);
        Register<AnimatorControllerComponent>(std::move(m));
    }

    // Procedural IK on top of the Animator Controller's pose (src/Game/IK.h). Runs in Play,
    // after the controller blends its layers and before the pose reaches the renderer.
    {
        ReflectComponent m;
        m.Name = "IK Rig"; m.Icon = ICON_FA_HAND; m.Category = "Rendering";
        m.Tooltip = "Inverse kinematics on this object's animated pose: up to two two-bone limbs (arms, legs)\n"
                    "and a look-at. Needs an Animator Controller on the same object.";
        m.Fields = {
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, Enabled), 0.0f,
              "Run the rig." },
            { "Weight", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, Weight), 0.01f,
              "Blend from the animated pose (0) to the solved one (1).", 0.0f, 1.0f },

            { "Limb A Enabled", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.Enabled), 0.0f, "Solve this limb." },
            { "Limb A Upper", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.Upper), 0.0f, "Upper bone, e.g. upperarm_r." },
            { "Limb A Lower", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.Lower), 0.0f, "Middle bone, e.g. lowerarm_r." },
            { "Limb A End", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.End), 0.0f, "End bone, e.g. hand_r." },
            { "Limb A Target", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.Target), 0.0f, "Bone the end reaches for." },
            { "Limb A Keep Animated Offset", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.KeepAnimatedOffset), 0.0f,
              "Reach for where the end sat relative to Target in the animated pose (hands keep\n"
              "their grip on a procedurally moved gun), instead of for Target itself." },
            { "Limb A Match Rotation", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.MatchRotation), 0.0f,
              "Also turn the end bone to the goal's rotation." },
            { "Limb A Weight", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.Weight), 0.01f, "Limb blend.", 0.0f, 1.0f },
            { "Limb A Pole Bone", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.PoleBone), 0.0f,
              "Bone the elbow/knee points toward (e.g. a hint bone). Empty = the animated elbow's own position." },
            { "Limb A Hint Weight", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.HintWeight), 0.01f,
              "0 = keep the animated bend plane (default); 1 = the elbow/knee sits in the plane of the pole and the root-target line, so it cannot flip when the target crosses the bend plane.", 0.0f, 1.0f },
            { "Limb A Hint Offset", T::Vec3, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.HintOffset), 0.01f,
              "Model-space offset added to the pole point: pushes the elbow/knee out, in or up." },
            { "Limb A Max Limb Scale", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.MaxLimbScale), 0.01f,
              "How far the limb may stretch toward an out-of-reach target, x its length. 1 = never (reaches along the straightened chain).", 1.0f, 2.0f },
            { "Limb A Grip Position", T::Vec3, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.GripPosition), 0.001f,
              "Moves the end's grip in the Target bone's own frame (model units). Zero = the grip as animated." },
            { "Limb A Grip Rotation", T::Vec3, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.GripRotation), 0.5f,
              "Turns the end's grip in the Target bone's frame: pitch, yaw, roll in degrees. Zero = as animated." },

            { "Limb B Enabled", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.Enabled), 0.0f, "Solve this limb." },
            { "Limb B Upper", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.Upper), 0.0f, "Upper bone, e.g. upperarm_l." },
            { "Limb B Lower", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.Lower), 0.0f, "Middle bone, e.g. lowerarm_l." },
            { "Limb B End", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.End), 0.0f, "End bone, e.g. hand_l." },
            { "Limb B Target", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.Target), 0.0f, "Bone the end reaches for." },
            { "Limb B Keep Animated Offset", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.KeepAnimatedOffset), 0.0f,
              "Reach for where the end sat relative to Target in the animated pose, instead of for Target itself." },
            { "Limb B Match Rotation", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.MatchRotation), 0.0f,
              "Also turn the end bone to the goal's rotation." },
            { "Limb B Weight", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.Weight), 0.01f, "Limb blend.", 0.0f, 1.0f },
            { "Limb B Pole Bone", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.PoleBone), 0.0f,
              "Bone the elbow/knee points toward (e.g. a hint bone). Empty = the animated elbow's own position." },
            { "Limb B Hint Weight", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.HintWeight), 0.01f,
              "0 = keep the animated bend plane (default); 1 = the elbow/knee sits in the plane of the pole and the root-target line, so it cannot flip when the target crosses the bend plane.", 0.0f, 1.0f },
            { "Limb B Hint Offset", T::Vec3, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.HintOffset), 0.01f,
              "Model-space offset added to the pole point: pushes the elbow/knee out, in or up." },
            { "Limb B Max Limb Scale", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.MaxLimbScale), 0.01f,
              "How far the limb may stretch toward an out-of-reach target, x its length. 1 = never (reaches along the straightened chain).", 1.0f, 2.0f },
            { "Limb B Grip Position", T::Vec3, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.GripPosition), 0.001f,
              "Moves the end's grip in the Target bone's own frame (model units). Zero = the grip as animated." },
            { "Limb B Grip Rotation", T::Vec3, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.GripRotation), 0.5f,
              "Turns the end's grip in the Target bone's frame: pitch, yaw, roll in degrees. Zero = as animated." },

            { "Look At Enabled", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtEnabled), 0.0f, "Aim a bone at another." },
            { "Look At Bone", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtBone), 0.0f, "The bone that turns, e.g. head." },
            { "Look At Target", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtTarget), 0.0f, "The bone it faces." },
            { "Look At Aim Axis", T::Vec3, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtAxis), 0.01f,
              "The bone's local axis that should point at the target." },
            { "Look At Max Angle", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtMaxAngle), 0.5f,
              "Largest turn, in degrees.", 0.0f, 180.0f },
            { "Look At Weight", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtWeight), 0.01f, "Look-at blend.", 0.0f, 1.0f },
        };
        // Collapsible groups in the Inspector.
        for (size_t i = 0; i < m.Fields.size(); ++i) {
            m.Fields[i].Group = i < 2 ? nullptr : i < 16 ? "Limb A" : i < 30 ? "Limb B" : "Look At";
        }
        Register<IKRigComponent>(std::move(m));
    }

    Register<AnimatorComponent>({
        "Animator", ICON_FA_PERSON_RUNNING,
        "Procedural motion driven every frame in Play mode - continuous spin, orbit around an "
        "axis, vertical bob, and light hue-cycling. All fields are additive and reversible "
        "(turning a rate back to 0 undoes its contribution).",
        "Scripts",
        {
            { "Spin Deg/Sec", T::Vec3, TARTARUS_REFLECT_FIELD(AnimatorComponent, SpinDegPerSec), 1.0f,
              "Continuous local rotation: the object turns about this vector's own direction at its length in degrees per second, so (0,90,0) is a yaw at 90 deg/s and (1,1,0) a spin about that diagonal." },
            { "Orbit Axis", T::Vec3, TARTARUS_REFLECT_FIELD(AnimatorComponent, OrbitAxis), 0.01f,
              "Axis this object revolves around, relative to its base position." },
            { "Orbit Speed", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, OrbitDegPerSec), 0.5f,
              "Revolution rate around the orbit axis, in degrees/second." },
            { "Orbit Radius", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, OrbitRadius), 0.05f,
              "Distance from the base position while orbiting, in world units." },
            { "Bob Amplitude", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, BobAmplitude), 0.01f,
              "Vertical sine offset from the base position, in world units." },
            { "Bob Frequency Hz", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, BobFreqHz), 0.02f,
              "Bob rate in Hz (cycles/second)." },
            { "Color Cycle Hz", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, ColorCycleHzPerSec), 0.01f,
              "Hue revolutions/second for this object's Light color. 0 leaves the color alone. "
              "Has no effect without a Light component." },
        },
    });

    // Migrated from hand-coded serialization/Inspector code onto reflection (#302 Wave 1a). The
    // three clip-plane / FOV fields are plain floats and multi-select already works generically
    // (#235); the one Camera-specific control, the "Align to View" button (it reads the editor
    // camera and writes this entity's TransformComponent), stays editor-side as a registered
    // inspector-extra keyed "Camera" (EditorLayer_Inspector.cpp), since reflection describes data
    // only and the editor camera isn't visible from TartarusGame.dll.
    Register<CameraComponent>({
        "Camera", ICON_FA_VIDEO,
        "The Game view renders through this camera while editing, so you can frame a shot "
        "without walking there. In Play it's the game camera too, unless the scene has a "
        "First Person Controller.",
        "Rendering",
        {
            { "Field of View", T::Float, TARTARUS_REFLECT_FIELD(CameraComponent, FovDegrees), 0.25f,
              "Vertical field of view, in degrees.", 1.0f, 179.0f },
            { "Near", T::Float, TARTARUS_REFLECT_FIELD(CameraComponent, NearPlane), 0.01f,
              "Closest distance the camera renders.", 0.001f, 100.0f },
            { "Far", T::Float, TARTARUS_REFLECT_FIELD(CameraComponent, FarPlane), 1.0f,
              "Farthest distance the camera renders.", 0.1f, 100000.0f },
        },
    });

    Register<FirstPersonControllerComponent>({
        "First Person Controller", ICON_FA_PERSON_WALKING,
        "Play spawns the first-person player here (feet at this position, facing this object's "
        "forward) with the settings below. WASD to move, Shift to sprint, Space to jump.",
        "Gameplay",
        {
            { "Primary Weapon Prefab", T::String, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, PrimaryWeaponPrefab), 0.0f,
              "Weapon prefab for slot 0. Its Weapon Definition supplies the animation set; takes priority over Animation Set." },
            { "Secondary Weapon Prefab", T::String, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, SecondaryWeaponPrefab), 0.0f,
              "Weapon prefab for slot 1 (key 2). Its Weapon Definition supplies the animation set." },
            { "Sidearm Weapon Prefab", T::String, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, SidearmWeaponPrefab), 0.0f,
              "Weapon prefab for the sidearm slot (key 3). Its Weapon Definition supplies the animation set." },
            { "Move Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, MoveSpeed), 0.05f,
              "Walking speed, metres per second.", 0.0f, 100.0f },
            { "Sprint Multiplier", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, SprintMultiplier), 0.01f,
              "Speed multiplier while Shift is held.", 1.0f, 10.0f },
            { "Jump Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, JumpSpeed), 0.05f,
              "Upward launch speed. Jump height is about Jump Speed^2 / (2 x Gravity).", 0.0f, 50.0f },
            { "Jump Buffer Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, JumpBufferTime), 0.005f,
              "A jump pressed this many seconds before landing still happens the moment you land.", 0.0f, 0.5f },
            { "Coyote Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, CoyoteTime), 0.005f,
              "A jump pressed this many seconds after stepping off an edge still counts.", 0.0f, 0.5f },
            { "Ground Accel Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, GroundAccelTime), 0.005f,
              "How quickly the player gets up to speed on the ground: the time constant, in seconds (about 2.3x\n"
              "this reaches 90% of full speed). 0 = instantly.", 0.0f, 1.0f },
            { "Ground Decel Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, GroundDecelTime), 0.005f,
              "... and slows down on letting go. 0 = stops dead.", 0.0f, 1.0f },
            { "Air Accel Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, AirAccelTime), 0.005f,
              "How quickly the input steers the player in the air. Higher keeps more of the jump's momentum.\n"
              "0 = full air control.", 0.0f, 5.0f },
            { "Doorway Assist", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, DoorwayAssist), 0.01f,
              "Clipping a doorway's jamb while walking into it eases the player sideways past it, up to this much of the\n"
              "capsule's radius. 0 = off.", 0.0f, 1.0f },
            { "Doorway Assist Max Angle", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, DoorwayAssistMaxAngle), 0.5f,
              "... only when the jamb is met within this many degrees of head on (sliding along a wall never counts).", 0.0f, 80.0f },
            { "Eye Height", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, EyeHeight), 0.01f,
              "Camera height above the feet.", 0.1f, 10.0f },
            { "Capsule Radius", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, CapsuleRadius), 0.01f,
              "Collision capsule radius.", 0.05f, 5.0f },
            { "Capsule Height", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, CapsuleHeight), 0.01f,
              "Total collision capsule height, feet to head.", 0.2f, 10.0f },
            { "Mouse Sensitivity", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, MouseSensitivity), 0.005f,
              "Degrees of turn per pixel of mouse movement.", 0.01f, 1.0f },
            { "Invert Y", T::Bool, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, InvertY), 0.0f,
              "Moving the mouse up looks down." },
            { "Stick Look Deg/Sec", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, StickLookDegPerSec), 5.0f,
              "Gamepad right stick: turn rate in degrees per second (not a delta).", 1.0f, 720.0f },
            { "Eye Radius", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, EyeRadius), 0.001f,
              "Camera lean collision: sphere radius used for wall detection, metres (keeps the near plane off the wall).", 0.01f, 1.0f },
            { "Field of View", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, FieldOfView), 0.25f,
              "Horizontal field of view in degrees, measured on a 16:9 screen (90 = the usual shooter FOV). "
              "Wider screens see more at the sides.", 30.0f, 150.0f },
            { "Kill Height", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, KillY), 0.5f,
              "Falling below this world height respawns the player at this object.", -100000.0f, 100000.0f },
            { "Gravity", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, Gravity), 0.1f,
              "How hard the player falls, m/s\xC2\xB2. Separate from the physics world's gravity (Project\n"
              "Settings > Physics, Earth's 9.81 by default): most games give the player a heavier,\n"
              "snappier fall than real life. Jump height is about Jump Speed\xC2\xB2 / (2 x Gravity).", 0.0f, 200.0f },
            { "Gravity Gun", T::Bool, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, GravityGun), 0.0f,
              "The built-in tool: right mouse picks up a rigidbody, left mouse throws it." },
            { "Min Throw Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, MinThrowSpeed), 0.1f,
              "Gravity gun: launch speed (m/s) of a quick click. Hold left mouse to charge up.", 0.0f, 200.0f },
            { "Max Throw Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, MaxThrowSpeed), 0.1f,
              "Gravity gun: launch speed (m/s) when fully charged.", 0.0f, 200.0f },
            { "Throw Charge Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ThrowChargeTime), 0.01f,
              "Gravity gun: seconds of holding left mouse to reach Max Throw Speed.", 0.05f, 10.0f },
            { "Throw Backspin", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ThrowBackspin), 0.05f,
              "Gravity gun: backspin (revolutions per second) put on a thrown ball, like a real shot.\n"
              "Only round (sphere collider) bodies get it.", 0.0f, 20.0f },
            { "Grab Range", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, GrabRange), 1.0f,
              "Gravity gun: aiming distance for the primary pick-up ray, metres.", 10.0f, 1000.0f },
            { "Assist Range", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, AssistRange), 1.0f,
              "Gravity gun: search radius when no object is under the exact crosshair, metres.", 1.0f, 500.0f },
            { "Assist Cone Deg", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, AssistConeDeg), 0.5f,
              "Gravity gun: within this many degrees of the crosshair during aim assist, degrees.", 1.0f, 45.0f },
            { "Scroll Turn Deg", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ScrollTurnDeg), 0.5f,
              "Gravity gun: rotation applied per scroll notch while holding an object, degrees.", 1.0f, 90.0f },
            { "Animation Set", T::String, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, AnimationSet), 0.0f,
              "Optional .fpsanim asset for a camera-bound first-person arms and weapon presentation." },
            { "Secondary Animation Set", T::String, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, SecondaryAnimationSet), 0.0f,
              "Optional second weapon (.fpsanim). In Play, 1 draws the Animation Set, 2 this one, 3 the sidearm and\n"
              "4 goes unarmed; switching holsters the weapon in hand first, and each weapon keeps its own ammo." },
            { "Sidearm Animation Set", T::String, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, SidearmAnimationSet), 0.0f,
              "Optional sidearm (.fpsanim), drawn with 3. A Sidearm Weapon Prefab takes priority." },
            { "View Model Offset", T::Vec3, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ViewModelOffset), 0.01f,
              "Residual nudge, in the play camera's frame, applied on top of the Camera Bone\n"
              "anchor. Leave at zero unless you are deliberately nudging the view model." },
            { "Camera Bone", T::String, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, CameraBone), 0.0f,
              "Rig bone the play camera sits on (\"head\" by default). The view model is placed so\n"
              "this bone lands exactly on the camera. Empty puts the model's root there instead." },
            { "View Model Rotation", T::Vec3, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ViewModelRotation), 0.5f,
              "Euler rotation offset, in degrees, applied after the play camera orientation." },
            { "View Model Scale", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ViewModelScale), 0.01f,
              "Scale of the first-person arms and weapon presentation.", 0.01f, 100.0f },
            { "View Model FOV", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ViewModelFov), 0.25f,
              "Vertical FOV, in degrees, the first-person arms and weapon are projected with in the\n"
              "renderer's view-model pass (drawn after a depth clear, so the world can't clip them).\n"
              "Independent of the world camera's FOV: this changes how the held weapon is framed,\n"
              "never the scene behind it. Narrower than the world FOV by default, which is what\n"
              "keeps the weapon reading as held instead of stretched.", 20.0f, 150.0f },
            { "Max Health", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, MaxHealth), 1.0f,
              "The player's health. Enemy rounds take it; at 0 the player dies and respawns here.", 1.0f, 100000.0f },
            { "Regen Delay", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, RegenDelay), 0.05f,
              "Seconds without being hit before health starts coming back.", 0.0f, 120.0f },
            { "Regen Rate", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, RegenRate), 0.5f,
              "Health per second coming back after Regen Delay. 0 = no regeneration.", 0.0f, 10000.0f },
            { "Respawn Delay", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, RespawnDelay), 0.05f,
              "Seconds from death to respawning here.", 0.0f, 60.0f },
            { "Spawn Protection", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, SpawnProtection), 0.05f,
              "Seconds after a respawn when nothing can hurt the player.", 0.0f, 60.0f },
        },
    });

    // True FPS (#405) - the player's own body under the play camera, walked by root motion.
    {
        ReflectComponent m;
        m.Name = "First Person Body"; m.Icon = ICON_FA_PERSON; m.Category = "Gameplay";
        m.Tooltip = "The player's own body in Play: it stands at the First Person Controller's feet facing the\n"
                    "view, motion matching poses it from the player's movement, and the camera sits in its head.\n"
                    "Put it on the body's root; its children are the pieces (head, torso, legs, feet or clothing);\n"
                    "motion matching poses the one with the whole skeleton (the torso), the rest follow it.";
        m.Fields = {
            { "MM Post Layers", T::Bool, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, MmPostLayers), 0.0f,
              "Temporary A/B switch (motion matching): off draws the matcher's pose with foot IK only - no Spine Aim /\n"
              "Twist, arm hang or hands-from-eye. Flip it in Play.", 0.0f, 0.0f },
            { "MM Head Ride", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, MmHeadRide), 0.01f,
              "Motion matching, unarmed: how much of the mocap head's own motion the camera rides. 0 = its slow average\n"
              "(the stabilized camera, within Eye Slack); 1 = the head as captured (the jog's bob and sway). Live in Play.", 0.0f, 1.0f },
            { "Walk Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, WalkSpeed), 0.01f,
              "Metres per second asked of the blend tree when walking - the default gait. Match the walk clips (1.53).", 0.0f, 20.0f },
            { "Run Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, RunSpeed), 0.01f,
              "Metres per second asked of the blend tree with Shift held (the jog). Match a clip's speed (the jog, 3.26).", 0.0f, 20.0f },
            { "Sprint Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, SprintSpeed), 0.01f,
              "... and while sprinting (the run clip, 4.72).", 0.0f, 20.0f },
            { "Player Run Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, PlayerRunSpeed), 0.01f,
              "The player's own run speed (m/s) when it should be faster than the clips travel. 0 = Run Speed.", 0.0f, 20.0f },
            { "Head Bone", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, HeadBone), 0.0f,
              "The bone the camera sits on." },
            { "Camera Offset", T::Vec3, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, CameraOffset), 0.005f,
              "The eyes from the head bone, in the body's frame (X right, Y up, Z forward), metres." },
            { "Armed Eye Offset", T::Vec3, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmedEyeOffset), 0.005f,
              "With the gun out, the eye goes where the weapon's arms rig has its camera against its shoulders, which is\n"
              "low (a first-person rig's camera sits just over its shoulders). This lifts it back toward the eyes (body\n"
              "frame: X right, Y up, Z forward, metres) so the chest and collar stay out of view. The gun doesn't move on\n"
              "screen; the body sits lower under it, and the hands reach further. Too much and a hand comes off the gun.\n"
              "Applies near level; it eases out looking up or down (gone by 60 degrees), where it isn't needed." },
            { "Hidden Parts", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, HiddenParts), 0.0f,
              "Body pieces that cast a shadow but aren't drawn in first person, comma separated: a child\n"
              "is hidden when its name contains one of these. The camera sits in the head." },
            { "Hidden Bones", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, HiddenBones), 0.0f,
              "Bones collapsed in Play, comma separated (empty = none) - e.g. the arms of a one-piece\n"
              "body while a first-person arms model draws them." },
        };
        m.Fields.push_back({ "Weapon Arms", T::Bool, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, WeaponArms), 0.0f,
              "The body's arms hold the weapon: they take the first-person arms' pose and reach their\n"
              "hands onto theirs, and the separate arms model stops being drawn (its gun still is)." });
        m.Fields.push_back({ "Spine Aim", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, SpineAim), 0.01f,
              "How much of the camera's pitch the spine takes: 0 = upright, 1 = the chest tilts as far\n"
              "as the view. The shoulders follow the view, so the hands stay in reach.", 0.0f, 1.0f });
        m.Fields.push_back({ "Spine Aim Down", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, SpineAimDown), 0.01f,
              "Spine Aim for looking down. Armed, the camera hangs off the shoulders as the arms rig's does, so it clears the\n"
              "chest looking down only as far as the chest pitches with the view: lower it and the torso may be seen from inside.", 0.0f, 1.0f });
        m.Fields.push_back({ "Spine Aim Max Up", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, SpineAimMaxUp), 0.5f,
              "Unarmed: the most (degrees) the chest pitches back with the view looking up, eased in. Past it only the head\n"
              "looks further. 0 = no limit. (Armed the arms need the full aim to reach the gun.)", 0.0f, 90.0f });
        m.Fields.push_back({ "Spine Aim Max Down", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, SpineAimMaxDown), 0.5f,
              "Unarmed: the most (degrees) the chest pitches forward with the view looking down, eased in. 0 = no limit.", 0.0f, 90.0f });
        m.Fields.push_back({ "Spine Weight 1", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Spine.Weight[0]), 0.05f,
              "Relative share of the spine's look, twist and shoulder turns that spine_01 takes. All equal = the even spread; 0 = it takes none.", 0.0f, 4.0f });
        m.Fields.back().Group = "Spine Distribution";
        m.Fields.push_back({ "Spine Max Angle 1", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Spine.MaxAngle[0]), 0.5f,
              "Most (degrees) spine_01 turns from one spread. 0 = no limit.", 0.0f, 90.0f });
        m.Fields.back().Group = "Spine Distribution";
        m.Fields.push_back({ "Spine Weight 2", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Spine.Weight[1]), 0.05f,
              "Relative share of the spine's look, twist and shoulder turns that spine_02 takes. All equal = the even spread; 0 = it takes none.", 0.0f, 4.0f });
        m.Fields.back().Group = "Spine Distribution";
        m.Fields.push_back({ "Spine Max Angle 2", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Spine.MaxAngle[1]), 0.5f,
              "Most (degrees) spine_02 turns from one spread. 0 = no limit.", 0.0f, 90.0f });
        m.Fields.back().Group = "Spine Distribution";
        m.Fields.push_back({ "Spine Weight 3", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Spine.Weight[2]), 0.05f,
              "Relative share of the spine's look, twist and shoulder turns that spine_03 takes. All equal = the even spread; 0 = it takes none.", 0.0f, 4.0f });
        m.Fields.back().Group = "Spine Distribution";
        m.Fields.push_back({ "Spine Max Angle 3", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Spine.MaxAngle[2]), 0.5f,
              "Most (degrees) spine_03 turns from one spread. 0 = no limit.", 0.0f, 90.0f });
        m.Fields.back().Group = "Spine Distribution";
        m.Fields.push_back({ "Spine Weight 4", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Spine.Weight[3]), 0.05f,
              "Relative share of the spine's look, twist and shoulder turns that spine_04 takes. All equal = the even spread; 0 = it takes none.", 0.0f, 4.0f });
        m.Fields.back().Group = "Spine Distribution";
        m.Fields.push_back({ "Spine Max Angle 4", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Spine.MaxAngle[3]), 0.5f,
              "Most (degrees) spine_04 turns from one spread. 0 = no limit.", 0.0f, 90.0f });
        m.Fields.back().Group = "Spine Distribution";
        m.Fields.push_back({ "Spine Weight 5", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Spine.Weight[4]), 0.05f,
              "Relative share of the spine's look, twist and shoulder turns that spine_05 takes. All equal = the even spread; 0 = it takes none.", 0.0f, 4.0f });
        m.Fields.back().Group = "Spine Distribution";
        m.Fields.push_back({ "Spine Max Angle 5", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Spine.MaxAngle[4]), 0.5f,
              "Most (degrees) spine_05 turns from one spread. 0 = no limit.", 0.0f, 90.0f });
        m.Fields.back().Group = "Spine Distribution";
        m.Fields.push_back({ "Pelvis Alpha", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Spine.PelvisAlpha), 0.01f,
              "Share (0..1) of the spine's turn the pelvis takes; the spine takes the rest, so the chest still turns as far.", 0.0f, 1.0f });
        m.Fields.back().Group = "Spine Distribution";
        m.Fields.push_back({ "Shoulder Line Match", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ShoulderLineMatch), 0.01f,
              "Armed, how far the chest takes the arms rig's stance - its shoulder line (bladed, the support shoulder forward) -\n"
              "instead of squaring to the view. 1 = the rig's: both hands then reach the gun without the shoulders moving.", 0.0f, 1.0f });
        m.Fields.push_back({ "Bone Map", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, BoneMap), 0.0f,
              "For a rig whose bones are not named like the UE5 mannequin: 'standard = theirs', comma or line\n"
              "separated, e.g. pelvis = Hips, foot_l = LeftFoot, foot_r = RightFoot. Unlisted bones keep the\n"
              "standard name. Weapon Arms still needs the body and the arms rig to share names." });
        m.Fields.push_back({ "Arms Piece", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmsPiece), 0.0f,
              "The child piece that is the body's arms (its name contains this). With Weapon Arms it is\n"
              "drawn with the gun, in the view-model pass, so its hands sit exactly where the rig's do." });
        m.Fields.push_back({ "Hands Piece", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, HandsPiece), 0.0f,
              "The child piece that is the body's hands, on a body whose hands are a piece of their own (a long\n"
              "sleeve hides the arms piece and leaves this one). It goes wherever the arms piece goes." });
        m.Fields.push_back({ "Turn Threshold", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, TurnThreshold), 1.0f,
              "Standing still, the body keeps its heading until the view is this many degrees off it,\n"
              "then turns on the spot with the turn clips. 0 = the body always faces the view.", 0.0f, 170.0f });
        m.Fields.push_back({ "Spine Twist", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, SpineTwist), 0.01f,
              "How much of the view's twist off the body's heading the spine takes: the chest faces the\n"
              "view while the feet have not turned yet.", 0.0f, 1.0f });
        m.Fields.push_back({ "Max Turn Rate", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, MaxTurnRate), 5.0f,
              "Standing still, the view turns no faster than this (degrees a second) past the Turn Threshold, where the feet must step round: what\n"
              "the turn clips can keep up with, so the feet don't slide. 0 = no limit.", 0.0f, 720.0f });
        m.Fields.push_back({ "Crouch Height", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, CrouchHeight), 0.01f,
              "The capsule's height while crouching (hold Crouch, Left Ctrl by default). 0 = no crouching.", 0.0f, 3.0f });
        m.Fields.push_back({ "Crouch Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, CrouchSpeed), 0.01f,
              "The move speed while crouched, as a fraction of Run Speed. The crouch-walk clips travel\n"
              "about 1.35 m/s, so Run Speed x this should be near that or the feet slide.", 0.05f, 1.0f });
        m.Fields.push_back({ "Foot IK", T::Bool, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootIK), 0.0f,
              "Each foot is put on the ground under it, the pelvis drops to the lower foot and the legs are re-solved, so the feet meet stairs and slopes." });
        m.Fields.push_back({ "Foot IK Max Drop", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootIKMaxDrop), 0.01f,
              "The most the pelvis may drop (metres) to let the lower foot reach the ground.", 0.0f, 1.0f });
        m.Fields.push_back({ "Eye Slack", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, EyeSlack), 0.001f,
              "How far (metres) the camera may trail or lead the body's shoulders. A clip that throws the shoulders about (a stop pulling the body back)\nwould otherwise carry them out of the arms' reach and a hand would come off the gun. Bigger motions move the camera with them.", 0.0f, 0.2f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Reach Slack", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ReachSlack), 0.001f,
              "The body's shoulders sit this much (metres) further toward the gun than the arms rig's, so the support arm is never at full stretch\n(a straight arm jumps at every small motion).", 0.0f, 0.2f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Shrug Start", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ShrugStart), 0.01f,
              "A shoulder shrugs toward the gun once its hand is this fraction of the arm's length away, instead of the arm stretching.", 0.5f, 1.2f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Shrug Max", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ShrugMax), 0.005f,
              "The most (metres) a shoulder may shrug toward the gun.", 0.0f, 0.4f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Shoulder Max Angle", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ShoulderMaxAngle), 0.5f,
              "The most (degrees) a collarbone turns to move its shoulder, shrug and steadying together. The shoulder turns about the\n"
              "collarbone's inner end, like a real one; past this the arm straightens instead.", 0.0f, 60.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Clavicle Follow", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ClavicleFollow), 0.01f,
              "How much of the arms rig's collarbone motion the body takes (its arms and its chest's stance). The rig has no torso or\n"
              "head in the way, so a reload swings its collarbones freely: taken whole, the body's shoulder hunched up at the camera.\n"
              "1 = all of it, 0 = the body's own pose. The hands stay on the gun either way.", 0.0f, 1.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Reach Lean Max", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ReachLeanMax), 0.5f,
              "The most (degrees) the chest leans toward a hand still out of reach once its collarbone has turned all it may\n"
              "(looking far up or down, the gun moves further than the chest). 0 = off.", 0.0f, 45.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Arm Steadiness", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmSteadiness), 0.01f,
              "How much the arms ignore the body's walk and run sway (Weapon Arms): 1 = the shoulders are held steady against the view and the\n"
              "elbows bend the way the rig's do, so the gait never reaches the elbows; 0 = the arms follow the chest.", 0.0f, 1.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Arm Steady Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmSteadyTime), 0.01f,
              "Seconds the steadied shoulders take to follow a lasting change (a crouch, the view pitching). Longer than a step so the sway is filtered out.", 0.05f, 2.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Arm Steady Max", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmSteadyMax), 0.005f,
              "Metres a shoulder is held off where the body's pose puts it in full; past this the hold eases off toward twice it (it never stops dead).", 0.0f, 0.3f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Look Down Push", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, LookDownPush), 0.005f,
              "Looking down, the eye comes this far (metres) forward over the chest. It moves the gun with it, away from the shoulders,\n"
              "so it costs the arms their reach - prefer Spine Aim Down. Straight down gets all of it. 0 = off.", 0.0f, 0.4f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Look Down Start", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, LookDownStart), 1.0f,
              "Degrees below level where the look-down push begins; it eases in from here to straight down.", 0.0f, 85.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Camera Hidden Bones", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, CameraHiddenBones), 0.0f,
              "Bones whose skin isn't drawn in the camera's own view on the pieces other than the arms, comma\n"
              "separated (at most 8): the torso's shoulders, which move with the arms' so the arms stay on them\n"
              "in every other view - but would bulge into the camera, where the arms are drawn over them." });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Near Hide", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NearHide), 0.005f,
              "Whatever of the body comes within this many metres of the eye is not drawn in the camera's view (its shadow stays). The camera\n"
              "sits in the body, and the near plane would otherwise slice the neck and shoulders into slivers. 0 = off.", 0.0f, 0.3f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Clothing Near Hide", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ClothingNearHide), 0.005f,
              "Near Hide for clothing (outfit pieces that aren't body parts), above, below and ahead of the eye - never less than Near Hide.\n"
              "Clothing is bulkier than skin and swings with the walk: a hood's rim or a jacket's shoulder would sweep across the view.", 0.0f, 0.4f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Clothing Near Hide Width", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ClothingNearHideWidth), 0.005f,
              "How far to either side of the eye clothing is hidden (metres): the shoulders' cloth sits off to the sides, at the\n"
              "view's edges, while the chest seen looking down is below. 0 = as far as Clothing Near Hide.", 0.0f, 0.5f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Collar Hide Drop", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, CollarHideDrop), 0.005f,
              "Clothing that sits around the neck in its bind pose - between the shoulder joints and no lower than this many metres\n"
              "below them - isn't drawn in the camera's view, whatever bones the pack skinned it to (a hood or a scarf on the chest).\n"
              "Up hides more of the collar; below 0 = off.", -0.01f, 0.2f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Arms Ease Out", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmsEaseOut), 0.005f,
              "Seconds the body's arms take to ease back to the locomotion pose once the gun is holstered.", 0.01f, 1.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Turn End Angle", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, TurnEndAngle), 0.5f,
              "A turn on the spot ends once the body is within this many degrees of the view.", 0.0f, 45.0f });
        m.Fields.back().Group = "Turning (advanced)";
        m.Fields.push_back({ "Foot Lock", T::Bool, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootLockEnabled), 0.0f,
              "A planted foot is held where it landed in the world (the player body and the soldiers), so the feet never skate when the clips and the capsule disagree - in a turn on the spot, a start or a stop as well as the walk.", 0.0f, 0.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Lock Height", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootLockHeight), 0.002f,
              "A foot plants once it is within this (metres) of its own lowest point...", 0.0f, 0.2f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Lock Lift", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootLockLift), 0.002f,
              "... and lets go once it rises this far (metres) over it (the heel rolling up onto the toe stays planted under this).", 0.0f, 0.3f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Lock Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootLockSpeed), 0.05f,
              "A foot only plants while its animation moves over the ground slower than this (m/s) - the body's own turn left out, so a foot the clip holds still plants however fast the body swings round: a swinging foot passing low never does.", 0.0f, 5.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Lock Drift", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootLockDrift), 0.005f,
              "A planted foot lets go and steps once its leg would need more than 98% of its length to stay on it, or - a safety - when the animation has wandered this far (metres) from it; it then catches up over Foot Lock Release.", 0.01f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Lock Release", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootLockRelease), 0.005f,
              "Seconds a let-go foot takes to catch up with its animation (eased, so it never pops).", 0.005f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Ray Up", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootRayUp), 0.05f,
              "The ground ray under each foot starts this far (metres) above the animated foot.", 0.0f, 2.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Ray Length", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootRayLength), 0.05f,
              "... and reaches this far (metres) down.", 0.1f, 3.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Max Raise", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootMaxRaise), 0.01f,
              "The most (metres) a foot is lifted to meet ground higher than the capsule's.", 0.0f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Pelvis Max Raise", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, PelvisMaxRaise), 0.01f,
              "The most (metres) the pelvis may rise when both feet are on higher ground. 0 = never: the legs reach for the step instead.", 0.0f, 0.5f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Offset Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootOffsetEase), 0.005f,
              "Seconds the ground height under each foot takes to follow the ray.", 0.005f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Normal Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootNormalEase), 0.005f,
              "Seconds the ground's slope under each foot takes to follow the ray.", 0.005f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot IK Fade", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootIKFade), 0.005f,
              "Seconds foot IK takes to fade in and out (it lets go in the air).", 0.005f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Tilt Max", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootTiltMax), 1.0f,
              "The most (degrees) a planted foot tilts to lie on a slope.", 0.0f, 60.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Stair Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StairEase), 0.005f,
              "How softly (seconds) the body - and the camera on it - takes stairs: it follows the ground's ramp under the travel, and its hips the lower foot, on a smooth spring (0.03 s when Foot IK is off).", 0.005f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Elbow Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ElbowEase), 0.005f,
              "Seconds the player body's elbow takes to follow its target direction (smooths elbow jitter).", 0.005f, 1.0f });
        m.Fields.back().Group = "Arms";
        m.Fields.push_back({ "Elbow Max Rate", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ElbowMaxRate), 10.0f,
              "The fastest (degrees per second) a player-body elbow may swing toward its target.", 30.0f, 3600.0f });
        m.Fields.back().Group = "Arms";
        m.Fields.push_back({ "NPC Turn Threshold", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcTurnThreshold), 1.0f,
              "NPCs: a still body this many degrees off its aim turns on the spot. NPCs copy this from the scene's player body when Play starts.", 0.0f, 180.0f });
        m.Fields.back().Group = "NPC Body";
        m.Fields.push_back({ "NPC Face Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFaceEase), 0.005f,
              "NPCs: seconds the body heading takes to ease toward the movement direction while walking.", 0.005f, 1.0f });
        m.Fields.back().Group = "NPC Body";
        m.Fields.push_back({ "NPC Head Max Yaw", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcHeadMaxYaw), 1.0f,
              "NPCs: the most (degrees) the head turns left or right past the chest to look at a target.", 0.0f, 120.0f });
        m.Fields.back().Group = "NPC Head";
        m.Fields.push_back({ "NPC Head Max Pitch", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcHeadMaxPitch), 1.0f,
              "NPCs: the most (degrees) the head nods up or down to look at a target.", 0.0f, 90.0f });
        m.Fields.back().Group = "NPC Head";
        m.Fields.push_back({ "NPC Foot IK Max Drop", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootIKMaxDrop), 0.01f,
              "NPCs: the most (metres) the pelvis drops to let the lower foot reach the ground.", 0.0f, 1.0f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot IK Max Raise", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootIKMaxRaise), 0.01f,
              "NPCs: the most (metres) a foot is lifted to meet higher ground.", 0.0f, 1.0f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot IK Pelvis Raise", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootIKPelvisRaise), 0.005f,
              "NPCs: the most (metres) the pelvis may rise when both feet are on higher ground.", 0.0f, 0.5f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot IK Tilt Max", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootIKTiltMax), 1.0f,
              "NPCs: the most (degrees) a planted foot tilts to lie on a slope.", 0.0f, 60.0f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot Offset Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootOffsetEase), 0.005f,
              "NPCs: seconds the ground height under each foot takes to follow the ray.", 0.005f, 1.0f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot Normal Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootNormalEase), 0.005f,
              "NPCs: seconds the ground's slope under each foot takes to follow the ray.", 0.005f, 1.0f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot IK Fade", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootIKFade), 0.005f,
              "NPCs: seconds foot IK takes to fade in and out.", 0.005f, 1.0f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "Grip Inset", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, GripInset), 0.001f,
              "Metres each hand moves toward its palm while holding a gun: for a body with no flesh on its hands (a skeleton),\n"
              "whose bones sit short of where the rig's skinned palm meets the gun. 0 for a skinned body.", 0.0f, 0.05f });
        m.Fields.push_back({ "Finger Curl Extra", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FingerCurlExtra), 0.5f,
              "Degrees each finger joint closes beyond the rig's grip: bare finger bones need more curl than skinned fingers\n"
              "to reach the gun. 0 for a skinned body.", 0.0f, 45.0f });
        // Groups, by name (not by position: reordering fields must not move them).
        {
            static const std::pair<const char*, const char*> kGroups[] = {
                {"Head Bone", "Camera"}, {"Camera Offset", "Camera"}, {"Armed Eye Offset", "Camera"},
                {"Weapon Arms", "Arms"}, {"Spine Aim", "Arms"}, {"Spine Aim Down", "Arms"}, {"Spine Aim Max Up", "Arms"}, {"Spine Aim Max Down", "Arms"}, {"Shoulder Line Match", "Arms"}, {"Arms Piece", "Arms"}, {"Hands Piece", "Arms"}, {"Grip Inset", "Arms"}, {"Finger Curl Extra", "Arms"},
                {"Turn Threshold", "Turning"}, {"Spine Twist", "Turning"}, {"Max Turn Rate", "Turning"},
                {"Crouch Height", "Crouch"}, {"Crouch Speed", "Crouch"},
                {"Foot IK", "Foot IK"}, {"Foot IK Max Drop", "Foot IK"}, {"Bone Map", "Camera"},
            };
            for (auto& f : m.Fields)
                for (const auto& [name, group] : kGroups)
                    if (std::strcmp(f.Name, name) == 0) f.Group = group;
        }
        Register<FirstPersonBodyComponent>(std::move(m));
    }

    // Character outfits (docs/CHARACTER_OUTFITS.md): the Inspector's outfit editor draws the choices.
    {
        ReflectComponent m;
        m.Name = "Character Outfit"; m.Icon = ICON_FA_PERSON_DRESS; m.Category = "Gameplay";
        m.Tooltip = "Dresses a modular character from a wardrobe: body, gender, race, hair and clothes.\n"
                    "Put it on the body's root. The outfit is its children with an Outfit Piece; the\n"
                    "editor below builds and swaps them. Works with First Person Body on the same object.";
        m.Fields = {
            { "Wardrobe", T::String, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, Wardrobe), 0.0f,
              "The pack's .wardrobe file: its bodies, races, slots and pairing rules." },
            { "Gender", T::Enum, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, Gender), 0.0f, "The body's gender." },
            { "Race", T::String, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, Race), 0.0f, "The head and skin." },
            { "Locks", T::String, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, Locks), 0.0f,
              "Slots Randomize leaves alone, comma separated." },
            { "Auto Hide Skin", T::Bool, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, AutoHide), 0.0f,
              "Skin covered by clothing (and a shirt under a jacket) isn't drawn, so it can't poke\n"
              "through as the body moves. Worked out from the models once per outfit change." },
            { "Randomize On Play", T::Bool, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, RandomizeOnPlay), 0.0f,
              "Every Play starts in a new random outfit (Locks kept). Stop puts the edited one back." },
            { "Bare Body (debug)", T::Bool, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, BareBody), 0.0f,
              "Debug: Play leaves the clothing off - only the character's own body parts are drawn - so the\n"
              "animation and skinning can be seen. Stop restores it." },
        };
        m.Fields[0].AssetPath = true;
        m.Fields[1].EnumLabels = "Male\0Female\0"; m.Fields[1].EnumCount = 2;
        for (int i = 1; i < 4; ++i) m.Fields[i].EditorHidden = true;
        Register<CharacterOutfitComponent>(std::move(m));
    }
    {
        ReflectComponent m;
        m.Name = "Outfit Piece"; m.Icon = ICON_FA_SHIRT; m.Category = "Gameplay";
        m.Tooltip = "One piece of the parent's Character Outfit (a body part or an item). Managed by the\n"
                    "parent's outfit editor; remove it to take the object out of the outfit.";
        m.Fields = {
            { "Slot", T::String, TARTARUS_REFLECT_FIELD(OutfitPieceComponent, Slot), 0.0f, "Where it's worn." },
            { "Item", T::String, TARTARUS_REFLECT_FIELD(OutfitPieceComponent, Item), 0.0f, "The model it was built from." },
            { "Flags", T::Int, TARTARUS_REFLECT_FIELD(OutfitPieceComponent, Flags), 0.0f, "Body part / head attached." },
        };
        m.Fields[1].AssetPath = true;
        m.Fields[2].EditorHidden = true;
        Register<OutfitPieceComponent>(std::move(m));
    }
}

} // namespace ComponentRegistry
