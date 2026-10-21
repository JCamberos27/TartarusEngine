#include "NpcBody.h"

#include "AnimatorController.h"      // AnimatorCrossfadeWeight
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
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/quaternion.hpp> // glm::rotation (the dragged leg)

#include <algorithm>
#include <cctype>
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

// npc_soldier.controller's wounded states (tools/gen_npc_soldier.py; NpcBody::TickWounded).
constexpr const char* kGoProne = "GoProne";          // Trigger: Any -> ProneDrop
constexpr const char* kGoDying = "GoDying";          // Trigger: Any -> InjuredIdle
constexpr const char* kCollapse = "Collapse";        // Trigger: the dying soldier goes down (InjuredDrop)
constexpr const char* kStateProneDrop = "ProneDrop"; // the collapse
constexpr const char* kStateProneCrawl = "ProneCrawl";
constexpr const char* kStateProneCrawlStart = "ProneCrawlStart";
constexpr const char* kStateInjuredWalk = "InjuredWalk";
constexpr const char* kStateInjuredWalkStart = "InjuredWalkStart";
constexpr const char* kStateInjuredDrop = "InjuredDrop";
constexpr const char* kStateInjuredLying = "InjuredLying";
constexpr float kProneTurnRate = 25.0f;  // degrees a second, on the belly
constexpr float kPronePivotRate = 60.0f; // ... hauling himself round on the spot off a wall (NpcBodyInput::Pivot)
constexpr float kSlideTurnRate = 400.0f; // degrees a second: the feet sliding round behind a facing past the spine's twist
constexpr float kDyingTurnRate = 70.0f;  // ... doubled over, staggering

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
    // The arms piece: the weapon hold's source (its shoulders and elbows are worked out on it). A long sleeve
    // leaves a modular body only its hands: then those (the same skeleton).
    m_ArmsIndex = -1;
    for (const char* want : {"arms", "hands"}) {
        for (size_t k = 0; k < m_Pieces.size() && m_ArmsIndex < 0; ++k)
            if (const auto* nc = reg.try_get<NameComponent>(m_Pieces[k])) {
                std::string name = nc->Name;
                for (char& c : name) c = (char)std::tolower((unsigned char)c);
                if (name == want) m_ArmsIndex = (int)k;
            }
        if (m_ArmsIndex >= 0) break;
    }
    for (entt::entity e : m_Pieces) {
        auto& ac = reg.get<AnimatorControllerComponent>(e);
        ac.Driver = e == m_Driver ? entt::null : m_Driver;
        ac.CopyDriverPose = e != m_Driver; // one skeleton: sample the clips once, on the torso
        ac.RootMotion.Mode = (int)RootMotionMode::InPlace;
    }
    m_Root = root;
    // The body stands where its root was put (the spawn) until its first Tick says otherwise: a replacement spawned after the
    // frame's moves is posed (LateUpdate) before it ever ticks, and from the default origin its eye, gun, hands and hitboxes came
    // out metres off for that frame - the gun drawn at the world's origin, the arms reaching for it.
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
        m_DriverNeck2 = d.NodeIndex("neck_02");
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
        }
        std::vector<char> lower((size_t)d.NodeCount(), 0);
        if (m_DriverPelvis >= 0) lower[(size_t)m_DriverPelvis] = 1;
        for (int i = 0; i < d.NodeCount(); ++i)
            if (!lower[(size_t)i] && !upper[(size_t)i] && d.NodeParent(i) >= 0 && lower[(size_t)d.NodeParent(i)]) lower[(size_t)i] = 1;
        std::vector<char> arm((size_t)d.NodeCount(), 0);
        for (int s = 0; s < 2; ++s)
            if (const int c = d.NodeIndex(FPBody::kBoneClavicle[s]); c >= 0) arm[(size_t)c] = 1;
        m_ArmNodes.clear();
        for (int i = 0; i < d.NodeCount(); ++i) {
            if (!arm[(size_t)i] && d.NodeParent(i) >= 0 && arm[(size_t)d.NodeParent(i)]) arm[(size_t)i] = 1;
            if (arm[(size_t)i]) m_ArmNodes.push_back(i);
        }
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

