#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Animation.h"       // LocalTRS
#include "FirstPersonBody.h" // IK::SpineDistribution, the foot lock settings

class Model;
class World;

// A dummy's body: the outfit pieces under one root (assets/AI/Soldier.json), posed by the player's motion matching
// (FirstPersonBodyMotion on the shared database). Capsule-led: the director moves the capsule; this stands the body on the
// capsule's feet, steps the matcher toward where the capsule is going (its trajectory along the crowd's corners), and
// afterwards bends the finished pose - the feet onto the ground, the chest and head toward what it looks at.
//
// Frame order:
//   Tick         before the animators: the body stood on the capsule at its heading
//   LateUpdate   after them: the matcher's step, a hit flinch's kick, foot IK, the look, the pose onto every piece; the eye

// The body's tuning: the player's First Person Body numbers, copied from the scene's player by NpcDirector.
struct NpcHoldSettings {
    float TurnThreshold = 1.15f;     // radians: a still body further off than this turns on the spot
    float TurnEndAngle = -1.0f;      // radians: a turn on the spot ends this close to the way (< 0: First Person Body's default)
    float FaceEase = 0.09f;          // seconds: heading easing while moving
    IK::SpineDistribution Spine;     // how the spine's turns are shared over spine_01..05 (default: even)
    float HeadMaxYaw = 1.2f;         // radians: head turns past chest (~70 deg)
    float HeadMaxPitch = 0.6f;       // radians: head nods up/down (~35 deg)
    // Foot IK
    float FootIKMaxDrop = 0.35f;     // m: pelvis drops to lower foot
    float FootIKMaxRaise = 0.35f;    // m: pelvis rises to higher foot
    float FootIKPelvisRaise = 0.08f; // m: pelvis height adjustment limit
    float FootIKTiltMax = 0.5f;      // radians: max foot angle to ground normal
    float FootOffsetEase = 0.05f;    // seconds: vertical foot adjustment easing
    float FootNormalEase = 0.08f;    // seconds: ground normal easing
    float FootIKFade = 0.15f;        // seconds: foot IK enable/disable easing
    IK::FootLockSettings FootLock;   // planted feet held in the world (the player body's settings)
};

struct NpcBodyInput {
    glm::vec3 Feet{0.0f};          // the capsule's foot position
    glm::vec3 Velocity{0.0f};      // how the capsule moved (m/s, world)
    glm::vec3 Wish{0.0f};          // where it is going and at what pace (m/s, world, flat); zero once its stop has begun
    bool Grounded = true;          // the capsule on the floor (else falling)
    float FallSpeed = 0.0f;        // m/s down while falling (the landing's impact)
    float AccelTime = 0.12f;       // seconds the capsule eases up to pace / down to a stop (the stop clip's distance matching)
    float DecelTime = 0.15f;
    float FacingYaw = 0.0f;        // the heading the body should have standing (radians, model +Z = sin/cos); moving, it faces the travel
    glm::vec3 LookPoint{0.0f};     // what the head looks at
    bool Sprint = false;           // running (the footsteps' stride and sound)
    bool FootIK = false;           // feet onto uneven ground (near the player: two rays and a leg solve a frame)
    int Style = MotionLabel::StyleWalk; // the style the gait asks for (walk, jog; the run is the sprint style)
    glm::vec3 Path[4]{};                // the navmesh path's next corners (world), for the trajectory; none: straight down the wish
    int PathCount = 0;
    bool PathArrives = false;           // the last corner is the goal (the stop is on it)
};

class NpcBody {
public:
    bool Start(World& world, entt::entity root);
    void Stop();
    bool IsActive() const { return m_Root != entt::null; }
    entt::entity Root() const { return m_Root; }

    void Tick(World& world, const NpcBodyInput& in, float dt);
    void SetHoldSettings(const NpcHoldSettings& s) { m_Set = s; }
    // A kick added to the driver's animated pose before the foot pass (the hit flinch, NpcFlinch::ApplyPose): `pose` and its
    // model-space `globals` (the body's root frame) are current on entry and must be on return. The foot pass then keeps the
    // planted feet where the clip has them, and the driver's pose carries the kick to every other piece.
    using PoseKick = std::function<void(const Model& driver, std::vector<LocalTRS>& pose, const std::vector<int>& parents,
                                        std::vector<glm::mat4>& globals)>;
    void LateUpdate(World& world, float dt, const PoseKick& kick = nullptr);

    // Each foot's height above the capsule's feet as last posed (m; [0] left, [1] right), for the footsteps. False when
    // the driver has no foot bones.
    bool FootHeights(float (&out)[2]) const;
    bool FootWorld(glm::vec3 (&out)[2]) const; // each final foot bone, world (after the foot pass)
    bool Sprinting() const { return m_In.Sprint; } // this frame's gait (NpcBodyInput::Sprint)

