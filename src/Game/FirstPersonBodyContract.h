#pragma once

#include "IK.h"

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct FirstPersonBodyComponent;

// The true-first-person body's (FirstPersonBody's) contract with its rig: the bones it poses, the Bone Map that renames
// them, and the setup checks (Validate), so a missing bone or piece is reported instead of silently doing nothing.
// Motion matching poses the body (since Phase 6): no Animator Controller drives it, so there are no parameters, states or
// tags to agree on.
namespace FPBody {

// Not a controller state: what BodyDebug reports as the body's state while motion matching poses it.
inline constexpr const char* kStateMotionMatching = "MotionMatching";

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
inline constexpr const char* kBoneBall[2] = {"ball_l", "ball_r"}; // the forefoot: where a motion matching body's planted foot pivots

// A Bone Map turns the standard (UE5 mannequin) bone names into a rig's own: "hand_l = LeftHand, foot_l = LeftFoot".
// Entries are separated by commas or new lines; unlisted bones keep their standard name.
std::map<std::string, std::string> ParseBoneMap(const std::string& text);
// The rig's name for a standard bone.
inline const std::string& MappedBone(const std::map<std::string, std::string>& map, const std::string& standard) {
    auto it = map.find(standard);
    return it == map.end() ? standard : it->second;
}

// Which body option needs a name.
enum class Feature { Core, FootIK, WeaponArms, SpineAim };
const char* FeatureName(Feature f);
bool FeatureEnabled(Feature f, const FirstPersonBodyComponent& cfg);

struct BoneSpec {
    const char* Name;
    Feature Needed;
    const char* What;
};
// The full table (the documentation and the validator both read it).
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
    bool HasDriverPiece = false;           // one with the whole skeleton (the pelvis, the head, a hand): motion matching's driver
    std::function<bool(const std::string&)> HasBone; // on the driver's model (empty = skip bone checks)
    std::vector<std::string> PieceNames;   // names of the pieces (for Arms Piece / Hidden Parts matching)
    int BodyCount = 1;                     // First Person Body components in the scene
};

// Whether a body piece named `pieceName` is part of the arms: its name contains the Arms Piece or the Hands
// Piece (case-insensitive; an empty setting matches nothing).
bool IsArmsPiece(const FirstPersonBodyComponent& cfg, const std::string& pieceName);

// Every problem (and a few notes) with a body setup, worst first. An empty result means all clear.
std::vector<Check> Validate(const ValidationInput& in);
// The worst severity in a result (Ok when empty).
Severity Worst(const std::vector<Check>& checks);

} // namespace FPBody
