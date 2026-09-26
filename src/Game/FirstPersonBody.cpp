#include "FirstPersonBody.h"
#include "BodyDebugDraw.h"
#include "FirstPersonBodyContract.h"

#include "Camera.h"
#include "Components.h"
#include "GameModuleAPI.h"
#include "IK.h"
#include "Log.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "World.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace {

// Model space: the Quantum / UE mannequin family faces +Z with +Y up, so its right is -X.
constexpr glm::vec3 kForward{0.0f, 0.0f, 1.0f};
constexpr glm::vec3 kRight{-1.0f, 0.0f, 0.0f};

glm::quat YawRotation(float yaw) { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }

// Frame-rate independent approach toward a target over `seconds` (0 = snap).
float Follow(float dt, float seconds) { return seconds > 1e-4f ? 1.0f - std::exp(-dt / seconds) : 1.0f; }

// A node's position in its model's space as posed now.
glm::vec3 ModelPoint(const Model& m, int node) {
    glm::mat4 g(1.0f);
    return m.NodeTransform(m.NodeName(node), g) ? glm::vec3(g[3]) : glm::vec3(0.0f);
}

// The rig's arm shapes onto `pose` (rotations only, every node under the clavicles: the body keeps
// its own bone lengths), blended by `weight`.
void CopyArmShape(const Model& m, const Model& rig, float weight, IK::Pose& pose, const std::vector<int>& parents,
                  const std::map<std::string, std::string>& boneMap) {
    const IK::Pose& rigPose = rig.AppliedLocalPose();
    if ((int)rigPose.size() != rig.NodeCount()) return;
    std::vector<char> under(pose.size(), 0);
    for (const char* clavicle : {FPBody::kBoneClavicle[0], FPBody::kBoneClavicle[1]})
        if (const int c = m.NodeIndex(FPBody::MappedBone(boneMap, clavicle)); c >= 0) under[c] = 1;
    for (int i = 0; i < (int)pose.size(); ++i) {
        if (!under[i] && parents[i] >= 0 && under[parents[i]]) under[i] = 1;
        if (!under[i]) continue;
        const int r = rig.NodeIndex(m.NodeName(i));
        if (r >= 0) pose[i].R = glm::normalize(glm::slerp(pose[i].R, rigPose[r].R, weight));
    }
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

} // namespace

float FirstPersonBodyYaw(const glm::vec3& front, float fallback) {
    const glm::vec2 flat(front.x, front.z);
    if (glm::dot(flat, flat) < 1e-8f) return fallback;
    return std::atan2(front.x, front.z); // rotating +Z by this about +Y gives (sin, 0, cos)
}

float FirstPersonBodyWrapAngle(float radians) {
    const float twoPi = 6.28318530718f;
    radians = std::fmod(radians, twoPi);
    if (radians > 3.14159265359f) radians -= twoPi;
    else if (radians <= -3.14159265359f) radians += twoPi;
    return radians;
}

bool FirstPersonBodyShouldTurn(float offset, float thresholdDegrees) {
    return thresholdDegrees > 0.0f && std::abs(offset) > glm::radians(thresholdDegrees);
}

glm::vec2 FirstPersonBodyLocalMove(const glm::vec3& worldVelocity, float yaw) {
    const glm::quat r = YawRotation(yaw);
    const glm::vec3 v(worldVelocity.x, 0.0f, worldVelocity.z);
    return {glm::dot(v, r * kRight), glm::dot(v, r * kForward)};
}

float FirstPersonBodyFootPelvis(float offL, float offR, float maxDrop, float maxRaise) {
    return std::clamp(std::min(offL, offR), -std::max(maxDrop, 0.0f), std::max(maxRaise, 0.0f));
}

glm::vec3 FirstPersonBodyEye(const glm::vec3& restHead, const glm::vec3& head, float bob, const glm::vec3& offset) {
    const float b = std::clamp(bob, 0.0f, 1.0f);
    return restHead + (head - restHead) * b + kRight * offset.x + glm::vec3(0.0f, offset.y, 0.0f) + kForward * offset.z;
}

void FirstPersonBody::Fail(const std::string& message) {
    m_LastError = message;
    Log::Warn("First Person Body: " + message);
    m_Body = entt::null;
}

