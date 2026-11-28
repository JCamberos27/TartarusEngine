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

// FirstPersonBody's free functions: the small, unit-tested pieces of math the body's update is made of (FirstPersonBody.h).

using namespace FirstPersonBodyDetail;

glm::vec3 FirstPersonBodyPalmDirection(const Model& rig, const glm::mat4& rigWorld, int side) {
    const std::string sfx = side ? "_r" : "_l";
    glm::mat4 h(1.0f), k(1.0f), t(1.0f);
    if (!rig.NodeTransform("hand" + sfx, h) || !rig.NodeTransform("middle_01" + sfx, k) || !rig.NodeTransform("middle_03" + sfx, t)) return glm::vec3(0.0f);
    const glm::vec3 hand = glm::vec3(rigWorld * h[3]), knuckle = glm::vec3(rigWorld * k[3]), tip = glm::vec3(rigWorld * t[3]);
    const glm::vec3 along = knuckle - hand;
    if (glm::length(along) < 1e-6f) return glm::vec3(0.0f);
    const glm::vec3 a = glm::normalize(along);
    glm::vec3 curl = tip - knuckle;
    curl -= a * glm::dot(curl, a);
    return glm::length(curl) > 1e-4f ? glm::normalize(curl) : glm::vec3(0.0f);
}
void FirstPersonBodyCurlFingers(const Model& m, std::vector<LocalTRS>& pose, float degrees) {
    if (degrees <= 0.0f || (int)pose.size() != m.NodeCount()) return;
    thread_local std::vector<LocalTRS> bind;
    m.BindLocalPose(bind);
    for (int i = 0; i < (int)pose.size(); ++i) {
        const std::string& n = m.NodeName(i);
        if (!(n.rfind("index_0", 0) == 0 || n.rfind("middle_0", 0) == 0 || n.rfind("ring_0", 0) == 0 || n.rfind("pinky_0", 0) == 0)) continue;
        glm::quat delta = glm::inverse(bind[(size_t)i].R) * pose[(size_t)i].R;
        // The short way round: q and -q are one rotation, but glm::angle of a negative-w quat reads it as the long way
        // (a slight curl came out past 175 degrees and was skipped, or curled backwards).
        if (delta.w < 0.0f) delta = -delta;
        const float angle = glm::angle(delta);
        if (angle < glm::radians(5.0f) || angle > glm::radians(175.0f)) continue; // no clear curl to follow
        pose[(size_t)i].R = glm::normalize(bind[(size_t)i].R * glm::angleAxis(angle + glm::radians(degrees), glm::axis(delta)));
    }
}
void FirstPersonBodyArmShapeLinksFrom(int count, const std::vector<int>& parents, const int (&clavicles)[2],
                                      const std::function<int(int)>& rigOf, std::vector<FirstPersonArmShapeLink>& out) {
    out.clear();
    count = std::min(count, (int)parents.size());
    std::vector<char> under((size_t)std::max(count, 0), 0), clavicle((size_t)std::max(count, 0), 0);
    for (const int c : clavicles)
        if (c >= 0 && c < count) under[(size_t)c] = clavicle[(size_t)c] = 1;
    for (int i = 0; i < count; ++i) {
        if (!under[(size_t)i] && parents[(size_t)i] >= 0 && under[(size_t)parents[(size_t)i]]) under[(size_t)i] = 1;
        if (!under[(size_t)i]) continue;
        if (const int r = rigOf(i); r >= 0) out.push_back({i, r, clavicle[(size_t)i] != 0});
    }
}
void FirstPersonBodyCopyArmShape(const std::vector<FirstPersonArmShapeLink>& links, const Model& rig, float weight,
                                 std::vector<LocalTRS>& pose, float clavicleWeight) {
    const IK::Pose& rigPose = rig.AppliedLocalPose();
    if ((int)rigPose.size() != rig.NodeCount()) return;
    const float clavWeight = weight * std::clamp(clavicleWeight, 0.0f, 1.0f);
    for (const FirstPersonArmShapeLink& l : links) {
        if (l.Body >= (int)pose.size() || l.Rig >= (int)rigPose.size()) continue;
        LocalTRS& t = pose[(size_t)l.Body];
        t.R = glm::normalize(glm::slerp(t.R, rigPose[(size_t)l.Rig].R, l.Clavicle ? clavWeight : weight));
    }
}
float FirstPersonBodyYaw(const glm::vec3& front, float fallback) {
    const glm::vec2 flat(front.x, front.z);
    if (glm::dot(flat, flat) < 1e-8f) return fallback;
    return std::atan2(front.x, front.z); // rotating +Z by this about +Y gives (sin, 0, cos)
}
glm::quat FirstPersonBodyShoulderLineTurn(const glm::vec3& bodyAcross, const glm::vec3& rigAcross, float weight) {
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    weight = std::clamp(weight, 0.0f, 1.0f);
    if (weight <= 0.0f || glm::dot(bodyAcross, bodyAcross) < 1e-10f || glm::dot(rigAcross, rigAcross) < 1e-10f) return none;
    const glm::quat full(glm::normalize(bodyAcross), glm::normalize(rigAcross));
    return glm::normalize(glm::slerp(none, full, weight));
}
glm::vec3 FirstPersonBodyShoulderLineTilt(const glm::vec3& bodyAcross, const glm::vec3& rigAcross, float tilt) {
    const float lb = glm::length(bodyAcross), lr = glm::length(rigAcross);
    if (lb < 1e-5f || lr < 1e-5f) return rigAcross;
    const glm::vec3 b = bodyAcross / lb, r = rigAcross / lr;
    const glm::vec2 flat(r.x, r.z);
    const float flatLen = glm::length(flat);
    if (flatLen < 1e-5f) return rigAcross; // a vertical line has no blade to keep
    const float y = glm::mix(b.y, r.y, std::clamp(tilt, 0.0f, 1.0f));
    const float horiz = std::sqrt(std::max(1.0f - y * y, 0.0f));
    return glm::vec3(flat.x / flatLen * horiz, y, flat.y / flatLen * horiz) * lr;
}
float FirstPersonBodyArmedEyeLift(float pitchRadians) {
    return 1.0f - glm::smoothstep(glm::radians(25.0f), glm::radians(60.0f), std::abs(pitchRadians));
}
float FirstPersonBodyFollow(float dt, float seconds, float snapBelow) {
    return seconds > snapBelow ? 1.0f - std::exp(-dt / seconds) : 1.0f;
}
float FirstPersonBodyAngleOf(const glm::quat& q) { return 2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f)); }
glm::quat FirstPersonBodyTurnClavicle(std::vector<LocalTRS>& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals,
                                      int clav, int upper, const glm::vec3& want, float maxAngle) {
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::vec3 root = IK::Position(globals[(size_t)clav]);
    const glm::vec3 from = IK::Position(globals[(size_t)upper]) - root, to = want - root;
    if (maxAngle <= 0.0f || glm::dot(from, from) < 1e-8f || glm::dot(to, to) < 1e-8f) return none;
    glm::quat q(glm::normalize(from), glm::normalize(to));
    const float angle = FirstPersonBodyAngleOf(q);
    if (angle < 1e-5f) return none;
    if (angle > maxAngle) q = glm::slerp(none, q, maxAngle / angle);
    IK::OffsetBone(pose, parents, globals, clav, glm::vec3(0.0f), q, root);
    return q;
}
bool FirstPersonBodyReachLean(const std::vector<glm::mat4>& globals, const int (&upper)[2], const int (&lower)[2], const int (&hand)[2],
                              const int (&clav)[2], const bool (&haveHand)[2], const glm::vec3 (&handPos)[2], const glm::mat4& toModel,
                              int chest, float shoulderMax, float leanMax, float weight, glm::quat& turn) {
    // The side furthest out of reach, counting what its collarbone can still give.
    glm::vec3 from(0.0f), to(0.0f);
    float worst = 0.0f;
    for (int s = 0; s < 2; ++s) {
        if (!haveHand[s]) continue;
        if (upper[s] < 0 || lower[s] < 0 || hand[s] < 0) continue;
        const glm::vec3 a = IK::Position(globals[(size_t)upper[s]]);
        const float armLen = glm::length(IK::Position(globals[(size_t)lower[s]]) - a) +
                             glm::length(IK::Position(globals[(size_t)hand[s]]) - IK::Position(globals[(size_t)lower[s]]));
        const float clavReach = clav[s] >= 0 ? glm::length(a - IK::Position(globals[(size_t)clav[s]])) * shoulderMax : 0.0f;
        const glm::vec3 target = glm::vec3(toModel * glm::vec4(handPos[s], 1.0f));
        const float dist = glm::length(target - a);
        const float excess = dist - armLen * 0.995f - clavReach;
        if (excess > worst && dist > 1e-5f) { worst = excess; from = a; to = a + (target - a) / dist * excess; }
    }
    if (worst <= 1e-4f) return false;
    const glm::vec3 root = IK::Position(globals[(size_t)chest]);
    if (!(glm::dot(from - root, from - root) > 1e-8f && glm::dot(to - root, to - root) > 1e-8f)) return false;
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    glm::quat q(glm::normalize(from - root), glm::normalize(to - root));
    if (const float angle = FirstPersonBodyAngleOf(q); angle > leanMax) q = glm::slerp(none, q, leanMax / angle);
    turn = glm::slerp(none, q, weight);
    return true;
}
glm::vec3 FirstPersonBodyRigEyeToShoulders(const glm::mat4& rigWorld, const glm::mat4& upperL, const glm::mat4& upperR,
                                           const glm::mat4& cameraBone, const Camera& cam) {
    const glm::vec3 fromEye = glm::mat3(rigWorld) * (0.5f * (glm::vec3(upperL[3]) + glm::vec3(upperR[3])) - glm::vec3(cameraBone[3]));
    return glm::vec3(glm::dot(fromEye, cam.Right()), glm::dot(fromEye, cam.Up()), glm::dot(fromEye, cam.Front()));
}
void FirstPersonBodyFollowShoulders(glm::vec3& eased, bool& have, const glm::vec3& animated, float eyeSlack, float slackSeconds, float dt) {
    if (!have) { eased = animated; have = true; }
    eased += (animated - eased) * FirstPersonBodyFollow(dt, kFirstPersonEyeFollowSeconds);
    const glm::vec3 gap = animated - eased;
    if (const float gapLen = glm::length(gap); gapLen > eyeSlack) {
        if (slackSeconds > 0.0f) eased += (animated - gap / gapLen * eyeSlack - eased) * FirstPersonBodyFollow(dt, slackSeconds);
        else eased = animated - gap / gapLen * eyeSlack;
    }
}
glm::vec3 FirstPersonBodyArmedEyeOffset(const glm::vec3& offset, float pitchRadians) {
    return (kRight * offset.x + glm::vec3(0.0f, offset.y, 0.0f) + kForward * offset.z) * FirstPersonBodyArmedEyeLift(pitchRadians);
}
float FirstPersonBodySpineAim(float pitchRadians, float spineAim, float spineAimDown) {
    return std::clamp(pitchRadians < 0.0f ? spineAimDown : spineAim, 0.0f, 1.0f);
}
float FirstPersonBodyCappedSpineAim(float pitchRadians, float spineAim, float maxRadians, float weight) {
    const float want = pitchRadians * spineAim;
    if (maxRadians <= 0.0f || std::fabs(want) < 1e-5f || weight <= 0.0f) return spineAim;
    // Eased: linear at first, flattening onto the cap (tanh), so the chest never stops dead.
    const float capped = maxRadians * std::tanh(want / maxRadians);
    return glm::mix(spineAim, capped / pitchRadians, std::clamp(weight, 0.0f, 1.0f));
}
float FirstPersonBodyMmAimFade(float speed, float from, float to) {
    if (!(to > from)) return speed <= from ? 1.0f : 0.0f;
    return 1.0f - glm::smoothstep(from, to, speed);
}
float FirstPersonBodyWrapAngle(float radians) {
    const float twoPi = 6.28318530718f;
    radians = std::fmod(radians, twoPi);
    if (radians > 3.14159265359f) radians -= twoPi;
    else if (radians <= -3.14159265359f) radians += twoPi;
    return radians;
}
void FirstPersonGaitPhase::Update(float spread, float lift, bool moving, float dt) {
    dt = std::max(dt, 0.0f);
    auto wrap = [](float v) { return v - std::floor(v + 0.5f); }; // (-0.5, 0.5]
    Still = moving ? 0.0f : Still + dt;
    if (Still > 1.0f) { Locked = false; Rate = 0.0f; PrevAngle = -1.0f; Reading = 0.0f; }
    if (moving && dt > 0.0f) {
        const float k = std::min(1.0f, dt * 1.5f); // the signals' size, over the last second or so
        SpreadAmp = std::max(0.02f, SpreadAmp + (std::abs(spread) * 1.5708f - SpreadAmp) * k);
        LiftAmp = std::max(0.01f, LiftAmp + (std::abs(lift) * 1.5708f - LiftAmp) * k);
        const float x = spread / SpreadAmp, y = -lift / LiftAmp; // the left foot ahead, then the right lifting
        if (x * x + y * y > 0.09f) { // round the loop, not crossing its middle
            float angle = std::atan2(y, x) / 6.28318531f;
            angle -= std::floor(angle);
            if (PrevAngle >= 0.0f) {
                const float d = wrap(angle - PrevAngle);
                if (d > -0.02f && d < 0.2f) {
                    // The loop isn't a true circle - the angle runs faster and slower within a stride - so the rate is its
                    // average over about a second (from the first reading, quicker).
                    const float r = std::max(d, 0.0f) / dt;
                    Rate += (r - Rate) * std::min(1.0f, dt * (Reading < 0.6f ? 5.0f : 1.0f));
                }
            }
            PrevAngle = angle;
            Reading += dt;
            // Followed once the rate has settled (until then the weapon's clips keep their own clock).
            if (Rate > 0.2f && Reading > 0.4f) {
                // Onto the reading, taken up over most of a second and never more than a fifth off the rate: the phase
                // keeps the steady rate and drifts onto the feet, rather than following the angle's own unevenness.
                const float error = wrap(angle - Phase - Rate * dt);
                const float most = std::max(0.2f * Rate, 0.1f) * dt;
                Phase += Locked ? std::clamp(error * std::min(1.0f, dt * 1.5f), -most, most) : error; // (locking: straight onto it)
                Locked = true;
            }
        }
    }
    Phase += Rate * dt;
    Phase -= std::floor(Phase);
}
float FirstPersonBodyEaseTwist(float eased, float twist, float limit, float halflife, float dt) {
    const float target = std::clamp(twist, -limit, limit);
    if (halflife <= 0.0f || dt <= 0.0f) return halflife <= 0.0f ? target : eased;
    return eased + (target - eased) * (1.0f - std::exp(-0.69314718f * dt / halflife));
}

