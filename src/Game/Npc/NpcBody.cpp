#include "NpcBody.h"

#include "Camera.h"
#include "Components.h"
#include "FirstPersonBody.h"         // FirstPersonBodyLocalMove / WrapAngle
#include "FirstPersonBodyContract.h" // FPBody:: parameter, state and bone names
#include "GameModuleAPI.h"           // RaycastHit, QueryFilter (the foot pass)
#include "IK.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "World.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace {

float Follow(float dt, float timeConstant) {
    return timeConstant > 0.0f ? 1.0f - std::exp(-dt / timeConstant) : 1.0f;
}

// A damped spring toward `target` (semi-implicit, in steps no longer than 1/60 s): `zeta` under 1 lets the
// body carry a touch past where it turns to and settle back, which reads as weight rather than a camera.
void Spring(float& x, float& rate, float target, float omega, float zeta, float dt) {
    for (float left = std::min(dt, 0.1f); left > 1e-6f; left -= 1.0f / 60.0f) {
        const float h = std::min(left, 1.0f / 60.0f);
        rate += (omega * omega * (target - x) - 2.0f * zeta * omega * rate) * h;
        x += rate * h;
    }
}

glm::quat YawRotation(float yaw) { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }

// NPC body tuning is now in NpcHoldSettings (copied from FirstPersonBodyComponent in NpcDirector::Start).
// Replaced: kTurnThreshold → m_Set.TurnThreshold, kMaxTwist → m_Set.MaxTwist, kMoveEase → m_Set.MoveEase,
// kFaceEase → m_Set.FaceEase, kAimLean → m_Set.AimLean, kAimLeanCrouched → m_Set.AimLeanCrouched,
// kReadyLeanCrouched → m_Set.ReadyLeanCrouched, kHeadMaxYaw → m_Set.HeadMaxYaw,
// kHeadMaxPitch → m_Set.HeadMaxPitch, kCowerHunch → m_Set.CowerHunch

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
        ac.CopyDriverPose = e != m_Driver; // one skeleton: sample the clips once, on the torso
        ac.RootMotion.Mode = (int)RootMotionMode::InPlace;
    }
    m_Root = root;
    for (size_t k = 0; k < m_Pieces.size(); ++k)
        if (m_Pieces[k] == m_Driver) m_DriverIndex = (int)k;
    {
        const Model& d = *m_DriverModel;
        m_DriverParents.resize((size_t)d.NodeCount());
        for (int i = 0; i < d.NodeCount(); ++i) m_DriverParents[(size_t)i] = d.NodeParent(i);
        m_DriverNeck = d.NodeIndex("neck_01");
        m_DriverNeck2 = d.NodeIndex("neck_02");
        m_DriverHead = d.NodeIndex("head");
        m_DriverSpineCount = 0;
        for (int k = 0; k < 5; ++k)
            if (const int b = d.NodeIndex(FPBody::kBoneSpine[k]); b >= 0) m_DriverSpine[m_DriverSpineCount++] = b;
        // The upper body: spine_01 and everything under it, mapped onto each piece by name.
        std::vector<char> upper((size_t)d.NodeCount(), 0);
        if (const int s = d.NodeIndex(FPBody::kBoneSpine[0]); s >= 0) upper[(size_t)s] = 1;
        for (int i = 0; i < d.NodeCount(); ++i)
            if (!upper[(size_t)i] && d.NodeParent(i) >= 0 && upper[(size_t)d.NodeParent(i)]) upper[(size_t)i] = 1;
        m_UpperMap.assign(m_Models.size(), {});
        for (size_t k = 0; k < m_Models.size(); ++k) {
            if ((int)k == m_DriverIndex) continue;
            const Model& m = *m_Models[k];
            for (int i = 0; i < d.NodeCount(); ++i)
                if (upper[(size_t)i])
                    if (const int pn = m.NodeIndex(d.NodeName(i)); pn >= 0) m_UpperMap[k].push_back({pn, i});
        }
        // The lower body for the foot pass: the pelvis and everything under it that isn't the upper body (the legs).
        m_DriverPelvis = d.NodeIndex(FPBody::kBonePelvis);
        for (int s = 0; s < 2; ++s) {
            m_DriverLeg[s][0] = d.NodeIndex(FPBody::kBoneThigh[s]);
            m_DriverLeg[s][1] = d.NodeIndex(FPBody::kBoneCalf[s]);
            m_DriverLeg[s][2] = d.NodeIndex(FPBody::kBoneFoot[s]);
        }
        std::vector<char> lower((size_t)d.NodeCount(), 0);
        if (m_DriverPelvis >= 0) lower[(size_t)m_DriverPelvis] = 1;
        for (int i = 0; i < d.NodeCount(); ++i)
            if (!lower[(size_t)i] && !upper[(size_t)i] && d.NodeParent(i) >= 0 && lower[(size_t)d.NodeParent(i)]) lower[(size_t)i] = 1;
        m_LowerMap.assign(m_Models.size(), {});
        for (size_t k = 0; k < m_Models.size(); ++k) {
            if ((int)k == m_DriverIndex) continue;
            const Model& m = *m_Models[k];
            for (int i = 0; i < d.NodeCount(); ++i)
                if (lower[(size_t)i])
                    if (const int pn = m.NodeIndex(d.NodeName(i)); pn >= 0) m_LowerMap[k].push_back({pn, i});
        }
    }
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
        m_Yaw += offset * Follow(dt, m_Set.FaceEase);
    } else {
        m_StillTime += dt;
        if (m_Turning) {
            m_TurnTime += dt;
            if (ac.InState(FPBody::kStateTurn) || ac.InState(FPBody::kStateCrouchTurn))
                m_Yaw += glm::radians(ac.RootMotion.DeltaYaw);
            offset = FirstPersonBodyWrapAngle(want - m_Yaw);
            const bool clipDone = m_TurnTime > 0.25f && !ac.InState(FPBody::kStateTurn) && !ac.InState(FPBody::kStateCrouchTurn);
            if (std::abs(offset) < glm::radians(8.0f) || clipDone || m_TurnTime > 1.6f) m_Turning = false;
        } else if (m_StillTime > 0.15f && NpcShouldTurn(offset, m_Set.TurnThreshold)) {
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
        const float maxLag = m_Set.TurnThreshold + 0.6f;
        if (std::abs(lag) > maxLag) m_Yaw = want - std::copysign(maxLag, lag);
    }
    m_Yaw = FirstPersonBodyWrapAngle(m_Yaw);
    world.SetWorldPose(m_Root, m_Feet, YawRotation(m_Yaw));

    // The blend tree's parameters are the clips' own speeds, which the soldier's gaits are tuned to.
    const glm::vec2 local = FirstPersonBodyLocalMove(flat, m_Yaw);
    m_Move += (local - m_Move) * Follow(dt, m_Set.MoveEase);
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

// Feet on uneven ground - a ramp, stairs, a slope - for a soldier near the player: a ray down under each animated foot,
// the pelvis dropped to the lower foot's ground, both legs re-solved to theirs, a planted foot tilted to its slope (the
// player body's foot IK, without its foot lock). Solved on the driver and handed to the other pieces; flat ground costs
// only the two rays.
void NpcBody::FootPass(float dt) {
    m_FootWeight += ((m_In.FootIK ? 1.0f : 0.0f) - m_FootWeight) * Follow(dt, m_Set.FootIKFade);
    const bool rigged = m_DriverPelvis >= 0 && m_DriverLeg[0][0] >= 0 && m_DriverLeg[0][1] >= 0 && m_DriverLeg[0][2] >= 0 &&
                        m_DriverLeg[1][0] >= 0 && m_DriverLeg[1][1] >= 0 && m_DriverLeg[1][2] >= 0;
    if (m_FootWeight < 1e-3f || !rigged) {
        m_HaveFootGround = false;
        return;
    }
    Model& m = *m_DriverModel;
    IK::Pose& pose = m_Pose;
    pose = m.AppliedLocalPose();
    if ((int)pose.size() != m.NodeCount()) return;
    IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
    const glm::mat4 rootW = RootWorld();
    const glm::mat3 toModel3 = glm::transpose(glm::mat3(rootW)); // a yaw: its inverse is its transpose
    float animHeight[2];
    for (int s = 0; s < 2; ++s) {
        const glm::vec3 foot = glm::vec3(rootW * glm::vec4(IK::Position(m_Globals[(size_t)m_DriverLeg[s][2]]), 1.0f));
        animHeight[s] = foot.y - m_Feet.y;
        float offset = 0.0f;
        glm::vec3 normal(0.0f, 1.0f, 0.0f);
        const float origin[3] = {foot.x, foot.y + 0.45f, foot.z}, down[3] = {0.0f, -1.0f, 0.0f};
        QueryFilter filter;
        filter.HitTriggers = 0;
        RaycastHit hit;
        if (PhysicsWorld::RaycastSolid(origin, down, 0.45f + m_Set.FootIKMaxDrop + 0.2f, filter, hit) && hit.Hit) {
            offset = std::clamp(hit.Point[1] - m_Feet.y, -m_Set.FootIKMaxDrop, m_Set.FootIKMaxRaise);
            normal = glm::normalize(glm::vec3(hit.Normal[0], hit.Normal[1], hit.Normal[2]));
            if (normal.y < 0.5f) normal = glm::vec3(0.0f, 1.0f, 0.0f); // a wall, not a floor
        }
        if (!m_HaveFootGround) { m_FootOffset[s] = offset; m_FootNormal[s] = normal; }
        m_FootOffset[s] += (offset - m_FootOffset[s]) * Follow(dt, m_Set.FootOffsetEase);
        m_FootNormal[s] = glm::normalize(m_FootNormal[s] + (normal - m_FootNormal[s]) * Follow(dt, m_Set.FootNormalEase));
    }
    m_HaveFootGround = true;
    const bool flat = std::abs(m_FootOffset[0]) < 0.004f && std::abs(m_FootOffset[1]) < 0.004f && m_FootNormal[0].y > 0.999f &&
                      m_FootNormal[1].y > 0.999f;
    if (flat) return; // the clips' own feet are right
    const float w = m_FootWeight;
    const float pelvisDelta = FirstPersonBodyFootPelvis(m_FootOffset[0], m_FootOffset[1], m_Set.FootIKMaxDrop, m_Set.FootIKPelvisRaise) * w;
    IK::OffsetBone(pose, m_DriverParents, m_Globals, m_DriverPelvis, glm::vec3(0.0f, pelvisDelta, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f));
    for (int s = 0; s < 2; ++s) {
        const int thigh = m_DriverLeg[s][0], calf = m_DriverLeg[s][1], foot = m_DriverLeg[s][2];
        const glm::vec3 target = IK::Position(m_Globals[(size_t)foot]) + glm::vec3(0.0f, m_FootOffset[s] * w - pelvisDelta, 0.0f);
        // A planted foot lies on its slope; one swinging through the air keeps the clip's angle.
        const float planted = 1.0f - std::clamp((animHeight[s] - 0.06f) / 0.09f, 0.0f, 1.0f);
        const glm::vec3 normal = toModel3 * m_FootNormal[s];
        const float angle = std::min(std::acos(std::clamp(normal.y, -1.0f, 1.0f)), m_Set.FootIKTiltMax) * planted * w;
        glm::quat footRot = IK::Rotation(m_Globals[(size_t)foot]);
        const glm::vec3 axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal);
        if (angle > 1e-4f && glm::dot(axis, axis) > 1e-8f) footRot = glm::angleAxis(angle, glm::normalize(axis)) * footRot;
        IK::SolveTwoBone(pose, m_DriverParents, m_Globals, thigh, calf, foot, target, &footRot, 1.0f);
    }
    m.ApplyLocalPose(pose);
    SyncLower();
}