bool FirstPersonBody::Start(World& world, Player& player) {
    *this = FirstPersonBody{};
    BodyDebug::Info() = BodyDebug::Snapshot{};
    auto& reg = world.Registry;
    auto bodies = reg.view<FirstPersonBodyComponent>(entt::exclude<InactiveTag>);
    if (bodies.begin() == bodies.end()) return false;
    const entt::entity body = *bodies.begin(); // the first; a scene has one player
    const auto& cfg = reg.get<FirstPersonBodyComponent>(body);

    // The pieces: the root itself and its children, each a rigged model on the one skeleton.
    auto rigged = [&](entt::entity e) {
        const auto* rc = reg.try_get<RenderableComponent>(e);
        return rc && rc->ModelRef && rc->ModelRef->NodeCount() > 0;
    };
    std::vector<entt::entity> pieces;
    if (rigged(body)) pieces.push_back(body);
    if (const auto* h = reg.try_get<HierarchyComponent>(body))
        for (entt::entity child : h->Children)
            if (reg.valid(child) && !reg.all_of<InactiveTag>(child) && rigged(child)) pieces.push_back(child);
    // The first piece with an Animator Controller drives; the others follow it.
    for (entt::entity e : pieces)
        if (reg.all_of<AnimatorControllerComponent>(e)) { m_Driver = e; break; }
    if (pieces.empty()) { Fail("needs rigged body pieces (the object's own model, or child objects with models)."); return false; }
    if (m_Driver == entt::null) {
        Fail("needs an Animator Controller on one of its pieces (e.g. animations/fps_body_locomotion.controller).");
        return false;
    }
    m_Body = body;

    // The clips' travel comes out of the pose and is handed to the Player - never applied to
    // an object: this class places the body itself. Turns stay in the pose: the view owns the yaw.
    auto& ac = reg.get<AnimatorControllerComponent>(m_Driver);
    ac.RootMotion.Mode = (int)RootMotionMode::InPlace;
    ac.RootMotion.Rotation = true; // the turn clips' yaw is read (DeltaYaw) to turn the body; the pose stays facing forward
    ac.RootMotion.Vertical = false;

    Model& model = *reg.get<RenderableComponent>(m_Driver).ModelRef;
    m_HeadNode = model.NodeIndex(cfg.HeadBone);
    if (m_HeadNode < 0)
        Log::Warn("First Person Body: the body has no bone '" + cfg.HeadBone + "' - the camera stays at the eye height.");
    else
        m_RestHead = glm::vec3(model.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, m_HeadNode)[3]);

    m_BoneMap = FPBody::ParseBoneMap(cfg.BoneMap);
    m_ShoulderNode[0] = model.NodeIndex(Bone(FPBody::kBoneUpperArm[0]));
    m_ShoulderNode[1] = model.NodeIndex(Bone(FPBody::kBoneUpperArm[1]));

    auto names = [](const std::string& csv) {
        std::vector<std::string> out;
        std::stringstream list(csv);
        for (std::string name; std::getline(list, name, ',');)
            if (!(name = Trim(name)).empty()) out.push_back(name);
        return out;
    };
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::vector<std::string> hiddenBones = names(cfg.HiddenBones), hiddenParts = names(cfg.HiddenParts);
    int shadowOnly = 0;
    for (entt::entity e : pieces) {
        Model* m = reg.get<RenderableComponent>(e).ModelRef.get();
        std::vector<int> nodes;
        for (const std::string& b : hiddenBones) {
            const int n = m->NodeIndex(b);
            if (n >= 0) nodes.push_back(n);
            else if (e == m_Driver) Log::Warn("First Person Body: Hidden Bones names '" + b + "', which the body doesn't have.");
        }
        m->SetHiddenNodes(nodes);
        m_Models.push_back(reg.get<RenderableComponent>(e).ModelRef);
        m_Pieces.push_back(e);
        if (e != m_Driver)
            if (auto* pac = reg.try_get<AnimatorControllerComponent>(e)) {
                pac->Driver = m_Driver;
                // The same root-motion handling as the driver's, or the pieces' poses part company
                // (a stripped turn on one, the turn left in the pose on another).
                const auto& dm = reg.get<AnimatorControllerComponent>(m_Driver).RootMotion;
                pac->RootMotion.Mode = dm.Mode;
                pac->RootMotion.Bone = dm.Bone;
                pac->RootMotion.Rotation = dm.Rotation;
                pac->RootMotion.Vertical = dm.Vertical;
            }
        // A hidden piece still casts its shadow; the camera is inside the head.
        const std::string name = lower(reg.all_of<NameComponent>(e) ? reg.get<NameComponent>(e).Name : std::string());
        for (const std::string& part : hiddenParts)
            if (e != body && name.find(lower(part)) != std::string::npos) {
                reg.get<RenderableComponent>(e).CastShadows = RenderableComponent::ShadowCasting::ShadowsOnly;
                ++shadowOnly;
                break;
            }
    }

    // The input asks the blend tree for speeds its clips have.
    m_RunSpeed = std::max(cfg.RunSpeed, 0.01f);
    m_CrouchHeight = cfg.CrouchHeight;
    m_CrouchSpeed = cfg.CrouchSpeed;
    player.MoveSpeed = m_RunSpeed;
    player.SprintMultiplier = std::max(cfg.SprintSpeed, 0.01f) / m_RunSpeed;

    Log::Info("First Person Body: '" + (reg.all_of<NameComponent>(body) ? reg.get<NameComponent>(body).Name : std::string("body")) +
              "' is the player's body - " + std::to_string(pieces.size()) + " pieces, " + std::to_string(shadowOnly) +
              " shadow-only, root motion at responsiveness " + std::to_string(cfg.Responsiveness).substr(0, 4) + ".");
    return true;
}

void FirstPersonBody::Stop(World& world) {
    BodyDebug::Info() = BodyDebug::Snapshot{};
    BodyDebug::Clear();
    if (m_PoseSource != entt::null && world.Registry.valid(m_PoseSource)) world.Registry.remove<PoseSourceTag>(m_PoseSource);
    for (entt::entity e : m_ArmsTagged)
        if (world.Registry.valid(e)) world.Registry.remove<ViewModelTag>(e);
    for (const auto& m : m_Models)
        if (m) m->SetHiddenNodes({}); // model instances outlive Play; the rest is snapshot state
    *this = FirstPersonBody{};
}

void FirstPersonBody::BeforePlayerMove(Player& player, Camera& camera) {
    camera.Position -= m_CameraApplied;
    m_CameraApplied = glm::vec3(0.0f);
    player.MaxYawRate = 0.0f;
    player.CrouchHeight = IsActive() ? m_CrouchHeight : 0.0f;
    player.CrouchSpeedMultiplier = m_CrouchSpeed;
    if (!IsActive()) return;
    if (m_Still) {
        // Aiming is free within the turn threshold either side of the body; the limit is on the turn
        // beyond it, which is what the feet have to step round to (the body's yaw is atan2(x, z) of the
        // view, and Camera::Yaw is measured from +X toward +Z: 90 degrees minus it).
        player.MaxYawRate = m_MaxTurnRate;
        player.YawFreeCenter = 90.0f - glm::degrees(m_Yaw);
        player.YawFreeRange = m_TurnThreshold;
    }
    // Airborne, the input steers (there is no travel in a fall clip to follow).
    player.RootMotionVelocity = m_RootVelocity;
    // The landing clip's own travel would stop a running player dead: while it plays, the input steers.
    player.RootMotionWeight = player.Grounded && !m_InLand ? 1.0f - std::clamp(m_Responsiveness, 0.0f, 1.0f) : 0.0f;
}