glm::vec2 FirstPersonBodyLocalMove(const glm::vec3& worldVelocity, float yaw) {
    const glm::quat r = YawRotation(yaw);
    const glm::vec3 v(worldVelocity.x, 0.0f, worldVelocity.z);
    return {glm::dot(v, r * kRight), glm::dot(v, r * kForward)};
}
float FirstPersonBodyLookDown(float pitchRadians, float startDegrees) {
    const float start = glm::radians(std::clamp(startDegrees, 0.0f, 89.0f));
    const float t = std::clamp((-pitchRadians - start) / (glm::radians(90.0f) - start), 0.0f, 1.0f);
    return std::sin(t * glm::radians(90.0f)); // as a head's forward swing: most of it in the first half
}
float FirstPersonBodyClearPush(const std::vector<glm::vec3>& points, const glm::vec3& a, const glm::vec3& b, const glm::vec3& dir,
                               float clearance, float maxPush, float step) {
    if (clearance <= 0.0f || points.empty()) return 0.0f;
    const glm::vec3 ab = b - a;
    const float abLen2 = std::max(glm::dot(ab, ab), 1e-9f);
    const float c2 = clearance * clearance;
    auto clearAt = [&](float s) {
        const glm::vec3 o = a + dir * s;
        for (const glm::vec3& p : points) {
            const float t = std::clamp(glm::dot(p - o, ab) / abLen2, 0.0f, 1.0f);
            const glm::vec3 d = p - (o + ab * t);
            if (glm::dot(d, d) < c2) return false;
        }
        return true;
    };
    step = std::max(step, 1e-3f);
    for (float s = 0.0f; s < maxPush; s += step)
        if (clearAt(s)) return s;
    return maxPush;
}
float FirstPersonBodyFootPelvis(float offL, float offR, float maxDrop, float maxRaise) {
    return std::clamp(std::min(offL, offR), -std::max(maxDrop, 0.0f), std::max(maxRaise, 0.0f));
}
void FirstPersonBodyStepSpring(float& value, float& velocity, float target, float targetRate, float lag, float dt) {
    // Critically damped, w = 2 / lag. Sub-stepped, so a long frame stays stable.
    const float w = 2.0f / std::max(lag, 1e-3f);
    const int n = std::clamp((int)std::ceil(dt * w / 0.2f), 1, 32);
    const float h = dt / (float)n;
    for (int i = 0; i < n; ++i) {
        velocity += (w * w * (target - value) + 2.0f * w * (targetRate - velocity)) * h;
        value += velocity * h;
    }
}
void FirstPersonBodyGroundRamp(const float* heights, int count, float span, float& height, float& slope) {
    height = slope = 0.0f;
    if (count <= 0) return;
    for (int i = 0; i < count; ++i) height += heights[i];
    height /= (float)count;
    // A window two treads long averages a flight of stairs into its slope: fitted over every sample (least squares).
    // From the two ends alone it flipped between half and all of the true slope as they crossed step edges, and the
    // body's climb rate - fed straight to its spring - jumped with it.
    if (count < 2 || span <= 1e-4f) return;
    float sxx = 0.0f, sxy = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float x = span * ((float)i / (float)(count - 1) - 0.5f);
        sxx += x * x;
        sxy += x * (heights[i] - height);
    }
    slope = sxy / sxx;
}
glm::vec3 FirstPersonBodyEye(const glm::vec3& restHead, const glm::vec3& head, float bob, const glm::vec3& offset) {
    const float b = std::clamp(bob, 0.0f, 1.0f);
    return restHead + (head - restHead) * b + kRight * offset.x + glm::vec3(0.0f, offset.y, 0.0f) + kForward * offset.z;
}
float FirstPersonBodyPieceNearHide(float nearHide, float clothingNearHide, bool clothing) {
    const float body = std::max(nearHide, 0.0f);
    return clothing ? std::max(body, clothingNearHide) : body;
}
bool FirstPersonBodyNearHidden(const glm::vec3& p, const glm::vec3& eye, const glm::vec3& right, float radius, float width) {
    if (radius <= 0.0f) return false;
    const glm::vec3 d = p - eye;
    if (width <= 0.0f) return glm::dot(d, d) < radius * radius;
    const float side = glm::dot(d, right);
    const glm::vec3 rest = d - right * side;
    return (side * side) / (width * width) + glm::dot(rest, rest) / (radius * radius) < 1.0f;
}
std::vector<std::uint8_t> FirstPersonBodyCollarVertices(const std::vector<glm::vec3>& bindPositions, const glm::vec3& neck,
                                                        const glm::vec3& shoulderL, const glm::vec3& shoulderR, float drop) {
    const glm::vec2 line(shoulderL.x - shoulderR.x, shoulderL.z - shoulderR.z);
    const float span = glm::length(line);
    if (drop < 0.0f || span < 1e-3f) return {};
    const glm::vec2 across = line / span, ahead(-across.y, across.x);
    // A little past the shoulder joints: the cloth over them (the shoulder tops) turns up toward the eye.
    const float halfWidth = span * 0.5f * 1.15f, halfDepth = 0.3f;
    const float floor = std::min(shoulderL.y, shoulderR.y) - drop;
    std::vector<std::uint8_t> out(bindPositions.size(), 0);
    for (size_t i = 0; i < bindPositions.size(); ++i) {
        const glm::vec3& p = bindPositions[i];
        const glm::vec2 d(p.x - neck.x, p.z - neck.z);
        out[i] = p.y >= floor && std::abs(glm::dot(d, across)) <= halfWidth && std::abs(glm::dot(d, ahead)) <= halfDepth;
    }
    return out;
}
