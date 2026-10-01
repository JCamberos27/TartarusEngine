#include "NpcBody.h"

#include "Components.h"
#include "FirstPersonBody.h"         // FirstPersonBodyLocalMove / WrapAngle
#include "FirstPersonBodyContract.h" // FPBody:: parameter, state and bone names
#include "IK.h"
#include "Model.h"
#include "World.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace {

float Follow(float dt, float timeConstant) {
    return timeConstant > 0.0f ? 1.0f - std::exp(-dt / timeConstant) : 1.0f;
}

glm::quat YawRotation(float yaw) { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }

constexpr float kTurnThreshold = 1.15f;  // radians (66 deg): a still body further off than this turns on the spot
constexpr float kMaxTwist = 1.2f;        // radians the spine twists toward the aim
constexpr float kMoveEase = 0.1f;        // seconds: the blend tree's parameters
constexpr float kFaceEase = 0.09f;       // seconds: the heading while moving

} // namespace

float NpcYawOf(const glm::vec3& dir, float fallback) {
    if (dir.x * dir.x + dir.z * dir.z < 1e-8f) return fallback;
    return std::atan2(dir.x, dir.z);
}

float NpcSpineTwist(float offset, float maxTwist) {
    return std::clamp(FirstPersonBodyWrapAngle(offset), -maxTwist, maxTwist);
}

bool NpcShouldTurn(float offset, float thresholdRadians) {
    return std::abs(FirstPersonBodyWrapAngle(offset)) > thresholdRadians;
}

bool NpcBody::Start(World& world, entt::entity root) {
    Stop();
    auto& reg = world.Registry;
    if (!reg.valid(root)) return false;
    const auto* h = reg.try_get<HierarchyComponent>(root);
    if (!h) return false;
    for (entt::entity c : h->Children) {
        if (!reg.valid(c)) continue;
        const auto* rc = reg.try_get<RenderableComponent>(c);
        if (!rc || !rc->ModelRef || !reg.all_of<AnimatorControllerComponent>(c)) continue;
        m_Pieces.push_back(c);
        m_Models.push_back(rc->ModelRef);
    }
    // The driver: the first piece with the whole skeleton (the torso). The rest follow it.
    for (size_t k = 0; k < m_Pieces.size(); ++k) {
        const Model& m = *m_Models[k];
        if (m.NodeIndex(FPBody::kBonePelvis) >= 0 && m.NodeIndex("head") >= 0 && m.NodeIndex(FPBody::kBoneHand[1]) >= 0) {
            m_Driver = m_Pieces[k];
            m_DriverModel = m_Models[k];
            break;
        }
    }
    if (m_Driver == entt::null) { m_Pieces.clear(); m_Models.clear(); return false; }
    for (entt::entity e : m_Pieces) {
        auto& ac = reg.get<AnimatorControllerComponent>(e);
        ac.Driver = e == m_Driver ? entt::null : m_Driver;
        ac.RootMotion.Mode = (int)RootMotionMode::InPlace;
    }
    m_Root = root;
    return true;
}

void NpcBody::Stop() {
    *this = NpcBody{};
}

glm::mat4 NpcBody::RootWorld() const {
    return glm::translate(glm::mat4(1.0f), m_Feet) * glm::mat4_cast(YawRotation(m_Yaw));
}