void FirstPersonBody::Tick(World& world, const Player& player, const Camera& camera, float dt) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) { m_Body = entt::null; return; }
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    m_Responsiveness = cfg.Responsiveness;
    BodyDebug::Clear();
    m_SinceTrigger += dt;

    // Stand at the capsule's feet, facing the view.
    if (PhysicsWorld::HasCharacter()) {
        float f[3];
        PhysicsWorld::GetCharacterFootPosition(f);
        // A stair pops the capsule up (or down) in a frame: the body (and so the camera on its head)
        // stays where it was and eases to the new height; the legs' IK reaches the step meanwhile.
        const float rise = f[1] - m_LastCapsuleY;
        if (cfg.FootIK && m_HaveCapsule && player.Grounded && m_LastGrounded && dt > 0.0f && std::abs(rise) > cfg.StairPopRise &&
            std::abs(rise) / dt > cfg.StairPopRate)
            m_StepOffset = std::clamp(m_StepOffset - rise, -0.3f, 0.3f);
        m_StepOffset -= m_StepOffset * Follow(dt, player.Grounded && cfg.FootIK ? cfg.StairEase : 0.03f);
        m_LastCapsuleY = f[1];
        m_LastGrounded = player.Grounded;
        m_HaveCapsule = true;
        m_Feet = glm::vec3(f[0], f[1] + m_StepOffset, f[2]);
    } else {
        m_Feet = camera.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f);
    }
    m_ViewYaw = FirstPersonBodyYaw(camera.Front(), m_ViewYaw);
    if (!m_HaveHeading) { m_Yaw = m_ViewYaw; m_HaveHeading = true; }
    world.SetWorldPose(m_Body, m_Feet, YawRotation(m_Yaw));
    auto fireTrigger = [&](AnimatorControllerComponent& a, const char* name) { a.SetTrigger(name); m_LastTrigger = name; m_SinceTrigger = 0.0f; };

    // The movement, in the body's frame, eased so the gait changes smoothly.
    const glm::vec2 target = FirstPersonBodyLocalMove(player.WishVelocity, m_Yaw);
    // Letting go at speed, the gait holds for the moment a stop clip is being picked (the blend would
    // otherwise slow the body on its own first, and the stop clip's own travel come on top of it).
    const bool holdForStop = cfg.StartStopClips && glm::length(target) < 0.01f && m_IdleTime < cfg.StopDebounce &&
                         glm::length(m_Move) > (player.Crouched ? cfg.StopMinSpeedCrouched : cfg.StopMinSpeed);
    if (!holdForStop) m_Move += (target - m_Move) * Follow(dt, cfg.ParamSmoothing);
    m_AirTime = player.Grounded ? 0.0f : m_AirTime + dt;
    m_Grounded = player.Grounded;
    m_InLand = reg.valid(m_Driver) && reg.get<AnimatorControllerComponent>(m_Driver).InState(FPBody::kStateLand);

    if (!reg.valid(m_Driver)) { m_Body = entt::null; return; }
    auto& ac = reg.get<AnimatorControllerComponent>(m_Driver);

    // The heading. Moving, the body faces the view (a quick ease, no pop). Standing still with a
    // Turn Threshold, it keeps its heading until the view is that far off, then a turn clip carries
    // it round (the clip's own yaw turns it, so the feet plant).
    const bool still = player.Grounded && glm::length(glm::vec2(player.WishVelocity.x, player.WishVelocity.z)) < 0.1f &&
                       glm::length(m_Move) < 0.2f;
    m_Still = still && cfg.TurnThreshold > 0.0f;
    m_MaxTurnRate = cfg.MaxTurnRate;
    m_TurnThreshold = cfg.TurnThreshold;
    float offset = FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw);
    if (cfg.TurnThreshold <= 0.0f) {
        m_Yaw = m_ViewYaw;
        m_Turning = false;
    } else if (m_Turning) {
        m_TurnTime += dt;
        if (ac.InState(FPBody::kStateTurn) || ac.InState(FPBody::kStateCrouchTurn)) {
            const float step = glm::radians(ac.RootMotion.DeltaYaw);
            m_Yaw += step;
            m_TurnDone += step;
        }
        offset = FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw);
        // Done when the clip has played out (a 180 takes longer than a 90), the view is reached, or
        // the player moves off; the timeout is only a guard.
        const bool clipDone = m_TurnTime > cfg.TurnMinTime && (!ac.InState(FPBody::kStateTurn) || ac.StateTime >= 0.97f);
        if (!still || std::abs(offset) < glm::radians(cfg.TurnEndAngle) || clipDone || m_TurnTime > cfg.TurnTimeout) {
            Log::Info("First Person Body: turned " + std::to_string(glm::degrees(m_TurnDone)).substr(0, 6) + " deg in " +
                      std::to_string(m_TurnTime).substr(0, 4) + " s, " + std::to_string(glm::degrees(offset)).substr(0, 6) + " deg off the view.");
            m_Turning = false;
        }
    } else if (still) {
        if (FirstPersonBodyShouldTurn(offset, cfg.TurnThreshold)) {
            m_Turning = true;
            m_TurnTime = 0.0f;
            m_TurnDone = 0.0f;
            ac.SetFloat(FPBody::kTurnAngle, std::clamp(glm::degrees(offset), -180.0f, 180.0f));
        }
    } else {
        m_Yaw += offset * Follow(dt, cfg.TurnMoveEase);
    }
    // The view can outrun a turn clip: the body never lags it by more than this (a bounded slide of
    // the feet beats a chest twisted right round). Keep Mouse Sensitivity low enough that the turn clips keep up.
    if (cfg.TurnThreshold > 0.0f) {
        const float lag = FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw), maxLag = glm::radians(std::max(cfg.TurnThreshold + cfg.TurnLagMargin, cfg.TurnLagFloor));
        if (std::abs(lag) > maxLag) m_Yaw = m_ViewYaw - std::copysign(maxLag, lag);
    }
    m_Yaw = FirstPersonBodyWrapAngle(m_Yaw);
    m_Twist = FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw);
    world.SetWorldPose(m_Body, m_Feet, YawRotation(m_Yaw));
    ac.SetBool(FPBody::kTurning, m_Turning);

    // Starting and stopping: a clip for each, picked by the direction, only from plain locomotion (a
    // trigger fired elsewhere would wait and go off later). A stop needs a moment of no input
    // (tapping between keys isn't one: 50 ms) and some speed to shed.
    {
        const glm::vec2 wishLocal = FirstPersonBodyLocalMove(player.WishVelocity, m_Yaw);
        const float wishLen = glm::length(wishLocal);
        const bool wantsMove = wishLen > 0.1f;
        const bool plain = cfg.StartStopClips && player.Grounded && (ac.InState(FPBody::kStateLocomotion) || ac.InState(FPBody::kStateCrouchLoco)) && !m_Turning;
        ac.SetBool(FPBody::kMoving, wantsMove);
        // Standing still, dropping into or rising out of a crouch plays its transition clip; on the
        // move it is just the crossfade between the gaits.
        if (cfg.StartStopClips && player.Grounded && !wantsMove && glm::length(m_Move) < 0.4f && player.Crouched != m_WasCrouched) {
            if (player.Crouched && ac.InState(FPBody::kStateLocomotion)) fireTrigger(ac, FPBody::kCrouchDown);
            if (!player.Crouched && ac.InState(FPBody::kStateCrouchLoco)) fireTrigger(ac, FPBody::kCrouchUp);
        }
        m_WasCrouched = player.Crouched;
        // How far the clips carry the body: logged, to tune them against the feel.
        const bool inStop = ac.InState(FPBody::kStateStop) || ac.InState(FPBody::kStateStopRun), inStart = ac.InState(FPBody::kStateStart);
        const float travel = glm::length(glm::vec2(ac.RootMotion.DeltaPosition.x, ac.RootMotion.DeltaPosition.z));
        if (inStop) m_StopDistance += travel;
        else if (m_StopDistance > 0.0f) {
            Log::Info("First Person Body: the stop clip carried the body " + std::to_string(m_StopDistance).substr(0, 4) + " m.");
            m_StopDistance = 0.0f;
        }
        if (inStart) m_StartDistance += travel;
        else if (m_StartDistance > 0.0f) {
            Log::Info("First Person Body: the start clip carried the body " + std::to_string(m_StartDistance).substr(0, 4) + " m.");
            m_StartDistance = 0.0f;
        }
        if (wantsMove) {
            const glm::vec2 dir = wishLocal / wishLen;
            if (plain && m_IdleTime >= cfg.StartIdleTime && glm::length(m_Move) < cfg.StartMaxMove) {
                ac.SetFloat(FPBody::kStartX, dir.x);
                ac.SetFloat(FPBody::kStartY, dir.y);
                fireTrigger(ac, FPBody::kStart);
            }
            m_LastDir = dir;
            m_LastSprint = glm::length(glm::vec2(player.WishVelocity.x, player.WishVelocity.z)) > m_RunSpeed * 1.05f;
            m_IdleTime = 0.0f;
            m_MoveTime += dt;
        } else {
            const float before = m_IdleTime;
            m_IdleTime += dt;
            // A stop needs a run to stop from: a tap of the keys (or a step or two) just eases to a halt.
            const bool ranEnough = m_MoveTime >= (player.Crouched ? cfg.StopMinRunTimeCrouched : cfg.StopMinRunTime);
            if (before < cfg.StopDebounce && m_IdleTime >= cfg.StopDebounce) m_MoveTime = 0.0f;
            if (plain && ranEnough && before < cfg.StopDebounce && m_IdleTime >= cfg.StopDebounce &&
                glm::length(m_Move) > (player.Crouched ? cfg.StopMinSpeedCrouched : cfg.StopMinSpeed)) {
                ac.SetFloat(FPBody::kStopX, m_LastDir.x);
                ac.SetFloat(FPBody::kStopY, m_LastDir.y);
                fireTrigger(ac, m_LastSprint && m_LastDir.y > cfg.StopRunForward ? FPBody::kStopRun : FPBody::kStop);
            }
        }
    }

    ac.SetFloat(FPBody::kMoveX, m_Move.x);
    ac.SetFloat(FPBody::kMoveY, m_Move.y);
    ac.SetFloat(FPBody::kSpeed, glm::length(m_Move));
    ac.SetBool(FPBody::kSprint, glm::length(glm::vec2(player.WishVelocity.x, player.WishVelocity.z)) > m_RunSpeed * 1.05f);
    ac.SetBool(FPBody::kGrounded, player.Grounded);
    ac.SetBool(FPBody::kCrouched, player.Crouched);
    // Off the ground for a moment (not a step down a stair): falling.
    ac.SetBool(FPBody::kAirborne, m_AirTime > cfg.AirborneDelay);
    if (player.Jumped) fireTrigger(ac, FPBody::kJump);

    // What the body is doing, for the Inspector's live readout and the Scene viewport's overlay.
    {
        BodyDebug::Snapshot& d = BodyDebug::Info();
        d.Valid = true;
        d.AnimatorState = ac.StateName;
        d.StateTime = ac.StateTime;
        d.Turning = m_Turning;
        d.ViewOffsetDeg = glm::degrees(FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw));
        d.TwistDeg = glm::degrees(m_Twist);
        d.Still = m_Still;
        d.IdleTime = m_IdleTime;
        d.MoveTime = m_MoveTime;
        d.MoveSpeed = glm::length(m_Move);
        d.RootSpeed = glm::length(m_RootVelocity);
        d.StepOffset = m_StepOffset;
        d.ArmsWeight = m_ArmsWeight;
        d.LastTrigger = m_LastTrigger;
        d.LastTriggerAgo = m_SinceTrigger;
        if (BodyDebug::Enabled()) {
            const glm::vec3 base = m_Feet + glm::vec3(0.0f, 0.03f, 0.0f);
            const auto heading = [&](float yaw) { return glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)); };
            BodyDebug::Arrow(base, base + heading(m_Yaw) * 0.9f, glm::vec4(1.0f, 0.85f, 0.2f, 1.0f));      // the body's heading
            BodyDebug::Arrow(base, base + heading(m_ViewYaw) * 0.9f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));   // where the view faces
            if (cfg.TurnThreshold > 0.0f) {                                                                // the turn threshold wedge
                const float t = glm::radians(cfg.TurnThreshold);
                const glm::vec4 wedge(1.0f, 0.55f, 0.15f, 0.9f);
                BodyDebug::Line(base, base + heading(m_Yaw + t) * 1.0f, wedge);
                BodyDebug::Line(base, base + heading(m_Yaw - t) * 1.0f, wedge);
                for (int i = 0; i < 12; ++i)
                    BodyDebug::Line(base + heading(m_Yaw - t + 2.0f * t * (float)i / 12.0f) * 1.0f,
                                    base + heading(m_Yaw - t + 2.0f * t * (float)(i + 1) / 12.0f) * 1.0f, wedge);
            }
            if (glm::length(m_RootVelocity) > 0.05f)                                                       // what the clips carry the capsule at
                BodyDebug::Arrow(base + glm::vec3(0.0f, 0.1f, 0.0f), base + glm::vec3(0.0f, 0.1f, 0.0f) + m_RootVelocity * 0.4f, glm::vec4(0.2f, 0.9f, 1.0f, 1.0f));
            if (std::abs(m_StepOffset) > 0.005f)                                                           // a stair being eased out
                BodyDebug::Line(base, base + glm::vec3(0.0f, m_StepOffset, 0.0f), glm::vec4(1.0f, 0.2f, 0.8f, 1.0f));
        }
    }
}

