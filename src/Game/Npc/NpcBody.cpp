#include "NpcBody.h"

#include "Components.h"
#include "FirstPersonBody.h"         // FirstPersonBodyWrapAngle / Follow / FootPelvis
#include "FirstPersonBodyContract.h" // FPBody:: parameter, state and bone names
#include "GameModuleAPI.h"           // RaycastHit, QueryFilter (the foot pass)
#include "IK.h"
#include "Log.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "World.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace {

float Follow(float dt, float timeConstant) { return FirstPersonBodyFollow(dt, timeConstant, 0.0f); }

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

} // namespace

float NpcYawOf(const glm::vec3& dir, float fallback) {
    if (dir.x * dir.x + dir.z * dir.z < 1e-8f) return fallback;
    return std::atan2(dir.x, dir.z);
}

float NpcSpineTwist(float offset, float maxTwist) {
    return std::clamp(FirstPersonBodyWrapAngle(offset), -maxTwist, maxTwist);
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
        if (!rc || !rc->ModelRef || rc->ModelRef->NodeCount() <= 0) continue; // a rigged piece (no Animator Controller needed: Phase 6)
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
    m_Root = root;
    // The body stands where its root was put (the spawn) until its first Tick says otherwise: a replacement spawned after the
    // frame's moves is posed (LateUpdate) before it ever ticks, and from the default origin its eye, hands and hitboxes came
    // out metres off for that frame.
    {
        const glm::mat4 w = world.ComposeWorldTransform(root);
        m_Feet = glm::vec3(w[3]);
        m_Yaw = NpcYawOf(glm::vec3(w[2]), 0.0f);
    }
    for (size_t k = 0; k < m_Pieces.size(); ++k)
        if (m_Pieces[k] == m_Driver) m_DriverIndex = (int)k;
    {
        const Model& d = *m_DriverModel;
        m_DriverParents.resize((size_t)d.NodeCount());
        for (int i = 0; i < d.NodeCount(); ++i) m_DriverParents[(size_t)i] = d.NodeParent(i);
        m_DriverNeck = d.NodeIndex("neck_01");
        m_DriverHead = d.NodeIndex("head");
        m_DriverSpineCount = 0;
        for (int k = 0; k < 5; ++k)
            if (const int b = d.NodeIndex(FPBody::kBoneSpine[k]); b >= 0) { m_DriverSpineSlot[m_DriverSpineCount] = k; m_DriverSpine[m_DriverSpineCount++] = b; }
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
            m_DriverBall[s] = d.NodeIndex(s == 0 ? "ball_l" : "ball_r");
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
    // Motion matching: the player's database and driver. (A piece that still carries an Animator Controller - a prefab from before
    // Phase 6 - is held: the matcher poses every piece.) Without the database (CI has no licensed assets) the dummy still comes:
    // it stands in its bind pose, warned once - never an error.
    m_Mm.SetDebug(&m_MmDebug);
    std::string error;
    if (m_Mm.Start(m_Models, (size_t)m_DriverIndex, error)) {
        m_MmActive = true;
        for (entt::entity e : m_Pieces)
            if (auto* ac = reg.try_get<AnimatorControllerComponent>(e)) ac->SkipUpdate = true;
    } else {
        static bool warned = false;
        if (!warned) Log::Warn("Dummies: " + error + " - they stand in their bind pose.");
        warned = true;
    }
    return true;
}

void NpcBody::Stop() {
    // The tuning stays (SetHoldSettings comes before Start, which stops first): only the body's state starts over.
    const NpcHoldSettings set = m_Set;
    *this = NpcBody{};
    m_Set = set;
}

glm::mat4 NpcBody::RootWorld() const {
    return glm::translate(glm::mat4(1.0f), m_Feet) * glm::mat4_cast(YawRotation(m_Yaw));
}

bool NpcBody::FootHeights(float (&out)[2]) const {
    if (!IsActive() || !m_DriverModel) return false;
    for (int s = 0; s < 2; ++s) {
        glm::mat4 g(1.0f);
        if (m_DriverLeg[s][2] < 0 || !m_DriverModel->NodeTransformAt(m_DriverLeg[s][2], g)) return false;
        out[s] = g[3].y; // the driver's model space stands on the feet, unrotated in height (RootWorld)
    }
    return true;
}

bool NpcBody::FootWorld(glm::vec3 (&out)[2]) const {
    if (!IsActive() || !m_DriverModel) return false;
    const glm::mat4 root = RootWorld();
    for (int s = 0; s < 2; ++s) {
        glm::mat4 g(1.0f);
        if (m_DriverLeg[s][2] < 0 || !m_DriverModel->NodeTransformAt(m_DriverLeg[s][2], g)) return false;
        out[s] = glm::vec3(root * g[3]);
    }
    return true;
}

void NpcBody::Tick(World& world, const NpcBodyInput& in, float dt) {
    (void)dt;
    if (!IsActive() || m_PoseExternal) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Driver) || !reg.valid(m_Root)) return;
    m_In = in;
    m_Feet = in.Feet;
    // The body stands on the capsule at its heading; the matcher steps in LateUpdate (StepMotionMatching), on the frames the
    // animation LOD poses, over the time since its last step.
    if (!m_HaveYaw) { m_Yaw = in.FacingYaw; m_HaveYaw = true; }
    world.SetWorldPose(m_Root, m_Feet, YawRotation(m_Yaw));
}

