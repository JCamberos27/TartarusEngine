#include "FirstPersonBody.h"
#include "FirstPersonBodyInternal.h"
#include "Profiler.h"
#include "BodyDebugDraw.h"
#include "AnimatorController.h"
#include "FirstPersonBodyContract.h"
#include "FirstPersonAnimation.h" // FirstPersonAnimatorContract::kTagHidden: the arms rig hidden (unarmed)

#include "Camera.h"
#include "Components.h"
#include "GameModuleAPI.h"
#include "IK.h"
#include "Log.h"
#include "Model.h"
#include "OutfitCoverage.h"
#include "OutfitSystem.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "SkinHideBuffer.h"
#include "World.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <tuple>
#include <cstdio>
#include <sstream>

// FirstPersonBody: the feet on the ground - the foot lock, the ground rays, the legs' reach and their two-bone IK.

using namespace FirstPersonBodyDetail;

void FirstPersonBody::ApplyFootIK(World& world, const FirstPersonBodyComponent& cfg, float dt) {
    auto& reg = world.Registry;
    const bool airborne = m_Mm.Airborne();
    const bool on = cfg.FootIK && m_Grounded && PhysicsWorld::HasCharacter() && reg.valid(m_Driver) && !airborne;
    m_FootWeight += ((on ? 1.0f : 0.0f) - m_FootWeight) * Follow(dt, cfg.FootIKFade);
    if (m_FootWeight < 1e-3f) {
        m_HaveFootGround = false;
        m_HaveFoot = false;
        m_Lock.Reset();
        return;
    }
    const auto* rc = reg.try_get<RenderableComponent>(m_Driver);
    if (!rc || !rc->ModelRef) return;
    const Model& drv = *rc->ModelRef;
    thread_local IK::Pose driverPose; // (a copy: the legs below re-pose the driver)
    driverPose = drv.AppliedLocalPose();
    if (driverPose.empty() || (int)driverPose.size() != drv.NodeCount()) return;
    const int footNode[2] = {drv.NodeIndex(Bone(FPBody::kBoneFoot[0])), drv.NodeIndex(Bone(FPBody::kBoneFoot[1]))};
    if (footNode[0] < 0 || footNode[1] < 0) return;

    const auto& t = reg.get<TransformComponent>(m_Body);
    const float scale = std::max(t.Scale.y, 1e-3f);
    const glm::quat yaw = YawRotation(m_Yaw);
    thread_local std::vector<int> parents; // (scratch kept between frames: no per-frame allocation)
    thread_local std::vector<glm::mat4> globals;
    parents.resize(driverPose.size());
    for (int i = 0; i < (int)driverPose.size(); ++i) parents[i] = drv.NodeParent(i);
    IK::ComputeGlobals(driverPose, parents, globals);

    const float maxDrop = std::max(cfg.FootIKMaxDrop, 0.0f);
    const float capsuleY = m_Feet.y - m_StepOffset;
    float animHeight[2];
    glm::vec3 lockShift[2] = {glm::vec3(0.0f), glm::vec3(0.0f)}; // world, horizontal: animated foot -> where it is drawn
    // Foot lock: a planted foot stays where it landed in the world - walking, turning on the spot, starting and stopping
    // alike - and catches up through its swing once it lifts, or steps once the leg can no longer reach it (IK::FootLock).
    // Motion matching: the lock holds the ball of the foot, not the ankle - the body turning over a planted foot (faster than
    // the clip turns: the facing's pull toward the view) swivels it on its ball, as a foot pivots, instead of sweeping its
    // toes over the floor; and a foot let go steps clear over the way the turn carries it (FootLockSettings::StepForTravel).
    IK::FootLockSettings lockSet = IK::FootLockFrom(cfg);
    // Experiment knobs (MmTune: FootLock.<field>).
    lockSet.Height = MmTune("FootLock.Height", lockSet.Height);
    lockSet.Lift = MmTune("FootLock.Lift", lockSet.Lift);
    lockSet.Speed = MmTune("FootLock.Speed", lockSet.Speed);
    lockSet.Reach = MmTune("FootLock.Reach", lockSet.Reach);
    lockSet.MaxDrift = MmTune("FootLock.MaxDrift", lockSet.MaxDrift);
    lockSet.Release = MmTune("FootLock.Release", lockSet.Release);
    lockSet.StepHeight = MmTune("FootLock.StepHeight", lockSet.StepHeight);
    lockSet.StepMax = MmTune("FootLock.StepMax", lockSet.StepMax);
    const bool pivot = true; // (motion matching: a planted foot pivots with the body's turn)
    lockSet.StepForTravel = pivot;
    lockSet.RestFloor = MmTune("FootLock.RestFloor", 1.0f) > 0.0f; // (a foot at rest near the ground is down: IK::FootLockSettings)
    const int ballNode[2] = {drv.NodeIndex(Bone(FPBody::kBoneBall[0])), drv.NodeIndex(Bone(FPBody::kBoneBall[1]))};
    for (int s = 0; s < 2; ++s) {
        const glm::vec3 footLocal = scale * IK::Position(globals[footNode[s]]); // the clip's foot, in the body
        const glm::vec3 footWorld = m_Feet + yaw * footLocal;                   // the animated foot
        animHeight[s] = footWorld.y - m_Feet.y;
        // The heel from the strike, then the ball once it is down (IK::FootLock::Step's `ball`). The offset moves the whole
        // foot, its rotation the clip's, so the held point stays put either way.
        const bool ball = pivot && ballNode[s] >= 0;
        const glm::vec3 ballLocal = ball ? scale * IK::Position(globals[ballNode[s]]) : glm::vec3(0.0f);
        const float ballHeight = ball ? (m_Feet + yaw * ballLocal).y - m_Feet.y : 0.0f;
        const glm::vec3 held = m_Lock.Step(s, lockSet, m_Feet, yaw, footLocal, animHeight[s], dt, ball ? &ballLocal : nullptr, ballHeight);
        lockShift[s] = held * m_FootWeight;
        const glm::vec3 drawn = footWorld + held; // the ground is read under the foot as drawn
        float offset = 0.0f, ground = 0.0f; // the ground off the body's base, and off the capsule's
        glm::vec3 normal(0.0f, 1.0f, 0.0f);
        const float origin[3] = {drawn.x, drawn.y + cfg.FootRayUp, drawn.z};
        const float down[3] = {0.0f, -1.0f, 0.0f};
        QueryFilter filter;
        filter.HitTriggers = 0;
        RaycastHit hit;
        const bool rayHit = PhysicsWorld::RaycastSolid(origin, down, cfg.FootRayLength, filter, hit) && hit.Hit;
        if (rayHit) {
            offset = std::clamp(hit.Point[1] - m_Feet.y, -maxDrop, cfg.FootMaxRaise);
            ground = std::clamp(hit.Point[1] - capsuleY, -1.0f, 1.0f);
            normal = glm::normalize(glm::vec3(hit.Normal[0], hit.Normal[1], hit.Normal[2]));
            if (normal.y < 0.5f) normal = glm::vec3(0.0f, 1.0f, 0.0f); // a wall, not a floor
        }
        if (BodyDebug::Enabled()) {
            const glm::vec3 from(origin[0], origin[1], origin[2]);
            const glm::vec3 to = rayHit ? glm::vec3(hit.Point[0], hit.Point[1], hit.Point[2]) : from + glm::vec3(0.0f, -cfg.FootRayLength, 0.0f);
            BodyDebug::Line(from, to, rayHit ? glm::vec4(0.3f, 1.0f, 0.4f, 1.0f) : glm::vec4(1.0f, 0.3f, 0.3f, 1.0f)); // the ground ray
            if (rayHit) BodyDebug::Cross(to, 0.04f, glm::vec4(0.3f, 1.0f, 0.4f, 1.0f));
            BodyDebug::Cross(footWorld, 0.05f, m_Lock.Locked(s) ? glm::vec4(0.3f, 1.0f, 0.4f, 1.0f) : glm::vec4(1.0f, 0.4f, 0.3f, 1.0f)); // the animated foot: green planted
            if (m_Lock.Locked(s)) BodyDebug::Cross(m_Lock.Pinned(s), 0.03f, glm::vec4(0.3f, 0.9f, 1.0f, 1.0f));                         // where it is held
        }
        if (!m_HaveFoot) { m_FootOffset[s] = offset; m_FootNormal[s] = normal; m_FootGround[s] = ground; }
        m_FootOffset[s] += (offset - m_FootOffset[s]) * Follow(dt, cfg.FootOffsetEase);
        m_FootGround[s] += (ground - m_FootGround[s]) * Follow(dt, cfg.FootOffsetEase);
        m_FootNormal[s] = glm::normalize(m_FootNormal[s] + (normal - m_FootNormal[s]) * Follow(dt, cfg.FootNormalEase));
    }
    m_HaveFoot = true;
    {
        BodyDebug::Snapshot& d = BodyDebug::Info();
        d.FootWeight = m_FootWeight;
        for (int s = 0; s < 2; ++s) { d.FootOffset[s] = m_FootOffset[s]; d.FootPlanted[s] = m_Lock.Locked(s); d.FootLock[s] = m_Lock.Locked(s) ? 1.0f : 0.0f; }
    }

    // The hips over the lower foot: the body's height spring takes it (in Tick, next frame), so the whole body - and
    // the camera on it - drops smoothly. Each foot's ground is read off the capsule's, not the body's: off the body's,
    // it moved as the body (and the camera) moved toward it - a loop through the spring.
    m_HaveFootGround = true;
    static const char* const kLegs[2][3] = {{FPBody::kBoneThigh[0], FPBody::kBoneCalf[0], FPBody::kBoneFoot[0]}, {FPBody::kBoneThigh[1], FPBody::kBoneCalf[1], FPBody::kBoneFoot[1]}};
    const glm::quat yawInverse = glm::inverse(yaw);
    // How far each foot is lowered: to its ground, but never further than its leg reaches straight down over where it is
    // drawn (the lock's Reach of the leg, read off the driver). The body drops at most ~0.35 m under the capsule; a foot
    // over ground lower than that (the trailing foot down a stair, a swing over a ramp) stops where the leg ends - a hair
    // over the ground - instead of asking the reach clamp to drag it off its line. A foot let go and still catching up
    // with its clip (the lock's lag easing out, the clip's leg near straight) lifts that little more on its way; a held
    // foot is never lifted off its ground (Overreach lets it go instead), nor a free one off the clip's own.
    float legOffset[2] = {m_FootOffset[0], m_FootOffset[1]};
    for (int s = 0; s < 2; ++s) {
        const int thigh = drv.NodeIndex(Bone(kLegs[s][0])), calf = drv.NodeIndex(Bone(kLegs[s][1]));
        if (thigh < 0 || calf < 0) continue;
        const bool catchingUp = !m_Lock.Locked(s) && glm::dot(lockShift[s], lockShift[s]) > 1e-8f;
        if (legOffset[s] >= 0.0f && !catchingUp) continue;
        const glm::vec3 hip = IK::Position(globals[thigh]), knee = IK::Position(globals[calf]), foot = IK::Position(globals[footNode[s]]);
        const float reach = lockSet.Reach * (glm::length(knee - hip) + glm::length(foot - knee));
        const glm::vec3 drawn = foot + yawInverse * glm::vec3(lockShift[s].x, 0.0f, lockShift[s].z) / scale;
        const glm::vec2 across(drawn.x - hip.x, drawn.z - hip.z);
        const float down = std::sqrt(std::max(reach * reach - glm::dot(across, across), 0.0f)); // the leg's reach below the hip
        float lowest = (hip.y - down - foot.y) * scale;                                         // m the foot may go down
        if (!catchingUp) lowest = std::min(lowest, 0.0f);
        legOffset[s] = std::max(legOffset[s], lowest);
    }
    // A held foot the hips have walked or climbed on from lets go and steps once its leg would need more than the lock's
    // Reach (IK::FootLock::Overreach), read off the driver (`globals` are still its) with the goal the legs get below.
    for (int s = 0; s < 2; ++s) {
        const int thigh = drv.NodeIndex(Bone(kLegs[s][0])), calf = drv.NodeIndex(Bone(kLegs[s][1]));
        if (thigh < 0 || calf < 0 || !m_Lock.Locked(s)) continue;
        const glm::vec3 hip = IK::Position(globals[thigh]), knee = IK::Position(globals[calf]), foot = IK::Position(globals[footNode[s]]);
        const float leg = glm::length(knee - hip) + glm::length(foot - knee);
        if (leg <= 1e-5f) continue;
        const glm::vec3 own = foot + glm::vec3(0.0f, legOffset[s] * m_FootWeight / scale, 0.0f); // the clip's foot on the ground
        m_Lock.Overreach(s, lockSet, glm::length(own + yawInverse * lockShift[s] / scale - hip) / leg, glm::length(own - hip) / leg);
    }
    for (const auto& mp : m_Models) {
        Model& m = *mp;
        thread_local IK::Pose pose;
        pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        const int pelvis = m.NodeIndex(Bone(FPBody::kBonePelvis));
        if (pelvis < 0) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        for (int s = 0; s < 2; ++s) {
            const int thigh = m.NodeIndex(Bone(kLegs[s][0])), calf = m.NodeIndex(Bone(kLegs[s][1])), foot = m.NodeIndex(Bone(kLegs[s][2]));
            if (thigh < 0 || calf < 0 || foot < 0) continue;
            const glm::vec3 hip = IK::Position(globals[thigh]);
            const float legLen = glm::length(IK::Position(globals[calf]) - hip) + glm::length(IK::Position(globals[foot]) - IK::Position(globals[calf]));
            // Never more than the leg has (IK::ClampReach): the frame a held foot lets go for reach, and any stair or slope
            // that asks for more, the foot gives a little toward the hip instead of the knee snapping straight.
            const glm::vec3 wanted = IK::Position(globals[foot]) + glm::vec3(0.0f, legOffset[s] * m_FootWeight / scale, 0.0f) +
                                     yawInverse * lockShift[s] / scale;
            const glm::vec3 target = IK::ClampReach(hip, wanted, legLen);
            // The foot lies on the ground while it is planted: its own rotation tilted to the normal.
            const float planted = 1.0f - std::clamp((animHeight[s] - 0.06f) / 0.09f, 0.0f, 1.0f);
            const glm::vec3 normal = yawInverse * m_FootNormal[s];
            const float angle = std::min(std::acos(std::clamp(normal.y, -1.0f, 1.0f)), glm::radians(cfg.FootTiltMax)) * planted * m_FootWeight;
            glm::quat footRot = IK::Rotation(globals[foot]);
            const glm::vec3 axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal);
            if (angle > 1e-4f && glm::dot(axis, axis) > 1e-8f) footRot = glm::angleAxis(angle, glm::normalize(axis)) * footRot;
            if (legLen > 1e-5f) BodyDebug::Info().LegReach[s] = glm::length(wanted - hip) / legLen; // asked, before the clamp
            BodyDebug::Info().ClampPull[s] = glm::length(wanted - target) * scale;                    // m the clamp moved the foot
            if (legLen > 1e-5f) BodyDebug::Info().ReachFree[s] = glm::length(wanted - yawInverse * lockShift[s] / scale - hip) / legLen;
            BodyDebug::Info().LockShift[s] = glm::length(glm::vec2(lockShift[s].x, lockShift[s].z));
            BodyDebug::Info().AnimHeight[s] = animHeight[s];
            // Flat ground and nothing pinned: leave the clip's own pose alone.
            if (glm::length(target - IK::Position(globals[foot])) < 5e-4f && angle < 1e-3f) continue;
            IK::SolveTwoBone(pose, parents, globals, thigh, calf, foot, target, &footRot, 1.0f);
        }
        m.ApplyLocalPose(pose);
    }
}