void FirstPersonBody::LateUpdate(World& world, Camera& camera, float dt, entt::entity weaponArms,
                                 const std::string& rigCameraBone) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) { m_Body = entt::null; return; }
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);

    // This step's root motion, for the capsule's next move.
    if (!reg.valid(m_Driver)) { m_Body = entt::null; return; }
    const auto& ac = reg.get<AnimatorControllerComponent>(m_Driver);
    const glm::vec3 d = ac.RootMotion.DeltaPosition;
    m_RootVelocity = dt > 0.0f && !m_Turning ? glm::vec3(d.x, 0.0f, d.z) / dt : glm::vec3(0.0f);

    ApplyFootIK(world, cfg, dt); // the pelvis and legs first: everything after reads the final pose

    // The chest follows the view's pitch first, so the head (and the camera on it) and the
    // shoulders are where they will be drawn.
    const auto* rc = reg.try_get<RenderableComponent>(m_Driver);
    const Model* model = rc ? rc->ModelRef.get() : nullptr;
    glm::mat4 head(1.0f);
    const bool haveHead = model && m_HeadNode >= 0 && model->NodeTransform(model->NodeName(m_HeadNode), head);
    const glm::vec3 headAnimated = glm::vec3(head[3]); // the clips' head, before the chest tilts
    const bool haveShoulders = model && m_ShoulderNode[0] >= 0 && m_ShoulderNode[1] >= 0;
    // The shoulders as the arms will be drawn: the arms piece with the rig's arm shape on it (the
    // clavicles move the shoulders), else the driver's. Model space.
    const Model* armRig = weaponArms != entt::null && reg.valid(weaponArms) && reg.all_of<RenderableComponent>(weaponArms)
                              ? reg.get<RenderableComponent>(weaponArms).ModelRef.get()
                              : nullptr;
    auto shoulderPair = [&](glm::vec3& left, glm::vec3& right) {
        left = right = glm::vec3(0.0f);
        if (haveShoulders) {
            left = ModelPoint(*model, m_ShoulderNode[0]);
            right = ModelPoint(*model, m_ShoulderNode[1]);
        }
        if (!armRig || armRig->AppliedLocalPose().empty()) return;
        std::string want = cfg.ArmsPiece;
        for (char& c : want) c = (char)std::tolower((unsigned char)c);
        for (size_t k = 0; k < m_Pieces.size() && !want.empty(); ++k) {
            if (m_Pieces[k] == m_Body || !reg.valid(m_Pieces[k]) || !reg.all_of<NameComponent>(m_Pieces[k])) continue;
            std::string name = reg.get<NameComponent>(m_Pieces[k]).Name;
            for (char& c : name) c = (char)std::tolower((unsigned char)c);
            if (name.find(want) == std::string::npos) continue;
            const Model& m = *m_Models[k];
            IK::Pose pose = m.AppliedLocalPose();
            const int ul = m.NodeIndex(Bone(FPBody::kBoneUpperArm[0])), ur = m.NodeIndex(Bone(FPBody::kBoneUpperArm[1]));
            if (pose.empty() || (int)pose.size() != m.NodeCount() || ul < 0 || ur < 0) break;
            std::vector<int> parents(pose.size());
            for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
            CopyArmShape(m, *armRig, m_ArmsWeight, pose, parents, m_BoneMap);
            std::vector<glm::mat4> globals;
            IK::ComputeGlobals(pose, parents, globals);
            left = IK::Position(globals[ul]);
            right = IK::Position(globals[ur]);
            break;
        }
    };
    auto shouldersNow = [&]() {
        glm::vec3 l, r;
        shoulderPair(l, r);
        return 0.5f * (l + r);
    };
    const glm::vec3 shouldersAnimated = shouldersNow();
    if (cfg.SpineAim > 0.0f || cfg.SpineTwist > 0.0f) ApplySpineAim(camera, cfg.SpineAim, m_Twist * cfg.SpineTwist);
    // Close the loop on the chest's heading: the clips' own spine yaw (a turn leads with the chest)
    // and the pitch tilt would leave the shoulder line off the view's, and one shoulder then sits
    // further from the gun than the other (the arm stretches). Turn the spine by what is left.
    if (cfg.SpineTwist > 0.0f) {
        glm::vec3 l, r;
        shoulderPair(l, r);
        const glm::vec3 across = l - r; // model +X (the mannequin's left) when the chest faces forward
        if (across.x * across.x + across.z * across.z > 1e-6f) {
            const float chestYaw = std::atan2(-across.z, across.x); // rotating +X about +Y by t gives z = -sin t
            const float error = FirstPersonBodyWrapAngle(m_Twist - chestYaw);
            if (std::abs(error) > 0.002f) ApplySpineAim(camera, 0.0f, error * std::min(cfg.SpineTwist, 1.0f));
        }
    }
    if (!haveHead) return;

    // The camera into the head: smoothed in the body's own frame, so it never trails the move.
    // The bob is the clips' head motion; what the chest's tilt does to the head goes on in full,
    // so the shoulders stay the same distance from the eye as the view pitches.
    glm::vec3 tilt(0.0f);
    if (model->NodeTransform(model->NodeName(m_HeadNode), head)) tilt = glm::vec3(head[3]) - headAnimated;
    const glm::vec3 target = FirstPersonBodyEye(m_RestHead, headAnimated, cfg.HeadBob, cfg.CameraOffset);
    if (!m_HaveEye) { m_Eye = target; m_HaveEye = true; }
    m_Eye += (target - m_Eye) * Follow(dt, cfg.CameraSmoothing);

    const auto& t = reg.get<TransformComponent>(m_Body);
    const glm::quat yaw = YawRotation(m_Yaw);
    glm::vec3 eyeWorld = m_Feet + yaw * (t.Scale * (m_Eye + tilt));

    if (m_ArmsWeight <= 1e-3f) { m_HaveShoulders = false; m_HaveRigOffset = false; }
    // Weapon arms: the eye goes where the arms rig's is relative to its shoulders, measured on the
    // rig (camera bone to its upper arms, in the camera's frame), put on the body's own shoulders.
    // The rig's hands are placed relative to the eye, so they are then within the body's reach.
    if (m_ArmsWeight > 1e-3f && haveShoulders && weaponArms != entt::null && reg.valid(weaponArms) &&
        reg.all_of<RenderableComponent>(weaponArms)) {
        const Model* rig = reg.get<RenderableComponent>(weaponArms).ModelRef.get();
        glm::mat4 sl(1.0f), sr(1.0f), cb(1.0f);
        if (rig && rig->NodeTransform(FPBody::kBoneUpperArm[0], sl) && rig->NodeTransform(FPBody::kBoneUpperArm[1], sr)) {
            if (!rigCameraBone.empty()) rig->NodeTransform(rigCameraBone, cb);
            const glm::mat4 rigWorld = world.ComposeWorldTransform(weaponArms);
            const glm::vec3 fromEye = glm::mat3(rigWorld) * (0.5f * (glm::vec3(sl[3]) + glm::vec3(sr[3])) - glm::vec3(cb[3]));
            const glm::vec3 inCamera(glm::dot(fromEye, camera.Right()), glm::dot(fromEye, camera.Up()), glm::dot(fromEye, camera.Front()));
            // The clips sway their shoulders about the camera; that must not move the view, so only a
            // slow drift of this offset is followed.
            if (!m_HaveRigOffset) { m_RigEyeToShoulders = inCamera; m_HaveRigOffset = true; }
            m_RigEyeToShoulders += (inCamera - m_RigEyeToShoulders) * Follow(dt, 0.4f);

            const glm::vec3 shoulders = shouldersNow();
            const glm::vec3 toRest = shoulders - shouldersAnimated; // the chest's tilt at the shoulders
            // The eye follows Head Bob of the shoulders' motion about their slow average - not about
            // the bind pose, whose shoulders sit elsewhere than the standing pose's, which would put
            // the shoulders (and so the hands' reach) that far off.
            if (!m_HaveShoulders) { m_ShouldersSlow = m_Shoulders = shouldersAnimated; m_HaveShoulders = true; }
            m_ShouldersSlow += (shouldersAnimated - m_ShouldersSlow) * Follow(dt, 0.5f);
            const glm::vec3 shoulderTarget = m_ShouldersSlow + (shouldersAnimated - m_ShouldersSlow) * std::clamp(cfg.HeadBob, 0.0f, 1.0f);
            m_Shoulders += (shoulderTarget - m_Shoulders) * Follow(dt, cfg.CameraSmoothing);
            // The eye may trail or under-follow the shoulders by only this much: a clip that throws the
            // shoulders about (a stop pulling the body back) would otherwise carry them out of the
            // arms' reach and a hand would come off the gun. Bigger motions move the camera with them.
            const float kEyeSlack = cfg.EyeSlack;
            const glm::vec3 gap = shouldersAnimated - m_Shoulders;
            if (const float gapLen = glm::length(gap); gapLen > kEyeSlack) m_Shoulders = shouldersAnimated - gap / gapLen * kEyeSlack;
            BodyDebug::Info().EyeSlack = glm::length(shouldersAnimated - m_Shoulders);
            // A little slack: the shoulders sit this much further toward the gun than the rig's, so the
            // support arm is never at full stretch (a straight arm jumps at every small motion).
            const float kReachSlack = cfg.ReachSlack;
            const glm::vec3 offset = camera.Right() * m_RigEyeToShoulders.x + camera.Up() * m_RigEyeToShoulders.y +
                                     camera.Front() * (m_RigEyeToShoulders.z + kReachSlack);
            const glm::vec3 fromShoulders = m_Feet + yaw * (t.Scale * (m_Shoulders + toRest)) - offset;
            // Remember where this puts the eye against the head's: unarmed the eye keeps that height, or
            // the camera would jump when the gun is holstered.
            const glm::vec3 delta = glm::inverse(yaw) * (fromShoulders - eyeWorld);
            if (!m_HaveArmedEye) { m_ArmedEyeDelta = delta; m_HaveArmedEye = true; }
            if (m_ArmsWeight > 0.99f) m_ArmedEyeDelta += (delta - m_ArmedEyeDelta) * Follow(dt, 0.5f);
            eyeWorld = glm::mix(eyeWorld, fromShoulders, std::min(m_ArmsWeight, 1.0f));
        }
    }
    if (m_HaveArmedEye) eyeWorld += yaw * m_ArmedEyeDelta * (1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f));
    if (BodyDebug::Enabled()) {
        const glm::vec3 shoulders = m_Feet + yaw * (t.Scale * m_Shoulders);
        BodyDebug::Cross(eyeWorld, 0.04f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));       // the camera
        BodyDebug::Cross(shoulders, 0.04f, glm::vec4(1.0f, 0.3f, 1.0f, 1.0f));      // the shoulders the eye is anchored to
        BodyDebug::Line(shoulders, eyeWorld, glm::vec4(1.0f, 0.3f, 1.0f, 0.7f));
    }
    m_CameraApplied = eyeWorld - camera.Position;
    camera.Position = eyeWorld;
}