void NpcBody::SyncLower() {
    const auto& src = m_DriverModel->AppliedLocalPose();
    if ((int)src.size() != m_DriverModel->NodeCount()) return;
    for (size_t k = 0; k < m_Models.size() && k < m_LowerMap.size(); ++k) {
        if ((int)k == m_DriverIndex || m_LowerMap[k].empty() || !m_Models[k]) continue;
        Model& m = *m_Models[k];
        IK::Pose& pose = m_Pose;
        pose = m.AppliedLocalPose();
        if ((int)pose.size() != m.NodeCount()) continue;
        // The pelvis moves (its translation too); the legs turn.
        for (const auto& [pn, dn] : m_LowerMap[k]) {
            pose[(size_t)pn].R = src[(size_t)dn].R;
            if (dn == m_DriverPelvis) pose[(size_t)pn].T = src[(size_t)dn].T;
        }
        m.ApplyLocalPose(pose);
    }
}

void NpcBody::LateUpdate(World& world, float dt, const Camera* weaponCam) {
    (void)world;
    if (!IsActive() || m_PoseExternal || !m_DriverModel) return;
    FootPass(dt);
    const glm::mat4 rootW = RootWorld();
    {
        glm::mat4 l(1.0f), r(1.0f);
        m_HaveShouldersAnimated = m_DriverModel->NodeTransform(FPBody::kBoneUpperArm[0], l) && m_DriverModel->NodeTransform(FPBody::kBoneUpperArm[1], r);
        if (m_HaveShouldersAnimated) m_ShouldersAnimated = 0.5f * (glm::vec3(l[3]) + glm::vec3(r[3]));
    }
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
        wantTwist = NpcSpineTwist(NpcYawOf(d, m_Yaw) - m_Yaw, m_Set.MaxTwist);
    } else if (!m_Turning) {
        // Looking about while not aiming: the twist only (a soldier scanning, not bending over).
        const glm::vec3 d = m_In.LookPoint - chestW;
        wantTwist = NpcSpineTwist(NpcYawOf(d, m_Yaw) - m_Yaw, 0.6f);
    }
    Spring(m_AimPitch, m_AimPitchRate, wantPitch, 15.0f, 0.8f, dt);
    Spring(m_AimTwist, m_AimTwistRate, wantTwist, 13.0f, 0.75f, dt);
    m_Lean += (std::clamp(m_In.Lean, -1.0f, 1.0f) - m_Lean) * Follow(dt, 0.12f);
    m_Cower += (std::clamp(m_In.Cower, 0.0f, 1.0f) - m_Cower) * Follow(dt, m_In.Cower > m_Cower ? 0.06f : 0.25f);
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
            const float leanWant = m_In.Crouched ? glm::mix(m_Set.ReadyLeanCrouched, m_Set.AimLeanCrouched, m_AimWeight) : m_Set.AimLean;
            const float weight = armedCrouch ? 1.0f : m_AimWeight;
            straighten = std::clamp(leanWant - leanNow, -0.9f, 0.4f) * weight;
        }
        m_Straighten += (straighten - m_Straighten) * Follow(dt, 0.1f);
    }

    // ... spread over the driver's spine; SyncPieces hands it to the other pieces (one skeleton).
    {
        Model& m = *m_DriverModel;
        IK::Pose& pose = m_Pose;
        pose = m.AppliedLocalPose();
        const int n = m_DriverSpineCount;
        if (n > 0 && (int)pose.size() == m.NodeCount()) {
            IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
            // Rotating +Z about +X by theta gives (0, -sin, cos): looking up is a negative angle about X.
            // The lean rolls about the body's forward (+Z): + = the top toward -X, the body's right.
            const glm::quat step = glm::angleAxis(m_AimTwist / (float)n, glm::vec3(0, 1, 0)) *
                                   glm::angleAxis((m_Straighten - m_AimPitch + m_Set.CowerHunch * m_Cower) / (float)n, glm::vec3(1, 0, 0)) *
                                   glm::angleAxis(m_Lean * 0.38f / (float)n, glm::vec3(0, 0, 1));
            OffsetSpine(step);
            m.ApplyLocalPose(pose);
            m_PiecesStale = true;
        }
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

    // The head's own look: off the sights it turns to what the soldier looks at (a sound, a squadmate calling,
    // the next corner) past where the chest faces; on them the gun owns it (HoldWeapon's cheek weld). Split
    // between the neck and the head so the turn bends rather than pivots at the skull.
    if (m_DriverNeck >= 0 && m_DriverHead >= 0) {
        glm::mat4 hd(1.0f);
        float lookYaw = 0.0f, lookPitch = 0.0f;
        if (m_DriverModel->NodeTransform("head", hd)) {
            const glm::vec3 d = glm::vec3(glm::inverse(rootW) * glm::vec4(m_In.LookPoint, 1.0f)) - glm::vec3(hd[3]);
            const float flat = std::sqrt(d.x * d.x + d.z * d.z);
            if (flat > 0.2f || std::abs(d.y) > 0.2f) {
                lookYaw = std::clamp(FirstPersonBodyWrapAngle(NpcYawOf(d, 0.0f) - m_AimTwist), -m_Set.HeadMaxYaw, m_Set.HeadMaxYaw);
                lookPitch = std::clamp(std::atan2(d.y, std::max(flat, 0.05f)), -m_Set.HeadMaxPitch, m_Set.HeadMaxPitch);
            }
        }
        const float free = 1.0f - std::clamp(m_AimWeight, 0.0f, 1.0f);
        lookYaw *= free;
        lookPitch = lookPitch * free - 0.45f * m_Cower; // ducking: chin down
        Spring(m_HeadYaw, m_HeadYawRate, lookYaw, 11.0f, 0.85f, dt);
        Spring(m_HeadPitch, m_HeadPitchRate, lookPitch, 11.0f, 0.85f, dt);
        if (std::abs(m_HeadYaw) > 1e-3f || std::abs(m_HeadPitch) > 1e-3f) {
            IK::Pose& pose = m_Pose;
            pose = m_DriverModel->AppliedLocalPose();
            if ((int)pose.size() == m_DriverModel->NodeCount()) {
                IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
                // Turn, then nod about the head's own side axis as turned (chest twist and the turn): looking up is
                // a negative turn about it (see the spine above).
                const glm::vec3 side = glm::angleAxis(m_AimTwist + m_HeadYaw, glm::vec3(0, 1, 0)) * glm::vec3(1, 0, 0);
                const glm::quat look = glm::angleAxis(-m_HeadPitch, side) * glm::angleAxis(m_HeadYaw, glm::vec3(0, 1, 0));
                const glm::quat neckShare = glm::slerp(glm::quat(1, 0, 0, 0), look, 0.4f);
                const glm::quat headShare = glm::slerp(glm::quat(1, 0, 0, 0), look, 0.6f);
                IK::OffsetBone(pose, m_DriverParents, m_Globals, m_DriverNeck, glm::vec3(0.0f), neckShare, IK::Position(m_Globals[(size_t)m_DriverNeck]));
                IK::OffsetBone(pose, m_DriverParents, m_Globals, m_DriverHead, glm::vec3(0.0f), headShare, IK::Position(m_Globals[(size_t)m_DriverHead]));
                m_DriverModel->ApplyLocalPose(pose);
                m_PiecesStale = true;
            }
        }
    }

    // The head bone, where the weapon's camera goes (its arms rig is placed by the same bone).
    glm::mat4 head(1.0f);
    m_Eye = m_DriverModel->NodeTransform("head", head) ? glm::vec3(rootW * head[3]) : m_Feet + glm::vec3(0.0f, 1.62f, 0.0f);
}