    float Yaw() const { return m_Yaw; }
    glm::vec3 Eye() const { return m_Eye; }               // world, after LateUpdate
    // A standard bone's world position as last posed; false when there is no such bone.
    bool BoneWorld(const World& world, const std::string& bone, glm::vec3& out) const;
    bool BoneWorld(const std::string& bone, glm::vec3& out) const;
    const std::vector<std::shared_ptr<Model>>& Models() const { return m_Models; }
    // The driver's skeleton (the one whose pose is solved), for the hitboxes.
    const Model* DriverModel() const { return m_DriverModel.get(); }
    glm::mat4 RootMatrix() const { return RootWorld(); }
    // The entity of every piece (for hit tests, the ragdoll).
    const std::vector<entt::entity>& Pieces() const { return m_Pieces; }
    entt::entity Driver() const { return m_Driver; }
    // The driver's current state name, for the debug overlay.
    std::string StateName(const World& world) const;
    // The pose is the ragdoll's from now on: Tick / LateUpdate stop touching the pose.
    void SetPoseOwnedElsewhere(bool owned) { m_PoseExternal = owned; }
    // Motion matching took (the database loaded and bound - not in CI, which has no licensed assets: the body stands in its bind
    // pose). Its gait speeds are what the capsule should move at.
    bool MotionMatching() const { return m_MmActive; }
    const FirstPersonBodyMotion& Motion() const { return m_Mm; }
    const MmPlayerDebug::Snapshot& MmDebug() const { return m_MmDebug; } // the last matcher update's (as the player's MmPlayerDebug)
    int MmUpdates() const { return m_MmUpdates; } // matcher updates since Start (the animation LOD holds some frames: fewer than frames)

private:
    glm::mat4 RootWorld() const;
    // Motion matching's step (LateUpdate, before the foot pass): the matcher over `dt` (the frames the LOD held included), the
    // heading from it, the pose onto every piece.
    void StepMotionMatching(World& world, float dt);

    entt::entity m_Root = entt::null;
    entt::entity m_Driver = entt::null;
    std::vector<entt::entity> m_Pieces;
    std::vector<std::shared_ptr<Model>> m_Models;       // per piece (index-matched)
    std::shared_ptr<Model> m_DriverModel;
    bool m_PoseExternal = false;

    glm::vec3 m_Feet{0.0f};
    float m_Yaw = 0.0f;
    bool m_HaveYaw = false;
    bool m_Turning = false;
    // The look: the chest's twist toward it, and the head's own turn past the chest (radians), sprung.
    float m_LookTwist = 0.0f, m_LookTwistRate = 0.0f;
    float m_HeadYaw = 0.0f, m_HeadPitch = 0.0f, m_HeadYawRate = 0.0f, m_HeadPitchRate = 0.0f;
    int m_DriverNeck = -1, m_DriverHead = -1;
    NpcBodyInput m_In;
    glm::vec3 m_Eye{0.0f};
    FirstPersonBodyMotion m_Mm;
    MmPlayerDebug::Snapshot m_MmDebug;
    bool m_MmActive = false;
    int m_MmUpdates = 0;

    NpcHoldSettings m_Set;
    // The pose passes solve on the driver alone; this gives every other piece that draws the upper body the driver's
    // rotations and stabilized spine offsets (one skeleton).
    void SyncPieces();
    // `stepFor(divisor)` = the turn one spine bone takes (the whole turn / divisor); see IK::ChainDivisors.
    void OffsetSpine(const std::function<glm::quat(float)>& stepFor); // m_Pose / m_Globals already hold the driver's pose and its globals
    // Feet onto uneven ground (LateUpdate, first): the pelvis drops to the lower foot's ground, the legs reach theirs.
    // `kick` (LateUpdate's) goes onto the pose first; a foot planted in the clip is solved back to where the clip has it.
    void FootPass(float dt, const PoseKick& kick);
    void SyncLower(); // the driver's pelvis and legs onto every other piece
    std::vector<std::vector<std::pair<int, int>>> m_LowerMap; // per piece: (piece node, driver node), pelvis and legs
    int m_DriverPelvis = -1, m_DriverLeg[2][3] = {{-1, -1, -1}, {-1, -1, -1}}; // thigh, calf, foot
    int m_DriverBall[2] = {-1, -1}; // the ball of each foot (where the toes point)
    IK::FootLock m_Lock;
    float m_FootWeight = 0.0f, m_FootOffset[2] = {0.0f, 0.0f};
    glm::vec3 m_FootNormal[2] = {glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    bool m_HaveFootGround = false;
    std::vector<std::vector<std::pair<int, int>>> m_UpperMap; // per piece: (piece node, driver node), spine_01 and below
    int m_DriverIndex = -1;
    std::vector<int> m_DriverParents;                          // the driver's node parents
    int m_DriverSpine[5] = {-1, -1, -1, -1, -1}, m_DriverSpineCount = 0;
    int m_DriverSpineSlot[5] = {0, 1, 2, 3, 4}; // the chain slot (spine_0N - 1) of each found bone, for the Spine distribution
    // Scratch.
    std::vector<glm::mat4> m_Globals;
    std::vector<LocalTRS> m_Pose;
    // Per piece: what of the skeleton it draws - anything the spine moves (kSkinsSpine), the arms (kSkinsArms), the
    // neck and head (kSkinsNeck). A piece that draws none of what a pass moves is left out of it (legs, shoes).
    enum : unsigned { kSkinsSpine = 1u, kSkinsArms = 2u, kSkinsNeck = 4u };
    std::vector<unsigned> m_PieceSkins;
    bool PieceTakes(size_t k, unsigned what) const {
        return k >= m_PieceSkins.size() || m_Pieces[k] == m_Driver || (m_PieceSkins[k] & what) != 0u;
    }
};

// --- the maths, exposed for tests ---------------------------------------------------------------
// Heading (radians, model +Z) of a flat direction; `fallback` when it has no length.
float NpcYawOf(const glm::vec3& dir, float fallback);
// Where to put the spine's twist (radians, + = toward model +X): `offset` (look heading minus body
// heading, wrapped) clamped to +-maxTwist.
float NpcSpineTwist(float offset, float maxTwist);