// Puts each foot on the ground under it: a ray down from the animated foot gives how far the ground
// is above / below the capsule's, the pelvis drops to the lower foot, and the legs are re-solved to
// the offset feet (tilted toward the ground while planted). On every piece so they stay one skeleton.
void FirstPersonBody::ApplyFootIK(World& world, const FirstPersonBodyComponent& cfg, float dt) {
    auto& reg = world.Registry;
    const bool on = cfg.FootIK && m_Grounded && PhysicsWorld::HasCharacter() && reg.valid(m_Driver) &&
                    !reg.get<AnimatorControllerComponent>(m_Driver).HasTag(FPBody::kTagAirborne);
    m_FootWeight += ((on ? 1.0f : 0.0f) - m_FootWeight) * Follow(dt, cfg.FootIKFade);
    if (m_FootWeight < 1e-3f) {
        m_HaveFoot = false;
        m_FootPlanted[0] = m_FootPlanted[1] = false;
        m_FootLockWeight[0] = m_FootLockWeight[1] = 0.0f;
        return;
    }
    const auto* rc = reg.try_get<RenderableComponent>(m_Driver);
    if (!rc || !rc->ModelRef) return;
    const Model& drv = *rc->ModelRef;
    const IK::Pose driverPose = drv.AppliedLocalPose();
    if (driverPose.empty() || (int)driverPose.size() != drv.NodeCount()) return;
    const int footNode[2] = {drv.NodeIndex(Bone(FPBody::kBoneFoot[0])), drv.NodeIndex(Bone(FPBody::kBoneFoot[1]))};
    if (footNode[0] < 0 || footNode[1] < 0) return;

    const auto& t = reg.get<TransformComponent>(m_Body);
    const float scale = std::max(t.Scale.y, 1e-3f);
    const glm::quat yaw = YawRotation(m_Yaw);
    std::vector<int> parents(driverPose.size());
    for (int i = 0; i < (int)driverPose.size(); ++i) parents[i] = drv.NodeParent(i);
    std::vector<glm::mat4> globals;
    IK::ComputeGlobals(driverPose, parents, globals);

    const float maxDrop = std::max(cfg.FootIKMaxDrop, 0.0f);
    float animHeight[2];
    glm::vec3 lockShift[2] = {glm::vec3(0.0f), glm::vec3(0.0f)}; // world, horizontal: animated foot -> pinned foot
    // No pinning while the body turns on the spot (the feet must step) or the heading swings.
    const bool yawSteady = !m_Turning && std::abs(FirstPersonBodyWrapAngle(m_Yaw - m_FootYaw)) < glm::radians(2.0f);
    m_FootYaw = m_Yaw;
    for (int s = 0; s < 2; ++s) {
        const glm::vec3 footWorld = m_Feet + yaw * (scale * IK::Position(globals[footNode[s]]));
        animHeight[s] = footWorld.y - m_Feet.y;
        // Foot lock: a planted foot stays where it landed instead of sliding when the animation and the
        // capsule's travel disagree a little; it lets go when the foot lifts or the mismatch gets big.
        const bool planted = animHeight[s] < cfg.FootPlantedHeight && yawSteady;
        if (planted) {
            if (!m_FootPlanted[s]) { m_FootPlanted[s] = true; m_FootLock[s] = footWorld; }
            const glm::vec3 drift(footWorld.x - m_FootLock[s].x, 0.0f, footWorld.z - m_FootLock[s].z);
            if (glm::dot(drift, drift) > cfg.FootLockDrift * cfg.FootLockDrift) m_FootLock[s] = footWorld; // too far: plant again here
        } else {
            m_FootPlanted[s] = false;
        }
        m_FootLockWeight[s] += ((m_FootPlanted[s] ? 1.0f : 0.0f) - m_FootLockWeight[s]) * Follow(dt, m_FootPlanted[s] ? cfg.FootLockEaseIn : cfg.FootLockEaseOut);
        lockShift[s] = glm::vec3(m_FootLock[s].x - footWorld.x, 0.0f, m_FootLock[s].z - footWorld.z) * (m_FootLockWeight[s] * m_FootWeight);
        float offset = 0.0f;
        glm::vec3 normal(0.0f, 1.0f, 0.0f);
        const float origin[3] = {footWorld.x, footWorld.y + cfg.FootRayUp, footWorld.z};
        const float down[3] = {0.0f, -1.0f, 0.0f};
        QueryFilter filter;
        filter.HitTriggers = 0;
        RaycastHit hit;
        const bool rayHit = PhysicsWorld::RaycastSolid(origin, down, cfg.FootRayLength, filter, hit) && hit.Hit;
        if (rayHit) {
            offset = std::clamp(hit.Point[1] - m_Feet.y, -maxDrop, cfg.FootMaxRaise);
            normal = glm::normalize(glm::vec3(hit.Normal[0], hit.Normal[1], hit.Normal[2]));
            if (normal.y < 0.5f) normal = glm::vec3(0.0f, 1.0f, 0.0f); // a wall, not a floor
        }
        if (BodyDebug::Enabled()) {
            const glm::vec3 from(origin[0], origin[1], origin[2]);
            const glm::vec3 to = rayHit ? glm::vec3(hit.Point[0], hit.Point[1], hit.Point[2]) : from + glm::vec3(0.0f, -cfg.FootRayLength, 0.0f);
            BodyDebug::Line(from, to, rayHit ? glm::vec4(0.3f, 1.0f, 0.4f, 1.0f) : glm::vec4(1.0f, 0.3f, 0.3f, 1.0f)); // the ground ray
            if (rayHit) BodyDebug::Cross(to, 0.04f, glm::vec4(0.3f, 1.0f, 0.4f, 1.0f));
            BodyDebug::Cross(footWorld, 0.05f, m_FootPlanted[s] ? glm::vec4(0.3f, 1.0f, 0.4f, 1.0f) : glm::vec4(1.0f, 0.4f, 0.3f, 1.0f)); // the animated foot: green planted
            if (m_FootPlanted[s]) BodyDebug::Cross(m_FootLock[s], 0.03f, glm::vec4(0.3f, 0.9f, 1.0f, 1.0f));                            // where it is pinned
        }
        if (!m_HaveFoot) { m_FootOffset[s] = offset; m_FootNormal[s] = normal; }
        m_FootOffset[s] += (offset - m_FootOffset[s]) * Follow(dt, cfg.FootOffsetEase);
        m_FootNormal[s] = glm::normalize(m_FootNormal[s] + (normal - m_FootNormal[s]) * Follow(dt, cfg.FootNormalEase));
    }
    m_HaveFoot = true;
    {
        BodyDebug::Snapshot& d = BodyDebug::Info();
        d.FootWeight = m_FootWeight;
        for (int s = 0; s < 2; ++s) { d.FootOffset[s] = m_FootOffset[s]; d.FootPlanted[s] = m_FootPlanted[s]; d.FootLock[s] = m_FootLockWeight[s]; }
    }

    const float pelvisDelta = FirstPersonBodyFootPelvis(m_FootOffset[0], m_FootOffset[1], maxDrop, cfg.PelvisMaxRaise) * m_FootWeight;
    static const char* const kLegs[2][3] = {{FPBody::kBoneThigh[0], FPBody::kBoneCalf[0], FPBody::kBoneFoot[0]}, {FPBody::kBoneThigh[1], FPBody::kBoneCalf[1], FPBody::kBoneFoot[1]}};
    const glm::quat yawInverse = glm::inverse(yaw);
    for (const auto& mp : m_Models) {
        Model& m = *mp;
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        const int pelvis = m.NodeIndex(Bone(FPBody::kBonePelvis));
        if (pelvis < 0) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        IK::OffsetBone(pose, parents, globals, pelvis, glm::vec3(0.0f, pelvisDelta / scale, 0.0f),
                       glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f));
        for (int s = 0; s < 2; ++s) {
            const int thigh = m.NodeIndex(Bone(kLegs[s][0])), calf = m.NodeIndex(Bone(kLegs[s][1])), foot = m.NodeIndex(Bone(kLegs[s][2]));
            if (thigh < 0 || calf < 0 || foot < 0) continue;
            const glm::vec3 target = IK::Position(globals[foot]) +
                                     glm::vec3(0.0f, (m_FootOffset[s] * m_FootWeight - pelvisDelta) / scale, 0.0f) +
                                     yawInverse * lockShift[s] / scale;
            // The foot lies on the ground while it is planted: its own rotation tilted to the normal.
            const float planted = 1.0f - std::clamp((animHeight[s] - 0.06f) / 0.09f, 0.0f, 1.0f);
            const glm::vec3 normal = yawInverse * m_FootNormal[s];
            const float angle = std::min(std::acos(std::clamp(normal.y, -1.0f, 1.0f)), glm::radians(cfg.FootTiltMax)) * planted * m_FootWeight;
            glm::quat footRot = IK::Rotation(globals[foot]);
            const glm::vec3 axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal);
            if (angle > 1e-4f && glm::dot(axis, axis) > 1e-8f) footRot = glm::angleAxis(angle, glm::normalize(axis)) * footRot;
            // Flat ground and nothing pinned: leave the clip's own pose alone.
            if (glm::length(target - IK::Position(globals[foot])) < 5e-4f && angle < 1e-3f) continue;
            IK::SolveTwoBone(pose, parents, globals, thigh, calf, foot, target, &footRot, 1.0f);
        }
        m.ApplyLocalPose(pose);
    }
}