void NpcBody::OffsetSpine(const glm::quat& step) {
    // Down the chain, each bone's own global is made current before the next is read; everything below (arms, fingers) is
    // refreshed once at the end instead of after every bone.
    for (int k = 0; k < m_DriverSpineCount; ++k) {
        const int b = m_DriverSpine[k];
        IK::OffsetBoneOnly(m_Pose, m_DriverParents, m_Globals, b, glm::vec3(0.0f), step, IK::Position(m_Globals[(size_t)b]));
        if (k + 1 < m_DriverSpineCount) IK::RefreshPath(m_Pose, m_DriverParents, m_Globals, b, m_DriverSpine[k + 1]);
    }
    if (m_DriverSpineCount > 0) IK::RefreshGlobals(m_Pose, m_DriverParents, m_Globals, m_DriverSpine[0]);
}

void NpcBody::Flinch(World& world, const glm::vec3& dirWorld, const glm::vec3* point, int part) {
    if (!IsActive() || m_PoseExternal || !world.Registry.valid(m_Driver)) return;
    auto& ac = world.Registry.get<AnimatorControllerComponent>(m_Driver);
    glm::vec2 push = FirstPersonBodyLocalMove(glm::vec3(dirWorld.x, 0.0f, dirWorld.z), m_Yaw);
    if (glm::length(push) < 1e-4f) push = glm::vec2(0.0f, -1.0f);
    push = glm::normalize(push);
    // Where it struck steers it too: an arm hit swings that shoulder back (the side the hit was on), a leg hit buckles
    // the stance (more along the body's length); a head or chest hit goes with the round's own line.
    if (point && part >= 0) {
        const glm::vec2 off = FirstPersonBodyLocalMove(glm::vec3(point->x - m_Feet.x, 0.0f, point->z - m_Feet.z), m_Yaw);
        if (part >= 3 && part <= 6 && std::abs(off.x) > 1e-3f) push.x += std::copysign(0.7f, off.x);
        else if (part >= 7) push.y += push.y >= 0.0f ? 0.5f : -0.5f;
        push = glm::normalize(push);
    }
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
