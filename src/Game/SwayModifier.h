#pragma once
#include "Animation.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

// Behavioral port of the supplied KINEMATION SwayModifierSettings/Job and KSpringMath.
enum class SwaySpace { BoneSpace, ParentBoneSpace, ComponentSpace, WorldSpace };
struct SwayVectorSpring {
    glm::vec3 Damping{0}, Stiffness{0}, Speed{0}, Scale{0}, Clamp{0};
};
struct SwaySpringSettings {
    SwayVectorSpring Position, Rotation;
    float DampingFactor = 0;
    SwaySpace Space = SwaySpace::ComponentSpace;
    float AdsScale = 0;
    static SwaySpringSettings ShooterAimPreset();
    static SwaySpringSettings ShooterMovePreset();
};
struct SwayModifierSettings {
    std::string WeaponBone = "ik_hand_gun"; // engine equivalent of CAS "IK weapon_bone"
    std::string WeaponAdditiveBone = "weapon_bone_additive";
    SwaySpringSettings AimingSway = SwaySpringSettings::ShooterAimPreset();
    SwaySpringSettings MovementSway = SwaySpringSettings::ShooterMovePreset();
    glm::quat SpaceOffset{1,0,0,0};
    float AdsCurveScale = 1;
    float AdsCurveSmoothing = 10;
};
struct SwayFloatSpringState { float Velocity = 0, Error = 0; };
float SwayFloatSpringInterp(float current, float target, float speed, float damping,
                           float stiffness, float scale, SwayFloatSpringState&, float dt);
struct SwayTransform { glm::vec3 Position{0}; glm::quat Rotation{1,0,0,0}; };
struct SwayModifierPose {
    SwayTransform Movement, Aim;
    float CurveWeight = 1;
};
class SwayModifierState {
public:
    void Reset() { *this = SwayModifierState{}; }
    // Inputs match Unity: horizontal/vertical look delta, right/forward movement axes.
    const SwayModifierPose& Update(const SwayModifierSettings&, glm::vec2 moveInput,
                                  glm::vec2 deltaLookInput, bool isAiming, float dt,
                                  float weight = 1, bool validWeaponBone = true);
private:
    glm::vec3 m_MovePositionTarget{0}, m_MoveRotationTarget{0};
    glm::vec3 m_MovePosition{0}, m_MoveRotation{0}, m_AimPosition{0}, m_AimRotation{0};
    glm::vec2 m_AimTarget{0};
    SwayFloatSpringState m_MovePosState[3], m_MoveRotState[3], m_AimPosState[3], m_AimRotState[3];
    SwayModifierPose m_Pose;
};

// Applies move, aim, then local additive-bone animation to the gun, preserving the source
// multiplication order in all four spaces. Locals are engine model units; output is before IK.
void ApplySwayModifier(const SwayModifierSettings&, const SwayModifierPose&,
                       std::vector<LocalTRS>& pose, const std::vector<int>& parents,
                       std::vector<glm::mat4>& globals, int weapon, int additive,
                       glm::quat modelToComponent, glm::quat modelToWorld,
                       float metresPerUnit, float weight);
