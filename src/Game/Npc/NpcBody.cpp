#include "NpcBody.h"

#include "Camera.h"
#include "Components.h"
#include "FirstPersonBody.h"         // FirstPersonBodyLocalMove / WrapAngle
#include "FirstPersonBodyContract.h" // FPBody:: parameter, state and bone names
#include "IK.h"
#include "Model.h"
#include "World.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
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
constexpr float kAimLean = 0.1f;         // radians (6 deg): the torso's forward lean aiming, standing
constexpr float kAimLeanCrouched = 0.22f; // ... and crouched (13 deg)
constexpr float kReadyLeanCrouched = 0.4f; // ... crouched at the low ready (23 deg)

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
    // The arms piece: the weapon hold's source (its shoulders and elbows are worked out on it).
    for (size_t k = 0; k < m_Pieces.size(); ++k)
        if (const auto* nc = reg.try_get<NameComponent>(m_Pieces[k])) {
            std::string name = nc->Name;
            for (char& c : name) c = (char)std::tolower((unsigned char)c);
            if (name == "arms") { m_ArmsIndex = (int)k; break; }
        }
    for (entt::entity e : m_Pieces) {
        auto& ac = reg.get<AnimatorControllerComponent>(e);
        ac.Driver = e == m_Driver ? entt::null : m_Driver;
        ac.RootMotion.Mode = (int)RootMotionMode::InPlace;
    }
    m_Root = root;
    m_HoldStagger = (int)(entt::to_integral(root) % 3u);
    m_PieceSkins.assign(m_Models.size(), 0u);
    for (size_t k = 0; k < m_Models.size(); ++k) {
        const Model& m = *m_Models[k];
        auto skinsUnder = [&](std::initializer_list<const char*> roots) {
            std::vector<char> under((size_t)m.NodeCount(), 0);
            for (const char* r : roots)
                if (const int i = m.NodeIndex(r); i >= 0) under[(size_t)i] = 1;
            for (int i = 0; i < m.NodeCount(); ++i) {
                if (!under[(size_t)i] && m.NodeParent(i) >= 0 && under[(size_t)m.NodeParent(i)]) under[(size_t)i] = 1;
                if (under[(size_t)i] && m.BoneId(m.NodeName(i)) >= 0) return true;
            }
            return false;
        };
        unsigned f = 0u;
        if (skinsUnder({FPBody::kBoneSpine[0]})) f |= kSkinsSpine;
        if (skinsUnder({FPBody::kBoneClavicle[0], FPBody::kBoneClavicle[1]})) f |= kSkinsArms;
        if (skinsUnder({"neck_01"})) f |= kSkinsNeck;
        m_PieceSkins[k] = f;
    }
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

void NpcBody::LateUpdate(World& world, float dt, const Camera* weaponCam) {
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
        // The chest takes the player's share of the aim's pitch (Spine Aim / Spine Aim Down); the gun, hung off
        // the shoulders on the weapon's camera, takes the rest.
        wantPitch *= std::clamp(wantPitch < 0.0f ? m_Set.SpineAimDown : m_Set.SpineAim, 0.0f, 1.0f);
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
    // Aiming, the torso comes up into a shooter's stance whatever the legs are doing: the clips' own lean (a
    // crouch walk hunches ~40 degrees, its head right over where the gun goes) is taken out, down to a slight
    // forward lean, and the aim's pitch goes on top of that rather than on top of the hunch.
    {
        glm::mat4 pelvis(1.0f), neck(1.0f);
        float straighten = 0.0f;
        if (m_DriverModel->NodeTransform(FPBody::kBonePelvis, pelvis) && m_DriverModel->NodeTransform("neck_01", neck)) {
            const glm::vec3 up = glm::vec3(neck[3]) - glm::vec3(pelvis[3]);
            const float leanNow = std::atan2(up.z, std::max(up.y, 1e-3f)); // + = forward
            // Crouched with a gun but not aiming (a low ready, moving between cover) the hunch is eased too, to a
            // ready crouch: the crouch walk's own was so deep the gun's stock rode up past the hood.
            const bool armedCrouch = m_In.Crouched && m_ArmsWeight > 0.5f;
            const float leanWant = m_In.Crouched ? glm::mix(kReadyLeanCrouched, kAimLeanCrouched, m_AimWeight) : kAimLean;
            const float weight = armedCrouch ? 1.0f : m_AimWeight;
            straighten = std::clamp(leanWant - leanNow, -0.9f, 0.4f) * weight;
        }
        m_Straighten += (straighten - m_Straighten) * Follow(dt, 0.1f);
    }

    // ... spread over the spine of every piece, so they stay one skeleton (each piece keeps its own
    // bones' rest frames; the rotation is the same in model space).
    std::vector<int> parents;
    std::vector<glm::mat4> globals;
    for (size_t pk = 0; pk < m_Models.size(); ++pk) {
        if (!PieceTakes(pk, kSkinsSpine)) continue;
        Model& m = *m_Models[pk];
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
                               glm::angleAxis((m_Straighten - m_AimPitch) / (float)n, glm::vec3(1, 0, 0)) *
                               glm::angleAxis(m_Lean * 0.38f / (float)n, glm::vec3(0, 0, 1));
        for (int k = 0; k < n; ++k) IK::OffsetBone(pose, parents, globals, spine[k], glm::vec3(0.0f), step, IK::Position(globals[(size_t)spine[k]]));
        m.ApplyLocalPose(pose);
    }

    // Armed, the chest takes the arms rig's stance: its shoulder line against the weapon's camera (the rig is
    // authored bladed, the left shoulder ahead of the right). Squared to the aim, the left shoulder sat behind the
    // rig's and the support hand came off the gun. The tilt is taken only by Clavicle Follow, the blade in full.
    const float lineMatch = std::clamp(m_Set.ShoulderLineMatch, 0.0f, 1.0f) * std::clamp(m_ArmsWeight, 0.0f, 1.0f);
    if (weaponCam && m_HaveRigLine && lineMatch > 1e-3f) {
        const glm::vec3 lineWorld = weaponCam->Right() * m_RigShoulderLine.x + weaponCam->Up() * m_RigShoulderLine.y +
                                    weaponCam->Front() * m_RigShoulderLine.z;
        const glm::vec3 lineModel = glm::inverse(YawRotation(m_Yaw)) * lineWorld;
        for (int pass = 0; pass < 2; ++pass) {
            glm::mat4 l(1.0f), r(1.0f);
            if (!m_DriverModel->NodeTransform(FPBody::kBoneUpperArm[0], l) || !m_DriverModel->NodeTransform(FPBody::kBoneUpperArm[1], r)) break;
            const glm::vec3 across = glm::vec3(l[3]) - glm::vec3(r[3]);
            const glm::vec3 target = FirstPersonBodyShoulderLineTilt(across, lineModel, m_Set.ClavicleFollow);
            const glm::quat turn = FirstPersonBodyShoulderLineTurn(across, target, lineMatch);
            if (2.0f * std::acos(std::clamp(std::abs(turn.w), 0.0f, 1.0f)) < 0.002f) break;
            RotateSpine(turn);
        }
    }

    // The head bone, where the weapon's camera goes (its arms rig is placed by the same bone).
    glm::mat4 head(1.0f);
    m_Eye = m_DriverModel->NodeTransform("head", head) ? glm::vec3(rootW * head[3]) : m_Feet + glm::vec3(0.0f, 1.62f, 0.0f);
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