void NpcBody::Tick(World& world, const NpcBodyInput& in, float dt) {
    if (!IsActive() || m_PoseExternal) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Driver) || !reg.valid(m_Root) || !reg.all_of<AnimatorControllerComponent>(m_Driver)) return;
    m_In = in;
    m_Feet = in.Feet;
    auto& ac = reg.get<AnimatorControllerComponent>(m_Driver);

    const glm::vec3 flat(in.Velocity.x, 0.0f, in.Velocity.z);
    const float speed = glm::length(flat);
    const bool moving = speed > 0.25f;
    if (!m_HaveYaw) { m_Yaw = in.FacingYaw; m_HaveYaw = true; }
    const float want = in.HoldFacing || !moving ? in.FacingYaw : NpcYawOf(flat, m_Yaw);
    float offset = FirstPersonBodyWrapAngle(want - m_Yaw);

    if (moving) {
        m_Turning = false;
        m_StillTime = 0.0f;
        m_Yaw += offset * Follow(dt, kFaceEase);
    } else {
        m_StillTime += dt;
        if (m_Turning) {
            m_TurnTime += dt;
            if (ac.InState(FPBody::kStateTurn) || ac.InState(FPBody::kStateCrouchTurn))
                m_Yaw += glm::radians(ac.RootMotion.DeltaYaw);
            offset = FirstPersonBodyWrapAngle(want - m_Yaw);
            const bool clipDone = m_TurnTime > 0.25f && !ac.InState(FPBody::kStateTurn) && !ac.InState(FPBody::kStateCrouchTurn);
            if (std::abs(offset) < glm::radians(8.0f) || clipDone || m_TurnTime > 1.6f) m_Turning = false;
        } else if (m_StillTime > 0.15f && NpcShouldTurn(offset, kTurnThreshold)) {
            m_Turning = true;
            m_TurnTime = 0.0f;
            ac.SetFloat(FPBody::kTurnAngle, std::clamp(glm::degrees(offset), -180.0f, 180.0f));
        } else if (in.Aiming) {
            // Aiming from where it stands: the shoulders square up to the target (a small shuffle;
            // the turn clips take the big swings).
            m_Yaw += offset * Follow(dt, 0.18f);
        }
        // Never more than the spine can twist behind what it faces: the feet slide a little instead.
        const float lag = FirstPersonBodyWrapAngle(want - m_Yaw);
        const float maxLag = kTurnThreshold + 0.6f;
        if (std::abs(lag) > maxLag) m_Yaw = want - std::copysign(maxLag, lag);
    }
    m_Yaw = FirstPersonBodyWrapAngle(m_Yaw);
    world.SetWorldPose(m_Root, m_Feet, YawRotation(m_Yaw));

    // The blend tree's parameters are the clips' own speeds, which the soldier's gaits are tuned to.
    const glm::vec2 local = FirstPersonBodyLocalMove(flat, m_Yaw);
    m_Move += (local - m_Move) * Follow(dt, kMoveEase);
    ac.SetFloat(FPBody::kMoveX, m_Move.x);
    ac.SetFloat(FPBody::kMoveY, m_Move.y);
    ac.SetFloat(FPBody::kSpeed, glm::length(m_Move));
    ac.SetFloat(FPBody::kPlayRate, 1.0f);
    ac.SetBool(FPBody::kSprint, in.Sprint && moving);
    ac.SetBool(FPBody::kGrounded, true);
    ac.SetBool(FPBody::kAirborne, false);
    ac.SetBool(FPBody::kMoving, moving);
    ac.SetBool(FPBody::kTurning, m_Turning);
    if (!moving && glm::length(m_Move) < 0.4f && in.Crouched != m_WasCrouched) {
        if (in.Crouched && ac.InState(FPBody::kStateLocomotion)) ac.SetTrigger(FPBody::kCrouchDown);
        if (!in.Crouched && ac.InState(FPBody::kStateCrouchLoco)) ac.SetTrigger(FPBody::kCrouchUp);
    }
    m_WasCrouched = in.Crouched;
    ac.SetBool(FPBody::kCrouched, in.Crouched);
}

