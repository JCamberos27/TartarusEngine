#include "NpcBody.h"

#include "AnimatorController.h"      // AnimatorCrossfadeWeight
#include "Components.h"
#include "FirstPersonBody.h"         // FirstPersonBodyLocalMove / WrapAngle
#include "FirstPersonBodyContract.h" // FPBody:: parameter, state and bone names
#include "GameModuleAPI.h"           // RaycastHit, QueryFilter (the foot pass)
#include "GameCommands.h"            // Gameplay::kBody* triggers
#include "GameFrames.h"              // Gameplay::BodyMotionFrame (the standing locomotion)
#include "GameRules.h"               // GameRules::UpdateBodyMotion
#include "IK.h"
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

constexpr float kSlideTurnRate = 200.0f; // degrees a second: the feet sliding round behind a facing past the spine's twist
constexpr float kMoveTurnRate = 240.0f;  // degrees a second: the most the heading swings on the move outside a turn clip


} // namespace

float NpcYawOf(const glm::vec3& dir, float fallback) {
    if (dir.x * dir.x + dir.z * dir.z < 1e-8f) return fallback;
    return std::atan2(dir.x, dir.z);
}

constexpr float kWarpMax = 60.0f; // degrees the orientation warp may turn the legs
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
        ac.CopyDriverPose = e != m_Driver; // one skeleton: sample the clips once, on the torso
        ac.RootMotion.Mode = (int)RootMotionMode::InPlace;
        // Every piece takes the turn clips' yaw out of its pose as the torso does (Tick): a piece that samples the clips
        // itself (the first-person arms: its rig doesn't rest as the torso's, so it can't copy it) otherwise keeps the
        // clip's turn on top of the body's own and swings its arms off the torso through every turn.
        ac.RootMotion.Rotation = true;
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
    if (!IsActive() || m_PoseExternal) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Driver) || !reg.valid(m_Root) || !reg.all_of<AnimatorControllerComponent>(m_Driver)) return;
    m_In = in;
    m_Feet = in.Feet;
    auto& ac = reg.get<AnimatorControllerComponent>(m_Driver);

    const glm::vec3 flat(in.Velocity.x, 0.0f, in.Velocity.z);
    const float speed = glm::length(flat);
    if (!m_HaveYaw) { m_Yaw = in.FacingYaw; m_HaveYaw = true; }
    // The clips' turns are read out of the pose (DeltaYaw) and turn the body, as the player body's driver: a turn on the spot
    // or a turning start swings the heading.
    ac.RootMotion.Rotation = true;
    // The player's body's own locomotion (BodyLocomotion, through GameRules as FirstPersonBody runs it) on the AI's
    // capsule - the same starts, stops, pivots, steps, turns, gait blend and landings, tuned as the player body's defaults. The
    // wish is the AI's: the gait's pace toward the goal, nothing once its stop has begun (NpcDirector::Move), so the stop clip
    // starts with the brake and is distance matched to it.
    static const FirstPersonBodyComponent kTune{};
    Gameplay::BodyMotionFrame& f = m_Loco;
    const glm::vec3 wish(in.Wish.x, 0.0f, in.Wish.z);
    const float wishSpeed = glm::length(wish);
    f.Velocity = {in.Velocity.x, in.Velocity.y, in.Velocity.z};
    f.WishVelocity = {wish.x, 0.0f, wish.z};
    f.Dt = dt;
    f.ViewYaw = in.HoldFacing || wishSpeed < 0.1f ? in.FacingYaw : NpcYawOf(wish, m_Yaw); // facing: as asked when still, else the way it goes
    f.Yaw = m_Yaw;
    f.RootYaw = ac.RootMotion.DeltaYaw;
    f.StateTime = ac.StateTime;
    f.PlayerRunSpeed = std::max(in.JogSpeed, 0.1f);
    f.PlayerSprintSpeed = std::max(in.RunSpeed, f.PlayerRunSpeed);
    f.RunSpeed = kTune.RunSpeed;
    f.ClipSprint = kTune.SprintSpeed;
    f.Grounded = in.Grounded;
    f.Crouched = in.Crouched;
    f.Jumped = 0;
    f.Responsiveness = 1.0f; // in place: the capsule leads, the clips keep pace
    f.MaxPlayRate = kTune.MaxPlayRate;
    f.ParamSmoothing = kTune.ParamSmoothing;
    f.TurnThreshold = glm::degrees(m_Set.TurnThreshold);
    f.TurnMinTime = kTune.TurnMinTime;
    f.TurnEndAngle = kTune.TurnEndAngle;
    f.TurnTimeout = kTune.TurnTimeout;
    f.TurnMoveEase = m_Set.FaceEase;
    f.TurnLagMargin = kTune.TurnLagMargin;
    f.TurnLagFloor = kTune.TurnLagFloor;
    f.StopDebounce = kTune.StopDebounce;
    f.StartIdleTime = kTune.StartIdleTime;
    f.StartMaxMove = kTune.StartMaxMove;
    f.StopMinRunTimeCrouched = kTune.StopMinRunTimeCrouched;
    f.StopMinRunTime = kTune.StopMinRunTime;
    f.StopMinSpeedCrouched = kTune.StopMinSpeedCrouched;
    f.StopMinSpeed = kTune.StopMinSpeed;
    f.StopRunForward = kTune.StopRunForward;
    f.AirborneDelay = kTune.AirborneDelay;
    f.StartStopClips = 1;
    f.IsTurn = ac.InState(FPBody::kStateTurn);
    f.IsCrouchTurn = ac.InState(FPBody::kStateCrouchTurn);
    f.IsLocomotion = ac.InState(FPBody::kStateLocomotion);
    f.IsCrouchLoco = ac.InState(FPBody::kStateCrouchLoco);
    f.IsStart = ac.HasTag(FPBody::kTagStart);
    f.IsStartTurn = ac.HasTag(FPBody::kTagStartTurn);
    f.IsStop = ac.HasTag(FPBody::kTagStop);
    f.IsPivot = ac.HasTag(FPBody::kTagPivot);
    f.IsStep = ac.HasTag(FPBody::kTagStep);
    f.IsFidget = ac.HasTag(FPBody::kTagFidget);
    f.IsSprint = ac.InState(FPBody::kStateSprint);
    f.AccelTime = in.AccelTime;
    f.DecelTime = in.DecelTime;
    f.SprintClip = FPBody::kSprintClipSpeed;
    f.Armed = 0; // unarmed: the unarmed idles and fidgets
    f.Busy = 0;
    f.Random = NextRandom();
    GameRules::UpdateBodyMotion(f);
    // Mid-crossfade nothing new starts on top (as the player body: the fade would be cut short).
    // (Not a turn on the spot fading back to the loop: moving off then is a start, else the loop fades up behind the capsule.)
    if (ac.StateName != m_LastState) {
        m_TurnFade = ac.InTransition && FPBody::TurnFadeToLoop(m_LastState, ac.StateName);
        m_LastState = ac.StateName;
    }
    if (!ac.InTransition) m_TurnFade = false;
    if (ac.InTransition && !m_TurnFade) {
        f.Triggers &= ~(Gameplay::kBodyStart | Gameplay::kBodyPivot | Gameplay::kBodyFidget);
        for (const char* t : {FPBody::kStart, FPBody::kPivot, FPBody::kFidget}) ac.ResetTrigger(t);
    }
    {
        float yaw = f.Yaw;
        const float step = FirstPersonBodyWrapAngle(yaw - m_Yaw);
        // A turn clip (on the spot, or a turning start) swings the heading by its own yaw. Otherwise it is never swung faster
        // than the feet can slide round (stood) or a walker turns (moving): moving off toward a goal behind, the locomotion's
        // lag clamp would whip it most of the way round in one frame before the turning start took over.
        if (!f.Turning && !f.IsStartTurn) {
            const float most = glm::radians(f.Moving ? kMoveTurnRate : kSlideTurnRate) * dt;
            yaw = m_Yaw + std::clamp(step, -most, most);
        }
        m_Yaw = FirstPersonBodyWrapAngle(yaw);
        f.Yaw = m_Yaw;
        f.Twist = FirstPersonBodyWrapAngle(f.ViewYaw - m_Yaw);
    }
    m_Turning = f.Turning != 0;
    m_Move = glm::vec2(f.Move.x, f.Move.y);
    m_LastDir = glm::vec2(f.LastDir.x, f.LastDir.y);
    world.SetWorldPose(m_Root, m_Feet, YawRotation(m_Yaw));

    if (f.SetTurnAngle) ac.SetFloat(FPBody::kTurnAngle, f.TurnAngle);
    ac.SetBool(FPBody::kTurning, f.Turning != 0);
    ac.SetBool(FPBody::kMoving, f.Moving != 0);
    if (f.Triggers & Gameplay::kBodyStart) {
        ac.SetFloat(FPBody::kStartX, f.StartDir.x);
        ac.SetFloat(FPBody::kStartY, f.StartDir.y);
        ac.SetFloat(FPBody::kStartGait, f.StartGait);
        ac.SetFloat(FPBody::kStartTurn, f.StartTurn);
        ac.SetFloat(FPBody::kStartTurnAmount, f.StartTurnAmount);
    }
    if (f.Triggers & (Gameplay::kBodyStop | Gameplay::kBodyStopRun)) {
        ac.SetFloat(FPBody::kStopX, f.StopDir.x);
        ac.SetFloat(FPBody::kStopY, f.StopDir.y);
        ac.SetFloat(FPBody::kStopGait, f.StopGait);
    }
    if (f.Triggers & Gameplay::kBodyPivot) {
        ac.SetFloat(FPBody::kPivotX, f.PivotDir.x);
        ac.SetFloat(FPBody::kPivotY, f.PivotDir.y);
        ac.SetFloat(FPBody::kPivotGait, f.PivotGait);
    }
    if (f.Triggers & Gameplay::kBodyStep) {
        ac.SetFloat(FPBody::kStepX, f.StepDir.x);
        ac.SetFloat(FPBody::kStepY, f.StepDir.y);
    }
    if (f.Triggers & Gameplay::kBodyFidget) ac.SetFloat(FPBody::kFidgetIndex, f.FidgetIndex);
    // The gait loop at the rate its clips travel at the capsule's speed (a blend's shared cycle runs a walk slow), as the
    // player body's.
    {
        const bool loop = !ac.InTransition && (ac.InState(FPBody::kStateLocomotion) || ac.InState(FPBody::kStateCrouchLoco));
        const float travel = dt > 1e-5f ? glm::length(glm::vec2(ac.RootMotion.DeltaPosition.x, ac.RootMotion.DeltaPosition.z)) / dt : 0.0f;
        float trim = 1.0f;
        if (loop && travel > 0.3f && speed > 0.3f) trim = std::clamp(m_RateTrim * speed / travel, 0.8f, 1.5f);
        m_RateTrim += (trim - m_RateTrim) * Follow(dt, 0.15f);
    }
    ac.SetFloat(FPBody::kPlayRate, f.PlayRate * m_RateTrim);
    // Orientation warp: a start, stop or gait blend whose clips travel off the way the capsule goes (a back-pedal start
    // whose root drifts right while the path bends left, the blend between two cardinal strafes) skates the feet sideways.
    // The legs are turned onto the capsule's travel (at most kWarpMax) and the spine turned back (FootPass). Not in a turn
    // on the spot, a turning start or a pivot (the travels cross there by design), nor at a crawl.
    {
        const float c = std::cos(m_Yaw), sn = std::sin(m_Yaw);
        const auto local = [&](const glm::vec3& v) { return glm::vec2(-v.x * c + v.z * sn, v.x * sn + v.z * c); };
        const glm::vec2 clipV = dt > 1e-5f ? local(glm::vec3(ac.RootMotion.DeltaPosition.x, 0.0f, ac.RootMotion.DeltaPosition.z) / dt) : glm::vec2(0.0f);
        const float k = Follow(dt, 0.2f);
        m_WarpClip += (clipV - m_WarpClip) * k;
        m_WarpCapsule += (local(flat) - m_WarpCapsule) * k;
        const bool gait = (ac.InState(FPBody::kStateLocomotion) || ac.InState(FPBody::kStateCrouchLoco) || ac.HasTag(FPBody::kTagStart) ||
                           ac.HasTag(FPBody::kTagStop)) && !ac.HasTag(FPBody::kTagStartTurn) && !ac.HasTag(FPBody::kTagPivot) && !m_Turning && in.Grounded;
        float want = 0.0f;
        if (gait && glm::length(m_WarpClip) > 0.4f && glm::length(m_WarpCapsule) > 0.4f) {
            const float a = FirstPersonBodyWrapAngle(std::atan2(m_WarpCapsule.x, m_WarpCapsule.y) - std::atan2(m_WarpClip.x, m_WarpClip.y));
            want = std::clamp(a, -glm::radians(kWarpMax), glm::radians(kWarpMax));
        }
        m_Warp += (want - m_Warp) * Follow(dt, 0.12f);
    }
    ac.SetFloat(FPBody::kStartDistance, f.StartDistance);
    ac.SetFloat(FPBody::kStopDistance, f.StopDistance);
    ac.SetFloat(FPBody::kPivotDistance, f.PivotDistance);
    ac.SetFloat(FPBody::kSprintRate, f.SprintRate);
    ac.SetBool(FPBody::kStartOff, f.StartOff != 0);
    const char* triggers[]{FPBody::kCrouchDown, FPBody::kCrouchUp, FPBody::kStart, FPBody::kStop, FPBody::kStopRun,
                           FPBody::kJump, FPBody::kPivot, FPBody::kStep, FPBody::kFidget};
    for (int i = 0; i < 9; ++i)
        if (f.Triggers & (1 << i)) ac.SetTrigger(triggers[i]);
    ac.SetFloat(FPBody::kMoveX, f.Move.x);
    ac.SetFloat(FPBody::kMoveY, f.Move.y);
    ac.SetFloat(FPBody::kSpeed, glm::length(glm::vec2(f.Move.x, f.Move.y)));
    ac.SetBool(FPBody::kSprint, f.Sprint != 0);
    ac.SetBool(FPBody::kGrounded, in.Grounded);
    ac.SetBool(FPBody::kAirborne, f.Airborne != 0);
    ac.SetBool(FPBody::kCrouched, in.Crouched);
    // Light vs heavy landing, as the player body's: the fall speed of the last airborne frame.
    if (!in.Grounded) m_FallSpeed = std::max(0.0f, in.FallSpeed);
    ac.SetFloat(FPBody::kLandImpact, m_FallSpeed);
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
        m_Lock.Reset();
        return;
    }
    Model& m = *m_DriverModel;
    IK::Pose& pose = m_Pose;
    pose = m.AppliedLocalPose();
    if ((int)pose.size() != m.NodeCount()) return;
    IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
    // The orientation warp (Tick): the pelvis - legs and all - turned about the vertical onto the capsule's travel, the
    // first spine bone turned back, so only the hips and legs move. A local-right angle is a turn toward -X in the model.
    bool warped = false;
    if (std::abs(m_Warp) > 1e-3f && m_DriverSpineCount > 0) {
        const float w = m_Warp * m_FootWeight;
        const glm::quat turn = glm::angleAxis(-w, glm::vec3(0.0f, 1.0f, 0.0f));
        IK::OffsetBone(pose, m_DriverParents, m_Globals, m_DriverPelvis, glm::vec3(0.0f), turn, IK::Position(m_Globals[(size_t)m_DriverPelvis]));
        const int spine = m_DriverSpine[0];
        IK::OffsetBone(pose, m_DriverParents, m_Globals, spine, glm::vec3(0.0f), glm::inverse(turn), IK::Position(m_Globals[(size_t)spine]));
        warped = true;
    }
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
    // Foot lock: a planted foot held where it landed in the world, as the player body's (IK::FootLock). Not while prone or
    // crawling (the legs drag) - the director turns foot IK off there.
    glm::vec3 shift[2];
    bool held = false;
    const float yawRate = dt > 1e-5f ? FirstPersonBodyWrapAngle(m_Yaw - m_LockYaw) / dt : 0.0f;
    m_LockYaw = m_Yaw;
    // (Swinging round on the move only: a turn on the spot's clip steps its own feet.)
    const bool travelling = !m_Turning && glm::length(glm::vec2(m_In.Velocity.x, m_In.Velocity.z)) > 0.3f;
    const IK::FootLockSettings lockSet = IK::FootLockTurning(m_Set.FootLock, travelling ? yawRate : 0.0f);
    for (int s = 0; s < 2; ++s) {
        const glm::vec3 anim = glm::vec3(rootW * glm::vec4(IK::Position(m_Globals[(size_t)m_DriverLeg[s][2]]), 1.0f)) + slide.Shift[s];
        const glm::vec3 lock = m_Lock.Step(s, lockSet, anim, anim.y - m_Feet.y, dt);
        shift[s] = slide.Shift[s] + lock;
        held = held || glm::dot(shift[s], shift[s]) > 1e-8f;
    }
    for (int s = 0; s < 2; ++s) {
        const glm::vec3 foot = glm::vec3(rootW * glm::vec4(IK::Position(m_Globals[(size_t)m_DriverLeg[s][2]]), 1.0f)) + shift[s];
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
    if (flat && !slideOn && !held) { // the clips' own feet are right
        if (warped) { m.ApplyLocalPose(pose); SyncLower(); }
        return;
    }
    const float w = m_FootWeight;
    const float pelvisDelta = (FirstPersonBodyFootPelvis(m_FootOffset[0], m_FootOffset[1], m_Set.FootIKMaxDrop, m_Set.FootIKPelvisRaise) - slide.PelvisDrop) * w;
    IK::OffsetBone(pose, m_DriverParents, m_Globals, m_DriverPelvis, glm::vec3(0.0f, pelvisDelta, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f));
    for (int s = 0; s < 2; ++s) {
        const int thigh = m_DriverLeg[s][0], calf = m_DriverLeg[s][1], foot = m_DriverLeg[s][2];
        glm::vec3 target = IK::Position(m_Globals[(size_t)foot]) + glm::vec3(0.0f, m_FootOffset[s] * w - pelvisDelta, 0.0f) + toModel3 * shift[s] * w;
        // Never past the leg's reach: a held foot the hips have walked on from (the clip's own leg already near straight)
        // asks for more leg than there is, and the knee solve snaps the shin through it - a twitch. Held at the reach (or the
        // clip's own stretch, if that is longer), the foot gives a little instead.
        {
            const glm::vec3 hip = IK::Position(m_Globals[(size_t)thigh]);
            const float leg = glm::length(IK::Position(m_Globals[(size_t)calf]) - hip) +
                              glm::length(IK::Position(m_Globals[(size_t)foot]) - IK::Position(m_Globals[(size_t)calf]));
            const float most = std::max(0.98f * leg, glm::length(IK::Position(m_Globals[(size_t)foot]) - hip));
            const glm::vec3 d = target - hip;
            const float len = glm::length(d);
            if (len > most && len > 1e-5f) target = hip + d * (most / len);
        }
        // A planted foot lies on its slope; one swinging through the air keeps the clip's angle.
        const float planted = 1.0f - std::clamp((animHeight[s] - 0.06f) / 0.09f, 0.0f, 1.0f);
        const glm::vec3 normal = toModel3 * m_FootNormal[s];
        const float angle = std::min(std::acos(std::clamp(normal.y, -1.0f, 1.0f)), m_Set.FootIKTiltMax) * planted * w;
        glm::quat footRot = IK::Rotation(m_Globals[(size_t)foot]);
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

void NpcBody::LateUpdate(World& world, float dt) {
    (void)world;
    if (!IsActive() || m_PoseExternal || !m_DriverModel) return;
    FootPass(dt);
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
            m_PiecesStale = true;
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
                m_PiecesStale = true;
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

void NpcBody::SyncPieces() {
    m_PiecesStale = false;
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
