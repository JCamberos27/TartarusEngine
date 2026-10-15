#include "BodyLocomotion.h"
#include "GameCommands.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace BodyLocomotion {
namespace {
using namespace Gameplay;
constexpr float kPi = 3.14159265358979f, kTau = 2 * kPi, Rad = kPi / 180;
constexpr float WalkJogSplit = 2.2f;     // clip m/s: below, a walk's start / stop / pivot; above, a jog's
constexpr float StartTurnMin = 40;       // degrees the body is off where it should face before a start turns it
constexpr float StartTurnTravel = 50;    // ... while the travel is within this of that facing
constexpr float PivotMinSpeed = 1.0f, PivotMinSpeedCrouched = .6f; // clip m/s the travel needs for a pivot to read
constexpr float StepMaxTime = .3f;       // a tap shorter than this is a step, not a start and a stop
constexpr float SprintWarpMax = 50;      // degrees the legs turn into a diagonal sprint (the torso keeps the view)
constexpr float FidgetMin = 10, FidgetMax = 20;

glm::vec3 V(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 V(const glm::vec3& v) { return {v.x, v.y, v.z}; }
float Wrap(float a) { a = std::fmod(a, kTau); if (a > kPi) a -= kTau; else if (a <= -kPi) a += kTau; return a; }
float Follow(float dt, float seconds) { return seconds > 1e-4f ? 1 - std::exp(-dt / seconds) : 1; }
glm::vec3 Local(const glm::vec3& v, float yaw) {
    return {-v.x * std::cos(yaw) + v.z * std::sin(yaw), v.x * std::sin(yaw) + v.z * std::cos(yaw), 0};
}
float Heading(const glm::vec3& v) { return std::atan2(v.x, v.z); }
float Sign(float v) { return v > 0 ? 1.0f : v < 0 ? -1.0f : 0.0f; }
float ClipSpeed(float speed, const BodyMotionFrame& f) {
    // Preserve the tuned walk/jog/sprint mapping in metres per second.
    speed = std::max(speed, 0.0f); const float run = std::max(f.PlayerRunSpeed, .01f);
    if (speed <= run || f.PlayerSprintSpeed <= run + .001f) return speed * f.RunSpeed / run;
    return f.RunSpeed + (speed - run) * (f.ClipSprint - f.RunSpeed) / (f.PlayerSprintSpeed - run);
}
glm::vec3 AsClip(const glm::vec3& v, const BodyMotionFrame& f) {
    const glm::vec3 local = Local(v, f.Yaw); const float s = glm::length(local);
    return s > 1e-4f ? local * (ClipSpeed(s, f) / s) : glm::vec3(0);
}
float Gait(float clipSpeed, bool sprint) { return sprint ? 2.0f : clipSpeed > WalkJogSplit ? 1.0f : 0.0f; }
}

float DistanceToTurn(float v, float target, float tau) {
    if (v <= 0 || tau <= 0) return 0;
    if (target <= 1e-3f) return v * tau;
    return std::max(0.0f, v * tau - target * tau * std::log(1 + v / target));
}

void Update(BodyMotionFrame& f) {
    f.Triggers = 0; f.SetTurnAngle = 0;
    const glm::vec3 velocity = V(f.Velocity), wishVelocity = V(f.WishVelocity);
    const glm::vec3 moving(velocity.x, 0, velocity.z); const float responsiveness = std::clamp(f.Responsiveness, 0.0f, 1.0f);
    const glm::vec3 wishWorld(wishVelocity.x, 0, wishVelocity.z); const float wishSpeed = glm::length(wishWorld);
    const float movingSpeed = glm::length(moving); const bool sprint = wishSpeed > f.PlayerRunSpeed * 1.05f;
    glm::vec3 move = V(f.Move);
    // Sprinting on a diagonal, the legs square up to the travel and the torso twists back to the view.
    float faceYaw = f.ViewYaw;
    if (sprint && f.Grounded && !f.Crouched && wishSpeed > .1f) {
        const float off = Wrap(Heading(wishWorld) - f.ViewYaw);
        if (std::abs(off) < 100 * Rad) faceYaw = f.ViewYaw + std::clamp(off, -SprintWarpMax * Rad, SprintWarpMax * Rad);
    }
    const glm::vec3 a = AsClip(wishVelocity, f), b = AsClip(moving, f);
    const glm::vec3 target = a + (b - a) * responsiveness;
    const float clip = ClipSpeed(movingSpeed, f);
    f.PlayRate = clip > .05f ? 1 + (std::clamp(movingSpeed / clip, 1.0f, std::max(f.MaxPlayRate, 1.0f)) - 1) *
                                   std::min(clip / .5f, 1.0f) * (sprint ? 1 : responsiveness) : 1;
    f.SprintRate = f.SprintClip > .1f ? std::clamp(movingSpeed / f.SprintClip, .8f, 1.5f) : 1;
    const bool hold = f.StartStopClips && glm::length(target) < .01f && f.IdleTime < f.StopDebounce &&
                      glm::length(move) > (f.Crouched ? f.StopMinSpeedCrouched : f.StopMinSpeed);
    if (!hold) move += (target - move) * Follow(f.Dt, f.ParamSmoothing);
    f.AirTime = f.Grounded ? 0 : f.AirTime + f.Dt;
    const bool still = f.Grounded && wishSpeed < .1f && glm::length(move) < .2f; f.Still = still && f.TurnThreshold > 0;
    float offset = Wrap(faceYaw - f.Yaw);
    if (f.TurnThreshold <= 0) { f.Yaw = faceYaw; f.Turning = 0; }
    else if (f.Turning) {
        f.TurnTime += f.Dt;
        if (f.IsTurn || f.IsCrouchTurn) { const float step = f.RootYaw * Rad; f.Yaw += step; f.TurnDone += step; }
        offset = Wrap(faceYaw - f.Yaw); const bool clipDone = f.TurnTime > f.TurnMinTime && (!f.IsTurn || f.StateTime >= .97f);
        if (!still || std::abs(offset) < f.TurnEndAngle * Rad || clipDone || f.TurnTime > f.TurnTimeout) f.Turning = 0;
    } else if (still) {
        // A turn on the spot is never under 45 degrees of clip: each side's smallest, so the sides never mix.
        if (std::abs(offset) > f.TurnThreshold * Rad) {
            f.Turning = 1; f.TurnTime = f.TurnDone = 0; const float deg = std::clamp(offset / Rad, -180.0f, 180.0f);
            f.TurnAngle = std::copysign(std::max(std::abs(deg), 45.0f), deg); f.SetTurnAngle = 1;
        }
    } else if (f.IsStartTurn) {
        // A start that turns: the body takes the clip's own yaw, up to where it should face.
        const float step = f.RootYaw * Rad;
        if (Sign(step) == Sign(offset)) f.Yaw += std::copysign(std::min(std::abs(step), std::abs(offset)), step);
    } else f.Yaw += offset * Follow(f.Dt, f.TurnMoveEase);
    if (f.TurnThreshold > 0 && !f.IsStartTurn) {
        const float lag = Wrap(faceYaw - f.Yaw), max = std::max(f.TurnThreshold + f.TurnLagMargin, f.TurnLagFloor) * Rad;
        if (std::abs(lag) > max) f.Yaw = faceYaw - std::copysign(max, lag);
    }
    f.Yaw = Wrap(f.Yaw); f.Twist = Wrap(f.ViewYaw - f.Yaw);
    const glm::vec3 wish = Local(wishVelocity, f.Yaw); const float wishLen = glm::length(wish); f.Moving = wishLen > .1f;
    const bool idling = f.IsLocomotion || f.IsCrouchLoco || f.IsStep || f.IsFidget;
    const bool plain = f.StartStopClips && f.Grounded && idling && !f.Turning;
    const bool going = f.StartStopClips && f.Grounded && !f.Turning && (f.IsLocomotion || f.IsCrouchLoco || f.IsStart || f.IsPivot || f.IsSprint);
    if (f.StartStopClips && f.Grounded && !f.Moving && glm::length(move) < .4f && f.Crouched != f.WasCrouched) {
        if (f.Crouched && f.IsLocomotion) f.Triggers |= kBodyCrouchDown;
        if (!f.Crouched && f.IsCrouchLoco) f.Triggers |= kBodyCrouchUp;
    }
    f.WasCrouched = f.Crouched;
    const float moveClip = glm::length(move);
    if (f.Moving) {
        const glm::vec3 dir = wish / wishLen;
        if (plain && f.IdleTime >= f.StartIdleTime && moveClip < f.StartMaxMove) {
            f.StartDir = V(dir); f.StartGait = Gait(ClipSpeed(wishSpeed, f), sprint); f.StartTurn = 0; f.StartTurnAmount = 1;
            // Moving off toward where the body should face but isn't yet (the view whipped round, or a new
            // heading): a start that turns it on the way.
            const float lag = Wrap(faceYaw - f.Yaw), travel = Wrap(Heading(wishWorld) - faceYaw);
            if (std::abs(lag) > StartTurnMin * Rad && std::abs(travel) < StartTurnTravel * Rad) {
                float amount = std::clamp(std::abs(lag) / (45 * Rad), 1.0f, 4.0f);
                if (f.Crouched) amount = std::nearbyint(amount); // half to even, as the original rounding did
                f.StartTurn = std::copysign(amount, lag); f.StartTurnAmount = amount;
            }
            f.Triggers |= kBodyStart; f.MoveDistance = 0;
        }
        // The travel reversing at speed: a pivot - the feet plant and push off the other way.
        const float pivotMin = f.Crouched ? PivotMinSpeedCrouched : PivotMinSpeed;
        if (going && !f.IsPivot && moveClip > pivotMin && glm::dot(dir, move / moveClip) < -.5f) {
            f.PivotDir = V(move / moveClip); f.PivotGait = Gait(moveClip, false); f.PivotReversed = 0; f.PivotTravel = 0;
            f.Triggers |= kBodyPivot;
        }
        f.LastDir = V(dir); f.LastSprint = sprint; f.IdleTime = 0; f.MoveTime += f.Dt; f.MoveDistance += movingSpeed * f.Dt;
    } else {
        const float before = f.IdleTime; f.IdleTime += f.Dt;
        const bool enough = f.MoveTime >= (f.Crouched ? f.StopMinRunTimeCrouched : f.StopMinRunTime);
        const bool released = before < f.StopDebounce && f.IdleTime >= f.StopDebounce;
        // A tap: one small step its way instead of a start cut short (the start has already given way to the idle
        // when the key lifts).
        if (released && (f.IsStart || f.IsLocomotion) && !f.Crouched && f.MoveTime < StepMaxTime && f.MoveDistance > .03f) {
            f.StepDir = f.LastDir; f.Triggers |= kBodyStep;
        } else if (going && enough && released && moveClip > (f.Crouched ? f.StopMinSpeedCrouched : f.StopMinSpeed)) {
            f.StopDir = f.LastDir; f.StopGait = Gait(moveClip, false);
            f.Triggers |= f.LastSprint && f.LastDir.y > f.StopRunForward ? kBodyStopRun : kBodyStop;
        }
        if (released) f.MoveTime = 0;
    }
    // Distance matching: how far the capsule has gone since the start, still has to go in the stop, and is from
    // the pivot's turnaround (negative until its travel the old way runs out).
    f.StartDistance = f.IsStart ? f.StartDistance + movingSpeed * f.Dt : 0;
    f.StopDistance = movingSpeed * f.DecelTime;
    if (f.IsPivot) {
        const glm::vec3 local = Local(moving, f.Yaw); const float along = local.x * f.PivotDir.x + local.y * f.PivotDir.y;
        if (!f.PivotReversed && along > .05f) f.PivotDistance = -DistanceToTurn(along, wishSpeed, f.AccelTime);
        else { f.PivotReversed = 1; f.PivotTravel += movingSpeed * f.Dt; f.PivotDistance = f.PivotTravel; }
    } else if ((f.Triggers & kBodyPivot) == 0) { f.PivotDistance = 0; f.PivotReversed = 0; }
    // Idle variety: after a while stood still, one fidget (with a gun, the ready stance's own; never while
    // aiming or just after shooting).
    const bool fidgetable = f.Grounded && !f.Moving && !f.Turning && !f.Busy && moveClip < .1f && (f.IsLocomotion || f.IsCrouchLoco);
    if (f.FidgetNext <= 0) f.FidgetNext = FidgetMin + (FidgetMax - FidgetMin) * f.Random;
    if (!fidgetable) f.FidgetTime = f.IsFidget ? f.FidgetTime : 0;
    else if ((f.FidgetTime += f.Dt) >= f.FidgetNext) {
        const int pick = (int)std::floor(f.Random * 997) % 997;
        f.FidgetIndex = (float)(f.Crouched ? pick % 6 : f.Armed ? pick % 2 : 2 + pick % 7);
        f.Triggers |= kBodyFidget; f.FidgetTime = 0; f.FidgetNext = 0;
    }
    f.Move = V(move);
    f.Sprint = sprint; f.Airborne = f.AirTime > f.AirborneDelay; if (f.Jumped) f.Triggers |= kBodyJump;
}
}