void NpcBody::LateUpdate(World& world, float dt) {
    (void)world;
    if (!IsActive() || m_PoseExternal || !m_DriverModel) return;
    const glm::mat4 rootW = RootWorld();
    // The aim, measured on the driver: from the chest, a pitch and a twist, eased.
    glm::vec3 chestW = m_Feet + glm::vec3(0.0f, 1.4f, 0.0f);
    {
        glm::mat4 g(1.0f);
        if (m_DriverModel->NodeTransform(FPBody::kBoneSpine[4], g) || m_DriverModel->NodeTransform(FPBody::kBoneSpine[2], g))
            chestW = glm::vec3(rootW * g[3]);
    }
    float wantPitch = 0.0f, wantTwist = 0.0f;
    if (m_In.Aiming) {
        const glm::vec3 d = m_In.AimPoint - chestW;
        const float flat = std::sqrt(d.x * d.x + d.z * d.z);
        wantPitch = std::clamp(std::atan2(d.y, std::max(flat, 0.05f)), -1.1f, 1.1f);
        wantTwist = NpcSpineTwist(NpcYawOf(d, m_Yaw) - m_Yaw, kMaxTwist);
    } else if (!m_Turning) {
        // Looking about while not aiming: the twist only (a soldier scanning, not bending over).
        const glm::vec3 d = m_In.LookPoint - chestW;
        wantTwist = NpcSpineTwist(NpcYawOf(d, m_Yaw) - m_Yaw, 0.6f);
    }
    m_AimPitch += (wantPitch - m_AimPitch) * Follow(dt, 0.07f);
    m_AimTwist += (wantTwist - m_AimTwist) * Follow(dt, 0.08f);
    m_Lean += (std::clamp(m_In.Lean, -1.0f, 1.0f) - m_Lean) * Follow(dt, 0.12f);
    m_AimWeight += ((m_In.Aiming ? 1.0f : 0.0f) - m_AimWeight) * Follow(dt, 0.12f);

    // ... spread over the spine of every piece, so they stay one skeleton (each piece keeps its own
    // bones' rest frames; the rotation is the same in model space).
    std::vector<int> parents;
    std::vector<glm::mat4> globals;
    for (const auto& mp : m_Models) {
        Model& m = *mp;
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        int spine[5], n = 0;
        for (int k = 0; k < 5; ++k)
            if (const int b = m.NodeIndex(FPBody::kBoneSpine[k]); b >= 0) spine[n++] = b;
        if (n == 0) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[(size_t)i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        // Rotating +Z about +X by theta gives (0, -sin, cos): looking up is a negative angle about X.
        // The lean rolls about the body's forward (+Z): + = the top toward -X, the body's right.
        const glm::quat step = glm::angleAxis(m_AimTwist / (float)n, glm::vec3(0, 1, 0)) *
                               glm::angleAxis(-m_AimPitch / (float)n, glm::vec3(1, 0, 0)) *
                               glm::angleAxis(m_Lean * 0.38f / (float)n, glm::vec3(0, 0, 1));
        for (int k = 0; k < n; ++k) IK::OffsetBone(pose, parents, globals, spine[k], glm::vec3(0.0f), step, IK::Position(globals[(size_t)spine[k]]));
        // Aiming: the head drops and tilts onto the stock (a cheek weld).
        if (m_AimWeight > 0.01f)
            if (const int head = m.NodeIndex("head"); head >= 0) {
                const glm::quat weld = glm::angleAxis(0.16f * m_AimWeight, glm::vec3(1, 0, 0)) * glm::angleAxis(0.22f * m_AimWeight, glm::vec3(0, 0, 1));
                IK::OffsetBone(pose, parents, globals, head, glm::vec3(0.0f), weld, IK::Position(globals[(size_t)head]));
            }
        m.ApplyLocalPose(pose);
    }

    // The head bone, where the weapon's camera goes (its arms rig is placed by the same bone).
    glm::mat4 head(1.0f);
    m_Eye = m_DriverModel->NodeTransform("head", head) ? glm::vec3(rootW * head[3]) : m_Feet + glm::vec3(0.0f, 1.62f, 0.0f);
}

void NpcBody::ReachHands(World& world, entt::entity armsRig, float weight) {
    if (!IsActive() || m_PoseExternal || armsRig == entt::null || weight <= 0.0f) return;
    auto& reg = world.Registry;
    if (!reg.valid(armsRig)) return;
    const auto* rrc = reg.try_get<RenderableComponent>(armsRig);
    const auto* rt = reg.try_get<TransformComponent>(armsRig);
    if (!rrc || !rrc->ModelRef || !rt) return;
    const Model& rig = *rrc->ModelRef;
    const glm::mat4 rigW = glm::translate(glm::mat4(1.0f), rt->Position) * glm::mat4_cast(rt->Rotation) * glm::scale(glm::mat4(1.0f), rt->Scale);
    const glm::mat4 toModel = glm::inverse(RootWorld()) * rigW;
    // The rig's hands and fingers in the body's model space (what every piece reaches for).
    struct Target { std::string Name; glm::quat Rot; };
    std::vector<Target> fingers;
    glm::mat4 handTarget[2];
    bool haveHand[2] = {false, false};
    for (int s = 0; s < 2; ++s) {
        glm::mat4 hand(1.0f);
        if (!rig.NodeTransform(FPBody::kBoneHand[s], hand)) continue;
        handTarget[s] = toModel * hand;
        haveHand[s] = true;
        const int rigHand = rig.NodeIndex(FPBody::kBoneHand[s]);
        for (int i = rigHand + 1; i < rig.NodeCount(); ++i) {
            int p = rig.NodeParent(i);
            while (p > rigHand) p = rig.NodeParent(p);
            if (p != rigHand) continue;
            glm::mat4 g(1.0f);
            if (rig.NodeTransform(rig.NodeName(i), g)) fingers.push_back({rig.NodeName(i), IK::Rotation(toModel * g)});
        }
    }
    std::vector<int> parents;
    std::vector<glm::mat4> globals;
    for (const auto& mp : m_Models) {
        Model& m = *mp;
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        if (m.NodeIndex(FPBody::kBoneHand[0]) < 0 && m.NodeIndex(FPBody::kBoneHand[1]) < 0) continue; // no arms here
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[(size_t)i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        for (int s = 0; s < 2; ++s) {
            if (!haveHand[s]) continue;
            const int up = m.NodeIndex(FPBody::kBoneUpperArm[s]), lo = m.NodeIndex(FPBody::kBoneLowerArm[s]), end = m.NodeIndex(FPBody::kBoneHand[s]);
            if (up < 0 || lo < 0 || end < 0) continue;
            const glm::quat rot = IK::Rotation(handTarget[s]);
            IK::SolveTwoBone(pose, parents, globals, up, lo, end, glm::vec3(handTarget[s][3]), &rot, weight);
        }
        // Fingers: each takes the rig finger's model-space rotation (its position stays on its own bone).
        for (const Target& f : fingers) {
            const int i = m.NodeIndex(f.Name);
            if (i < 0) continue;
            const int par = parents[(size_t)i];
            const glm::quat parentRot = par >= 0 ? IK::Rotation(globals[(size_t)par]) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            const glm::quat local = glm::normalize(glm::inverse(parentRot) * f.Rot);
            pose[(size_t)i].R = glm::slerp(pose[(size_t)i].R, local, weight);
            IK::RefreshGlobals(pose, parents, globals, i);
        }
        m.ApplyLocalPose(pose);
    }
}

void NpcBody::Flinch(World& world, const glm::vec3& dirWorld) {
    if (!IsActive() || m_PoseExternal || !world.Registry.valid(m_Driver)) return;
    auto& ac = world.Registry.get<AnimatorControllerComponent>(m_Driver);
    glm::vec2 push = FirstPersonBodyLocalMove(glm::vec3(dirWorld.x, 0.0f, dirWorld.z), m_Yaw);
    if (glm::length(push) < 1e-4f) push = glm::vec2(0.0f, -1.0f);
    push = glm::normalize(push);
    ac.SetFloat("HitX", push.x);
    ac.SetFloat("HitY", push.y);
    ac.SetTrigger("Hit");
}

bool NpcBody::BoneWorld(const World& world, const std::string& bone, glm::vec3& out) const {
    (void)world;
    return BoneWorld(bone, out);
}

bool NpcBody::BoneWorld(const std::string& bone, glm::vec3& out) const {
    if (!m_DriverModel) return false;
    glm::mat4 g(1.0f);
    if (!m_DriverModel->NodeTransform(bone, g)) return false;
    out = glm::vec3(RootWorld() * g[3]);
    return true;
}

std::string NpcBody::StateName(const World& world) const {
    if (m_Driver == entt::null || !world.Registry.valid(m_Driver)) return {};
    const auto* ac = world.Registry.try_get<AnimatorControllerComponent>(m_Driver); // gone once ragdolled
    return ac ? ac->StateName : std::string("ragdoll");
}
