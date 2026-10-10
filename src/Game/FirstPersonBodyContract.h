#pragma once

#include "AnimatorController.h"
#include "IK.h"

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct FirstPersonBodyComponent;

// The contract between the true-first-person body (FirstPersonBody) and the Animator Controller
// that animates it: the parameters it drives, the states it watches, the tag it reads, and the
// bones it poses. FirstPersonBody.cpp uses these names, and FirstPersonBodyValidate checks a
// setup against them - so a missing state or bone is reported instead of silently doing nothing.
// The reference controller is project/assets/Animations/Controllers/fps_body_locomotion.controller.
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
// The richer graph (all optional: the body sets them and the graph may leave them unread).
inline constexpr const char* kStartGait = "StartGait";             // Float: 0 walk, 1 jog, 2 run
inline constexpr const char* kStopGait = "StopGait";
inline constexpr const char* kStartTurn = "StartTurn";             // Float: a start that turns the body, + left, in 45 degree steps
inline constexpr const char* kStartTurnAmount = "StartTurnAmount"; // Float: |StartTurn|, 1..4
inline constexpr const char* kStartDistance = "StartDistance";     // Float: metres since the start (distance matching)
inline constexpr const char* kStopDistance = "StopDistance";       // Float: metres still to go in the stop
inline constexpr const char* kPivotDistance = "PivotDistance";     // Float: metres from the pivot's turnaround
inline constexpr const char* kPivot = "Pivot";                     // Trigger: the travel reversed
inline constexpr const char* kPivotX = "PivotX";                   // Float: the direction it was going
inline constexpr const char* kPivotY = "PivotY";
inline constexpr const char* kPivotGait = "PivotGait";
inline constexpr const char* kStep = "Step";                       // Trigger: a tap, one small step
inline constexpr const char* kStepX = "StepX";
inline constexpr const char* kStepY = "StepY";
inline constexpr const char* kFidget = "Fidget";                   // Trigger: an idle fidget
inline constexpr const char* kFidgetIndex = "FidgetIndex";         // Float: which (an integer)
inline constexpr const char* kSprintRate = "SprintRate";           // Float: the sprint loop's play rate
inline constexpr float kSprintClipSpeed = 4.25f;                   // m/s the sprint clip (AM_Loco_Run_Fast_01) travels
// Optional: how much faster than authored the gait plays (the Locomotion state's speed parameter), so the
// feet keep up with a player moving faster than the clips travel (Player Run / Sprint Speed). 1 = authored.
inline constexpr const char* kPlayRate = "PlayRate";

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
inline constexpr const char* kStateSprint = "Sprint";
// Tags the body reads to know what kind of state plays (a graph may have several of each).
inline constexpr const char* kTagStart = "Start";
inline constexpr const char* kTagStartTurn = "StartTurn"; // a start whose yaw turns the body
inline constexpr const char* kTagStop = "Stop";
inline constexpr const char* kTagPivot = "Pivot";
inline constexpr const char* kTagStep = "Step";
inline constexpr const char* kTagFidget = "Fidget";

// The tag foot IK reads: on the states where the feet are off the ground (Jump, Fall).
inline constexpr const char* kTagAirborne = "Airborne";

// Bones the body poses (the UE5 mannequin's names, which the Quantum body and the first-person
// arms share).
inline constexpr const char* kBonePelvis = "pelvis";
inline constexpr const char* kBoneSpine[5] = {"spine_01", "spine_02", "spine_03", "spine_04", "spine_05"};

// Crossfade weight of gait/turn/start/stop states; actions retain their authored motion.
// Updates crouch to the gait's actual standing/crouched crossfade when available.
float LocomotionSpineWeight(const AnimatorControllerComponent& animator, float& crouch);
// Model-space lower-body retention: 10% at spine_01, 5% at spine_02, none above.
// Pelvis and legs are never targeted. Stability 0 keeps the authored gait.
void BlendLocomotionSpine(IK::Pose& pose, const std::vector<int>& parents, const std::array<int, 5>& bones,
                         const std::vector<glm::mat4>& stand, const std::vector<glm::mat4>& crouch,
                         float crouchWeight, float stability, float crouchDrop = 0.0f);
// Undoes BlendLocomotionSpine on `bones` of a pose posed further since: each bone's local is the clips' own
// (`authored`) with whatever was added after stabilizing (`stabilized` to the pose's current) kept on top.
void RestoreAuthoredSpine(IK::Pose& pose, const std::vector<int>& bones, const std::vector<LocalTRS>& authored,
                          const std::vector<LocalTRS>& stabilized);
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

// The standard locomotion graph (the reference project/assets/Animations/Controllers/fps_body_locomotion.controller): its
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