// Tilts the spine by `amount` of the camera's pitch and twists it by `twist` radians, spread evenly over spine_01..spine_05 (pitch about
// the model's X axis, twist about Y), on every piece so they stay one skeleton.
void FirstPersonBody::ApplySpineAim(const Camera& camera, float amount, float twist) {
    const float pitch = std::asin(std::clamp(camera.Front().y, -1.0f, 1.0f)); // up is positive
    const char* const* kSpine = FPBody::kBoneSpine;
    std::vector<int> parents;
    std::vector<glm::mat4> globals;
    for (const auto& mp : m_Models) {
        Model& m = *mp;
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        std::vector<int> bones;
        for (const std::string& name : {Bone(kSpine[0]), Bone(kSpine[1]), Bone(kSpine[2]), Bone(kSpine[3]), Bone(kSpine[4])})
            if (const int i = m.NodeIndex(name); i >= 0) bones.push_back(i);
        if (bones.empty()) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        // Rotating +Z about +X by theta gives (0, -sin, cos): looking up needs a negative angle.
        // The twist about the model's up turns the chest toward the view (positive = to the left).
        const glm::quat step = glm::angleAxis(twist / (float)bones.size(), glm::vec3(0.0f, 1.0f, 0.0f)) *
                               glm::angleAxis(-pitch * amount / (float)bones.size(), glm::vec3(1.0f, 0.0f, 0.0f));
        // Only the next spine bone's global is read before ApplyLocalPose re-derives the lot, so
        // refresh just the path to it rather than the whole upper body after every bone.
        for (size_t b = 0; b < bones.size(); ++b) {
            const int i = bones[b];
            if (b > 0) IK::RefreshPath(pose, parents, globals, bones[b - 1], i);
            IK::OffsetBoneOnly(pose, parents, globals, i, glm::vec3(0.0f), step, IK::Position(globals[i]));
        }
        m.ApplyLocalPose(pose);
    }
}

