#pragma once

#include "AnimatorController.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

struct FirstPersonBodyComponent;

// The contract between the true-first-person body (FirstPersonBody) and the Animator Controller
// that animates it: the parameters it drives, the states it watches, the tag it reads, and the
// bones it poses. FirstPersonBody.cpp uses these names, and FirstPersonBodyValidate checks a
// setup against them - so a missing state or bone is reported instead of silently doing nothing.
// The reference controller is project/animations/fps_body_locomotion.controller.
namespace FPBody {

// Parameters the body sets every frame (Float unless noted).
inline constexpr const char* kMoveX = "MoveX";           // the input in the body's frame, m/s, smoothed
inline constexpr const char* kMoveY = "MoveY";
inline constexpr const char* kSpeed = "Speed";           // |MoveX, MoveY|
inline constexpr const char* kSprint = "Sprint";         // Bool
inline constexpr const char* kGrounded = "Grounded";     // Bool
inline constexpr const char* kAirborne = "Airborne";     // Bool: off the ground for > 0.15 s
inline constexpr const char* kCrouched = "Crouched";     // Bool
inline constexpr const char* kJump = "Jump";             // Trigger
inline constexpr const char* kTurning = "Turning";       // Bool: turning on the spot
inline constexpr const char* kTurnAngle = "TurnAngle";   // degrees the view is off the body's heading
inline constexpr const char* kMoving = "Moving";         // Bool: the input asks to move
inline constexpr const char* kStart = "Start";           // Trigger: a push-off clip
inline constexpr const char* kStop = "Stop";             // Trigger: a braking clip
inline constexpr const char* kStopRun = "StopRun";       // Trigger: a braking clip from a sprint
inline constexpr const char* kStartX = "StartX";         // Float: the push-off's direction
inline constexpr const char* kStartY = "StartY";
inline constexpr const char* kStopX = "StopX";           // Float: the braking direction
inline constexpr const char* kStopY = "StopY";
inline constexpr const char* kCrouchDown = "CrouchDown"; // Trigger: stand -> crouch while still
inline constexpr const char* kCrouchUp = "CrouchUp";     // Trigger: crouch -> stand while still

// States the body watches by name.
inline constexpr const char* kStateLocomotion = "Locomotion";
inline constexpr const char* kStateJump = "Jump";
inline constexpr const char* kStateFall = "Fall";
inline constexpr const char* kStateLand = "Land";
inline constexpr const char* kStateTurn = "Turn";
inline constexpr const char* kStateCrouchTurn = "CrouchTurn";
inline constexpr const char* kStateStart = "Start";
inline constexpr const char* kStateStop = "Stop";
inline constexpr const char* kStateStopRun = "StopRun";
inline constexpr const char* kStateCrouchLoco = "CrouchLoco";
inline constexpr const char* kStateCrouchDown = "CrouchDown";
inline constexpr const char* kStateCrouchUp = "CrouchUp";

// The tag foot IK reads: on the states where the feet are off the ground (Jump, Fall).
inline constexpr const char* kTagAirborne = "Airborne";

// Bones the body poses (the UE5 mannequin's names, which the Quantum body and the first-person
// arms share).
inline constexpr const char* kBonePelvis = "pelvis";
inline constexpr const char* kBoneSpine[5] = {"spine_01", "spine_02", "spine_03", "spine_04", "spine_05"};
inline constexpr const char* kBoneUpperArm[2] = {"upperarm_l", "upperarm_r"};
inline constexpr const char* kBoneLowerArm[2] = {"lowerarm_l", "lowerarm_r"};
inline constexpr const char* kBoneHand[2] = {"hand_l", "hand_r"};
inline constexpr const char* kBoneClavicle[2] = {"clavicle_l", "clavicle_r"};
inline constexpr const char* kBoneThigh[2] = {"thigh_l", "thigh_r"};
inline constexpr const char* kBoneCalf[2] = {"calf_l", "calf_r"};
inline constexpr const char* kBoneFoot[2] = {"foot_l", "foot_r"};

// A Bone Map turns the standard (UE5 mannequin) bone names into a rig's own: "hand_l = LeftHand, foot_l = LeftFoot".
// Entries are separated by commas or new lines; unlisted bones keep their standard name.
std::map<std::string, std::string> ParseBoneMap(const std::string& text);
// The rig's name for a standard bone.
inline const std::string& MappedBone(const std::map<std::string, std::string>& map, const std::string& standard) {
    auto it = map.find(standard);
    return it == map.end() ? standard : it->second;
}

// Which body option needs a name.
enum class Feature { Core, Turning, StartStop, Crouch, FootIK, WeaponArms, SpineAim };
const char* FeatureName(Feature f);
bool FeatureEnabled(Feature f, const FirstPersonBodyComponent& cfg);

struct ParamSpec {
    const char* Name;
    AnimatorController::ParamType Type;
    Feature Needed;
    const char* What;
};
struct StateSpec {
    const char* Name;
    Feature Needed;
    const char* What;
    bool Required; // false: the graph may route around it (the body only reads it)
};
struct BoneSpec {
    const char* Name;
    Feature Needed;
    const char* What;
};
// The full tables (the documentation and the validator both read these).
const std::vector<ParamSpec>& Params();
const std::vector<StateSpec>& States();
const std::vector<BoneSpec>& Bones();

enum class Severity { Ok = 0, Info, Warning, Error };
struct Check {
    Severity Level = Severity::Ok;
    std::string Message;
    std::string Hint; // what to do about it
};

// What FirstPersonBodyValidate looks at. The caller fills what it has: a check whose input is
// missing is skipped.
struct ValidationInput {
    const FirstPersonBodyComponent* Config = nullptr;
    bool HasPieces = false;                // rigged pieces under the body root
    bool HasDriverPiece = false;           // one with an Animator Controller
    const AnimatorController* Controller = nullptr; // the driver's controller, loaded (null = not loaded)
    bool ControllerSet = false;            // the driver piece names a controller path
    std::function<bool(const std::string&)> HasBone; // on the driver's model (empty = skip bone checks)
    std::vector<std::string> PieceNames;   // names of the pieces (for Arms Piece / Hidden Parts matching)
    int BodyCount = 1;                     // First Person Body components in the scene
};

// Every problem (and a few notes) with a body setup, worst first. An empty result means all clear.
std::vector<Check> Validate(const ValidationInput& in);
// The worst severity in a result (Ok when empty).
Severity Worst(const std::vector<Check>& checks);

// The standard locomotion graph (the reference project/animations/fps_body_locomotion.controller): its
// 14 states, 20 parameters and 63 tuned transitions (start / stop offsets, exit times, crossfades), one
// "main" track. Clips are named by role - the reference clip's file name without the "AM_" prefix, e.g.
// "Loco_Walk_Fwd" - and `clipForRole` gives the path for each (empty = none yet, for the Animator to fill).
AnimatorController BuildLocomotionController(const std::function<std::string(const std::string&)>& clipForRole);
// Every role the graph uses, in graph order.
std::vector<std::string> LocomotionRoles();
// The file among `files` whose name (minus a leading "AM_" and the extension) equals `role`, case
// insensitively; "" when none does.
std::string PickLocomotionClip(const std::string& role, const std::vector<std::string>& files);

} // namespace FPBody