// Feet on uneven ground - a ramp, stairs, a slope - for a dummy near the player: a ray down under each animated foot,
// the pelvis dropped to the lower foot's ground, both legs re-solved to theirs, a planted foot tilted to its slope (the
// player body's foot IK, without its foot lock). Solved on the driver and handed to the other pieces; flat ground costs
// only the two rays. A hit flinch's `kick` goes on first, and a foot the clip has planted is solved back to where the clip has
// it (the kick bends the knee and hip instead of sliding or lifting the foot); a foot in the air keeps the kick.
void NpcBody::FootPass(float dt, const PoseKick& kick) {
    m_FootWeight += ((m_In.FootIK ? 1.0f : 0.0f) - m_FootWeight) * Follow(dt, m_Set.FootIKFade);
    const bool rigged = m_DriverPelvis >= 0 && m_DriverLeg[0][0] >= 0 && m_DriverLeg[0][1] >= 0 && m_DriverLeg[0][2] >= 0 &&
                        m_DriverLeg[1][0] >= 0 && m_DriverLeg[1][1] >= 0 && m_DriverLeg[1][2] >= 0;
    Model& m = *m_DriverModel;
    IK::Pose& pose = m_Pose;
    if (m_FootWeight < 1e-3f || !rigged) {
        m_HaveFootGround = false;
        m_Lock.Reset();
        if (kick) { // no foot pass (far off, out of view): the kick goes on as it is
            pose = m.AppliedLocalPose();
            if ((int)pose.size() == m.NodeCount()) {
                IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
                kick(m, pose, m_DriverParents, m_Globals);
                m.ApplyLocalPose(pose);
            }
        }
        return;
    }
    pose = m.AppliedLocalPose();
    if ((int)pose.size() != m.NodeCount()) return;
    IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
    const glm::mat4 rootW = RootWorld();
    const glm::mat3 toModel3 = glm::transpose(glm::mat3(rootW)); // a yaw: its inverse is its transpose
    // The kick (after the warp, so the upper body's turn back has happened and the kick's root-space axes hold for it). What it
    // moved each foot (model space), in proportion to how planted the clip has that foot, is taken back off the foot every step
    // below reads - lock, ground ray, target - so the lock never sees it and the leg solve puts the foot back.
    // (The foot's turn the same: a planted foot keeps the clip's angle on the ground.)
    glm::vec3 kickFoot[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    glm::quat unkickedRot[2] = {glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)};
    float kickPlanted[2] = {0.0f, 0.0f};
    bool kicked = false, legsKicked = false;
    if (kick) {
        glm::vec3 before[2];
        for (int s = 0; s < 2; ++s) {
            before[s] = IK::Position(m_Globals[(size_t)m_DriverLeg[s][2]]);
            unkickedRot[s] = IK::Rotation(m_Globals[(size_t)m_DriverLeg[s][2]]);
        }
        kick(m, pose, m_DriverParents, m_Globals);
        kicked = true;
        for (int s = 0; s < 2; ++s) {
            const float height = glm::vec3(rootW * glm::vec4(before[s], 1.0f)).y - m_Feet.y;
            kickPlanted[s] = 1.0f - std::clamp((height - 0.06f) / 0.09f, 0.0f, 1.0f);
            kickFoot[s] = (IK::Position(m_Globals[(size_t)m_DriverLeg[s][2]]) - before[s]) * kickPlanted[s];
            legsKicked = legsKicked || glm::dot(kickFoot[s], kickFoot[s]) > 1e-8f;
        }
    }
    const auto animFoot = [&](int s) { return IK::Position(m_Globals[(size_t)m_DriverLeg[s][2]]) - kickFoot[s]; }; // model space
    float animHeight[2];
    // Foot lock: a planted foot held where it landed in the world, as the player body's (IK::FootLock).
    glm::vec3 shift[2];
    bool held = false;
    const glm::quat facing = YawRotation(m_Yaw); // (rootW's turn)
    for (int s = 0; s < 2; ++s) {
        const glm::vec3 anim = glm::vec3(rootW * glm::vec4(animFoot(s), 1.0f));
        shift[s] = m_Lock.Step(s, m_Set.FootLock, m_Feet, facing, animFoot(s), anim.y - m_Feet.y, dt);
        held = held || glm::dot(shift[s], shift[s]) > 1e-8f;
    }
    for (int s = 0; s < 2; ++s) {
        const glm::vec3 foot = glm::vec3(rootW * glm::vec4(animFoot(s), 1.0f)) + shift[s];
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
    if (flat && !held && !legsKicked) { // the clips' own feet are right
        if (kicked) { m.ApplyLocalPose(pose); SyncLower(); }
        return;
    }
    const float w = m_FootWeight;
    const float pelvisDelta = (FirstPersonBodyFootPelvis(m_FootOffset[0], m_FootOffset[1], m_Set.FootIKMaxDrop, m_Set.FootIKPelvisRaise)) * w;
    IK::OffsetBone(pose, m_DriverParents, m_Globals, m_DriverPelvis, glm::vec3(0.0f, pelvisDelta, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f));
    for (int s = 0; s < 2; ++s) {
        const int thigh = m_DriverLeg[s][0], calf = m_DriverLeg[s][1], foot = m_DriverLeg[s][2];
        const glm::vec3 own = IK::Position(m_Globals[(size_t)foot]) - kickFoot[s] * w + glm::vec3(0.0f, m_FootOffset[s] * w - pelvisDelta, 0.0f);
        glm::vec3 target = own + toModel3 * shift[s] * w;
        // A held foot the hips have walked on from lets go and steps once its leg would need more than the lock's Reach
        // (IK::FootLock::Overreach; the hip as the pelvis now stands); and the leg is never asked for more than it has
        // (IK::ClampReach, as the player's): this frame's goal gives a little toward the hip instead of the knee snapping straight.
        {
            const glm::vec3 hip = IK::Position(m_Globals[(size_t)thigh]);
            const float leg = glm::length(IK::Position(m_Globals[(size_t)calf]) - hip) +
                              glm::length(IK::Position(m_Globals[(size_t)foot]) - IK::Position(m_Globals[(size_t)calf]));
            if (leg > 1e-5f) {
                m_Lock.Overreach(s, m_Set.FootLock, glm::length(target - hip) / leg, glm::length(own - hip) / leg);
                target = IK::ClampReach(hip, target, leg);
            }
        }
        // A planted foot lies on its slope; one swinging through the air keeps the clip's angle.
        const float planted = 1.0f - std::clamp((animHeight[s] - 0.06f) / 0.09f, 0.0f, 1.0f);
        const glm::vec3 normal = toModel3 * m_FootNormal[s];
        const float angle = std::min(std::acos(std::clamp(normal.y, -1.0f, 1.0f)), m_Set.FootIKTiltMax) * planted * w;
        glm::quat footRot = IK::Rotation(m_Globals[(size_t)foot]);
        if (kickPlanted[s] > 0.0f) footRot = glm::slerp(footRot, unkickedRot[s], kickPlanted[s] * w); // (no kick: the same turn)
        const glm::vec3 axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal);
        if (angle > 1e-4f && glm::dot(axis, axis) > 1e-8f) footRot = glm::angleAxis(angle, glm::normalize(axis)) * footRot;
        // A leg the clip has (nearly) straight has no bend plane of its own: bent by the solve, its knee would go whichever
        // way that frame's rounding said - forward one frame, sideways the next (a twitch). Near straight, it bends toward
        // the toes.
        IK::TwoBoneHint hint;
        {
            const glm::vec3 hip = IK::Position(m_Globals[(size_t)thigh]), knee = IK::Position(m_Globals[(size_t)calf]),
                            ankle = IK::Position(m_Globals[(size_t)foot]);
            const float leg = glm::length(knee - hip) + glm::length(ankle - knee);
            const float straight = leg > 1e-4f ? glm::length(ankle - hip) / leg : 0.0f;
            const float u = std::clamp((straight - 0.97f) / 0.025f, 0.0f, 1.0f);
            glm::vec3 toes(0.0f, 0.0f, 1.0f); // the model faces +Z
            if (m_DriverBall[s] >= 0) {
                const glm::vec3 d = IK::Position(m_Globals[(size_t)m_DriverBall[s]]) - ankle;
                if (d.x * d.x + d.z * d.z > 1e-6f) toes = glm::normalize(glm::vec3(d.x, 0.0f, d.z));
            }
            // (The knee out toward the toes: cross(hip->ankle, hip->knee) for a knee bulging that way.)
            hint.BendAxis = glm::cross(ankle - hip, toes);
            hint.BendAxisWeight = u * u * (3.0f - 2.0f * u);
        }
        IK::SolveTwoBone(pose, m_DriverParents, m_Globals, thigh, calf, foot, target, &footRot, 1.0f, 0.0f, &hint);
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

void NpcBody::LateUpdate(World& world, float dt, const PoseKick& kick) {
    if (!IsActive() || m_PoseExternal || !m_DriverModel) return;
    if (m_MmActive) StepMotionMatching(world, dt);
    FootPass(dt, kick);
    const glm::mat4 rootW = RootWorld();
    // The look, measured from the chest: a twist toward it (not while turning on the spot), sprung.
    glm::vec3 chestW = m_Feet + glm::vec3(0.0f, 1.4f, 0.0f);
    {
        glm::mat4 g(1.0f);
        if (m_DriverModel->NodeTransform(FPBody::kBoneSpine[4], g) || m_DriverModel->NodeTransform(FPBody::kBoneSpine[2], g))
            chestW = glm::vec3(rootW * g[3]);
    }
    float wantTwist = 0.0f;
    if (!m_Turning) wantTwist = NpcSpineTwist(NpcYawOf(m_In.LookPoint - chestW, m_Yaw) - m_Yaw, 0.6f);
    Spring(m_LookTwist, m_LookTwistRate, wantTwist, 13.0f, 0.75f, dt);
    // ... spread over the driver's spine; SyncPieces hands it to the other pieces (one skeleton).
    if (std::abs(m_LookTwist) > 1e-4f) {
        Model& m = *m_DriverModel;
        IK::Pose& pose = m_Pose;
        pose = m.AppliedLocalPose();
        if (m_DriverSpineCount > 0 && (int)pose.size() == m.NodeCount()) {
            IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
            OffsetSpine([&](float d) { return glm::angleAxis(m_LookTwist / d, glm::vec3(0, 1, 0)); });
            m.ApplyLocalPose(pose);
        }
    }
    // The head's own look past where the chest faces, split between the neck and the head so the turn bends rather than
    // pivots at the skull.
    if (m_DriverNeck >= 0 && m_DriverHead >= 0) {
        glm::mat4 hd(1.0f);
        float lookYaw = 0.0f, lookPitch = 0.0f;
        if (m_DriverModel->NodeTransform("head", hd)) {
            const glm::vec3 d = glm::vec3(glm::inverse(rootW) * glm::vec4(m_In.LookPoint, 1.0f)) - glm::vec3(hd[3]);
            const float flat = std::sqrt(d.x * d.x + d.z * d.z);
            if (flat > 0.2f || std::abs(d.y) > 0.2f) {
                lookYaw = std::clamp(FirstPersonBodyWrapAngle(NpcYawOf(d, 0.0f) - m_LookTwist), -m_Set.HeadMaxYaw, m_Set.HeadMaxYaw);
                lookPitch = std::clamp(std::atan2(d.y, std::max(flat, 0.05f)), -m_Set.HeadMaxPitch, m_Set.HeadMaxPitch);
            }
        }
        Spring(m_HeadYaw, m_HeadYawRate, lookYaw, 11.0f, 0.85f, dt);
        Spring(m_HeadPitch, m_HeadPitchRate, lookPitch, 11.0f, 0.85f, dt);
        if (std::abs(m_HeadYaw) > 1e-3f || std::abs(m_HeadPitch) > 1e-3f) {
            IK::Pose& pose = m_Pose;
            pose = m_DriverModel->AppliedLocalPose();
            if ((int)pose.size() == m_DriverModel->NodeCount()) {
                IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
                // Turn, then nod about the head's own side axis as turned: looking up is a negative turn about it.
                const glm::vec3 side = glm::angleAxis(m_LookTwist + m_HeadYaw, glm::vec3(0, 1, 0)) * glm::vec3(1, 0, 0);
                const glm::quat look = glm::angleAxis(-m_HeadPitch, side) * glm::angleAxis(m_HeadYaw, glm::vec3(0, 1, 0));
                const glm::quat neckShare = glm::slerp(glm::quat(1, 0, 0, 0), look, 0.4f);
                const glm::quat headShare = glm::slerp(glm::quat(1, 0, 0, 0), look, 0.6f);
                IK::OffsetBone(pose, m_DriverParents, m_Globals, m_DriverNeck, glm::vec3(0.0f), neckShare, IK::Position(m_Globals[(size_t)m_DriverNeck]));
                IK::OffsetBone(pose, m_DriverParents, m_Globals, m_DriverHead, glm::vec3(0.0f), headShare, IK::Position(m_Globals[(size_t)m_DriverHead]));
                m_DriverModel->ApplyLocalPose(pose);
            }
        }
    }
    // Every frame, not only after a pass of ours moved the driver: a piece whose rig doesn't rest as the torso's (the
    // first-person arms, under an extra Armature node) can't copy the torso's pose and samples the clips on its own, and
    // that drifts off the torso (its shoulders 30-40 cm out on the move). The torso's pose is the one skeleton.
    SyncLower();
    SyncPieces();
    glm::mat4 head(1.0f);
    m_Eye = m_DriverModel->NodeTransform("head", head) ? glm::vec3(rootW * head[3]) : m_Feet + glm::vec3(0.0f, 1.62f, 0.0f);
}

void NpcBody::StepMotionMatching(World& world, float dt) {
    if (!(dt > 0.0f)) return;
    static const FirstPersonBodyComponent kTune{};
    FirstPersonBodyMotion::Input mi;
    mi.Dt = dt;
    mi.Feet = m_In.Feet;
    mi.Velocity = m_In.Velocity;
    mi.Wish = glm::vec3(m_In.Wish.x, 0.0f, m_In.Wish.z);
    // The facing wanted: the way it goes while moving; standing, as asked (the look), past the Turn Threshold.
    mi.ViewYaw = glm::length(mi.Wish) > 0.1f ? NpcYawOf(mi.Wish, m_Yaw) : m_In.FacingYaw;
    mi.Grounded = m_In.Grounded;
    mi.Sprint = m_In.Sprint;
    mi.Style = m_In.Style;
    mi.TurnThreshold = m_Set.TurnThreshold;
    mi.TurnEndAngle = m_Set.TurnEndAngle >= 0.0f ? m_Set.TurnEndAngle : glm::radians(std::max(kTune.TurnEndAngle, 0.0f));
    // The capsule's ease to pace (an exponential, time constant AccelTime) as the predictor's spring - as the player's.
    mi.VelocityHalflife = std::max(m_In.AccelTime, 0.02f) * 0.6931f;
    mi.Path = m_In.Path;
    mi.PathCount = std::clamp(m_In.PathCount, 0, 4);
    mi.ArriveTime = m_In.PathArrives ? std::max(m_In.DecelTime, 0.05f) : 0.0f;
    m_Mm.Tick(mi, m_Yaw);
    m_Turning = m_Mm.Turning();
    ++m_MmUpdates;
    world.SetWorldPose(m_Root, m_Feet, YawRotation(m_Yaw));
    m_Mm.Pose(m_Models);
}

void NpcBody::SyncPieces() {
    if (!m_DriverModel) return;
    const auto& src = m_DriverModel->AppliedLocalPose();
    if ((int)src.size() != m_DriverModel->NodeCount()) return;
    for (size_t k = 0; k < m_Models.size() && k < m_UpperMap.size(); ++k) {
        if ((int)k == m_DriverIndex || m_UpperMap[k].empty() || !m_Models[k]) continue;
        if (!PieceTakes(k, kSkinsSpine | kSkinsArms | kSkinsNeck)) continue;
        Model& m = *m_Models[k];
        IK::Pose& pose = m_Pose;
        pose = m.AppliedLocalPose();
        if ((int)pose.size() != m.NodeCount()) continue;
        // The spine's translations too (its stabilization cancels the pelvis's sway), so clothing follows the torso.
        for (const auto& [pn, dn] : m_UpperMap[k]) {
            pose[(size_t)pn].R = src[(size_t)dn].R;
            if (std::find(m_DriverSpine, m_DriverSpine + m_DriverSpineCount, dn) != m_DriverSpine + m_DriverSpineCount)
                pose[(size_t)pn].T = src[(size_t)dn].T;
        }
        m.ApplyLocalPose(pose);
    }
}

void NpcBody::OffsetSpine(const std::function<glm::quat(float)>& stepFor) {
    // Each bone's share of the turn: even (the bone count) with the default Spine distribution.
    float divisor[8], weight[8];
    const int n = std::min(m_DriverSpineCount, 5);
    for (int k = 0; k < n; ++k) weight[k] = m_Set.Spine.Weight[std::min(m_DriverSpineSlot[k], 4)];
    IK::ChainDivisors(weight, n, divisor);
    const float pelvisAlpha = std::clamp(m_Set.Spine.PelvisAlpha, 0.0f, 1.0f);
    if (pelvisAlpha > 0.0f && m_DriverPelvis >= 0 && m_DriverSpineCount > 0) { // the pelvis takes its share first
        IK::RefreshPath(m_Pose, m_DriverParents, m_Globals, -1, m_DriverPelvis);
        IK::OffsetBoneOnly(m_Pose, m_DriverParents, m_Globals, m_DriverPelvis, glm::vec3(0.0f), stepFor(1.0f / pelvisAlpha),
                           IK::Position(m_Globals[(size_t)m_DriverPelvis]));
        IK::RefreshPath(m_Pose, m_DriverParents, m_Globals, m_DriverPelvis, m_DriverSpine[m_DriverSpineCount - 1]);
    }
    // Down the chain, each bone's own global is made current before the next is read; everything below (arms, fingers) is
    // refreshed once at the end instead of after every bone.
    for (int k = 0; k < m_DriverSpineCount; ++k) {
        const int b = m_DriverSpine[k];
        float d = k < n ? divisor[k] : (float)m_DriverSpineCount;
        if (pelvisAlpha > 0.0f) d /= 1.0f - pelvisAlpha;
        glm::quat step = stepFor(d);
        if (k < n) step = IK::ClampStepAngle(step, m_Set.Spine.MaxAngle[std::min(m_DriverSpineSlot[k], 4)]);
        IK::OffsetBoneOnly(m_Pose, m_DriverParents, m_Globals, b, glm::vec3(0.0f), step, IK::Position(m_Globals[(size_t)b]));
        if (k + 1 < m_DriverSpineCount) IK::RefreshPath(m_Pose, m_DriverParents, m_Globals, b, m_DriverSpine[k + 1]);
    }
    if (m_DriverSpineCount > 0) IK::RefreshGlobals(m_Pose, m_DriverParents, m_Globals, m_DriverSpine[0]);
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
    if (m_PoseExternal) return "ragdoll"; // (alive and going down, or dead: the physics has the pose)
    if (m_Driver == entt::null || !world.Registry.valid(m_Driver)) return {};
    return m_MmActive ? FPBody::kStateMotionMatching : "bind pose";
}