bool NpcBody::FootHeights(float (&out)[2]) const {
    if (!IsActive() || !m_DriverModel) return false;
    for (int s = 0; s < 2; ++s) {
        glm::mat4 g(1.0f);
        if (m_DriverLeg[s][2] < 0 || !m_DriverModel->NodeTransformAt(m_DriverLeg[s][2], g)) return false;
        out[s] = g[3].y; // the driver's model space stands on the feet, unrotated in height (RootWorld)
    }
    return true;
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
    if (in.Prone || in.Dying) {
        TickWounded(ac, in, flat, speed, dt);
        world.SetWorldPose(m_Root, m_Feet, YawRotation(m_Yaw));
        return;
    }
    const float want = in.HoldFacing || !moving ? in.FacingYaw : NpcYawOf(flat, m_Yaw);
    float offset = FirstPersonBodyWrapAngle(want - m_Yaw);

    const float stillBefore = m_StillTime;
    if (moving) {
        m_Turning = false;
        m_StillTime = 0.0f;
        if (ac.HasTag(FPBody::kTagStartTurn)) {
            // A start that turns: the body takes the clip's own yaw, up to the new heading.
            const float step = glm::radians(ac.RootMotion.DeltaYaw);
            if (step * offset > 0.0f) m_Yaw += std::copysign(std::min(std::abs(step), std::abs(offset)), step);
        } else {
            m_Yaw += offset * Follow(dt, m_Set.FaceEase);
        }
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
            // Never under 45 degrees of clip (each side's smallest), so the two sides' clips never mix.
            const float deg = std::clamp(glm::degrees(offset), -180.0f, 180.0f);
            ac.SetFloat(FPBody::kTurnAngle, std::copysign(std::max(std::abs(deg), 45.0f), deg));
        } else if (in.Aiming) {
            // Aiming from where it stands: the shoulders square up to the target (a small shuffle;
            // the turn clips take the big swings).
            m_Yaw += offset * Follow(dt, 0.18f);
        }
        // Never more than the spine can twist behind what it faces: the feet slide a little instead.
        const float lag = FirstPersonBodyWrapAngle(want - m_Yaw);
        const float maxLag = m_Set.TurnThreshold + 0.6f;
        // (At most a fast pivot's rate: set outright, a facing that jumped - a peek's look flipping across the cover - swung the
        // body up to 90 degrees in one frame, the head a metre round the feet.)
        if (std::abs(lag) > maxLag) {
            const float fix = lag - std::copysign(maxLag, lag), most = glm::radians(kSlideTurnRate) * dt;
            m_Yaw += std::clamp(fix, -most, most);
        }
    }
    m_Yaw = FirstPersonBodyWrapAngle(m_Yaw);
    world.SetWorldPose(m_Root, m_Feet, YawRotation(m_Yaw));

    // The blend tree's parameters are the clips' own speeds, which the soldier's gaits are tuned to.
    const glm::vec2 local = FirstPersonBodyLocalMove(flat, m_Yaw);
    m_Move += (local - m_Move) * Follow(dt, m_Set.MoveEase);
    ac.SetFloat(FPBody::kMoveX, m_Move.x);
    ac.SetFloat(FPBody::kMoveY, m_Move.y);
    ac.SetFloat(FPBody::kSpeed, glm::length(m_Move));
    // The gait loop at the rate its clips travel at the capsule's speed (a blend's shared cycle runs a walk slow), as the
    // player body's.
    {
        const bool loop = !ac.InTransition && (ac.InState(FPBody::kStateLocomotion) || ac.InState(FPBody::kStateCrouchLoco));
        const float travel = dt > 1e-5f ? glm::length(glm::vec2(ac.RootMotion.DeltaPosition.x, ac.RootMotion.DeltaPosition.z)) / dt : 0.0f;
        float trim = 1.0f;
        if (loop && travel > 0.3f && speed > 0.3f) trim = std::clamp(m_RateTrim * speed / travel, 0.8f, 1.5f);
        m_RateTrim += (trim - m_RateTrim) * Follow(dt, 0.15f);
    }
    ac.SetFloat(FPBody::kPlayRate, m_RateTrim);
    ac.SetFloat(FPBody::kSprintRate, std::clamp(speed / FPBody::kSprintClipSpeed, 0.8f, 1.5f));
    // Starts, stops and idle fidgets, as the player body has them (the travel is the AI's capsule's either way): moving off
    // toward a new heading turns the body on the way; otherwise a start the way it goes.
    const glm::vec2 dir = glm::length(local) > 1e-3f ? glm::normalize(local) : glm::vec2(0.0f, 1.0f);
    const bool idling = ac.InState(FPBody::kStateLocomotion) || ac.InState(FPBody::kStateCrouchLoco) || ac.HasTag(FPBody::kTagFidget) ||
                        ac.HasTag(FPBody::kTagStep);
    const float gait = in.Sprint ? 2.0f : speed > 2.2f ? 1.0f : 0.0f;
    if (moving && !m_WasMoving && idling && stillBefore > 0.25f) {
        const float heading = FirstPersonBodyWrapAngle(NpcYawOf(flat, m_Yaw) - m_Yaw);
        float turn = 0.0f;
        if (!in.HoldFacing && std::abs(heading) > glm::radians(45.0f)) {
            turn = std::clamp(std::abs(heading) / glm::radians(45.0f), 1.0f, 4.0f);
            if (in.Crouched) turn = std::round(turn);
            turn = std::copysign(turn, heading);
        }
        ac.SetFloat(FPBody::kStartX, dir.x);
        ac.SetFloat(FPBody::kStartY, dir.y);
        ac.SetFloat(FPBody::kStartGait, gait);
        ac.SetFloat(FPBody::kStartTurn, turn);
        ac.SetFloat(FPBody::kStartTurnAmount, std::max(std::abs(turn), 1.0f));
        ac.SetTrigger(FPBody::kStart);
        m_StartDistance = 0.0f;
    }
    if (moving) { m_LastDir = dir; m_LastSprint = in.Sprint; m_MoveFor += dt; }
    if (!moving && m_WasMoving) {
        const bool going = ac.InState(FPBody::kStateLocomotion) || ac.InState(FPBody::kStateCrouchLoco) || ac.HasTag(FPBody::kTagStart) ||
                           ac.InState(FPBody::kStateSprint);
        if (going && m_MoveFor >= 0.5f && glm::length(m_Move) > (in.Crouched ? 0.6f : 1.0f)) {
            ac.SetFloat(FPBody::kStopX, m_LastDir.x);
            ac.SetFloat(FPBody::kStopY, m_LastDir.y);
            ac.SetFloat(FPBody::kStopGait, glm::length(m_Move) > 2.2f ? 1.0f : 0.0f);
            ac.SetTrigger(m_LastSprint && m_LastDir.y > 0.7f ? FPBody::kStopRun : FPBody::kStop);
        }
        m_MoveFor = 0.0f;
    }
    m_WasMoving = moving;
    m_StartDistance = ac.HasTag(FPBody::kTagStart) ? m_StartDistance + speed * dt : 0.0f;
    ac.SetFloat(FPBody::kStartDistance, m_StartDistance);
    ac.SetFloat(FPBody::kStopDistance, speed * 0.1f);
    // A soldier stood at ease (not aiming) fidgets now and then: the ready stance's own, or a crouch's.
    if (!moving && !in.Aiming && !m_Turning && (ac.InState(FPBody::kStateLocomotion) || ac.InState(FPBody::kStateCrouchLoco))) {
        if (m_FidgetNext <= 0.0f) m_FidgetNext = 10.0f + 10.0f * NextRandom();
        if (m_StillTime > m_FidgetNext) {
            ac.SetFloat(FPBody::kFidgetIndex, std::floor(NextRandom() * (in.Crouched ? 6.0f : 2.0f) * 0.9999f));
            ac.SetTrigger(FPBody::kFidget);
            m_FidgetNext = m_StillTime + 10.0f + 10.0f * NextRandom();
        }
    } else if (moving) {
        m_FidgetNext = 0.0f;
    }
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

