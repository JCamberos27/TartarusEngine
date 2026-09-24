#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <utility>
#include <vector>

class AssetLibrary;
class Model;
struct AnimatorController;
struct AnimatorLayerRuntime;
struct FirstPersonAdsSettings;
struct IKRigComponent;

// Carrying hip clips onto the sights (see FirstPersonAdsSettings). For each state tagged with the
// carry tag, measured once from the clip's first frame against the reference (aim) state's:
//  - the gun correction C: the rigid move, about the camera bone, that takes the clip's gun onto
//    the aim pose's (applied to the gun bone only; the arm IK follows it);
//  - per arm, the elbow swivel that puts the solved elbow on the aim pose's;
//  - the local rotations the arms' other bones (twist helpers) need to match the aim pose.
// With all three the solved first frame is the aim pose exactly, so the action starts and ends
// there and moves like the hip clip in between.
struct AdsCarryAction {
    int State = -1; // base-layer state index
    std::string StateName;
    glm::quat R{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 T{0.0f};
    float Swivel[2] = {0.0f, 0.0f}; // radians, IK limb A (right) and B (left)
    std::vector<std::pair<std::string, glm::quat>> Locals;
};

// What BuildAdsCarry found, for the weapon Inspector.
struct AdsCarryReport {
    struct Entry {
        std::string State;
        std::string Problem;          // empty = carried
        float GunOffsetCm = 0.0f;     // how far the gun moves onto the sights
        float GunTurnDeg = 0.0f;
        float SwivelDeg[2] = {0.0f, 0.0f};
        int MatchedBones = 0;         // twist helpers corrected
    };
    std::string Reference;            // the aim state measured against ("" = none found)
    bool UsesIK = false;              // false: the whole rig is carried instead of just the gun
    std::vector<Entry> Entries;
    std::vector<std::string> Warnings;
};

struct AdsCarryResult {
    std::vector<AdsCarryAction> Actions;
    AdsCarryReport Report;
};

struct AdsCarryInputs {
    Model* Arms = nullptr;
    AssetLibrary* Assets = nullptr;
    const AnimatorController* Controller = nullptr;
    const FirstPersonAdsSettings* Settings = nullptr;
    std::string ArmsTrack;            // the controller track the arms play
    std::string GunBone;              // the arms rig's weapon socket / IK gun bone
    std::string CameraBone;           // the bone pinned to the eye (the correction's pivot)
    // The arms IK rig as Play runs it (null = no IK): its offset slots and limbs.
    const IKRigComponent* Rig = nullptr;
    int AdsOffset = 0, ProceduralOffset = 1;
};

AdsCarryResult BuildAdsCarry(const AdsCarryInputs& in);

// The last report measured for a weapon definition (by its file path), kept after Play stops so
// the weapon Inspector can show it. Null when that weapon hasn't been played this session.
void PublishAdsCarryReport(const std::string& weaponPath, const AdsCarryReport& report);
const AdsCarryReport* FindAdsCarryReport(const std::string& weaponPath);

// How much of the current base-layer blend is a carried action, and that action's correction
// scaled by it (times `hold`, how far aim is held, 0..1). `dt` > 0 advances each crossfade the
// way the controller is about to, for offsets written before it runs.
struct AdsCarrySample {
    const AdsCarryAction* Action = nullptr; // the dominant carried action, or null
    float Weight = 0.0f;
    glm::quat R{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 T{0.0f};
    float Swivel[2] = {0.0f, 0.0f};
};
AdsCarrySample EvaluateAdsCarry(const AnimatorLayerRuntime& layer, const std::vector<AdsCarryAction>& actions,
                                float dt, float hold);