void FirstPersonBody::ArmsLateUpdate(World& world, entt::entity weaponArms, float viewModelFov, float dt) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) return;
    const FirstPersonBodyComponent& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    const bool enabled = cfg.WeaponArms;
    const bool haveRig = enabled && weaponArms != entt::null && reg.valid(weaponArms) &&
                         reg.all_of<RenderableComponent>(weaponArms) && viewModelFov > 0.0f;
    // The rig keeps posing (its hands are the targets) but is neither drawn nor casts a shadow: the
    // body's arms, which follow it, are what is seen and what shadows.
    if (haveRig) {
        if (!reg.all_of<PoseSourceTag>(weaponArms)) reg.emplace<PoseSourceTag>(weaponArms);
        m_PoseSource = weaponArms;
    } else if (m_PoseSource != entt::null) {
        if (reg.valid(m_PoseSource)) reg.remove<PoseSourceTag>(m_PoseSource); // Weapon Arms off: it is the visible arms again
        m_PoseSource = entt::null;
    }
    // Holstered, the rig is inactive: the arms ease back to the locomotion clips' pose, but out of the
    // view-model pass at once (there they would show as a hand at the bottom of the view).
    const bool follow = haveRig && !reg.all_of<InactiveTag>(weaponArms);
    // Drawn, the arms take the rig's hands from the first frame: eased in, the draw (and Play's first
    // frames) would show the hands out of the idle pose while the gun is already up. Only off eases.
    if (follow) m_ArmsWeight = 1.0f;
    else m_ArmsWeight -= m_ArmsWeight * Follow(dt, cfg.ArmsEaseOut);
    const bool viewModelArms = follow;
    // The body's arms piece goes into the view-model pass with the gun (its hands then sit where the
    // rig's do); off, it is an ordinary piece of the body again.
    for (size_t k = 0; k < m_Pieces.size(); ++k) {
        const entt::entity e = m_Pieces[k];
        if (e == m_Body || !reg.valid(e) || !reg.all_of<NameComponent>(e)) continue;
        std::string name = reg.get<NameComponent>(e).Name, want = reg.get<FirstPersonBodyComponent>(m_Body).ArmsPiece;
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        for (char& c : want) c = (char)std::tolower((unsigned char)c);
        if (want.empty() || name.find(want) == std::string::npos) continue;
        if (viewModelArms) {
            if (!reg.all_of<ViewModelTag>(e)) { reg.emplace<ViewModelTag>(e); m_ArmsTagged.push_back(e); }
        } else if (reg.all_of<ViewModelTag>(e)) {
            reg.remove<ViewModelTag>(e);
        }
        // Easing off a holstered gun the pose is still the rig's (its hands, no gun to hold): shown, it
        // would be a pair of hands hanging in the view for a few frames. Hidden until it has settled.
        auto& rc = reg.get<RenderableComponent>(e);
        const bool easeOut = haveRig && !follow && m_ArmsWeight > 1e-3f;
        if (easeOut && !m_ArmsEasedOut) { m_ArmsShadow = (int)rc.CastShadows; m_ArmsEasedOut = true; }
        if (easeOut) rc.CastShadows = RenderableComponent::ShadowCasting::ShadowsOnly;
        else if (m_ArmsEasedOut) { rc.CastShadows = (RenderableComponent::ShadowCasting)m_ArmsShadow; m_ArmsEasedOut = false; }
    }
    if (m_ArmsWeight < 1e-3f || !haveRig) return;

    const Model* rig = reg.get<RenderableComponent>(weaponArms).ModelRef.get();
    if (!rig || rig->AppliedLocalPose().empty()) return;
    const IK::Pose& rigPose = rig->AppliedLocalPose();
    if ((int)rigPose.size() != rig->NodeCount()) return;
    const glm::mat4 rigWorld = world.ComposeWorldTransform(weaponArms);

    struct Side { const char *Clavicle, *Upper, *Lower, *Hand; };
    static const Side kSides[2] = {{FPBody::kBoneClavicle[0], FPBody::kBoneUpperArm[0], FPBody::kBoneLowerArm[0], FPBody::kBoneHand[0]},
                                   {FPBody::kBoneClavicle[1], FPBody::kBoneUpperArm[1], FPBody::kBoneLowerArm[1], FPBody::kBoneHand[1]}};
    // The rig's hands, exactly: the arms piece is drawn with the gun in the same projection.
    glm::vec3 handPos[2]{};
    glm::quat handRot[2]{};
    bool haveHand[2] = {false, false};
    for (int s = 0; s < 2; ++s) {
        glm::mat4 h(1.0f);
        if (!rig->NodeTransform(kSides[s].Hand, h)) continue;
        const glm::mat4 w = rigWorld * h;
        handPos[s] = glm::vec3(w[3]);
        handRot[s] = IK::Rotation(w);
        haveHand[s] = true;
    }

    std::vector<int> parents;
    std::vector<glm::mat4> globals;
    for (size_t k = 0; k < m_Models.size(); ++k) {
        Model& m = *m_Models[k];
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount() || !reg.valid(m_Pieces[k])) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);

        // The rig's arm shapes first, so the elbows bend the way the animation has them; the solve
        // then only fixes the hands.
        CopyArmShape(m, *rig, m_ArmsWeight, pose, parents, m_BoneMap);

        const glm::mat4 toModel = glm::inverse(world.ComposeWorldTransform(m_Pieces[k]));
        const glm::quat toModelRot = IK::Rotation(toModel);
        IK::ComputeGlobals(pose, parents, globals);
        for (int s = 0; s < 2; ++s) {
            if (!haveHand[s]) continue;
            const int upper = m.NodeIndex(Bone(kSides[s].Upper)), lower = m.NodeIndex(Bone(kSides[s].Lower)),
                      hand = m.NodeIndex(Bone(kSides[s].Hand));
            if (upper < 0 || lower < 0 || hand < 0) continue;
            const glm::vec3 target = glm::vec3(toModel * glm::vec4(handPos[s], 1.0f));
            const glm::quat rot = glm::normalize(toModelRot * handRot[s]);
            // A hand beyond the arm's reach: the shoulder shrugs toward it instead of the arm
            // stretching (the two skeletons' shoulders can sit a little apart).
            if (const int clav = m.NodeIndex(Bone(kSides[s].Clavicle)); clav >= 0) {
                const glm::vec3 a = IK::Position(globals[upper]);
                const float armLen = glm::length(IK::Position(globals[lower]) - a) + glm::length(IK::Position(globals[hand]) - IK::Position(globals[lower]));
                const glm::vec3 toTarget = target - a;
                const float dist = glm::length(toTarget);
                const float excess = std::min(dist - armLen * cfg.ShrugStart, cfg.ShrugMax);
                if (excess > 1e-4f && dist > 1e-5f)
                    IK::OffsetBone(pose, parents, globals, clav, toTarget / dist * excess * m_ArmsWeight, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f));
            }
            IK::SolveTwoBone(pose, parents, globals, upper, lower, hand, target, &rot, m_ArmsWeight);
        }
        m.ApplyLocalPose(pose);
    }
}
