#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <string>
#include <vector>

class Model;
class World;

// An enemy soldier's body: the Quantum outfit pieces under one root (assets/AI/Soldier.json), on the
// npc_soldier controller (the player body's locomotion plus a flinch layer). Capsule-led: the AI moves
// the capsule and this turns that into the controller's parameters, stands the body on the capsule's
// feet, turns it on the spot with the turn clips, and afterwards bends the finished pose - the spine
// toward what it aims at, the head toward what it looks at, the hands onto the weapon rig's.
//
// Frame order:
//   Tick         before the animators: heading, parameters, triggers
//   LateUpdate   after them: spine aim and lean, head look; the eye for the weapon's camera
//   ReachHands   after the weapon has placed its rig: arms onto its hands, the pose onto every piece
struct NpcBodyInput {
    glm::vec3 Feet{0.0f};          // the capsule's foot position
    glm::vec3 Velocity{0.0f};      // how the capsule moved (m/s, world)
    float FacingYaw = 0.0f;        // the heading the body should have (radians, model +Z = sin/cos)
    bool HoldFacing = false;       // face FacingYaw even while moving (strafing); else face the travel
    glm::vec3 AimPoint{0.0f};      // what the spine and gun aim at (world)
    bool Aiming = false;           // false: the spine doesn't aim (running, reloading behind cover)
    glm::vec3 LookPoint{0.0f};     // what the head looks at
    bool Crouched = false;
    bool Sprint = false;
    float Lean = 0.0f;             // -1 (left) .. 1 (right), a peek round cover
};

class NpcBody {
public:
    bool Start(World& world, entt::entity root);
    void Stop();
    bool IsActive() const { return m_Root != entt::null; }
    entt::entity Root() const { return m_Root; }

    void Tick(World& world, const NpcBodyInput& in, float dt);
    void LateUpdate(World& world, float dt);
    // `armsRig`: the weapon's arms entity (null = none, the arms stay as animated). `weight` 0..1.
    void ReachHands(World& world, entt::entity armsRig, float weight);

    // A hit: the upper body flinches away along `dirWorld` (the round's travel).
    void Flinch(World& world, const glm::vec3& dirWorld);

    float Yaw() const { return m_Yaw; }
    bool Turning() const { return m_Turning; }
    glm::vec3 Eye() const { return m_Eye; }               // world, after LateUpdate
    glm::vec3 Feet() const { return m_Feet; }
    // A standard bone's world position as last posed; false when there is no such bone.
    bool BoneWorld(const World& world, const std::string& bone, glm::vec3& out) const;
    bool BoneWorld(const std::string& bone, glm::vec3& out) const;
    const std::vector<std::shared_ptr<Model>>& Models() const { return m_Models; }
    glm::mat4 RootMatrix() const { return RootWorld(); }
    // The entity of every piece (for hit tests, the ragdoll).
    const std::vector<entt::entity>& Pieces() const { return m_Pieces; }
    entt::entity Driver() const { return m_Driver; }
    // The driver's current state name and the animator's root speed, for the debug overlay.
    std::string StateName(const World& world) const;
    // The pose is the ragdoll's from now on: Tick / LateUpdate stop touching the pose.
    void SetPoseOwnedElsewhere(bool owned) { m_PoseExternal = owned; }

private:
    glm::mat4 RootWorld() const;

    entt::entity m_Root = entt::null;
    entt::entity m_Driver = entt::null;
    std::vector<entt::entity> m_Pieces;
    std::vector<std::shared_ptr<Model>> m_Models;       // per piece (index-matched)
    std::shared_ptr<Model> m_DriverModel;
    bool m_PoseExternal = false;

    glm::vec3 m_Feet{0.0f};
    float m_Yaw = 0.0f;
    bool m_HaveYaw = false;
    glm::vec2 m_Move{0.0f};
    bool m_Turning = false;
    float m_TurnTime = 0.0f;
    float m_StillTime = 0.0f;
    bool m_WasCrouched = false;
    // Aim, eased: the spine's pitch and twist (radians), and the lean.
    float m_AimPitch = 0.0f, m_AimTwist = 0.0f, m_Lean = 0.0f, m_AimWeight = 0.0f;
    NpcBodyInput m_In;
    glm::vec3 m_Eye{0.0f};
};

// --- the maths, exposed for tests ---------------------------------------------------------------
// Heading (radians, model +Z) of a flat direction; `fallback` when it has no length.
float NpcYawOf(const glm::vec3& dir, float fallback);
// Where to put the spine's twist (radians, + = toward model +X): `offset` (aim heading minus body
// heading, wrapped) clamped to +-maxTwist.
float NpcSpineTwist(float offset, float maxTwist);
// Whether a still body `offset` radians off where it should face turns on the spot.
bool NpcShouldTurn(float offset, float thresholdRadians);