// Wounded on the ground: npc_soldier.controller's scripted states (tools/gen_npc_soldier.py).
//   Prone (shot down by a leg): GoProne -> ProneDrop (the collapse) -> ProneIdle <-> ProneCrawlStart / ProneCrawl /
//   ProneCrawlStop on Moving.
//   Dying (gut-shot): GoDying -> InjuredIdle <-> InjuredWalkStart / InjuredWalk / InjuredWalkStop on Moving; Collapse ->
//   InjuredDrop -> InjuredLying.
// The body only ever goes forward in these clips, so it faces its travel - turning no faster than a body on the ground (or
// doubled over) can - and the loops play at the rate their own travel matches the capsule's (the standing locomotion's
// Play Rate trim): no skating hands, knees or feet.
void NpcBody::TickWounded(AnimatorControllerComponent& ac, const NpcBodyInput& in, const glm::vec3& flat, float speed, float dt) {
    if (in.Prone && !m_WasProne) ac.SetTrigger(kGoProne);
    if (in.Dying && !m_WasDying) ac.SetTrigger(kGoDying);
    if (in.Dying && in.Collapse && !m_WasCollapse) ac.SetTrigger(kCollapse);
    m_WasProne = in.Prone;
    m_WasDying = in.Dying;
    m_WasCollapse = in.Collapse;
    m_Turning = false;
    // Moving, held: a crawl that checks for a frame at a wall or a doorframe goes on as one crawl, not stop-start-stop
    // (each a second-long clip). It starts again only on real movement.
    m_MoveHold = speed > 0.2f ? 0.4f : std::max(0.0f, m_MoveHold - dt);
    const bool moving = speed > 0.1f || m_MoveHold > 0.0f;
    const bool down = ac.InState(kStateProneDrop) || ac.InState(kStateInjuredDrop) || ac.InState(kStateInjuredLying);
    // On the belly the capsule follows the body (NpcDirector::Move), so the body steers toward where the crawl wants to go;
    // on its feet it turns after the travel.
    const bool steer = in.Prone && glm::length(in.Steer) > 1e-3f;
    if ((moving || steer) && !down) {
        // At most this fast (degrees a second): on the belly the body swings round slowly.
        const float maxRate = glm::radians(in.Prone ? (in.Pivot ? kPronePivotRate : kProneTurnRate) : kDyingTurnRate);
        const float off = FirstPersonBodyWrapAngle(NpcYawOf(steer ? in.Steer : flat, m_Yaw) - m_Yaw);
        m_Yaw = FirstPersonBodyWrapAngle(m_Yaw + std::clamp(off * Follow(dt, 0.25f), -maxRate * dt, maxRate * dt));
    }
    m_Move += (FirstPersonBodyLocalMove(flat, m_Yaw) - m_Move) * Follow(dt, m_Set.MoveEase);
    ac.SetFloat(FPBody::kMoveX, m_Move.x);
    ac.SetFloat(FPBody::kMoveY, m_Move.y);
    ac.SetFloat(FPBody::kSpeed, glm::length(m_Move));
    ac.SetBool(FPBody::kMoving, moving && !down);
    ac.SetBool(FPBody::kSprint, false);
    ac.SetBool(FPBody::kTurning, false);
    // The loop's rate: its clip's own travel against the capsule's speed (eased, within reason).
    const bool loop = !ac.InTransition && (ac.InState(kStateProneCrawl) || ac.InState(kStateInjuredWalk));
    const float travel = dt > 1e-5f ? glm::length(glm::vec2(ac.RootMotion.DeltaPosition.x, ac.RootMotion.DeltaPosition.z)) / dt : 0.0f;
    float trim = 1.0f;
    if (loop && travel > 0.1f && speed > 0.1f) trim = std::clamp(m_RateTrim * speed / travel, 0.7f, in.Prone ? 1.7f : 1.4f); // (a desperate crawl)
    m_RateTrim += (trim - m_RateTrim) * Follow(dt, 0.15f);
    ac.SetFloat(FPBody::kPlayRate, m_RateTrim);
    // Starts by the distance gone, as the standing ones (StartDistance; the stop's from what is left at this speed).
    m_StartDistance = ac.InState(kStateProneCrawlStart) || ac.InState(kStateInjuredWalkStart) ? m_StartDistance + speed * dt : 0.0f;
    ac.SetFloat(FPBody::kStartDistance, m_StartDistance);
    ac.SetFloat(FPBody::kStopDistance, speed * 0.1f);
    // A collapse goes where its clip goes (forward, as he falls): its travel, for the director to move the capsule by.
    const bool falling = ac.InState(kStateProneDrop) || ac.InState(kStateInjuredDrop);
    // (The animator's delta is the world's already - the driver stands at the body's yaw: turned again, a fall forward went back.)
    m_FallTravel = falling ? glm::vec3(ac.RootMotion.DeltaPosition.x, 0.0f, ac.RootMotion.DeltaPosition.z) : glm::vec3(0.0f);
    m_Breath += dt;
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
        m_Slide.Reset();
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
    // Foot slide correction (stride warp, then pin) on the animated feet; off = the clips' own feet.
    IK::FootSlideSettings slideSet = m_Set.FootSlide;
    const int probe = IK::FootSlideProbeMode();
    if (probe == 1) { slideSet = IK::FootSlideSettings{}; slideSet.PinEnabled = slideSet.StrideEnabled = true; }
    const bool slideOn = slideSet.Active();
    IK::FootSlideOutput slide;
    if (slideOn || probe >= 0) {
        const float unit = glm::length(glm::vec3(rootW[0]));
        IK::FootSlideInput sin;
        sin.Feet = m_Feet;
        sin.Velocity = glm::vec3(m_In.Velocity.x, 0.0f, m_In.Velocity.z);
        sin.PlantHeight = 0.05f;
        sin.Dt = dt;
        for (int s = 0; s < 2; ++s) {
            sin.Foot[s] = glm::vec3(rootW * glm::vec4(IK::Position(m_Globals[(size_t)m_DriverLeg[s][2]]), 1.0f));
            sin.Height[s] = sin.Foot[s].y - m_Feet.y;
        }
        sin.Pelvis = glm::vec3(rootW * glm::vec4(IK::Position(m_Globals[(size_t)m_DriverPelvis]), 1.0f));
        sin.LegLength = unit * (glm::length(IK::Position(m_Globals[(size_t)m_DriverLeg[0][0]]) - IK::Position(m_Globals[(size_t)m_DriverLeg[0][1]])) +
                                glm::length(IK::Position(m_Globals[(size_t)m_DriverLeg[0][1]]) - IK::Position(m_Globals[(size_t)m_DriverLeg[0][2]])));
        slide = m_Slide.Step(slideSet, sin, !slideOn);
        if (probe >= 0) IK::FootSlideProbeLog("npc", m_Slide.Stats, glm::length(glm::vec2(sin.Velocity.x, sin.Velocity.z)), m_Slide.ClipSpeed(), m_Slide.Scale());
    }
    for (int s = 0; s < 2; ++s) {
        const glm::vec3 foot = glm::vec3(rootW * glm::vec4(IK::Position(m_Globals[(size_t)m_DriverLeg[s][2]]), 1.0f)) + slide.Shift[s];
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
    if (flat && !slideOn) return; // the clips' own feet are right
    const float w = m_FootWeight;
    const float pelvisDelta = (FirstPersonBodyFootPelvis(m_FootOffset[0], m_FootOffset[1], m_Set.FootIKMaxDrop, m_Set.FootIKPelvisRaise) - slide.PelvisDrop) * w;
    IK::OffsetBone(pose, m_DriverParents, m_Globals, m_DriverPelvis, glm::vec3(0.0f, pelvisDelta, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f));
    for (int s = 0; s < 2; ++s) {
        const int thigh = m_DriverLeg[s][0], calf = m_DriverLeg[s][1], foot = m_DriverLeg[s][2];
        const glm::vec3 target = IK::Position(m_Globals[(size_t)foot]) + glm::vec3(0.0f, m_FootOffset[s] * w - pelvisDelta, 0.0f) + toModel3 * slide.Shift[s] * w;
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

void NpcBody::DragLegPass(const World& world, float dt) {
    const int s = m_In.HurtLeg;
    const auto* ac = world.Registry.try_get<AnimatorControllerComponent>(m_Driver);
    // In once the collapse is over (it keeps its own legs).
    const bool down = m_In.Prone && s >= 0 && s < 2 && ac && !ac->InState(kStateProneDrop) && ac->HasTag("Prone");
    m_Drag += ((down ? 1.0f : 0.0f) - m_Drag) * Follow(dt, 0.35f);
    // The body's turn (radians a second), for the leg's trailing lag.
    const float yawRate = dt > 1e-5f ? FirstPersonBodyWrapAngle(m_Yaw - m_DragPrevYaw) / dt : 0.0f;
    m_DragPrevYaw = m_Yaw;
    if (m_Drag < 1e-3f || s < 0 || s > 1) return;
    const int thigh = m_DriverLeg[s][0], calf = m_DriverLeg[s][1], foot = m_DriverLeg[s][2];
    if (thigh < 0 || calf < 0 || foot < 0) return;
    Model& m = *m_DriverModel;
    IK::Pose& pose = m_Pose;
    pose = m.AppliedLocalPose();
    m.BindLocalPose(m_BindScratch);
    if ((int)pose.size() != m.NodeCount() || m_BindScratch.size() != pose.size()) return;
    const glm::quat id(1.0f, 0.0f, 0.0f, 0.0f);
    // Nearly straight - the bind pose's hip and knee: a standing leg, which under a body on its belly trails behind it - with
    // a share of the crawl's own hip and knee left in (it doesn't push, but it isn't a post: it shifts with the body), the
    // ankle relaxed half way ...
    pose[(size_t)thigh].R = glm::slerp(pose[(size_t)thigh].R, m_BindScratch[(size_t)thigh].R, 0.88f * m_Drag);
    pose[(size_t)calf].R = glm::slerp(pose[(size_t)calf].R, m_BindScratch[(size_t)calf].R, 0.82f * m_Drag);
    pose[(size_t)foot].R = glm::slerp(pose[(size_t)foot].R, m_BindScratch[(size_t)foot].R, 0.5f * m_Drag);
    IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
    glm::vec3 hip = IK::Position(m_Globals[(size_t)thigh]);
    // ... rolled out about its own length so the foot lies on its inner side, not up on its toes (the model's +X is its left) ...
    {
        const glm::vec3 axis = IK::Position(m_Globals[(size_t)foot]) - hip;
        const int ball = m_DriverModel->NodeIndex(s == 0 ? "ball_l" : "ball_r");
        if (ball >= 0 && glm::dot(axis, axis) > 1e-6f) {
            const glm::vec3 a = glm::normalize(axis), outward(s == 0 ? 1.0f : -1.0f, 0.0f, 0.0f);
            const glm::vec3 toe = IK::Position(m_Globals[(size_t)ball]) - IK::Position(m_Globals[(size_t)foot]);
            const float roll = glm::radians(30.0f);
            const float sign = glm::dot(glm::angleAxis(roll, a) * toe, outward) >= glm::dot(glm::angleAxis(-roll, a) * toe, outward) ? 1.0f : -1.0f;
            IK::OffsetBone(pose, m_DriverParents, m_Globals, thigh, glm::vec3(0.0f), glm::slerp(id, glm::angleAxis(sign * roll, a), m_Drag), hip);
        }
    }
    // ... trailing the body's turns a little (sprung: it swings past and settles) ...
    Spring(m_DragLag, m_DragLagRate, std::clamp(-yawRate * 0.35f, -0.35f, 0.35f), 6.0f, 0.6f, dt);
    if (std::abs(m_DragLag) > 1e-4f)
        IK::OffsetBone(pose, m_DriverParents, m_Globals, thigh, glm::vec3(0.0f), glm::angleAxis(m_DragLag * m_Drag, glm::vec3(0.0f, 1.0f, 0.0f)), hip);
    // ... and swung down about the hip until the ankle drags on the floor (the model stands on y = 0).
    const glm::vec3 ankle = IK::Position(m_Globals[(size_t)foot]);
    const glm::vec3 from = ankle - hip, to = glm::vec3(ankle.x, 0.07f, ankle.z) - hip;
    if (glm::dot(from, from) > 1e-6f && glm::dot(to, to) > 1e-6f) {
        const glm::quat swing = glm::rotation(glm::normalize(from), glm::normalize(to));
        IK::OffsetBone(pose, m_DriverParents, m_Globals, thigh, glm::vec3(0.0f), glm::slerp(id, swing, m_Drag), hip);
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
    if (!IsActive() || m_PoseExternal || !m_DriverModel) return;
    FootPass(dt);
    DragLegPass(world, dt);
    // A gun let go of while alive: the arms ease off the hold onto the clips' (ReleaseWeaponHold).
    if (m_ReleaseBlend > 0.0f) {
        Model& m = *m_DriverModel;
        IK::Pose& pose = m_Pose;
        pose = m.AppliedLocalPose();
        if (pose.size() == m_ReleasePose.size()) {
            const float w = AnimatorCrossfadeWeight(m_ReleaseBlend);
            for (int i : m_ArmNodes) pose[(size_t)i].R = glm::slerp(pose[(size_t)i].R, m_ReleasePose[(size_t)i].R, w);
            m.ApplyLocalPose(pose);
            m_PiecesStale = true;
        }
        m_ReleaseBlend = std::max(0.0f, m_ReleaseBlend - dt / 0.3f);
    }
    // Lying gut-shot (a held pose): laboured breathing in the chest, quick and shallow, slowing as he fades.
    if (m_In.Dying && m_DriverSpineCount >= 3) {
        const auto* ac = world.Registry.try_get<AnimatorControllerComponent>(m_Driver);
        const bool lying = ac && ac->InState(kStateInjuredLying);
        m_BreathWeight += ((lying ? 1.0f : 0.0f) - m_BreathWeight) * Follow(dt, 0.5f);
        if (m_BreathWeight > 1e-3f) {
            Model& m = *m_DriverModel;
            IK::Pose& pose = m_Pose;
            pose = m.AppliedLocalPose();
            if ((int)pose.size() == m.NodeCount()) {
                const float rate = glm::mix(0.55f, 0.3f, std::clamp(m_Breath / 20.0f, 0.0f, 1.0f)); // breaths a second
                m_BreathPhase += rate * dt;
                const float b = 0.5f - 0.5f * std::cos(m_BreathPhase * 6.2831853f);
                // The upper spine lifts with each breath, about its own side axis.
                for (int k = m_DriverSpineCount - 2; k < m_DriverSpineCount; ++k) {
                    const int bone = m_DriverSpine[k];
                    pose[(size_t)bone].R = pose[(size_t)bone].R * glm::angleAxis(glm::radians(1.6f) * b * m_BreathWeight, glm::vec3(1.0f, 0.0f, 0.0f));
                }
                m.ApplyLocalPose(pose);
                m_PiecesStale = true;
            }
        }
    }
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
    } else if (!m_Turning && !m_In.Prone && !m_In.Dying) { // (wounded on the ground or doubled over, the head looks alone)
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
        if (!m_In.Prone && !m_In.Dying && m_DriverModel->NodeTransform(FPBody::kBonePelvis, pelvis) && m_DriverModel->NodeTransform("neck_01", neck)) {
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
            OffsetSpine([&](float d) {
                return glm::angleAxis(m_AimTwist / d, glm::vec3(0, 1, 0)) *
                       glm::angleAxis((m_Straighten - m_AimPitch + m_Set.CowerHunch * m_Cower) / d, glm::vec3(1, 0, 0)) *
                       glm::angleAxis(m_Lean * 0.38f / d, glm::vec3(0, 0, 1));
            });
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
            glm::quat turn = FirstPersonBodyShoulderLineTurn(across, target, lineMatch);
            const float angle = 2.0f * std::acos(std::clamp(std::abs(turn.w), 0.0f, 1.0f));
            if (angle < 0.002f) break;
            // A stance's blade, not the rig's arms mid-reload: they swing round the gun, the line they give with them, and matched
            // in full the chest (and the head on it) was thrown 45 cm round in one frame and back the next.
            constexpr float kMaxLineTurn = 0.2f; // radians a pass
            if (angle > kMaxLineTurn) turn = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), turn, kMaxLineTurn / angle);
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

    // Up off the wound fall's ragdoll: the pose as it lay, blended out of (last, over everything above).
    RecoverPass(dt);

    // The head bone, where the weapon's camera goes (its arms rig is placed by the same bone).
    glm::mat4 head(1.0f);
    m_Eye = m_DriverModel->NodeTransform("head", head) ? glm::vec3(rootW * head[3]) : m_Feet + glm::vec3(0.0f, 1.62f, 0.0f);
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

bool NpcBody::BeginRecover(World& world, const glm::vec3& feet, float yaw, float blendTime, float limbSpeed, const char* state) {
    if (!IsActive() || !m_DriverModel) return false;
    auto& reg = world.Registry;
    // The ragdoll wrote every piece's pose against the root the body went down from; the root moves to where he lies, so the pose is
    // re-rooted there (the skeleton's top nodes take the change of frame): the same bones in the same places, drawn from the new root.
    const glm::mat4 before = RootWorld();
    m_Feet = feet;
    m_Yaw = FirstPersonBodyWrapAngle(yaw);
    m_HaveYaw = true;
    const glm::mat4 rebase = glm::inverse(RootWorld()) * before; // rigid: a yaw and a shift
    const glm::quat rebaseR = glm::normalize(glm::quat_cast(glm::mat3(rebase)));
    m_RecoverPose.assign(m_Models.size(), {});
    m_RecoverGroup.assign(m_Models.size(), {});
    for (size_t k = 0; k < m_Models.size(); ++k) {
        if (!m_Models[k]) continue;
        Model& m = *m_Models[k];
        // Each node's region: the arm or leg whose root (collarbone, hip) it hangs under, else the trunk.
        {
            const int roots[4] = {m.NodeIndex(FPBody::kBoneClavicle[0]), m.NodeIndex(FPBody::kBoneClavicle[1]), m.NodeIndex(FPBody::kBoneThigh[0]),
                                  m.NodeIndex(FPBody::kBoneThigh[1])};
            std::vector<unsigned char>& group = m_RecoverGroup[k];
            group.assign((size_t)m.NodeCount(), (unsigned char)kRecoverTrunk);
            for (int i = 0; i < m.NodeCount(); ++i)
                for (int a = i, guard = 0; a >= 0 && guard < 128; a = m.NodeParent(a), ++guard) {
                    const int* r = std::find(roots, roots + 4, a);
                    if (r != roots + 4) { group[(size_t)i] = (unsigned char)(kRecoverArmL + (r - roots)); break; }
                }
        }
        std::vector<LocalTRS>& pose = m_RecoverPose[k];
        pose = m.AppliedLocalPose();
        if ((int)pose.size() != m.NodeCount()) m.BindLocalPose(pose);
        for (int i = 0; i < (int)pose.size(); ++i) {
            if (m.NodeParent(i) >= 0) continue;
            pose[(size_t)i].T = glm::vec3(rebase * glm::vec4(pose[(size_t)i].T, 1.0f));
            pose[(size_t)i].R = glm::normalize(rebaseR * pose[(size_t)i].R);
        }
        m.ApplyLocalPose(pose); // this frame draws it as it was, from the new root
    }
    world.SetWorldPose(m_Root, m_Feet, YawRotation(m_Yaw));
    // The animators go on from the prone pose, not from where they were held when he went down (nor through ProneDrop).
    for (entt::entity e : m_Pieces) {
        auto* ac = reg.valid(e) ? reg.try_get<AnimatorControllerComponent>(e) : nullptr;
        if (!ac) continue;
        ac->SkipUpdate = false;
        ac->SkippedTime = 0.0f;
        for (const char* t : {kGoProne, kGoDying, kCollapse, "Hit"}) ac->ResetTrigger(t);
        if (const auto ctrl = GetAnimatorController(ac->Controller)) AnimatorStartInState(*ctrl, *ac, state, 1.0f); // (at its held end)
    }
    m_WasProne = true; // (no GoProne: he is down already)
    m_In.Prone = true;
    m_Move = glm::vec2(0.0f);
    m_RateTrim = 1.0f;
    m_StartDistance = 0.0f;
    m_FallTravel = glm::vec3(0.0f);
    m_ReleaseBlend = 0.0f; // the gun-hold arms he let go of went with the fall
    m_Drag = 0.0f;
    m_DragPrevYaw = m_Yaw;
    m_DragLag = m_DragLagRate = 0.0f;
    m_HaveFootGround = false;
    m_RecoverTime = std::max(blendTime, 1e-3f);
    m_RecoverSpeed = std::max(limbSpeed, 0.1f);
    m_RecoverAge = 0.0f;
    m_RecoverTimed = false;
    m_PiecesStale = false;
    m_PoseExternal = false;
    return true;
}

void NpcBody::RecoverPass(float dt) {
    if (m_RecoverAge < 0.0f) return;
    if (m_PiecesStale) SyncPieces(); // the driver's passes onto the pieces first: the blend is the last word on every one
    // The first animated frame: each region's time from how far its ends have to go (driver, model space), so none moves faster than
    // the limb speed allows at the eased blend's peak (1.5x its average) - never under the blend time, never over 2.5x it.
    if (!m_RecoverTimed) {
        m_RecoverTimed = true;
        for (float& t : m_RecoverGroupTime) t = m_RecoverTime;
        const Model& d = *m_DriverModel;
        const IK::Pose& now = d.AppliedLocalPose();
        if (m_DriverIndex >= 0 && m_DriverIndex < (int)m_RecoverPose.size() && now.size() == m_RecoverPose[(size_t)m_DriverIndex].size() &&
            m_DriverParents.size() == now.size()) {
            std::vector<glm::mat4> to, from;
            IK::ComputeGlobals(now, m_DriverParents, to);
            IK::ComputeGlobals(m_RecoverPose[(size_t)m_DriverIndex], m_DriverParents, from);
            auto gap = [&](const char* bone) {
                const int b = d.NodeIndex(bone);
                return b >= 0 ? glm::length(IK::Position(to[(size_t)b]) - IK::Position(from[(size_t)b])) : 0.0f;
            };
            const float far[kRecoverGroups] = {std::max(gap("head"), gap(FPBody::kBonePelvis)), gap(FPBody::kBoneHand[0]), gap(FPBody::kBoneHand[1]),
                                               gap(FPBody::kBoneFoot[0]), gap(FPBody::kBoneFoot[1])};
            for (int g = 0; g < kRecoverGroups; ++g)
                m_RecoverGroupTime[g] = std::clamp(1.5f * far[g] / m_RecoverSpeed, m_RecoverTime, 2.5f * m_RecoverTime);
        }
    }
    // Every bone of every piece, from the pose the fall left toward the animated one, eased, on its region's clock. Each bone's
    // rotation is blended in the model's space (not its parent's: a local blend compounds down a limb, and a hand swept round on the
    // arc of every joint above it travels twice as far as it has to), its local offset as is (the pelvis's carries the body's lie
    // onto the root; the bones keep their lengths).
    float w[kRecoverGroups];
    bool done = true;
    for (int g = 0; g < kRecoverGroups; ++g) {
        const float u = m_RecoverAge / std::max(m_RecoverGroupTime[g], 1e-3f);
        w[g] = AnimatorCrossfadeWeight(1.0f - u);
        done = done && u >= 1.0f;
    }
    std::vector<int>& parents = m_PieceParents;
    thread_local std::vector<glm::mat4> to, from; // scratch, reused
    thread_local std::vector<glm::quat> blended;
    for (size_t k = 0; k < m_Models.size() && k < m_RecoverPose.size(); ++k) {
        if (!m_Models[k]) continue;
        Model& m = *m_Models[k];
        const std::vector<LocalTRS>& lying = m_RecoverPose[k];
        const std::vector<unsigned char>& group = m_RecoverGroup[k];
        IK::Pose& pose = m_Pose;
        pose = m.AppliedLocalPose();
        if (pose.size() != lying.size() || group.size() != lying.size()) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[(size_t)i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, to);
        IK::ComputeGlobals(lying, parents, from);
        blended.resize(pose.size());
        for (size_t i = 0; i < pose.size(); ++i) {
            // (`blended`: each bone's rotation in the model as drawn now, parents first.)
            const int p = parents[i];
            const glm::quat above = p >= 0 ? blended[(size_t)p] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            const float wi = w[group[i]];
            if (wi > 0.0f) {
                const glm::quat want = glm::normalize(glm::slerp(IK::Rotation(to[i]), IK::Rotation(from[i]), wi));
                pose[i].R = glm::normalize(glm::inverse(above) * want);
                pose[i].T = glm::mix(pose[i].T, lying[i].T, wi);
            }
            blended[i] = glm::normalize(above * pose[i].R);
        }
        m.ApplyLocalPose(pose);
    }
    m_RecoverAge += std::max(dt, 0.0f);
    if (done) {
        m_RecoverAge = -1.0f;
        m_RecoverPose.clear();
        m_RecoverGroup.clear();
    }
}

std::string NpcBody::AnimDebug(const World& world) const {
    if (m_PoseExternal || m_Driver == entt::null || !world.Registry.valid(m_Driver)) return "-";
    const auto* ac = world.Registry.try_get<AnimatorControllerComponent>(m_Driver);
    if (!ac || ac->Layers.empty()) return "-";
    char buf[96];
    std::string s;
    for (const AnimatorLayerRuntime::Item& it : ac->Layers[0].Stack) {
        std::snprintf(buf, sizeof buf, "[%d ph %.3f fade %.2f%s] ", it.State, it.Phase, it.Fade, it.Matched ? " matched" : "");
        s += buf;
    }
    if (!ac->History.empty()) {
        const AnimatorTransitionLog& h = ac->History.back();
        std::snprintf(buf, sizeof buf, "last %.2f s ago: ", ac->Clock - h.Time);
        s += buf + h.From + " -> " + h.To + " (" + h.Why + ")";
    }
    return s;
}

std::string NpcBody::StateName(const World& world) const {
    if (m_PoseExternal) return "ragdoll"; // (alive and going down, or dead: the physics has the pose)
    if (m_Driver == entt::null || !world.Registry.valid(m_Driver)) return {};
    const auto* ac = world.Registry.try_get<AnimatorControllerComponent>(m_Driver); // gone once ragdolled
    return ac ? ac->StateName : std::string("ragdoll");
}
