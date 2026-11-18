#include "GameFrames.h"
#include "GameRules.h"
#include <stdexcept>
#include "FirstPersonBody.h"
#include "Profiler.h"
#include "BodyDebugDraw.h"
#include "AnimatorController.h"
#include "FirstPersonBodyContract.h"
#include "FirstPersonAnimation.h" // FirstPersonAnimatorContract::kTagHidden: the arms rig hidden (unarmed)

#include "Camera.h"
#include "Components.h"
#include "Core/Env.h"
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
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <sstream>

namespace {

// Model space: the Quantum / UE mannequin family faces +Z with +Y up, so its right is -X.
constexpr glm::vec3 kForward{0.0f, 0.0f, 1.0f};
constexpr float kHandEyeMin = 0.30f, kHandEyeSoft = 0.06f; // m (KeepHandsFromEye)
constexpr float kMmChestTwistMax = 60.0f;       // deg: under MM, the chest's twist toward the view at most this far off the body
// MM Spine Aim (the owner, 2026-11-17: looking up or down the torso bends standing; moving, the head looks and the body runs
// as captured): the chest's share of the view's pitch, full below kMmAimFadeFrom m/s, gone by the walk/jog midpoint.
constexpr float kMmAimFadeFrom = 0.1f;
// Looking down, the drawn upper torso is kept this far past the camera's near plane (m), the eye moved forward over it by
// at most kEyeBodyPushMax (FirstPersonBody::EyeBodyPush).
constexpr float kEyeBodyClearance = 0.03f, kEyeBodyPushMax = 0.10f; // (past 10 cm the camera hangs ahead of the body: the
                                                                     // look-down limit takes over)
constexpr float kNeckRimReach = 0.12f; // m: the torso's open border this close to the neck is the neck opening
constexpr float kNeckRimMargin = 0.1f;  // the view's frustum widened this much (of its half-width) for the rim
// The look-down limit: never above this (degrees; past it the opening isn't the eye's to fix), held over a stride.
constexpr float kLookLimitHighest = -45.0f, kLookLimitHold = 0.6f;
constexpr glm::vec3 kRight{-1.0f, 0.0f, 0.0f};

glm::quat YawRotation(float yaw) { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }

// Frame-rate independent approach toward a target over `seconds` (0 = snap).
float Follow(float dt, float seconds) { return FirstPersonBodyFollow(dt, seconds); }

// A node's position in its model's space as posed now.
glm::vec3 ModelPoint(const Model& m, int node) {
    glm::mat4 g(1.0f);
    return m.NodeTransform(m.NodeName(node), g) ? glm::vec3(g[3]) : glm::vec3(0.0f);
}

// A node's position in its model's bind pose; the driver's when the piece lacks it (one skeleton). False
// when neither has it.
bool BindPoint(const Model& piece, const Model& driver, const std::string& node, glm::vec3& out) {
    for (const Model* m : {&piece, &driver})
        if (const int n = m->NodeIndex(node); n >= 0) {
            out = glm::vec3(m->SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, n)[3]);
            return true;
        }
    return false;
}

// A clothing piece's collar (FirstPersonBodyCollarVertices) as a hide buffer; null when none of it is. `bitsOut`, when
// given, gets the same bits on the CPU (the camera probe). Cached per model and drop: an outfit change in Play
// restarts the body, and the pieces it keeps mustn't hitch.
std::shared_ptr<SkinHideBuffer> CollarVerts(const std::shared_ptr<Model>& pieceRef, const Model& driver, const std::string& neckBone,
                                            const std::array<std::string, 2>& shoulderBones, float drop,
                                            std::shared_ptr<const std::vector<std::uint8_t>>* bitsOut = nullptr) {
    if (bitsOut) bitsOut->reset();
    if (drop < 0.0f || !pieceRef) return nullptr;
    struct Entry {
        float Drop;
        std::weak_ptr<Model> Alive; // the key's model, while it lives (a freed address can be reused)
        std::shared_ptr<SkinHideBuffer> Buffer;
        std::shared_ptr<const std::vector<std::uint8_t>> Bits;
    };
    // Deliberately leaked, like SkinHideBuffer's empty buffer: its buffers go with the GL context, not at exit.
    static auto& cache = *new std::map<const Model*, Entry>();
    for (auto it = cache.begin(); it != cache.end();) it = it->second.Alive.expired() ? cache.erase(it) : std::next(it);
    const Model& piece = *pieceRef;
    if (auto it = cache.find(&piece); it != cache.end() && it->second.Drop == drop) {
        if (bitsOut) *bitsOut = it->second.Bits;
        return it->second.Buffer;
    }

    std::shared_ptr<SkinHideBuffer> buffer;
    std::shared_ptr<const std::vector<std::uint8_t>> bits;
    glm::vec3 neck, shoulderL, shoulderR;
    if (BindPoint(piece, driver, neckBone, neck) && BindPoint(piece, driver, shoulderBones[0], shoulderL) &&
        BindPoint(piece, driver, shoulderBones[1], shoulderR)) {
        std::vector<glm::vec3> positions;
        std::vector<unsigned int> indices;
        piece.CollisionGeometry(positions, indices);
        auto collar = FirstPersonBodyCollarVertices(positions, neck, shoulderL, shoulderR, drop);
        if (std::find(collar.begin(), collar.end(), (std::uint8_t)1) != collar.end()) {
            buffer = std::make_shared<SkinHideBuffer>(OutfitCoverage::Pack(collar));
            bits = std::make_shared<const std::vector<std::uint8_t>>(std::move(collar));
        }
    }
    cache[&piece] = Entry{drop, pieceRef, buffer, bits};
    if (bitsOut) *bitsOut = bits;
    return buffer;
}

// The rig's arm shapes onto `pose` (rotations only, every node under the clavicles: the body keeps
// its own bone lengths), blended by `weight` - the clavicles themselves by `weight * clavicleWeight`
// (Clavicle Follow: the rig's collarbones swing freely, with no torso or head in their way).
void CopyArmShape(const Model& m, const Model& rig, float weight, IK::Pose& pose, const std::vector<int>& parents,
                  const std::map<std::string, std::string>& boneMap, float clavicleWeight) {
    const IK::Pose& rigPose = rig.AppliedLocalPose();
    if ((int)rigPose.size() != rig.NodeCount()) return;
    std::vector<char> under(pose.size(), 0), clavicle(pose.size(), 0);
    for (const char* name : {FPBody::kBoneClavicle[0], FPBody::kBoneClavicle[1]})
        if (const int c = m.NodeIndex(FPBody::MappedBone(boneMap, name)); c >= 0) under[c] = clavicle[c] = 1;
    const float clavWeight = weight * std::clamp(clavicleWeight, 0.0f, 1.0f);
    for (int i = 0; i < (int)pose.size(); ++i) {
        if (!under[i] && parents[i] >= 0 && under[parents[i]]) under[i] = 1;
        if (!under[i]) continue;
        const int r = rig.NodeIndex(m.NodeName(i));
        if (r >= 0) pose[i].R = glm::normalize(glm::slerp(pose[i].R, rigPose[r].R, clavicle[i] ? clavWeight : weight));
    }
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

} // namespace

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

void FirstPersonBodyCopyArmShape(const Model& m, const Model& rig, float weight, std::vector<LocalTRS>& pose, const std::vector<int>& parents,
                                 float clavicleWeight) {
    static const std::map<std::string, std::string> kNoMap;
    CopyArmShape(m, rig, weight, pose, parents, kNoMap, clavicleWeight);
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

void FirstPersonBodyArmShapeLinks(const Model& m, const Model& rig, const std::vector<int>& parents,
                                  std::vector<FirstPersonArmShapeLink>& out) {
    const int clavicles[2] = {m.NodeIndex(FPBody::kBoneClavicle[0]), m.NodeIndex(FPBody::kBoneClavicle[1])};
    FirstPersonBodyArmShapeLinksFrom(m.NodeCount(), parents, clavicles, [&](int i) { return rig.NodeIndex(m.NodeName(i)); }, out);
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

void FirstPersonBody::CopyArmShapeCached(const Model& m, const Model& rig, float weight, std::vector<LocalTRS>& pose,
                                         const std::vector<int>& parents, float clavicleWeight) {
    if (pose.size() != parents.size() || (int)pose.size() != m.NodeCount()) {
        CopyArmShape(m, rig, weight, pose, parents, m_BoneMap, clavicleWeight);
        return;
    }
    ArmShapeCache* entry = nullptr;
    for (ArmShapeCache& e : m_ArmShapeCache)
        if (e.Body == &m && e.Rig == &rig && e.BodyNodes == m.NodeCount() && e.RigNodes == rig.NodeCount()) { entry = &e; break; }
    if (!entry) {
        // The same pairs CopyArmShape finds by name each call (the clavicles through the Bone Map).
        const int clavicles[2] = {m.NodeIndex(FPBody::MappedBone(m_BoneMap, FPBody::kBoneClavicle[0])),
                                  m.NodeIndex(FPBody::MappedBone(m_BoneMap, FPBody::kBoneClavicle[1]))};
        m_ArmShapeCache.push_back({&m, &rig, m.NodeCount(), rig.NodeCount(), {}});
        entry = &m_ArmShapeCache.back();
        FirstPersonBodyArmShapeLinksFrom(m.NodeCount(), parents, clavicles, [&](int i) { return rig.NodeIndex(m.NodeName(i)); }, entry->Links);
    }
    FirstPersonBodyCopyArmShape(entry->Links, rig, weight, pose, clavicleWeight);
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

glm::vec3 FirstPersonGunSeat(const FirstPersonWorldGunInput& gun, const FirstPersonGunSeatBody& body, glm::vec3* meshPush,
                             const glm::vec3* cachedMeshPush) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    glm::vec3 across = body.UpperR - body.UpperL;
    across.y = 0.0f;
    const glm::vec3 right = glm::length(across) > 1e-4f ? glm::normalize(across) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 front = glm::normalize(glm::cross(up, right));
    // The butt into the right shoulder pocket while shouldered.
    const glm::vec3 pocket = body.UpperR + right * gun.Pocket.x + up * gun.Pocket.y + front * gun.Pocket.z;
    glm::vec3 shift = (pocket - gun.ButtWorld) * std::clamp(gun.Shouldered, 0.0f, 1.0f);
    // Keep-outs: the neck, and the head (with a hood on it) centred a little over the head bone.
    const glm::vec3 fwd = glm::length(gun.ForwardWorld) > 1e-4f ? glm::normalize(gun.ForwardWorld) : front;
    const struct { glm::vec3 Centre; float Radius; } keepOut[2] = {{body.Neck, gun.NeckRadius}, {body.Head + up * 0.07f, gun.HeadRadius}};
    for (int pass = 0; pass < 4; ++pass)
        for (const auto& k : keepOut) {
            const glm::vec3 a = gun.ButtWorld + shift, b = a + fwd * gun.GunLength;
            const glm::vec3 ab = b - a;
            const float t = std::clamp(glm::dot(k.Centre - a, ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
            glm::vec3 away = a + ab * t - k.Centre;
            const float d = glm::length(away);
            if (d >= k.Radius) continue;
            away = d > 1e-4f ? away / d : right; // dead on: out to the gun's side
            shift += away * (k.Radius - d);
        }
    // The drawn head, neck and hood, whatever the outfit (the spheres are only their rough shape): straight out from the
    // hood sphere's centre, as far as it takes. On the sights the cheek is welded onto the stock, 2.5 cm off it: kept the
    // full Mesh Clearance away there, a crouched head (low over the gun) pushed the butt down off the shoulder.
    const glm::vec3 beforeMesh = shift;
    const float headClearance = glm::mix(gun.MeshClearance, std::min(gun.MeshClearance, 0.025f), std::clamp(gun.CheekWeld, 0.0f, 1.0f));
    if (body.HeadPoints && headClearance > 0.0f) {
        const glm::vec3 a = gun.ButtWorld + shift, b = a + fwd * gun.GunLength;
        const glm::vec3 centre = keepOut[1].Centre;
        const glm::vec3 ab = b - a;
        const float t = std::clamp(glm::dot(centre - a, ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
        glm::vec3 away = a + ab * t - centre;
        away = glm::length(away) > 1e-4f ? glm::normalize(away) : right;
        const float room = std::max(0.0f, gun.MaxShift - glm::length(shift));
        shift += away * FirstPersonBodyClearPush(*body.HeadPoints, a, b, away, headClearance, room, 0.01f);
    }
    // ... and the drawn torso whenever the butt isn't in the pocket (a reload tucks it under the arm; looking steeply
    // down, a shouldered stock would lie down the chest): straight out from the chest, by that weight.
    if (body.TorsoPoints && body.Chest && gun.MeshClearance > 0.0f && gun.TorsoKeepOut > 1e-3f) {
        const glm::vec3 a = gun.ButtWorld + shift, b = a + fwd * gun.GunLength;
        const glm::vec3 ab = b - a;
        const float t = std::clamp(glm::dot(*body.Chest - a, ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
        glm::vec3 away = a + ab * t - *body.Chest;
        away = glm::length(away) > 1e-4f ? glm::normalize(away) : front;
        const float room = std::max(0.0f, gun.MaxShift - glm::length(shift));
        shift += away * FirstPersonBodyClearPush(*body.TorsoPoints, a, b, away, gun.MeshClearance, room, 0.01f) *
                 std::clamp(gun.TorsoKeepOut, 0.0f, 1.0f);
    }
    if (body.HeadPoints || body.TorsoPoints) {
        if (meshPush) *meshPush = shift - beforeMesh;
    } else if (cachedMeshPush) {
        shift += *cachedMeshPush;
    }
    if (const float len = glm::length(shift); len > gun.MaxShift && len > 1e-6f) shift *= gun.MaxShift / len;
    return shift;
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

bool FirstPersonBody::IsArmsPiece(const entt::registry& reg, entt::entity e) const {
    if (!reg.valid(m_Body) || !reg.valid(e)) return false;
    const auto* cfg = reg.try_get<FirstPersonBodyComponent>(m_Body);
    const auto* name = reg.try_get<NameComponent>(e);
    return cfg && name && FPBody::IsArmsPiece(*cfg, name->Name);
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
    if (pieces.empty()) { Fail("needs rigged body pieces (the object's own model, or child objects with models)."); return false; }
    // The driver: the first piece with the whole skeleton (the pelvis, the head, a hand - the torso), whose pose motion matching
    // solves; the others follow it. (Before Phase 6: the first piece with an Animator Controller.)
    m_BoneMap = FPBody::ParseBoneMap(cfg.BoneMap);
    for (entt::entity e : pieces) {
        const Model& m = *reg.get<RenderableComponent>(e).ModelRef;
        if (m.NodeIndex(Bone(FPBody::kBonePelvis)) >= 0 && m.NodeIndex(Bone(cfg.HeadBone)) >= 0 && m.NodeIndex(Bone(FPBody::kBoneHand[1])) >= 0) {
            m_Driver = e;
            break;
        }
    }
    if (m_Driver == entt::null) {
        Fail("needs a piece with the whole skeleton (the pelvis, the head and the hands: the torso).");
        return false;
    }
    m_Body = body;
    if (const auto* outfit = reg.try_get<CharacterOutfitComponent>(body)) m_OutfitVersion = outfit->Version;

    Model& model = *reg.get<RenderableComponent>(m_Driver).ModelRef;
    m_HeadNode = model.NodeIndex(cfg.HeadBone);
    if (m_HeadNode < 0)
        Log::Warn("First Person Body: the body has no bone '" + cfg.HeadBone + "' - the camera stays at the eye height.");
    else
        m_RestHead = glm::vec3(model.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, m_HeadNode)[3]);
    m_HaveBindPosture = false;
    m_EyeClearBuilt = false;
    m_EyeClearPoints.clear();
    m_EyeClear = 0.0f;
    if (const int pelvisNode = model.NodeIndex(Bone(FPBody::kBonePelvis)); pelvisNode >= 0) {
        const glm::mat4 bp = model.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, pelvisNode);
        m_BindPelvis = IK::Rotation(bp);
    }
    if (const int chest = model.NodeIndex(Bone(FPBody::kBoneSpine[4])); chest >= 0 && m_HeadNode >= 0) {
        m_BindChest = IK::Rotation(model.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, chest));
        m_BindHead = IK::Rotation(model.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, m_HeadNode));
        m_HaveBindPosture = true;
    }
    m_HaveMetricYaw = false;

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
        auto& bodyTag = reg.emplace_or_replace<PlayerBodyTag>(e);
        bodyTag.NearHide = std::max(cfg.NearHide, 0.0f);
        // The torso's shoulders, out of the camera's view (the arms piece keeps its own).
        const bool isArms = IsArmsPiece(reg, e);
        if (!isArms) {
            int count = 0;
            for (const std::string& b : names(cfg.CameraHiddenBones))
                if (const int id = m->BoneId(Bone(b)); id >= 0 && count < 8) bodyTag.CameraHideBones[count++] = id;
        }
        // Clothing (an outfit piece that isn't a body part): its sleeves go into the view-model pass with
        // the arms (PlayerBodyTag). The sleeve bones: the Camera Hidden Bones and everything under them.
        bodyTag.HasSleeves = bodyTag.HasHeadBones = false;
        std::fill(std::begin(bodyTag.SleeveBones), std::end(bodyTag.SleeveBones), 0u);
        std::fill(std::begin(bodyTag.HeadBones), std::end(bodyTag.HeadBones), 0u);
        const auto* piece = reg.try_get<OutfitPieceComponent>(e);
        if (!isArms && piece && !(piece->Flags & OutfitPieceBodyPart)) {
            // Marks the palette bones at and under `roots` in `bits`; true if any.
            auto markUnder = [&](const std::vector<std::string>& roots, std::uint32_t (&bits)[16]) {
                bool any = false;
                std::vector<char> under((size_t)m->NodeCount(), 0);
                for (int i = 0; i < m->NodeCount(); ++i) { // parents come before their children
                    const int p = m->NodeParent(i);
                    under[(size_t)i] = (p >= 0 && under[(size_t)p]) ||
                                       std::find(roots.begin(), roots.end(), m->NodeName(i)) != roots.end();
                    if (!under[(size_t)i]) continue;
                    const int id = m->BoneId(m->NodeName(i));
                    if (id < 0 || id >= 16 * 32) continue;
                    bits[id >> 5] |= 1u << (id & 31);
                    any = true;
                }
                return any;
            };
            // The sleeves: the arm proper, from the upper arm down. Not the collarbones - with the gun up
            // they turn toward the eye, and a garment's shoulder (bulkier than skin) then wrapped the camera:
            // its inside showed as a black flap, swinging with the walk.
            const std::vector<std::string> arms = {Bone(FPBody::kBoneUpperArm[0]), Bone(FPBody::kBoneUpperArm[1])};
            bodyTag.HasSleeves = markUnder(arms, bodyTag.SleeveBones);
            // Never in the camera's view, like the bare torso's shoulders (Camera Hidden Bones): the neck and
            // everything above it (a hood, a collar), the collarbones' cloth (the shoulder tops), and what the
            // neck's parent moves alone (the collar's base, right under the eye).
            std::string headRoot = Bone(cfg.HeadBone), collarBase;
            if (const int h = m->NodeIndex(headRoot); h >= 0 && m->NodeParent(h) >= 0) {
                const int neck = m->NodeParent(h);
                std::string lowerNeck = m->NodeName(neck);
                for (char& c : lowerNeck) c = (char)std::tolower((unsigned char)c);
                if (lowerNeck.find("neck") != std::string::npos) {
                    headRoot = m->NodeName(neck);
                    if (m->NodeParent(neck) >= 0) collarBase = m->NodeName(m->NodeParent(neck));
                }
            }
            bodyTag.HasHeadBones = markUnder({headRoot, Bone(FPBody::kBoneClavicle[0]), Bone(FPBody::kBoneClavicle[1])},
                                             bodyTag.HeadBones);
            // ... minus the arms (the collarbones' subtree holds them): those are the sleeves' to show or hide.
            for (int w = 0; w < 16; ++w) bodyTag.HeadBones[w] &= ~bodyTag.SleeveBones[w];
            if (!collarBase.empty())
                if (const int id = m->BoneId(collarBase); id >= 0 && id < 16 * 32) {
                    bodyTag.HeadBones[id >> 5] |= 1u << (id & 31);
                    bodyTag.HasHeadBones = true;
                }
            // Bones can't tell everything: some packs skin a hood or a scarf to the chest bone (the Quantum
            // open-hood winter jacket, the warm hat's braids). Clothing near the eye isn't drawn in the camera's
            // view either (Clothing Near Hide, wider to the sides); the chest, ~25-30 cm below and ahead, still
            // is when looking down. Tick keeps these up to date with the component.
            bodyTag.Clothing = true;
            bodyTag.NearHide = FirstPersonBodyPieceNearHide(cfg.NearHide, cfg.ClothingNearHide, true);
            bodyTag.NearHideWidth = std::max(cfg.ClothingNearHideWidth, 0.0f);
            // ... and by where it sits rather than what moves it: what's around the neck in the bind pose.
            bodyTag.CollarVerts = CollarVerts(reg.get<RenderableComponent>(e).ModelRef, *reg.get<RenderableComponent>(m_Driver).ModelRef, headRoot,
                                              {Bone(FPBody::kBoneUpperArm[0]), Bone(FPBody::kBoneUpperArm[1])}, cfg.CollarHideDrop,
                                              &bodyTag.CollarBits);
        }
        m_Models.push_back(reg.get<RenderableComponent>(e).ModelRef);
        m_Pieces.push_back(e);
        // A hidden piece is only left out of the camera's own view (the camera is inside the head - and so
        // is whatever an outfit hangs on it: hair, a hat, glasses). Its shadow, the Scene view and other
        // views keep it.
        const std::string name = lower(reg.all_of<NameComponent>(e) ? reg.get<NameComponent>(e).Name : std::string());
        const auto* outfitPiece = reg.try_get<OutfitPieceComponent>(e);
        bool hide = outfitPiece && (outfitPiece->Flags & OutfitPieceHeadAttached);
        for (const std::string& part : hiddenParts) hide = hide || name.find(lower(part)) != std::string::npos;
        if (e != body && hide) {
            reg.get_or_emplace<PlayerBodyTag>(e).CameraHidden = true;
            ++shadowOnly;
        }
    }

    // Split poses: the world twins (only Weapon Arms poses the body's arms differently per view).
    if (cfg.WeaponArms) MakeTwins(world);

    // The input asks the blend tree for speeds its clips have.
    // Player Run / Sprint Speed (when set) move the capsule faster than the clips travel; the clips
    // then play faster to keep pace (Tick's PlayRate).
    m_RunSpeed = std::max(cfg.RunSpeed, 0.01f);
    m_PlayerRunSpeed = cfg.PlayerRunSpeed > 0.0f ? cfg.PlayerRunSpeed : m_RunSpeed;
    // Walk is the default gait and Shift the jog: the capsule tops out at the run (jog) speed, so the sprint
    // never plays. The walk keeps the jog's ratio of player to clip speed. (Motion matching sets the gait speeds below.)
    m_PlayerSprintSpeed = m_PlayerRunSpeed;
    m_PlayerWalkSpeed = std::clamp(cfg.WalkSpeed, 0.01f, m_RunSpeed) * m_PlayerRunSpeed / m_RunSpeed;
    m_CrouchHeight = cfg.CrouchHeight;
    m_CrouchSpeed = cfg.CrouchSpeed;
    player.MoveSpeed = m_PlayerWalkSpeed;
    player.SprintMultiplier = m_PlayerRunSpeed / m_PlayerWalkSpeed;
    player.GaitSpeeds.Active = false;

    // Motion matching: the pose from the database, the capsule at the mocap's gait speeds as captured -
    // walk by default, the jog with Shift, the pack's crouch-walk crouched; strafes (and backwards) at the pack's strafe
    // speeds. (The pack has no sustained sprint: an asset gap, docs/MOTION_MATCHING_HANDOFF.md.)
    {
        size_t driverIndex = 0;
        for (size_t k = 0; k < m_Pieces.size(); ++k)
            if (m_Pieces[k] == m_Driver) driverIndex = k;
        std::string error;
        m_Mm.SetDebug(&MmPlayerDebug::Info()); // the player's (the Inspector, --npc-test mmplayer)
        if (!m_Mm.Start(m_Models, driverIndex, error)) {
            // (CI has no licensed assets: no body there - a warning, never an error.)
            Fail(error + " - the body is off.");
            m_Body = entt::null;
            return false;
        } else {
            const GaitSpeedTable& g = m_Mm.Gaits();
            m_MmSpeeds.Active = true;
            m_MmSpeeds.Forward[0] = g.Walk;
            m_MmSpeeds.Forward[1] = g.Jog;
            m_MmSpeeds.Forward[2] = g.CrouchWalk;
            // Sideways and backwards apart (the walk's sideways takes are slower than its walk backwards).
            const int slotStyle[4] = {MotionLabel::StyleWalk, MotionLabel::StyleJog, MotionLabel::StyleCrouch, MotionLabel::StyleStroll};
            for (int k = 0; k < 3; ++k) {
                m_MmSpeeds.Strafe[k] = g.StyleStrafe(slotStyle[k]);
                m_MmSpeeds.Back[k] = g.StyleBack(slotStyle[k]);
            }
            // The stroll: the Walk action, or a partial stick (Player::StrollStick).
            m_MmSpeeds.Forward[3] = g.Legs[4] > 0 ? g.Stroll : 0.0f;
            m_MmSpeeds.Strafe[3] = g.Legs[4] > 0 ? g.StyleStrafe(MotionLabel::StyleStroll) : 0.0f;
            m_MmSpeeds.Back[3] = g.Legs[4] > 0 ? g.StyleBack(MotionLabel::StyleStroll) : 0.0f;
            // The sprint: slot 4 (a press of Sprint while moving, held - Player::SprintHeld), at the sprint style's measured speeds
            // (the old locomotion's jog loop: docs/MOTION_MATCHING_HANDOFF.md, "Asset gaps").
            m_MmSpeeds.Forward[4] = g.Legs[5] > 0 ? g.Sprint : 0.0f;
            m_MmSpeeds.Strafe[4] = g.Legs[5] > 0 ? g.StyleStrafe(MotionLabel::StyleSprint) : 0.0f;
            m_MmSpeeds.Back[4] = g.Legs[5] > 0 ? g.StyleBack(MotionLabel::StyleSprint) : 0.0f;
            player.GaitSpeeds = m_MmSpeeds;
            char b[400];
            std::snprintf(b, sizeof b,
                          "stroll %.2f / walk %.2f / jog %.2f / crouch %.2f m/s, sideways %.2f / %.2f / %.2f / %.2f, backwards %.2f / %.2f / "
                          "%.2f / %.2f, turn in place %.0f deg/s",
                          m_MmSpeeds.Forward[3], g.Walk, m_MmSpeeds.Forward[1], m_MmSpeeds.Forward[2], m_MmSpeeds.Strafe[3],
                          m_MmSpeeds.Strafe[0], m_MmSpeeds.Strafe[1], m_MmSpeeds.Strafe[2], m_MmSpeeds.Back[3], m_MmSpeeds.Back[0],
                          m_MmSpeeds.Back[1], m_MmSpeeds.Back[2], glm::degrees(m_Mm.TurnInPlaceRate()));
            Log::Info(std::string("First Person Body: motion matching - ") + b + ".");
        }
    }

    Log::Info("First Person Body: '" + (reg.all_of<NameComponent>(body) ? reg.get<NameComponent>(body).Name : std::string("body")) +
              "' is the player's body - " + std::to_string(pieces.size()) + " pieces, " + std::to_string(shadowOnly) +
              " hidden from the camera, root motion at responsiveness " + std::to_string(cfg.Responsiveness).substr(0, 4) + ".");
    return true;
}

bool FirstPersonBody::OutfitChanged(const World& world) const {
    if (m_Body == entt::null || !world.Registry.valid(m_Body)) return false;
    const auto* outfit = world.Registry.try_get<CharacterOutfitComponent>(m_Body);
    return outfit && outfit->Version != m_OutfitVersion;
}

void FirstPersonBody::Stop(World& world) {
    m_ArmShapeCache.clear(); // keyed by model address: the next Start's models may reuse this run's
    BodyDebug::Info() = BodyDebug::Snapshot{};
    BodyDebug::Clear();
    if (m_PoseSource != entt::null && world.Registry.valid(m_PoseSource)) world.Registry.remove<PoseSourceTag>(m_PoseSource);
    for (entt::entity e : m_ArmsTagged)
        if (world.Registry.valid(e)) world.Registry.remove<ViewModelTag>(e);
    for (const auto& m : m_Models)
        if (m) m->SetHiddenNodes({}); // model instances outlive Play; the rest is snapshot state
    for (entt::entity e : m_Pieces)
        if (world.Registry.valid(e)) world.Registry.remove<PlayerBodyTag, OwnerViewOnlyTag>(e);
    for (entt::entity e : m_Twins)
        if (world.Registry.valid(e)) world.DestroyEntityAndChildren(e);
    m_Mm.Stop();
    *this = FirstPersonBody{};
}

// A world twin per piece: the same model (an instance: its own pose), materials, shadows and outfit hiding,
// drawn in every view but the player's own camera, which draws the piece itself. Root-level objects, placed
// onto their piece every frame (SyncTwins), so the outfit's own hierarchy never sees them.
void FirstPersonBody::MakeTwins(World& world) {
    auto& reg = world.Registry;
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    std::vector<std::string> hiddenBones;
    {
        std::stringstream list(cfg.HiddenBones);
        for (std::string name; std::getline(list, name, ',');)
            if (!(name = Trim(name)).empty()) hiddenBones.push_back(name);
    }
    for (size_t k = 0; k < m_Pieces.size(); ++k) {
        const entt::entity e = m_Pieces[k];
        std::shared_ptr<Model> inst = m_Models[k]->CreateInstance();
        const std::string name = reg.all_of<NameComponent>(e) ? reg.get<NameComponent>(e).Name : std::string("piece");
        const entt::entity t = world.CreateModelEntity(inst, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "[Runtime] World " + name);
        // Copied by value: adding a component can move its storage (and any reference into it).
        const RenderableComponent src = reg.get<RenderableComponent>(e);
        auto& dst = reg.get<RenderableComponent>(t);
        dst.Materials = src.Materials;
        dst.CastShadows = src.CastShadows;
        dst.ReceiveShadows = src.ReceiveShadows;
        if (const auto* hide = reg.try_get<OutfitHideTag>(e)) {
            const OutfitHideTag copy = *hide;
            reg.emplace_or_replace<OutfitHideTag>(t, copy);
        }
        if (const auto* pull = reg.try_get<OutfitLayerTag>(e)) {
            const OutfitLayerTag copy = *pull;
            reg.emplace_or_replace<OutfitLayerTag>(t, copy);
        }
        if (const auto* layer = reg.try_get<LayerComponent>(e)) {
            const LayerComponent copy = *layer;
            reg.emplace_or_replace<LayerComponent>(t, copy);
        }
        std::vector<int> nodes;
        for (const std::string& b : hiddenBones)
            if (const int n = inst->NodeIndex(b); n >= 0) nodes.push_back(n);
        inst->SetHiddenNodes(nodes);
        reg.emplace_or_replace<HiddenFromOwnerTag>(t);
        reg.emplace_or_replace<OwnerViewOnlyTag>(e);
        m_Twins.push_back(t);
        m_TwinModels.push_back(std::move(inst));
    }
    SyncTwins(world);
}

void FirstPersonBody::SyncTwins(World& world) {
    auto& reg = world.Registry;
    for (size_t k = 0; k < m_Twins.size() && k < m_Pieces.size(); ++k) {
        if (!reg.valid(m_Twins[k]) || !reg.valid(m_Pieces[k])) continue;
        const glm::mat4 w = world.ComposeWorldTransform(m_Pieces[k]);
        const glm::vec3 scale(glm::length(glm::vec3(w[0])), glm::length(glm::vec3(w[1])), glm::length(glm::vec3(w[2])));
        const glm::mat3 r(glm::vec3(w[0]) / std::max(scale.x, 1e-6f), glm::vec3(w[1]) / std::max(scale.y, 1e-6f),
                          glm::vec3(w[2]) / std::max(scale.z, 1e-6f));
        world.SetWorldPose(m_Twins[k], glm::vec3(w[3]), glm::normalize(glm::quat_cast(r)));
        reg.get<TransformComponent>(m_Twins[k]).Scale = scale;
        const bool inactive = reg.all_of<InactiveTag>(m_Pieces[k]);
        if (inactive != reg.all_of<InactiveTag>(m_Twins[k])) {
            if (inactive) reg.emplace_or_replace<InactiveTag>(m_Twins[k]);
            else reg.remove<InactiveTag>(m_Twins[k]);
        }
        const IK::Pose& pose = m_Models[k]->AppliedLocalPose();
        // The piece's pose, Spine Stability and all: one body, seen from inside and out (only the arms' targets differ).
        if (!pose.empty()) m_TwinModels[k]->ApplyLocalPose(pose);
        // The piece's look can change without its outfit's version: the hiding lands when its coverage has
        // been worked out (often after Start), and a colourway step after the pieces.
        const auto& src = reg.get<RenderableComponent>(m_Pieces[k]);
        auto& dst = reg.get<RenderableComponent>(m_Twins[k]);
        if (dst.Materials != src.Materials) dst.Materials = src.Materials;
        const auto* hide = reg.try_get<OutfitHideTag>(m_Pieces[k]);
        const auto* twinHide = reg.try_get<OutfitHideTag>(m_Twins[k]);
        if (!hide && twinHide) reg.remove<OutfitHideTag>(m_Twins[k]);
        else if (hide && (!twinHide || twinHide->Buffer != hide->Buffer)) {
            const OutfitHideTag copy = *hide;
            reg.emplace_or_replace<OutfitHideTag>(m_Twins[k], copy);
        }
        const auto* pull = reg.try_get<OutfitLayerTag>(m_Pieces[k]);
        const auto* twinPull = reg.try_get<OutfitLayerTag>(m_Twins[k]);
        if (!pull && twinPull) reg.remove<OutfitLayerTag>(m_Twins[k]);
        else if (pull && (!twinPull || twinPull->Pull != pull->Pull)) reg.emplace_or_replace<OutfitLayerTag>(m_Twins[k]).Pull = pull->Pull;
    }
}

void FirstPersonBody::PlaceHeadAttachedTwins(World& world) {
    // As OutfitSystem::UpdateAttachments places the pieces (the head piece's head bone skinning matrix, from
    // its own place), but off the head twin: rigid head wear then follows the world head's tilt, this frame.
    auto& reg = world.Registry;
    int headTwin = -1;
    for (size_t k = 0; k < m_Pieces.size() && k < m_Twins.size(); ++k)
        if (reg.valid(m_Pieces[k]) && reg.all_of<OutfitPieceComponent>(m_Pieces[k]) && reg.get<OutfitPieceComponent>(m_Pieces[k]).Slot == "Head")
            headTwin = (int)k;
    if (headTwin < 0 || !reg.valid(m_Twins[headTwin]) || !m_TwinModels[headTwin]) return;
    const int bone = m_TwinModels[headTwin]->BoneId("head");
    if (bone < 0) return;
    const glm::mat4 follow = world.ComposeWorldTransform(m_Twins[headTwin]) * m_TwinModels[headTwin]->FinalBoneMatrix(bone);
    const glm::vec3 scale(glm::length(glm::vec3(follow[0])), glm::length(glm::vec3(follow[1])), glm::length(glm::vec3(follow[2])));
    if (scale.x < 1e-6f || scale.y < 1e-6f || scale.z < 1e-6f) return;
    const glm::quat rot = glm::normalize(glm::quat_cast(glm::mat3(glm::vec3(follow[0]) / scale.x, glm::vec3(follow[1]) / scale.y, glm::vec3(follow[2]) / scale.z)));
    for (size_t k = 0; k < m_Pieces.size() && k < m_Twins.size(); ++k) {
        if (!reg.valid(m_Pieces[k]) || !reg.valid(m_Twins[k]) || !reg.all_of<OutfitPieceComponent>(m_Pieces[k])) continue;
        const int flags = reg.get<OutfitPieceComponent>(m_Pieces[k]).Flags;
        if (!(flags & OutfitPieceHeadAttached) || (flags & OutfitPieceBodyPart)) continue;
        if (!m_TwinModels[k] || m_TwinModels[k]->BoneCount() > 0) continue; // skinned: it follows by its bones
        const float fit = reg.get<OutfitPieceComponent>(m_Pieces[k]).Fit;
        const glm::mat4 fitted = follow * OutfitSystem::FitMatrix(*m_TwinModels[k], fit);
        world.SetWorldPose(m_Twins[k], glm::vec3(fitted[3]), rot);
        reg.get<TransformComponent>(m_Twins[k]).Scale = scale * (fit > 0.0f ? fit : 1.0f);
    }
}

void FirstPersonBody::BeforePlayerMove(Player& player, Camera& camera) {
    camera.Position -= m_CameraApplied;
    m_CameraApplied = glm::vec3(0.0f);
    player.MaxYawRate = 0.0f;
    player.MinPitch = IsActive() && m_Mm.Active() ? m_LookLimit : -89.0f;
    player.CrouchHeight = IsActive() ? m_CrouchHeight : 0.0f;
    // Crouch Speed is a fraction of the run (jog); the capsule's base is the walk.
    player.CrouchSpeedMultiplier = m_CrouchSpeed * (m_PlayerWalkSpeed > 0.0f ? m_PlayerRunSpeed / m_PlayerWalkSpeed : 1.0f);
    player.GaitSpeeds.Active = false;
    if (!IsActive()) return;
    {
        // Capsule-led: the input moves the capsule (at the gait table's speeds), never the clips.
        player.RootMotionVelocity = glm::vec3(0.0f);
        player.RootMotionWeight = 0.0f;
        player.GaitSpeeds = m_MmSpeeds;
        if (m_Still && m_TurnThreshold > 0.0f) {
            // Turning on the spot: the view turns no faster than the database's turn-in-place clips do (and Max Turn Rate,
            // when set) beyond the threshold, as with the controller's turn clips.
            float rate = glm::degrees(m_Mm.TurnInPlaceRate());
            if (m_MaxTurnRate > 0.0f) rate = rate > 0.0f ? std::min(rate, m_MaxTurnRate) : m_MaxTurnRate;
            player.MaxYawRate = rate;
            player.YawFreeCenter = 90.0f - glm::degrees(m_Yaw);
            player.YawFreeRange = m_TurnThreshold;
        }
    }
}

void FirstPersonBody::Tick(World& world, const Player& player, const Camera& camera, float dt) {
    if (!IsActive()) return;
    // The player's own eye, before the presentation's view punch goes on (FirstPersonPresentation::Update): whatever
    // is on the camera past this by LateUpdate is that punch (LateUpdate keeps it).
    m_CameraBase = camera.Position;
    m_HaveCameraBase = true;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) { m_Body = entt::null; return; }
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    m_SpineCrouch = std::clamp(player.CrouchBlend, 0.0f, 1.0f);
    const float bodyScale = std::max(reg.get<TransformComponent>(m_Body).Scale.y, 1e-3f);
    m_SpineCrouchDrop = cfg.CrouchHeight > 0.0f ? std::max(player.Size.y - cfg.CrouchHeight, 0.0f) / bodyScale : 0.0f;
    m_Responsiveness = cfg.Responsiveness;
    for (entt::entity e : m_Pieces)
        if (auto* tag = reg.valid(e) ? reg.try_get<PlayerBodyTag>(e) : nullptr) {
            // Live, for the Inspector - clothing keeping its own (Start's), not reset to the body's.
            tag->NearHide = FirstPersonBodyPieceNearHide(cfg.NearHide, cfg.ClothingNearHide, tag->Clothing);
            tag->NearHideWidth = tag->Clothing ? std::max(cfg.ClothingNearHideWidth, 0.0f) : 0.0f;
        }
    BodyDebug::Clear();
    m_SinceTrigger += dt;

    // Stand at the capsule's feet, facing the view.
    if (PhysicsWorld::HasCharacter()) {
        float f[3];
        PhysicsWorld::GetCharacterFootPosition(f);
        // The body's height is its own, in the world: on stairs the capsule climbs each step in a few frames (Jolt's
        // stair walk), a staircase the camera on the head felt as a kink every step (~180 m/s^2 on the Sandbox stairs).
        // The body follows the ground's ramp instead: the ground sampled along the travel over a window two treads
        // long and averaged - on stairs their slope; standing still the window closes to the ground under the capsule -
        // plus the hips' drop to the lower foot, on one critically damped spring fed the ramp's climb, so it follows a
        // flight without lag. The legs' IK reaches each step meanwhile.
        const float rise = m_HaveCapsule ? f[1] - m_LastCapsuleY : 0.0f;
        const float capsuleRate = dt > 0.0f ? rise / dt : 0.0f;
        if (!m_HaveBodyY || std::abs(m_BodyY - f[1]) > 1.0f) { m_BodyY = f[1]; m_BodyVy = 0.0f; m_HaveBodyY = true; } // a teleport
        float target = f[1], rate = capsuleRate;
        const bool onFeet = cfg.FootIK && player.Grounded;
        if (onFeet && dt > 0.0f) {
            constexpr int kRays = 16;
            constexpr float kWindow = 0.8f; // metres at a run; it closes below 1 m/s
            const float speed = glm::length(m_GroundVelocity);
            const float window = kWindow * std::min(speed, 1.0f);
            float ramp = 0.0f, slope = 0.0f, uneven = 0.0f;
            if (window > 0.02f) {
                const glm::vec3 along = m_GroundVelocity / speed;
                float heights[kRays];
                QueryFilter filter;
                filter.HitTriggers = 0;
                for (int i = 0; i < kRays; ++i) {
                    const float s = window * ((i + 0.5f) / kRays - 0.5f);
                    const float origin[3] = {f[0] + along.x * s, f[1] + 0.5f, f[2] + along.z * s};
                    const float down[3] = {0.0f, -1.0f, 0.0f};
                    RaycastHit hit;
                    // Off an edge, deeper than two stairs (a gap the capsule bridges), or a wall's top: the capsule's own
                    // ground, so a drop never pulls the body down into it.
                    const bool ground = PhysicsWorld::RaycastSolid(origin, down, 1.0f, filter, hit) && hit.Hit && hit.Normal[1] > 0.5f &&
                                        hit.Point[1] - f[1] > -0.42f;
                    heights[i] = ground ? std::min(hit.Point[1] - f[1], 0.45f) : 0.0f;
                    uneven = std::max(uneven, ground ? std::abs(heights[i]) : 1.0f); // no ground there: not level
                }
                FirstPersonBodyGroundRamp(heights, kRays, window * (kRays - 1) / kRays, ramp, slope);
            }
            float hips = 0.0f;
            if (m_HaveFootGround)
                hips = FirstPersonBodyFootPelvis(m_FootGround[0] - ramp, m_FootGround[1] - ramp, std::min(cfg.FootIKMaxDrop, 0.3f), cfg.PelvisMaxRaise);
            // The body's height off the capsule's eases off past 0.35 m toward 0.4 m (walking a ledge's edge, the hips
            // dropped to a foot over the drop), and with it the ramp's climb fed to the spring: a hard limit, the body
            // stopped against it with a jolt.
            float off = ramp + hips * m_FootWeight, free = 1.0f;
            if (const float over = std::abs(off) - 0.35f; over > 0.0f) {
                const float t = std::tanh(over / 0.05f);
                off = std::copysign(0.35f + 0.05f * t, off);
                free = 1.0f - t * t;
            }
            target = f[1] + off;
            // The ramp climbs at its slope times the speed. Level under the window, the ground's own motion (a lift)
            // is the capsule's: followed at its rate - as is the capsule's, held at the limit.
            // (A lift's pace only: faster, the capsule is popping up a step or off an edge.)
            const float level = std::clamp(1.0f - uneven / 0.03f, 0.0f, 1.0f);
            const float ground = std::clamp(capsuleRate, -1.5f, 1.5f);
            rate = slope * speed + (ground - slope * speed) * level;
            rate = ground + (rate - ground) * free;
        }
        FirstPersonBodyStepSpring(m_BodyY, m_BodyVy, target, rate, onFeet ? cfg.StairEase : 0.03f, dt);
        m_BodyY = std::clamp(m_BodyY, f[1] - 0.5f, f[1] + 0.5f); // a safety: the target stays within 0.4 m
        m_StepOffset = m_BodyY - f[1];
        m_LastCapsuleY = f[1];
        m_GroundVelocity = glm::vec3(player.Velocity.x, 0.0f, player.Velocity.z);
        m_HaveCapsule = true;
        m_Feet = glm::vec3(f[0], f[1] + m_StepOffset, f[2]);
    } else {
        m_Feet = camera.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f);
    }
    m_ViewYaw = FirstPersonBodyYaw(camera.Front(), m_ViewYaw);
    if (!m_HaveHeading) { m_Yaw = m_ViewYaw; m_HaveHeading = true; }
    if (!reg.valid(m_Driver)) { m_Body = entt::null; return; }
    TickMotionMatching(world, player, cfg, dt);
}

// The gait's phase, read off the feet (their spread along the travel against their height difference) - the clips'
// feet as LateUpdate had them before foot IK: after it, stairs and slopes lift one foot over the other and skew the
// phase (the bob and the weapon's sway dipped off the footfalls on stairs).
void FirstPersonBody::UpdateGaitPhase(const Player& player, float dt) {
    const glm::vec2 move = FirstPersonBodyLocalMove(glm::vec3(player.Velocity.x, 0.0f, player.Velocity.z), m_Yaw);
    const float speed = glm::length(move);
    if (player.Grounded && speed > 0.3f && m_HaveGaitFeet) {
        const glm::vec2 dir = move / speed;
        const glm::vec3 d = m_GaitFeet[0] - m_GaitFeet[1]; // driver model space (the body's frame: kRight, up, kForward)
        m_Gait.Update(glm::dot(d, kRight) * dir.x + glm::dot(d, kForward) * dir.y, d.y, true, dt);
    } else {
        m_Gait.Update(0.0f, 0.0f, false, dt);
    }
}

// Motion matching (FirstPersonBodyMotion): the pose is the matcher's (any Animator Controller left on a piece is held); the
// matcher runs on the capsule's state and turns the heading; the body stands at the capsule's feet (the root clamped there).
void FirstPersonBody::TickMotionMatching(World& world, const Player& player, const FirstPersonBodyComponent& cfg, float dt) {
    auto& reg = world.Registry;
    // (A piece that still carries an Animator Controller - a scene from before Phase 6 - is held: the matcher poses every piece.)
    for (entt::entity e : m_Pieces)
        if (auto* ac = reg.valid(e) ? reg.try_get<AnimatorControllerComponent>(e) : nullptr) { ac->SkipUpdate = true; ac->SkippedTime = 0.0f; }
    m_RootVelocity = glm::vec3(0.0f);

    FirstPersonBodyMotion::Input in;
    in.Dt = dt;
    in.Feet = m_Feet - glm::vec3(0.0f, m_StepOffset, 0.0f); // the capsule's own feet
    in.Velocity = player.Velocity;
    in.Wish = player.WishVelocity;
    in.ViewYaw = m_ViewYaw;
    in.Grounded = player.Grounded;
    in.Crouched = player.Crouched;
    in.Sprint = player.Sprinting;
    // The style asked for: the gait slot the capsule moves in (Player::DirectionalSpeeds).
    in.Style = player.Gait == 1   ? MotionLabel::StyleJog
               : player.Gait == 3 ? MotionLabel::StyleStroll
               : player.Gait == 4 ? MotionLabel::StyleSprint
                                  : MotionLabel::StyleWalk;
    in.Jumped = player.Jumped;
    in.JumpSpeed = player.JumpSpeed;
    in.Gravity = player.Gravity;
    in.TurnThreshold = glm::radians(std::max(cfg.TurnThreshold, 0.0f));
    in.TurnEndAngle = glm::radians(std::max(cfg.TurnEndAngle, 0.0f));
    // The capsule's approach (an exponential, time constant GroundAccelTime) as the predictor's spring.
    in.VelocityHalflife = player.GroundAccelTime > 0.0f ? player.GroundAccelTime * 0.6931f : 0.05f;
    const float radius = std::max(0.05f, player.Size.x * 0.5f);
    in.Sweep = [radius](const glm::vec3& from, const glm::vec3& to, glm::vec3& stopAt, glm::vec3& normal) {
        glm::vec3 d = to - from;
        d.y = 0.0f;
        const float len = glm::length(d);
        if (len < 1e-6f) return false;
        d /= len;
        // A wall, not a stair: a sphere just over the capsule's step and another at head height both stop within half a
        // metre of each other. (One sphere at knee height read every riser of the steep stairs as a wall, and one over the
        // step height read the flight's later risers a second ahead: the predicted path stopped, and the matcher played
        // idle or slow frames on the climb.) Low obstacles - a 1 m ledge - aren't predicted; the capsule still stops there.
        const glm::vec3 o = from + glm::vec3(0.0f, 0.4f + radius, 0.0f), head = from + glm::vec3(0.0f, 1.5f, 0.0f);
        QueryFilter f;
        f.HitTriggers = 0;
        RaycastHit hit, high;
        if (!PhysicsWorld::SphereCastSolid(&o.x, &d.x, radius, len, f, hit) || !hit.Hit) return false;
        if (std::fabs(hit.Normal[1]) > 0.7f) return false; // a floor or ramp, not a wall
        if (!PhysicsWorld::SphereCastSolid(&head.x, &d.x, radius, len, f, high) || !high.Hit || high.Distance > hit.Distance + 0.5f) return false;
        stopAt = from + d * std::max(0.0f, hit.Distance - 0.01f);
        normal = glm::vec3(hit.Normal[0], 0.0f, hit.Normal[2]);
        return true;
    };
    m_Mm.Tick(in, m_Yaw);

    m_Twist = FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw);
    m_Still = m_Mm.Still();
    m_Turning = m_Mm.Turning();
    m_TurnThreshold = cfg.TurnThreshold;
    m_MaxTurnRate = cfg.MaxTurnRate;
    m_Grounded = player.Grounded;
    m_InLand = false;
    m_Move = FirstPersonBodyLocalMove(glm::vec3(player.Velocity.x, 0.0f, player.Velocity.z), m_Yaw);
    m_IdleTime = m_Still ? m_IdleTime + dt : 0.0f;
    m_MoveTime = m_Still ? 0.0f : m_MoveTime + dt;
    m_AirTime = player.Grounded ? 0.0f : m_AirTime + dt;
    if (!player.Grounded) m_FallSpeed = std::max(0.0f, -player.Velocity.y);
    world.SetWorldPose(m_Body, m_Feet, YawRotation(m_Yaw));
    UpdateGaitPhase(player, dt);

    BodyDebug::Snapshot& d = BodyDebug::Info();
    d.Valid = true;
    d.AnimatorState = FPBody::kStateMotionMatching;
    d.StateTime = 0.0f;
    d.Turning = m_Turning;
    d.BodyYawDeg = glm::degrees(m_Yaw);
    d.ViewOffsetDeg = glm::degrees(FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw));
    d.TwistDeg = glm::degrees(m_Twist);
    d.Still = m_Still;
    d.IdleTime = m_IdleTime;
    d.MoveTime = m_MoveTime;
    d.MoveSpeed = glm::length(m_Move);
    // The clips' travel as shown: the matched frame's own root speed at the play rate it is warped to.
    d.RootSpeed = m_Mm.Airborne() ? 0.0f : m_Mm.AnimSpeed() * m_Mm.PlayRate();
    d.StepOffset = m_StepOffset;
    d.ArmsWeight = m_ArmsWeight;
    d.LastTrigger = m_Mm.Airborne() ? "Airborne (MM)" : std::string();
    d.LastTriggerAgo = 0.0f;
    if (BodyDebug::Enabled()) {
        const glm::vec3 base = m_Feet + glm::vec3(0.0f, 0.03f, 0.0f);
        const auto heading = [&](float yaw) { return glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)); };
        BodyDebug::Arrow(base, base + heading(m_Yaw) * 0.9f, glm::vec4(1.0f, 0.85f, 0.2f, 1.0f));
        BodyDebug::Arrow(base, base + heading(m_ViewYaw) * 0.9f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    }
}

void FirstPersonBody::LateUpdate(World& world, Camera& camera, float dt, entt::entity weaponArms,
                                 const std::string& rigCameraBone) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) { m_Body = entt::null; return; }
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    m_Spine = cfg.Spine;

    // This step's root motion, for the capsule's next move.
    if (!reg.valid(m_Driver)) { m_Body = entt::null; return; }
    m_RootVelocity = glm::vec3(0.0f); // capsule-led: no clip travel
    // Motion matching: this step's pose onto every piece (the controller's update was held), before anything reads it.
    if (m_Mm.Active()) {
        m_Mm.Pose(m_Models);
        CaptureMmClip();
    }
    // The presentation's view punch on the camera this frame (Update / RefreshViewKick put it on after Tick): the walk
    // bob, landing dip, lean and ADS camera share. It stays on - the camera goes to the body's eye plus this, below.
    const glm::vec3 viewKick = m_HaveCameraBase ? camera.Position - m_CameraBase : glm::vec3(0.0f);
    m_HaveCameraBase = false; // this frame's Tick's only

    // This frame's Weapon Arms weight, before anything here reads it - the eye most of all. (Set only in ArmsLateUpdate,
    // a draw's first frame put the eye where the unarmed one is while the arms took the rig's hands: a pop.) The rig as
    // ArmsLateUpdate will find it: hidden by the arms animator's Hidden tag as of this frame's update, which
    // FirstPersonPresentation::LateUpdate (after this) turns into its InactiveTag.
    {
        bool follow = cfg.WeaponArms && m_ViewModelFovOk && weaponArms != entt::null && reg.valid(weaponArms) &&
                      reg.all_of<RenderableComponent>(weaponArms);
        if (follow) {
            if (reg.all_of<InactiveTag>(weaponArms) && !reg.all_of<DeactivatedTag>(weaponArms)) follow = false; // off for another reason
            else if (const auto* armsAc = reg.try_get<AnimatorControllerComponent>(weaponArms))
                follow = !armsAc->HasTag(FirstPersonAnimatorContract::kTagHidden);
            else follow = !reg.all_of<InactiveTag>(weaponArms);
        }
        m_ArmsWeightPrev = m_ArmsWeight;
        if (follow) m_ArmsWeight = 1.0f;
        else m_ArmsWeight -= m_ArmsWeight * Follow(dt, cfg.ArmsEaseOut);
        m_ArmsWeightEased = true;
        m_EyeArmsWeight = m_ArmsWeight;
    }

    BodyDebug::Info().ChestPitchClipDeg = ChestPitchDeg();
    TrunkAngles(BodyDebug::Info().TrunkClipDeg);
    if (const Model* drv = DriverModel(); drv)
        if (glm::mat4 pv(1.0f); drv->NodeTransform(Bone(FPBody::kBonePelvis), pv)) BodyDebug::Info().PelvisHeightClip = pv[3].y;
    // The A/B switch (FirstPersonBodyComponent::MmPostLayers): off, motion matching's pose is drawn with foot IK only.
    const bool mmLayers = cfg.MmPostLayers;
    BodyDebug::Info().ChestPitchStableDeg = ChestPitchDeg();
    // The clips' feet, before foot IK moves them: what next Tick reads the gait's phase off.
    m_HaveGaitFeet = FootPoints(world, m_GaitFeet);
    {
    PROFILE_SCOPE("FPB foot IK");
    ApplyFootIK(world, cfg, dt); // the pelvis and legs first: everything after reads the final pose
    }
    if (IK::FootSlideMeterOn()) { // the feet as drawn (a visible piece that has them), for the --npc-test slide meter
        const float scale = std::max(reg.get<TransformComponent>(m_Body).Scale.y, 1e-3f);
        for (const auto& mp : m_Models) {
            glm::mat4 n0(1.0f), n1(1.0f), b0(1.0f), b1(1.0f);
            if (!mp || !mp->NodeTransform(Bone(FPBody::kBoneFoot[0]), n0) || !mp->NodeTransform(Bone(FPBody::kBoneFoot[1]), n1)) continue;
            const glm::quat yaw = YawRotation(m_Yaw);
            const glm::vec3 feet[2] = {m_Feet + yaw * (scale * glm::vec3(n0[3])), m_Feet + yaw * (scale * glm::vec3(n1[3]))};
            // The balls too: a foot pivoting on its ball (or heel) is turning, not sliding (FootSlideMeterTotals::MaxContactDrift).
            const bool balls = mp->NodeTransform(Bone(FPBody::kBoneBall[0]), b0) && mp->NodeTransform(Bone(FPBody::kBoneBall[1]), b1);
            const glm::vec3 ball[2] = {m_Feet + yaw * (scale * glm::vec3(b0[3])), m_Feet + yaw * (scale * glm::vec3(b1[3]))};
            IK::FootSlideMeterFor("player").Step(feet, m_Feet.y, dt, balls ? &ball : nullptr);
            BodyDebug::Info().DrawnFoot[0] = feet[0];
            BodyDebug::Info().DrawnFoot[1] = feet[1];
            break;
        }
    }
    PROFILE_SCOPE("FPB spine + shoulders");

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
        for (size_t k = 0; k < m_Pieces.size(); ++k) {
            if (m_Pieces[k] == m_Body || !IsArmsPiece(reg, m_Pieces[k])) continue;
            const Model& m = *m_Models[k];
            // Scratch kept between calls: this runs five or six times a frame.
            thread_local IK::Pose pose;
            thread_local std::vector<int> parents;
            thread_local std::vector<glm::mat4> globals;
            pose = m.AppliedLocalPose();
            const int ul = m.NodeIndex(Bone(FPBody::kBoneUpperArm[0])), ur = m.NodeIndex(Bone(FPBody::kBoneUpperArm[1]));
            if (pose.empty() || (int)pose.size() != m.NodeCount() || ul < 0 || ur < 0) break;
            parents.resize(pose.size());
            for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
            CopyArmShapeCached(m, *armRig, m_ArmsWeight, pose, parents, cfg.ClavicleFollow);
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
    const float viewPitch = std::asin(std::clamp(camera.Front().y, -1.0f, 1.0f));
    // Unarmed, the chest's pitch at the view is capped (Spine Aim Max Up / Down, eased in): past it the head looks on alone.
    const float aimUnarmed = 1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f);
    float spineAim = FirstPersonBodyCappedSpineAim(viewPitch, FirstPersonBodySpineAim(viewPitch, cfg.SpineAim, cfg.SpineAimDown),
                                                   glm::radians(viewPitch >= 0.0f ? cfg.SpineAimMaxUp : cfg.SpineAimMaxDown), aimUnarmed);
    // Motion matching, unarmed: the aim fades out with speed (FirstPersonBodyMmAimFade) - moving, the head looks and the body
    // moves as captured. (Armed, the chest still carries the arms rig's aim.)
    if (m_Mm.Active()) {
        const GaitSpeedTable& g = m_Mm.Gaits();
        const float fade = FirstPersonBodyMmAimFade(glm::length(m_Move), kMmAimFadeFrom, 0.5f * (g.Walk + g.Jog));
        spineAim *= 1.0f - aimUnarmed * (1.0f - fade);
        BodyDebug::Info().MmAimFade = fade;
    }
    // The torso's forward bend (BodyDebug): pelvis to the top spine bone off the vertical, in the driver's model space.
    const auto torsoBend = [&]() {
        glm::mat4 p(1.0f), c(1.0f);
        if (!model || !model->NodeTransform(Bone(FPBody::kBonePelvis), p) || !model->NodeTransform(Bone(FPBody::kBoneSpine[4]), c)) return 0.0f;
        const glm::vec3 d = glm::vec3(c[3]) - glm::vec3(p[3]);
        return glm::degrees(std::atan2(glm::dot(d, kForward), d.y));
    };
    BodyDebug::Info().ClipBendDeg = torsoBend();
    BodyDebug::Info().ViewPitchDeg = glm::degrees(viewPitch);
    // The pitch only: the chest's heading (Spine Twist, or the rig's stance) is closed-loop below, measured off the
    // shoulders - the clips' own spine yaw is in it, so an open-loop twist here was only ever a first guess.
    if (mmLayers && spineAim > 0.0f) {
        // Arm Hang, unarmed (armed, the arms take the rig's pose and reach the gun: ArmsLateUpdate).
        const float hang = 1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f);
        thread_local std::vector<ArmHangPose> arms;
        if (hang > 1e-3f) CaptureArms(arms);
        ApplySpineAim(camera, spineAim, 0.0f);
        BodyDebug::Info().ArmDownAimDeg = ArmDownDeg();
        if (hang > 1e-3f) HangArms(arms, hang);
    } else {
        BodyDebug::Info().ArmDownAimDeg = ArmDownDeg();
    }
    BodyDebug::Info().ArmDownDeg = ArmDownDeg();
    // Armed, the chest takes the arms rig's stance: its shoulder line, measured against the camera. The
    // rig is authored bladed (the left shoulder ~13 cm ahead of the right, ~20 degrees): squared to the
    // view, the body's left shoulder sat ~7 cm behind the rig's, and the support hand came off the gun.
    const float lineMatch = std::clamp(cfg.ShoulderLineMatch, 0.0f, 1.0f) * std::clamp(m_ArmsWeight, 0.0f, 1.0f);
    bool matched = false;
    if (armRig && lineMatch > 1e-3f && reg.valid(weaponArms)) {
        glm::mat4 sl(1.0f), sr(1.0f);
        if (armRig->NodeTransform(FPBody::kBoneUpperArm[0], sl) && armRig->NodeTransform(FPBody::kBoneUpperArm[1], sr)) {
            const glm::vec3 across = glm::mat3(world.ComposeWorldTransform(weaponArms)) * (glm::vec3(sl[3]) - glm::vec3(sr[3]));
            const glm::vec3 inCamera(glm::dot(across, camera.Right()), glm::dot(across, camera.Up()), glm::dot(across, camera.Front()));
            if (!m_HaveRigShoulderLine) { m_RigShoulderLine = inCamera; m_HaveRigShoulderLine = true; }
            m_RigShoulderLine += (inCamera - m_RigShoulderLine) * Follow(dt, 0.15f);
            const glm::vec3 rigWorld = camera.Right() * m_RigShoulderLine.x + camera.Up() * m_RigShoulderLine.y + camera.Front() * m_RigShoulderLine.z;
            const glm::vec3 rigModel = glm::inverse(YawRotation(m_Yaw)) * rigWorld;
            // Unarmed the chest squares to the view; easing between, the target is between the two.
            const glm::vec3 square(std::cos(m_Twist), 0.0f, -std::sin(m_Twist));
            const glm::vec3 target = glm::mix(square * glm::length(rigModel), rigModel, lineMatch);
            // Twice: the clavicles the rig lends the arms move the shoulders a little as the chest turns.
            for (int pass = 0; pass < 2; ++pass) {
                glm::vec3 l, r;
                shoulderPair(l, r);
                // The rig's line tilts as its collarbones swing (a reload's reach lifts its support shoulder):
                // matched whole, the chest rolled that shoulder up at the camera. The body takes the rig's tilt
                // only by Clavicle Follow; the stance's blade (its turn about the vertical) is matched in full.
                const glm::vec3 lineTarget = FirstPersonBodyShoulderLineTilt(l - r, target, cfg.ClavicleFollow);
                const glm::quat turn = FirstPersonBodyShoulderLineTurn(l - r, lineTarget, 1.0f);
                if (FirstPersonBodyAngleOf(turn) > 0.002f) ApplySpineRotation(turn);
            }
            matched = true;
            auto blade = [](const glm::vec3& v) { return glm::degrees(std::atan2(v.z, v.x)); }; // + = left shoulder forward
            glm::vec3 l, r;
            shoulderPair(l, r);
            const glm::vec3 chest = YawRotation(m_Yaw) * (l - r);
            BodyDebug::Info().RigBlade = blade(m_RigShoulderLine);
            BodyDebug::Info().ChestBlade = blade(glm::vec3(glm::dot(chest, camera.Right()), 0.0f, glm::dot(chest, camera.Front())));
        }
    }
    if (!matched) m_HaveRigShoulderLine = false;
    // Unarmed, the chest's heading: Spine Twist of the way from where the clips have it (a turn leads with the chest) to
    // the view's, measured off the shoulder line - one pass, about the model's up, so the line's heading lands exactly
    // (every bone's share turns about the same axis; short of it only where the spine's per-bone limits stop it). Off it,
    // one shoulder sits further from the view than the other.
    if (mmLayers && cfg.SpineTwist > 0.0f && !matched) {
        // Motion matching: an additive yaw - the view off the character facing (which turns as the mocap turns), capped,
        // spread over the spine - on top of the clip's chest. Not a closed loop: the gait's own counter-rotation of the
        // shoulders against the hips passes untouched (re-aimed every frame, the shoulder line was held square to the view:
        // the jog's chest swung 1.36x the clip's against the pelvis).
        const float lim = glm::radians(kMmChestTwistMax);
        const float twist = std::clamp(m_Twist, -lim, lim) * std::min(cfg.SpineTwist, 1.0f);
        if (std::abs(twist) > 0.002f) ApplySpineAim(camera, 0.0f, twist);
    } else if (mmLayers && cfg.SpineTwist > 0.0f && !matched) {
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
    // What the chest's tilt does to the head goes on in full, so the shoulders stay the same distance from the eye as
    // the view pitches. The head bob is procedural (the weapon's Camera Motion: a dip on each footfall, the landing's -
    // the view punch kept on top below), so the eye takes none of the clips' own bob: it follows the head's slow
    // average (its standing height, a crouch, a stop's lean), trailing the head by at most Eye Slack and pulled back
    // in over 40 ms past it - as the armed eye follows the shoulders.
    glm::vec3 tilt(0.0f);
    if (model->NodeTransform(model->NodeName(m_HeadNode), head)) tilt = glm::vec3(head[3]) - headAnimated;
    FirstPersonBodyFollowShoulders(m_HeadEased, m_HaveEye, headAnimated, cfg.EyeSlack, 0.04f, dt);
    // Motion matching, unarmed: the eye rides the mocap head's own motion by MM Head Ride (the owner, 2026-11-17: the
    // stabilized eye over a body bobbing as captured felt stiff, and the neck rose into the view at every jogging step).
    const float headRide = std::clamp(cfg.MmHeadRide, 0.0f, 1.0f) * (1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f));
    m_HeadRide = headRide;
    BodyDebug::Info().HeadRide = headRide;
    m_Eye = FirstPersonBodyEye(m_RestHead, glm::mix(m_HeadEased, headAnimated, headRide), 1.0f, cfg.CameraOffset);
    BodyDebug::Info().HeadHeight = headAnimated.y * reg.get<TransformComponent>(m_Body).Scale.y;

    const auto& t = reg.get<TransformComponent>(m_Body);
    const glm::quat yaw = YawRotation(m_Yaw);
    glm::vec3 eyeWorld = m_Feet + yaw * (t.Scale * (m_Eye + tilt));

    // The chest's view: the view's heading, pitched only as far as the chest is (its share of the view's
    // pitch). Arm Steadiness holds the shoulders in it. (The eye is hung off the shoulders in the view's
    // own frame - the rig's shoulders pivot with the whole view, so that is where the hands are reached
    // from; looking down, the chest pitching further, Spine Aim Down, keeps the camera out of it.)
    const float chestPitch = viewPitch * spineAim;
    const glm::vec3 flat(std::sin(m_ViewYaw), 0.0f, std::cos(m_ViewYaw));
    const glm::vec3 chestFront = flat * std::cos(chestPitch) + glm::vec3(0.0f, std::sin(chestPitch), 0.0f);
    const glm::quat toChest(camera.Front(), chestFront); // the view's frame onto the chest's (about the view's right)
    const glm::vec3 chestRight = toChest * camera.Right(), chestUp = toChest * camera.Up();

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
            const glm::vec3 inCamera = FirstPersonBodyRigEyeToShoulders(rigWorld, sl, sr, cb, camera);
            // The clips sway their shoulders about the camera; that must not move the view, so only a
            // slow drift of this offset is followed.
            if (!m_HaveRigOffset) { m_RigEyeToShoulders = inCamera; m_HaveRigOffset = true; }
            m_RigEyeToShoulders += (inCamera - m_RigEyeToShoulders) * Follow(dt, 0.4f);

            const glm::vec3 shoulders = shouldersNow();
            const glm::vec3 toRest = shoulders - shouldersAnimated; // the chest's tilt at the shoulders
            // The eye follows the shoulders' slow average, none of their motion about it (the bob is the
            // procedural camera's) - not the bind pose, whose shoulders sit elsewhere than the standing
            // pose's, which would put the shoulders (and so the hands' reach) that far off.
            // The eye may trail or under-follow the shoulders by only Eye Slack: a clip that throws the
            // shoulders about (a stop pulling the body back) would otherwise carry them out of the
            // arms' reach and a hand would come off the gun. Bigger motions move the camera with them.
            // Past the slack the eye is pulled back in fast (40 ms) rather than snapped to it: snapped, its speed jumped
            // in one frame - a kink felt as a jolt when the body crouched or stood on the move.
            FirstPersonBodyFollowShoulders(m_Shoulders, m_HaveShoulders, shouldersAnimated, cfg.EyeSlack, 0.04f, dt);
            BodyDebug::Info().EyeSlack = glm::length(shouldersAnimated - m_Shoulders);
            // A little slack: the shoulders sit this much further toward the gun than the rig's, so the
            // support arm is never at full stretch (a straight arm jumps at every small motion).
            const float kReachSlack = cfg.ReachSlack;
            const glm::vec3 offset = camera.Right() * m_RigEyeToShoulders.x + camera.Up() * m_RigEyeToShoulders.y +
                                     camera.Front() * (m_RigEyeToShoulders.z + kReachSlack);
            // Armed Eye Offset: the rig's camera sits lower against its shoulders than an eye does; lifted back up -
            // looking level, where the chest's top was in the view's corner. It goes looking down (the chest is then
            // under the eye, and lifted, part of it came out of Near Hide and its cut edge showed) and looking up (the
            // lifted gun took the support hand past its reach, off the handguard).
            const glm::vec3 lift = FirstPersonBodyArmedEyeOffset(cfg.ArmedEyeOffset, viewPitch);
            const glm::vec3 fromShoulders = m_Feet + yaw * (t.Scale * (m_Shoulders + toRest + lift)) - offset;
            // Remember where this puts the eye against the head's: unarmed the eye keeps that height, or
            // the camera would jump when the gun is holstered. Taken as it is (both eyes are already smoothed),
            // and held from the frame the arms start to ease off.
            const glm::vec3 delta = glm::inverse(yaw) * (fromShoulders - eyeWorld);
            if (!m_HaveArmedEye || m_ArmsWeight > 0.99f) m_ArmedEyeDelta = delta;
            m_HaveArmedEye = true;
            eyeWorld = glm::mix(eyeWorld, fromShoulders, std::min(m_ArmsWeight, 1.0f));
        }
    }
    if (m_HaveArmedEye) eyeWorld += yaw * m_ArmedEyeDelta * (1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f));
    // Looking down, the eye comes forward over the chest as a head does, pitching at the neck. The camera
    // rides at the head bone (the skull's base, over the neck), so straight down it looked into the
    // torso's neck opening: the torso was seen from inside (culled away) and the legs through it.
    eyeWorld += flat * (cfg.LookDownPush * FirstPersonBodyLookDown(viewPitch, cfg.LookDownStart));
    // ... and on, as far as it takes to keep the drawn upper torso past the near plane (unarmed: armed, the eye is the rig's,
    // and the chest still pitches with the aim).
    // Past what the push can do, the look-down limit (motion matching, unarmed - the owner, 2026-11-17: "limit the amount we can
    // look down a little bit so it doesn't clip"): the steepest pitch from which the opening is out of the view.
    {
        const float share = 1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f);
        float limit = -89.0f;
        const float want = viewPitch < glm::radians(-10.0f) && share > 1e-3f ? share * EyeBodyPush(world, camera, eyeWorld, flat, limit) : 0.0f;
        if (share < 0.5f) limit = -89.0f;
        m_LookLimitAge += dt;
        if (limit >= m_LookLimitHeld || m_LookLimitAge > kLookLimitHold) {
            m_LookLimitHeld = limit;
            m_LookLimitAge = 0.0f;
        }
        m_LookLimit += (m_LookLimitHeld - m_LookLimit) * Follow(dt, m_LookLimitHeld > m_LookLimit ? 0.06f : 0.3f);
        BodyDebug::Info().LookLimitDeg = m_LookLimit;
        // (In at once - eased in, it trailed an eye riding the jog's bob and the neck showed for a frame or two at a step - out gently.)
        m_EyeClear = want > m_EyeClear ? want : m_EyeClear + (want - m_EyeClear) * Follow(dt, 0.15f);
        eyeWorld += flat * m_EyeClear;
        BodyDebug::Info().EyeBodyPush = m_EyeClear;
    }
    // The lean (a corner peek): the kick's sideways step is the head swinging over on the hips, and the arms rig - placed
    // off the camera - swings with it. The eye alone moving would put the rig's hands up to 30 cm off the body's
    // shoulders and a hand off the gun, so the spine leans the same way: the shoulders (and the head) go over with the
    // view and the arms reach the gun from there. Rolled about the view's flat forward, after the eye is worked out (the
    // camera is the unleaned eye plus the whole kick), sized so the shoulders' midpoint moves sideways as far as the kick
    // does: estimated off the spine's length, measured, trimmed once (the spine's spread and limits set the pivot). The
    // kick's drop and the rest (bob, landing dip, ADS camera share) stay on the camera alone - centimetres, which Arm
    // Steadiness (held in the camera's frame, below) and the elbows take up.
    m_LeanWanted = m_LeanShoulders = 0.0f;
    {
        glm::vec3 flatRight = camera.Right();
        flatRight.y = 0.0f;
        const float len = glm::length(flatRight);
        const float side = len > 1e-4f ? glm::dot(viewKick, flatRight / len) : 0.0f;
        m_LeanWanted = side;
        glm::mat4 root(1.0f);
        if (std::abs(side) > 1e-3f && haveShoulders && model->NodeTransform(Bone(FPBody::kBoneSpine[0]), root)) {
            const glm::vec3 rightModel = glm::inverse(yaw) * (flatRight / len);
            // About up x right: turning a point above the pivot by a positive angle carries it toward rightModel.
            const glm::vec3 axis = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), rightModel));
            const float scale = std::max(t.Scale.x, 1e-3f);
            const float want = side / scale;
            const glm::vec3 start = shouldersNow();
            const float lever = glm::length(start - glm::vec3(root[3]));
            if (lever > 0.05f) {
                float moved = 0.0f;
                for (int pass = 0; pass < 2; ++pass) {
                    const float angle = std::asin(std::clamp((want - moved) / lever, -0.9f, 0.9f));
                    if (std::abs(angle) < 1e-4f) break;
                    ApplySpineRotation(glm::angleAxis(angle, axis));
                    moved = glm::dot(shouldersNow() - start, rightModel);
                }
                m_LeanShoulders = moved * scale;
            }
        }
    }
    BodyDebug::Info().TorsoBendDeg = torsoBend();
    if (BodyDebug::Enabled()) {
        const glm::vec3 shoulders = m_Feet + yaw * (t.Scale * m_Shoulders);
        BodyDebug::Cross(eyeWorld, 0.04f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));       // the camera
        BodyDebug::Cross(shoulders, 0.04f, glm::vec4(1.0f, 0.3f, 1.0f, 1.0f));      // the shoulders the eye is anchored to
        BodyDebug::Line(shoulders, eyeWorld, glm::vec4(1.0f, 0.3f, 1.0f, 0.7f));
    }
    // The camera: the body's eye with the view punch kept on top. What this adds is measured from the camera as it came
    // in (the player's own eye + the kick), so it is the eye less the player's own eye: next frame RemoveViewKick takes
    // the kick off and BeforePlayerMove this, and the camera is the player's own eye again - nothing accumulates.
    m_BodyEye = eyeWorld;
    BodyDebug::Info().BodyEye = eyeWorld;
    m_ViewKick = viewKick;
    const glm::vec3 cameraIn = camera.Position;
    camera.Position = eyeWorld + viewKick;
    m_CameraApplied = camera.Position - cameraIn;
    BodyDebug::Info().EyeApplied = m_CameraApplied;
    // Arm Steadiness holds the shoulders in this frame: it moves with the camera (the eye, the bob, the lean - the arms
    // rig hangs off it) and the chest's pitch, so only the gait's sway shows in it (the rig's own frame pitches with the
    // view, the chest less).
    m_ChestView = glm::mat4(glm::vec4(chestRight, 0.0f), glm::vec4(chestUp, 0.0f), glm::vec4(chestFront, 0.0f), glm::vec4(camera.Position, 1.0f));
    m_HaveChestView = true;
    {
        const float unarmed = 1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f);
        BodyDebug::Info().HandEyePush[0] = BodyDebug::Info().HandEyePush[1] = 0.0f;
        if (mmLayers && unarmed > 1e-3f) KeepHandsFromEye(glm::inverse(yaw) * (camera.Position - m_Feet) / std::max(t.Scale.y, 1e-3f), unarmed);
    }
    PostureMetrics(world, camera, dt);
}

// Puts each foot on the ground under it: a ray down from the animated foot gives how far the ground
// is above / below the capsule's, the pelvis drops to the lower foot, and the legs are re-solved to
// the offset feet (tilted toward the ground while planted). On every piece so they stay one skeleton.
bool FirstPersonBody::FootPoints(const World& world, glm::vec3 (&out)[2]) const {
    const auto& reg = world.Registry;
    if (!IsActive() || !reg.valid(m_Body) || !reg.valid(m_Driver)) return false;
    const auto* rc = reg.try_get<RenderableComponent>(m_Driver);
    if (!rc || !rc->ModelRef) return false;
    const float scale = std::max(reg.get<TransformComponent>(m_Body).Scale.y, 1e-3f);
    for (int s = 0; s < 2; ++s) {
        glm::mat4 n(1.0f);
        if (!rc->ModelRef->NodeTransform(Bone(FPBody::kBoneFoot[s]), n)) return false;
        out[s] = scale * glm::vec3(n[3]);
    }
    return true;
}

bool FirstPersonBody::FootHeights(const World& world, float (&out)[2]) const {
    const auto& reg = world.Registry;
    if (!IsActive() || !reg.valid(m_Body) || !reg.valid(m_Driver)) return false;
    const auto* rc = reg.try_get<RenderableComponent>(m_Driver);
    if (!rc || !rc->ModelRef) return false;
    // As ApplyFootIK places the animated feet: the driver's model space sits on the feet, scaled by the root.
    const float scale = std::max(reg.get<TransformComponent>(m_Body).Scale.y, 1e-3f);
    for (int s = 0; s < 2; ++s) {
        glm::mat4 n(1.0f);
        if (!rc->ModelRef->NodeTransform(Bone(FPBody::kBoneFoot[s]), n)) return false;
        out[s] = scale * n[3].y;
    }
    return true;
}

bool FirstPersonBody::BoneWorld(const World& world, const std::string& standard, glm::vec3& out, bool worldTwins) const {
    glm::mat4 m(1.0f);
    if (!BoneWorldMatrix(world, standard, m, worldTwins)) return false;
    out = glm::vec3(m[3]);
    return true;
}

bool FirstPersonBody::BoneWorldMatrix(const World& world, const std::string& standard, glm::mat4& out, bool worldTwins) const {
    if (!IsActive() || !world.Registry.valid(m_Body)) return false;
    const auto& reg = world.Registry;
    auto isArms = [&](size_t k) { return IsArmsPiece(reg, m_Pieces[k]); };
    // The world twins when poses are split (what every view but the player's camera shows), else the pieces.
    const bool twins = worldTwins && !m_Twins.empty();
    const auto& ents = twins ? m_Twins : m_Pieces;
    const auto& models = twins ? m_TwinModels : m_Models;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t k = 0; k < m_Pieces.size() && k < models.size() && k < ents.size(); ++k) {
            if (!reg.valid(ents[k]) || !models[k]) continue;
            if (pass == 0 ? !isArms(k) : m_Pieces[k] != m_Driver) continue;
            glm::mat4 n(1.0f);
            if (!models[k]->NodeTransform(Bone(standard), n)) continue;
            out = world.ComposeWorldTransform(ents[k]) * n;
            return true;
        }
    return false;
}

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
        IK::Pose pose = m.AppliedLocalPose();
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

void FirstPersonBody::CaptureArms(std::vector<ArmHangPose>& out) const {
    thread_local std::vector<int> parents;
    thread_local std::vector<glm::mat4> globals;
    out.assign(m_Models.size(), ArmHangPose{});
    for (size_t k = 0; k < m_Models.size(); ++k) {
        if (!m_Models[k]) continue;
        const Model& m = *m_Models[k];
        const IK::Pose& pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        for (int s = 0; s < 2; ++s)
            if (const int u = m.NodeIndex(Bone(FPBody::kBoneUpperArm[s])); u >= 0) {
                out[k].Rot[s] = IK::Rotation(globals[(size_t)u]);
                out[k].Have[s] = true;
            }
    }
}

void FirstPersonBody::HangArms(const std::vector<ArmHangPose>& saved, float weight) {
    thread_local std::vector<int> parents;
    thread_local std::vector<glm::mat4> globals;
    thread_local IK::Pose pose;
    for (size_t k = 0; k < m_Models.size() && k < saved.size(); ++k) {
        if (!m_Models[k] || !(saved[k].Have[0] || saved[k].Have[1])) continue;
        Model& m = *m_Models[k];
        pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        bool changed = false;
        for (int s = 0; s < 2; ++s) {
            const int u = m.NodeIndex(Bone(FPBody::kBoneUpperArm[s]));
            if (u < 0 || !saved[k].Have[s] || parents[(size_t)u] < 0) continue;
            const glm::quat now = IK::Rotation(globals[(size_t)u]);
            const glm::quat want = glm::slerp(now, saved[k].Rot[s], std::clamp(weight, 0.0f, 1.0f));
            const glm::quat parent = IK::Rotation(globals[(size_t)parents[(size_t)u]]);
            pose[(size_t)u].R = glm::normalize(glm::inverse(parent) * want);
            changed = true;
        }
        if (changed) m.ApplyLocalPose(pose);
    }
}

void FirstPersonBody::KeepHandsFromEye(const glm::vec3& eyeModel, float weight) {
    thread_local std::vector<int> parents;
    thread_local std::vector<glm::mat4> globals;
    thread_local IK::Pose pose;
    // The targets from the driver (every piece is one skeleton): a hand closer than the minimum goes out along the line
    // from the eye, to a soft maximum of its distance and the minimum.
    const Model* driver = DriverModel();
    if (!driver) return;
    glm::vec3 target[2];
    bool push[2] = {false, false};
    for (int s = 0; s < 2; ++s) {
        glm::mat4 h(1.0f);
        if (!driver->NodeTransform(Bone(FPBody::kBoneHand[s]), h)) continue;
        const glm::vec3 d = glm::vec3(h[3]) - eyeModel;
        const float dist = glm::length(d);
        if (dist < 1e-4f || dist > kHandEyeMin + 3.0f * kHandEyeSoft) continue;
        const float x = dist - kHandEyeMin;
        const float want = kHandEyeMin + 0.5f * (x + std::sqrt(x * x + kHandEyeSoft * kHandEyeSoft)); // soft max(dist, min)
        if (want - dist < 1e-4f) continue;
        target[s] = eyeModel + d / dist * want;
        push[s] = true;
        BodyDebug::Info().HandEyePush[s] = (want - dist) * std::clamp(weight, 0.0f, 1.0f); // (model units: metres on the body)
    }
    if (!push[0] && !push[1]) return;
    for (const auto& mp : m_Models) {
        if (!mp) continue;
        Model& m = *mp;
        pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        bool changed = false;
        for (int s = 0; s < 2; ++s) {
            if (!push[s]) continue;
            const int u = m.NodeIndex(Bone(FPBody::kBoneUpperArm[s])), l = m.NodeIndex(Bone(FPBody::kBoneLowerArm[s])),
                      e = m.NodeIndex(Bone(FPBody::kBoneHand[s]));
            if (u < 0 || l < 0 || e < 0) continue;
            const glm::quat handRot = IK::Rotation(globals[(size_t)e]);
            changed |= IK::SolveTwoBone(pose, parents, globals, u, l, e, target[s], &handRot, std::clamp(weight, 0.0f, 1.0f));
        }
        if (changed) m.ApplyLocalPose(pose);
    }
}

float FirstPersonBody::ArmDownDeg() const {
    const Model* m = nullptr;
    for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k)
        if (m_Pieces[k] == m_Driver) m = m_Models[k].get();
    if (!m) return 0.0f;
    float worst = 0.0f;
    for (int s = 0; s < 2; ++s) {
        glm::mat4 u(1.0f), l(1.0f);
        if (!m->NodeTransform(Bone(FPBody::kBoneUpperArm[s]), u) || !m->NodeTransform(Bone(FPBody::kBoneLowerArm[s]), l)) continue;
        const glm::vec3 d = glm::vec3(l[3]) - glm::vec3(u[3]);
        if (glm::length(d) < 1e-5f) continue;
        worst = std::max(worst, glm::degrees(std::acos(std::clamp(-glm::normalize(d).y, -1.0f, 1.0f))));
    }
    return worst;
}

const Model* FirstPersonBody::DriverModel() const {
    for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k)
        if (m_Pieces[k] == m_Driver) return m_Models[k].get();
    return nullptr;
}

bool FirstPersonBody::ChestTurn(glm::quat& out) const {
    const Model* m = DriverModel();
    glm::mat4 c(1.0f);
    if (!m || !m_HaveBindPosture || !m->NodeTransform(Bone(FPBody::kBoneSpine[4]), c)) return false;
    out = glm::normalize(IK::Rotation(c) * glm::inverse(m_BindChest));
    return true;
}

float FirstPersonBody::ChestPitchDeg() const {
    glm::quat turn;
    if (!ChestTurn(turn)) return 0.0f;
    const glm::vec3 up = turn * glm::vec3(0.0f, 1.0f, 0.0f);
    return glm::degrees(std::atan2(glm::dot(up, kForward), up.y));
}

void FirstPersonBody::CaptureMmClip() {
    const Model* m = DriverModel();
    const MotionDatabase* db = m_Mm.Database();
    if (!m || !db) return;
    const auto& bones = db->Stream().Bones();
    if (m_MmLayerNodes.size() != bones.size()) {
        m_MmLayerNodes.assign(bones.size(), -1);
        for (size_t i = 0; i < bones.size(); ++i) m_MmLayerNodes[i] = m->NodeIndex(bones[i]);
    }
    const IK::Pose& pose = m->AppliedLocalPose();
    m_MmClipRot.resize(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        const int n = m_MmLayerNodes[i];
        m_MmClipRot[i] = n >= 0 && n < (int)pose.size() ? pose[(size_t)n].R : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
}

bool FirstPersonBody::TrunkAngles(float (&out)[9]) const {
    const Model* m = DriverModel();
    glm::mat4 p(1.0f);
    glm::quat chest;
    if (!m || !m_HaveBindPosture || !ChestTurn(chest) || !m->NodeTransform(Bone(FPBody::kBonePelvis), p)) return false;
    const glm::quat pelvis = glm::normalize(IK::Rotation(p) * glm::inverse(m_BindPelvis));
    const auto angles = [](const glm::quat& q, float* o) { // yaw (+ toward the body's right), roll (+ toward its left), pitch (+ forward)
        const glm::vec3 f = q * kForward, u = q * glm::vec3(0.0f, 1.0f, 0.0f);
        o[0] = glm::degrees(std::atan2(glm::dot(f, kRight), glm::dot(f, kForward)));
        o[1] = glm::degrees(std::atan2(glm::dot(u, -kRight), u.y));
        o[2] = glm::degrees(std::atan2(glm::dot(u, kForward), u.y));
    };
    angles(pelvis, out);
    angles(chest, out + 3);
    angles(glm::normalize(glm::inverse(pelvis) * chest), out + 6);
    return true;
}

void FirstPersonBody::PostureMetrics(const World& world, const Camera& camera, float dt) {
    BodyDebug::Snapshot& d = BodyDebug::Info();
    if (dt > 0.0f && m_HaveMetricYaw) d.BodyYawRateDeg = glm::degrees(FirstPersonBodyWrapAngle(m_Yaw - m_MetricYaw)) / dt;
    m_MetricYaw = m_Yaw;
    m_HaveMetricYaw = true;
    const Model* m = DriverModel();
    if (!m || !world.Registry.valid(m_Body)) return;
    TrunkAngles(d.TrunkDeg);
    if (m_Mm.Active() && m_MmClipRot.size() == m_MmLayerNodes.size() && m_Mm.Database()) {
        const IK::Pose& pose = m->AppliedLocalPose();
        d.LayerDeg.resize(m_MmLayerNodes.size());
        for (size_t i = 0; i < m_MmLayerNodes.size(); ++i) {
            const int n = m_MmLayerNodes[i];
            d.LayerDeg[i] = n >= 0 && n < (int)pose.size() ? glm::degrees(MotionMath::QuatAngle(m_MmClipRot[i], pose[(size_t)n].R)) : 0.0f;
        }
        d.LayerBones = &m_Mm.Database()->Stream().Bones();
    }
    glm::quat chest;
    if (ChestTurn(chest)) {
        const glm::vec3 up = chest * glm::vec3(0.0f, 1.0f, 0.0f);
        d.ChestPitchDeg = glm::degrees(std::atan2(glm::dot(up, kForward), up.y));
        d.ChestRollDeg = glm::degrees(std::atan2(glm::dot(up, -kRight), up.y));
        glm::mat4 h(1.0f);
        if (m_HeadNode >= 0 && m->NodeTransform(m->NodeName(m_HeadNode), h)) {
            // The head's turn off the bind pose, in the chest's frame (the chest's own turn taken out).
            const glm::quat head = glm::normalize(IK::Rotation(h) * glm::inverse(m_BindHead));
            const glm::vec3 hu = glm::inverse(chest) * (head * glm::vec3(0.0f, 1.0f, 0.0f));
            d.HeadPitchDeg = glm::degrees(std::atan2(glm::dot(hu, kForward), hu.y));
            const glm::vec3 hw = head * glm::vec3(0.0f, 1.0f, 0.0f);
            d.HeadWorldPitchDeg = glm::degrees(std::atan2(glm::dot(hw, kForward), hw.y));
        }
    }
    const float scale = std::max(world.Registry.get<TransformComponent>(m_Body).Scale.y, 1e-3f);
    const glm::quat yaw = YawRotation(m_Yaw);
    for (int s = 0; s < 2; ++s) {
        glm::mat4 hand(1.0f), sh(1.0f);
        if (!m->NodeTransform(Bone(FPBody::kBoneHand[s]), hand) || !m->NodeTransform(Bone(FPBody::kBoneUpperArm[s]), sh)) continue;
        d.HandOverShoulder[s] = scale * (hand[3].y - sh[3].y);
        const glm::vec3 handWorld = m_Feet + yaw * (scale * glm::vec3(hand[3]));
        const glm::vec3 toHand = handWorld - camera.Position;
        d.HandToEye[s] = glm::length(toHand);
        d.HandInView[s] = d.HandToEye[s] < 0.8f && d.HandToEye[s] > 1e-4f &&
                          glm::dot(toHand / d.HandToEye[s], camera.Front()) > std::cos(glm::radians(35.0f));
    }
    if (glm::mat4 pv(1.0f); m->NodeTransform(Bone(FPBody::kBonePelvis), pv)) d.PelvisHeight = scale * pv[3].y;
    glm::mat4 fl(1.0f), fr(1.0f);
    if (m->NodeTransform(Bone(FPBody::kBoneFoot[0]), fl) && m->NodeTransform(Bone(FPBody::kBoneFoot[1]), fr))
        d.FeetGap = scale * glm::dot(glm::vec3(fl[3]) - glm::vec3(fr[3]), -kRight);
}

// Tilts the spine by `amount` of the camera's pitch and twists it by `twist` radians, spread evenly over spine_01..spine_05 (pitch about
// the model's X axis, twist about Y), on every piece so they stay one skeleton.
void FirstPersonBody::ApplySpineAim(const Camera& camera, float amount, float twist) {
    const float pitch = std::asin(std::clamp(camera.Front().y, -1.0f, 1.0f)); // up is positive
    // Rotating +Z about +X by theta gives (0, -sin, cos): looking up needs a negative angle.
    // The twist about the model's up turns the chest toward the view (positive = to the left).
    RotateSpine([&](float n) {
        return glm::angleAxis(twist / n, glm::vec3(0.0f, 1.0f, 0.0f)) *
               glm::angleAxis(-pitch * amount / n, glm::vec3(1.0f, 0.0f, 0.0f));
    });
}

void FirstPersonBody::ApplySpineRotation(const glm::quat& modelDelta) {
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    RotateSpine([&](float n) { return glm::normalize(glm::slerp(none, modelDelta, 1.0f / n)); });
}

void FirstPersonBody::RotateSpine(const std::function<glm::quat(float)>& stepFor) {
    const char* const* kSpine = FPBody::kBoneSpine;
    RotateChain({kSpine[0], kSpine[1], kSpine[2], kSpine[3], kSpine[4]}, stepFor, nullptr, &m_Spine);
}

void FirstPersonBody::RotateChain(const std::vector<std::string>& chain, const std::function<glm::quat(float)>& stepFor,
                                  const std::vector<std::shared_ptr<Model>>* models, const IK::SpineDistribution* dist) {
    // Scratch kept between calls (several chains a frame, each over every piece's full pose).
    thread_local std::vector<int> parents;
    thread_local std::vector<glm::mat4> globals;
    thread_local IK::Pose pose;
    thread_local std::vector<int> bones, slot; // slot: the bone's place in the chain (its weight and limit)
    const auto& list = models ? *models : m_Models;
    for (size_t k = 0; k < list.size(); ++k) {
        const auto& mp = list[k];
        if (!mp) continue;
        Model& m = *mp;
        pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        bones.clear();
        slot.clear();
        for (size_t c = 0; c < chain.size(); ++c)
            if (const int i = m.NodeIndex(Bone(chain[c])); i >= 0) { bones.push_back(i); slot.push_back((int)c); }
        if (bones.empty()) continue;
        // A piece skinning nothing the chain moves (legs, feet) draws the same either way. The driver always
        // turns: its head and shoulders are read for the camera and the arms.
        if ((k >= m_Pieces.size() || m_Pieces[k] != m_Driver) && !SkinsUnder(m, bones, chain.front())) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        // Only the chain and what's above it are read before ApplyLocalPose re-derives the rest.
        globals.resize(pose.size());
        IK::RefreshPath(pose, parents, globals, -1, bones.back());
        // Each bone's share of the turn: even (the bone count) unless the distribution says otherwise.
        float divisor[8], weight[8];
        const size_t count = std::min<size_t>(bones.size(), 5);
        for (size_t b = 0; b < count; ++b) weight[b] = dist ? dist->Weight[std::min(slot[b], 4)] : 1.0f;
        IK::ChainDivisors(weight, (int)count, divisor);
        const float pelvisAlpha = dist ? std::clamp(dist->PelvisAlpha, 0.0f, 1.0f) : 0.0f;
        if (pelvisAlpha > 0.0f) // the pelvis takes its share first; the spine the rest
            if (const int pelvis = m.NodeIndex(Bone(FPBody::kBonePelvis)); pelvis >= 0) {
                IK::RefreshPath(pose, parents, globals, -1, pelvis);
                IK::OffsetBoneOnly(pose, parents, globals, pelvis, glm::vec3(0.0f), stepFor(1.0f / pelvisAlpha),
                                   IK::Position(globals[pelvis]));
                IK::RefreshPath(pose, parents, globals, pelvis, bones.back());
            }
        // Only the next spine bone's global is read before ApplyLocalPose re-derives the lot, so
        // refresh just the path to it rather than the whole upper body after every bone.
        for (size_t b = 0; b < bones.size(); ++b) {
            const int i = bones[b];
            if (b > 0) IK::RefreshPath(pose, parents, globals, bones[b - 1], i);
            float d = b < count ? divisor[b] : (float)bones.size();
            if (pelvisAlpha > 0.0f) d /= 1.0f - pelvisAlpha;
            glm::quat step = stepFor(d);
            if (dist && b < count) step = IK::ClampStepAngle(step, dist->MaxAngle[std::min(slot[b], 4)]);
            IK::OffsetBoneOnly(pose, parents, globals, i, glm::vec3(0.0f), step, IK::Position(globals[i]));
        }
        m.ApplyLocalPose(pose);
    }
}

bool FirstPersonBody::SkinsUnder(const Model& m, const std::vector<int>& roots, const std::string& key) {
    const auto id = std::make_pair(&m, key);
    if (const auto it = m_SkinsUnder.find(id); it != m_SkinsUnder.end()) return it->second;
    std::vector<char> under((size_t)m.NodeCount(), 0);
    for (int r : roots) if (r >= 0 && r < m.NodeCount()) under[r] = 1;
    bool skins = false;
    for (int i = 0; i < m.NodeCount() && !skins; ++i) { // parents first
        if (!under[i] && m.NodeParent(i) >= 0 && under[m.NodeParent(i)]) under[i] = 1;
        skins = under[i] && m.BoneId(m.NodeName(i)) >= 0;
    }
    m_SkinsUnder[id] = skins;
    return skins;
}

bool FirstPersonBody::SkinsUpperBody(const Model& m) {
    if (const auto it = m_SkinsUpperBody.find(&m); it != m_SkinsUpperBody.end()) return it->second;
    std::vector<char> under((size_t)m.NodeCount(), 0);
    int chest = -1;
    for (int b = 4; b >= 0 && chest < 0; --b) chest = m.NodeIndex(Bone(FPBody::kBoneSpine[b]));
    if (chest >= 0) under[chest] = 1;
    for (int s = 0; s < 2; ++s)
        if (const int c = m.NodeIndex(Bone(FPBody::kBoneClavicle[s])); c >= 0) under[c] = 1;
    bool skins = false;
    for (int i = 0; i < m.NodeCount(); ++i) { // parents first
        if (!under[i] && m.NodeParent(i) >= 0 && under[m.NodeParent(i)]) under[i] = 1;
        if (under[i] && m.BoneId(m.NodeName(i)) >= 0) { skins = true; break; }
    }
    m_SkinsUpperBody[&m] = skins;
    return skins;
}

// The arms' twist bones after an arm solve: each forearm's twist bones carry the hand's roll by how far down the forearm
// they sit, each upper arm's undo its roll toward the shoulder (IK::SpreadTwist). Their skin then turns along the limb
// instead of wringing at the wrist and the shoulder.
bool FirstPersonArmTwist::Apply(Model& m, float* roll, float* residual) {
    auto it = m_Rigs.find(&m);
    if (it == m_Rigs.end() || it->second.Nodes != m.NodeCount()) {
        Rig r;
        r.Nodes = m.NodeCount();
        m.BindLocalPose(r.Bind);
        std::vector<int> parents(m.NodeCount());
        std::vector<std::string> names(m.NodeCount());
        for (int i = 0; i < m.NodeCount(); ++i) { parents[i] = m.NodeParent(i); names[i] = m.NodeName(i); }
        for (int s = 0; s < 2; ++s) {
            r.Upper[s] = m.NodeIndex(FPBody::kBoneUpperArm[s]);
            r.Lower[s] = m.NodeIndex(FPBody::kBoneLowerArm[s]);
            r.Hand[s] = m.NodeIndex(FPBody::kBoneHand[s]);
            if (r.Upper[s] < 0 || r.Lower[s] < 0 || r.Hand[s] < 0) continue;
            r.UpperTwist[s] = IK::FindTwistBones(r.Bind, parents, names, r.Upper[s], r.Lower[s]);
            r.LowerTwist[s] = IK::FindTwistBones(r.Bind, parents, names, r.Lower[s], r.Hand[s]);
        }
        it = m_Rigs.insert_or_assign(&m, std::move(r)).first;
    }
    const Rig& r = it->second;
    IK::Pose pose = m.AppliedLocalPose();
    if ((int)pose.size() != m.NodeCount() || pose.size() != r.Bind.size()) return false;
    bool any = false;
    for (int s = 0; s < 2; ++s) {
        if (r.Upper[s] < 0 || r.Lower[s] < 0 || r.Hand[s] < 0) continue;
        if (!r.LowerTwist[s].empty()) {
            const float turn = IK::EndRoll(pose, r.Bind, r.Lower[s], r.Hand[s]);
            IK::SpreadTwist(pose, r.Bind, r.Lower[s], r.Hand[s], r.LowerTwist[s], turn, true);
            float most = 0.0f;
            for (const IK::TwistBone& t : r.LowerTwist[s]) most = std::max(most, t.Share);
            if (roll) roll[s] = glm::degrees(turn);
            if (residual) residual[s] = glm::degrees(turn * (1.0f - most));
            any = true;
        }
        if (!r.UpperTwist[s].empty()) {
            IK::SpreadTwist(pose, r.Bind, r.Upper[s], r.Lower[s], r.UpperTwist[s], IK::SegmentRoll(pose, r.Bind, r.Upper[s], r.Lower[s]), false);
            any = true;
        }
    }
    if (any) m.ApplyLocalPose(pose);
    return any;
}

void FirstPersonBody::ArmTwist(const std::vector<std::shared_ptr<Model>>& models) {
    PROFILE_SCOPE("FPB arm twist");
    for (const auto& mp : models)
        if (mp) m_ArmTwist.Apply(*mp);
}

float FirstPersonAnchorWeight(float distance, float nearDist, float farDist) {
    if (farDist <= nearDist) return distance > nearDist ? 1.0f : 0.0f;
    const float x = std::clamp((distance - nearDist) / (farDist - nearDist), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

glm::mat4 FirstPersonPartialMove(const glm::mat4& move, const glm::mat4& m, float weight) {
    const glm::vec3 scale(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    const glm::quat turn = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), IK::Rotation(move), weight);
    const glm::vec3 from(m[3]), to(move * m[3]);
    glm::mat4 out = glm::translate(glm::mat4(1.0f), glm::mix(from, to, weight)) * glm::mat4_cast(glm::normalize(turn * IK::Rotation(m)));
    for (int c = 0; c < 3; ++c) out[c] *= scale[c];
    return out;
}

void FirstPersonBody::ArmsLateUpdate(World& world, entt::entity weaponArms, float viewModelFov, float dt, const Camera* camera,
                                     const glm::mat4& thirdPersonGun, const FirstPersonWorldGunInput* /*gun*/,
                                     const Model* thirdPersonRig, const std::string& gunSocket,
                                     const FirstPersonHandAnchor* /*handAnchor*/) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) return;
    const FirstPersonBodyComponent& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    m_Spine = cfg.Spine;
    BodyDebug::Info().TwinHandGap[0] = BodyDebug::Info().TwinHandGap[1] = -1.0f; // (none this frame until the twins' solve)
    // The world twins start from the pieces' pose as it stands now: the same clips, spine passes and feet - one body. From
    // here both get their arms onto a gun: the pieces onto the rig's hands, the twins onto the world gun's hold.
    {
    PROFILE_SCOPE("FPB sync twins");
    SyncTwins(world);
    }
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
    // LateUpdate already set this frame's weight (so the eye used it, not last frame's): re-decided here from where it
    // started the frame, with this rig's own state - the same unless LateUpdate's guess at it was off.
    m_ViewModelFovOk = viewModelFov > 0.0f;
    if (m_ArmsWeightEased) m_ArmsWeight = m_ArmsWeightPrev;
    m_ArmsWeightEased = false;
    if (follow) m_ArmsWeight = 1.0f;
    else m_ArmsWeight -= m_ArmsWeight * Follow(dt, cfg.ArmsEaseOut);
    const bool viewModelArms = follow;
    // Easing off a holstered gun (a weapon swap's holster to draw, or going unarmed) the arms piece is hidden
    // from the camera, below.
    const bool armsHidden = haveRig && !follow && m_ArmsWeight > 1e-3f;
    // Clothing's sleeves go with the arms (PlayerBodyTag::SleeveBones): into the view-model pass, or hidden with
    // them - left in view, an empty sleeve hung where the arm had been.
    for (entt::entity e : m_Pieces)
        if (auto* tag = reg.valid(e) ? reg.try_get<PlayerBodyTag>(e) : nullptr) {
            tag->SleevesInViewModel = viewModelArms && tag->HasSleeves;
            tag->SleevesHidden = armsHidden && tag->HasSleeves;
        }
    // The body's arms piece goes into the view-model pass with the gun (its hands then sit where the
    // rig's do); off, it is an ordinary piece of the body again.
    for (size_t k = 0; k < m_Pieces.size(); ++k) {
        const entt::entity e = m_Pieces[k];
        if (e == m_Body || !reg.valid(e) || !IsArmsPiece(reg, e)) continue;
        if (viewModelArms) {
            if (!reg.all_of<ViewModelTag>(e)) { reg.emplace<ViewModelTag>(e); m_ArmsTagged.push_back(e); }
        } else if (reg.all_of<ViewModelTag>(e)) {
            reg.remove<ViewModelTag>(e);
        }
        // Easing off a holstered gun the pose is still the rig's (its hands, no gun to hold): shown, it
        // would be a pair of hands hanging in the view for a few frames. Hidden until it has settled.
        auto& rc = reg.get<RenderableComponent>(e);
        // Per piece: the arms and the hands may both be pieces, each with its own setting to go back to.
        auto saved = std::find_if(m_ArmsEasedOut.begin(), m_ArmsEasedOut.end(), [e](const auto& s) { return s.first == e; });
        if (armsHidden) {
            if (saved == m_ArmsEasedOut.end()) m_ArmsEasedOut.emplace_back(e, (int)rc.CastShadows);
            rc.CastShadows = RenderableComponent::ShadowCasting::ShadowsOnly;
        } else if (saved != m_ArmsEasedOut.end()) {
            rc.CastShadows = (RenderableComponent::ShadowCasting)saved->second;
            m_ArmsEasedOut.erase(saved);
        }
    }
    if (m_ArmsWeight < 1e-3f || !haveRig) {
        m_HaveShoulderAnchor[0] = m_HaveShoulderAnchor[1] = false;
        m_HaveElbowAim[0] = m_HaveElbowAim[1] = false;
        m_WorldHaveShoulderAnchor[0] = m_WorldHaveShoulderAnchor[1] = false;
        m_WorldHaveElbowAim[0] = m_WorldHaveElbowAim[1] = false;
        m_WorldGunDelta = glm::mat4(1.0f);
        if (!m_Twins.empty()) PlaceHeadAttachedTwins(world);
        // (Unarmed too: what the camera sees of the body doesn't need the rig.)
        if (camera && (BodyDebug::Enabled() || BodyDebug::ProbeRequested())) CameraProbe(world, *camera, viewModelFov, dt);
        if (camera && BodyDebug::ProbeRequested()) InsideProbe(world, *camera);
        return;
    }

    const Model* rig = reg.get<RenderableComponent>(weaponArms).ModelRef.get();
    if (!rig || rig->AppliedLocalPose().empty()) return;
    const IK::Pose& rigPose = rig->AppliedLocalPose();
    if ((int)rigPose.size() != rig->NodeCount()) return;
    const glm::mat4 rigWorld = world.ComposeWorldTransform(weaponArms);
    // The frame the shoulders are held in: the chest's view (LateUpdate), else the rig's.
    const glm::mat4 anchorFrame = m_HaveChestView ? m_ChestView : rigWorld;
    const glm::mat4 anchorInverse = glm::inverse(anchorFrame);
    // Arm Steadiness: the rig's arms are fixed to the view, but the body's shoulders and chest sway with
    // the gait. With the hands pinned to the rig's, that sway came out at the elbows (the support arm,
    // nearly straight, most of all). Steady, each shoulder is held at a slow average of where it sits
    // relative to the eye (in the chest's view frame), and each elbow bends in the plane the rig's does.
    const float steady = std::clamp(cfg.ArmSteadiness, 0.0f, 1.0f) * m_ArmsWeight;

    struct Side { const char *Clavicle, *Upper, *Lower, *Hand; };
    static const Side kSides[2] = {{FPBody::kBoneClavicle[0], FPBody::kBoneUpperArm[0], FPBody::kBoneLowerArm[0], FPBody::kBoneHand[0]},
                                   {FPBody::kBoneClavicle[1], FPBody::kBoneUpperArm[1], FPBody::kBoneLowerArm[1], FPBody::kBoneHand[1]}};
    float gripInset = 0.0f, fingerCurl = 0.0f; // the body's settings plus the worn race's (a skeleton's)
    OutfitSystem::RaceGrip(world, m_Body, gripInset, fingerCurl);
    gripInset += cfg.GripInset;
    fingerCurl += cfg.FingerCurlExtra;

    // What the arms reach for (world): each hand, Grip Inset's way, and the upper arm and elbow (its bend plane) of the
    // arms posed by `Shape`, whose arm shapes and fingers the solve copies.
    struct ArmTargets {
        const Model* Shape = nullptr;
        glm::vec3 Pos[2]{}, Palm[2]{}, Shoulder[2]{}, Elbow[2]{};
        glm::quat Rot[2]{};
        bool Have[2] = {false, false}, HaveElbow[2] = {false, false};
    };
    // `from`'s arms as placed by `place` (its model space to world).
    auto readTargets = [&](const Model& from, const glm::mat4& place, ArmTargets& out) {
        out = ArmTargets{};
        out.Shape = &from;
        for (int s = 0; s < 2; ++s) {
            glm::mat4 h(1.0f), u(1.0f), l(1.0f);
            if (!from.NodeTransform(kSides[s].Hand, h)) continue;
            const glm::mat4 w = place * h;
            out.Pos[s] = glm::vec3(w[3]);
            out.Rot[s] = IK::Rotation(w);
            out.Palm[s] = FirstPersonBodyPalmDirection(from, place, s);
            out.Have[s] = true;
            if (from.NodeTransform(kSides[s].Upper, u) && from.NodeTransform(kSides[s].Lower, l)) {
                out.Shoulder[s] = glm::vec3(place * u[3]);
                out.Elbow[s] = glm::vec3(place * l[3]);
                out.HaveElbow[s] = true;
            }
        }
    };
    // The first person: the rig's hands, exactly - the arms piece is drawn with the gun in the same projection.
    ArmTargets firstPerson;
    readTargets(*rig, rigWorld, firstPerson);

    // The arms onto `t`'s hands: the pieces (first person), then the world twins (the world gun's hold). Solved once, on
    // the arms piece (else the driver); every other piece that skins the upper body takes the rig's arm shape (fingers,
    // twist bones) and then the solved bones as they came out - the chest's lean, the collarbones, the arms - so the
    // pieces stay one skeleton (as NpcBody::SyncPieces hands the soldiers' upper body round). Each call keeps its own
    // steadiness and elbow state.
    const float shoulderMax = glm::radians(std::clamp(cfg.ShoulderMaxAngle, 0.0f, 90.0f));
    const float leanMax = glm::radians(std::clamp(cfg.ReachLeanMax, 0.0f, 60.0f));
    auto solveArms = [&](const std::vector<std::shared_ptr<Model>>& models, const std::vector<entt::entity>& ents, const ArmTargets& t,
                         glm::vec3 (&anchor)[2], bool (&haveAnchor)[2], glm::vec3 (&elbowAim)[2], bool (&haveElbowAim)[2],
                         bool firstPersonView) {
        size_t source = models.size();
        for (size_t k = 0; k < models.size() && k < m_Pieces.size() && source == models.size(); ++k)
            if (models[k] && reg.valid(m_Pieces[k]) && IsArmsPiece(reg, m_Pieces[k]) && models[k]->NodeIndex(Bone(kSides[0].Upper)) >= 0)
                source = k;
        for (size_t k = 0; k < models.size() && k < m_Pieces.size() && source == models.size(); ++k)
            if (models[k] && m_Pieces[k] == m_Driver) source = k;
        if (source >= models.size() || source >= ents.size() || !reg.valid(ents[source]) || !t.Shape) return;
        Model& m = *models[source];
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) return;
        std::vector<int> parents(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        std::vector<glm::mat4> globals;
        const glm::mat4 pieceWorld = world.ComposeWorldTransform(ents[source]);
        const glm::mat4 toModel = glm::inverse(pieceWorld);
        const glm::quat toModelRot = IK::Rotation(toModel);

        // Arm Steadiness filters the walk's sway, so it reads the shoulders as the body's own clips have them: read after
        // the rig's arm shapes, a reload's shoulder move was held back like sway and eased home late.
        IK::ComputeGlobals(pose, parents, globals);
        glm::vec3 ownShoulder[2]{};
        for (int s = 0; s < 2; ++s)
            if (const int u = m.NodeIndex(Bone(kSides[s].Upper)); u >= 0)
                ownShoulder[s] = glm::vec3(pieceWorld * glm::vec4(IK::Position(globals[u]), 1.0f));

        // The rig's arm shapes first, so the elbows bend the way the animation has them; the solve then only fixes the hands.
        CopyArmShapeCached(m, *t.Shape, m_ArmsWeight, pose, parents, cfg.ClavicleFollow);
        FirstPersonBodyCurlFingers(m, pose, fingerCurl * m_ArmsWeight);
        IK::ComputeGlobals(pose, parents, globals);

        // Reach Lean: a hand still out of reach once its collarbone has turned all it may (the gun pitches with the whole
        // view, the chest only by Spine Aim) - the chest leans toward it, at the top of the spine, by at most Reach Lean Max.
        int chest = -1; // the top spine bone the piece has
        for (int b = 4; b >= 0 && chest < 0; --b) chest = m.NodeIndex(Bone(FPBody::kBoneSpine[b]));
        bool leaned = false;
        if (chest >= 0 && leanMax > 0.0f) {
            int upper[2], lower[2], hand[2], clav[2];
            for (int s = 0; s < 2; ++s) {
                upper[s] = m.NodeIndex(Bone(kSides[s].Upper));
                lower[s] = m.NodeIndex(Bone(kSides[s].Lower));
                hand[s] = m.NodeIndex(Bone(kSides[s].Hand));
                clav[s] = m.NodeIndex(Bone(kSides[s].Clavicle));
            }
            if (glm::quat q; FirstPersonBodyReachLean(globals, upper, lower, hand, clav, t.Have, t.Pos, toModel, chest, shoulderMax, leanMax,
                                                      m_ArmsWeight, q)) {
                IK::OffsetBone(pose, parents, globals, chest, glm::vec3(0.0f), q, IK::Position(globals[chest]));
                leaned = true;
                if (firstPersonView) BodyDebug::Info().ReachLean = glm::degrees(FirstPersonBodyAngleOf(q));
            }
        }
        for (int s = 0; s < 2; ++s) {
            if (!t.Have[s]) continue;
            const int upper = m.NodeIndex(Bone(kSides[s].Upper)), lower = m.NodeIndex(Bone(kSides[s].Lower)),
                      hand = m.NodeIndex(Bone(kSides[s].Hand));
            if (upper < 0 || lower < 0 || hand < 0) continue;
            const glm::vec3 target = glm::vec3(toModel * glm::vec4(t.Pos[s] + t.Palm[s] * gripInset * m_ArmsWeight, 1.0f));
            const glm::quat rot = glm::normalize(toModelRot * t.Rot[s]);
            const int clav = m.NodeIndex(Bone(kSides[s].Clavicle));
            if (firstPersonView) {
                // Before any shoulder move: how far the hand is, of the arm's length, and how far the body's shoulder is from the rig's.
                const glm::vec3 a = IK::Position(globals[upper]);
                const float armLen = glm::length(IK::Position(globals[lower]) - a) + glm::length(IK::Position(globals[hand]) - IK::Position(globals[lower]));
                if (armLen > 1e-5f) BodyDebug::Info().Reach[s] = glm::length(target - a) / armLen;
                if (t.HaveElbow[s]) BodyDebug::Info().ShoulderGap[s] = glm::length(glm::vec3(pieceWorld * glm::vec4(a, 1.0f)) - t.Shoulder[s]);
            }
            float turned = 0.0f; // radians the collarbone has turned so far this frame
            // Steady: the shoulder back to its slow average in the anchor frame, so the gait's sway doesn't reach the arm.
            // Bigger departures (a crouch, a stop clip) are followed, trailing by at most Arm Steady Max. The shoulders move
            // by turning each clavicle about its root (the collarbone's inner end) - never by sliding it, which stretched the
            // skin between the neck and the shoulder into a hunch.
            if (steady > 0.0f && clav >= 0) {
                const glm::vec3 shoulderWorld = ownShoulder[s];
                const glm::vec3 inFrame = glm::vec3(anchorInverse * glm::vec4(shoulderWorld, 1.0f));
                if (!haveAnchor[s]) { anchor[s] = inFrame; haveAnchor[s] = true; }
                anchor[s] += (inFrame - anchor[s]) * Follow(dt, cfg.ArmSteadyTime);
                glm::vec3 shift = glm::vec3(anchorFrame * glm::vec4(anchor[s], 1.0f)) - shoulderWorld;
                const float maxShift = std::max(cfg.ArmSteadyMax, 0.0f);
                if (const float len = glm::length(shift); len > maxShift) {
                    // Past the limit the anchor is dragged along, so it never trails by more.
                    shift *= len > 1e-6f ? maxShift / len : 0.0f;
                    anchor[s] = glm::vec3(anchorInverse * glm::vec4(shoulderWorld + shift, 1.0f));
                }
                const glm::vec3 want = IK::Position(globals[upper]) + glm::mat3(toModel) * shift * steady;
                turned += FirstPersonBodyAngleOf(FirstPersonBodyTurnClavicle(pose, parents, globals, clav, upper, want, shoulderMax));
                if (firstPersonView) BodyDebug::Info().ArmSteadyShift[s] = glm::length(shift);
            }
            // A hand beyond the arm's reach: the shoulder shrugs toward it instead of the arm stretching (the two
            // skeletons' shoulders can sit a little apart).
            if (clav >= 0) {
                const glm::vec3 a = IK::Position(globals[upper]);
                const float armLen = glm::length(IK::Position(globals[lower]) - a) + glm::length(IK::Position(globals[hand]) - IK::Position(globals[lower]));
                const glm::vec3 toTarget = target - a;
                const float dist = glm::length(toTarget);
                const float excess = std::min(dist - armLen * cfg.ShrugStart, cfg.ShrugMax);
                if (excess > 1e-4f && dist > 1e-5f) {
                    FirstPersonBodyTurnClavicle(pose, parents, globals, clav, upper, a + toTarget / dist * excess * m_ArmsWeight, shoulderMax - turned);
                    if (firstPersonView) BodyDebug::Info().ShrugShift[s] = glm::length(glm::mat3(pieceWorld) * (IK::Position(globals[upper]) - a));
                }
            }
            // Steady: the elbow into the rig's bend plane, as the solve's pole. The solve keeps the plane the copied arm shape
            // has, and that shape is the rig's relative to the body's chest - so the chest's sway, and the pitch Spine Aim
            // leaves out, swung the elbow about the shoulder-hand line. Measured in the view's frame, the rig's plane holds still.
            IK::TwoBoneHint hint;
            if (steady > 0.0f && t.HaveElbow[s]) {
                const glm::vec3 a = IK::Position(globals[upper]), b = IK::Position(globals[lower]), c = IK::Position(globals[hand]);
                const glm::vec3 line = glm::mix(c, target, std::clamp(m_ArmsWeight, 0.0f, 1.0f)) - a; // the solve's own target, from the shoulder
                const float lab = glm::length(b - a), lcb = glm::length(c - b);
                if (glm::dot(line, line) > 1e-8f && lab > 1e-6f && lcb > 1e-6f) {
                    const glm::vec3 axis = glm::normalize(line);
                    const glm::vec3 rigBend = glm::mat3(toModel) * (t.Elbow[s] - t.Shoulder[s]);
                    const glm::vec3 want = rigBend - axis * glm::dot(rigBend, axis);
                    // A near-straight rig arm (the melee's thrust and its recovery) has no bend plane to speak of: its elbow's
                    // few millimetres off the line swing anywhere frame to frame, while the body's arm (its shoulder nearer the
                    // hand) is already well bent. Turned onto that noise the elbow spun; left alone it looped out behind the
                    // back. So the elbow heads down and a little out - where an elbow hangs under a rifle - and onto the rig's
                    // plane as the rig's arm bends: how far its elbow stands off the line, as a share of its upper arm, none
                    // under 5%, all of it from 20%.
                    const float rigLen = std::max(glm::length(rigBend), 1e-4f);
                    const float trust = glm::smoothstep(0.05f, 0.2f, glm::length(want) / rigLen);
                    const glm::vec3 outward = s == 0 ? -kRight : kRight; // the upper arm's side (_l is the body's left)
                    glm::vec3 pole = glm::vec3(0.0f, -1.0f, 0.0f) + outward * 0.5f;
                    pole -= axis * glm::dot(pole, axis);
                    glm::vec3 aim = trust > 0.0f ? glm::normalize(want) * trust : glm::vec3(0.0f);
                    if (glm::dot(pole, pole) > 1e-8f) aim += glm::normalize(pole) * (1.0f - trust);
                    // Where the elbow heads is eased, and turns at most so fast: the rig's clips jump (the melee's 2-frame
                    // blend in), and in a tight fold - the melee's recovery, the body's shoulder nearer the gun than the
                    // rig's - the aim swings as fast as the shoulder-to-hand line does. Followed outright, the elbow whipped
                    // round. Held in the chest's view frame.
                    const glm::mat3 modelToAnchor = glm::mat3(anchorInverse) * glm::mat3(pieceWorld);
                    if (glm::dot(aim, aim) > 1e-8f) {
                        const glm::vec3 aimTo = glm::normalize(modelToAnchor * aim);
                        if (!haveElbowAim[s]) { elbowAim[s] = aimTo; haveElbowAim[s] = true; }
                        const float angle = std::acos(std::clamp(glm::dot(elbowAim[s], aimTo), -1.0f, 1.0f));
                        if (angle > 1e-5f) {
                            const float step = std::min(angle * Follow(dt, cfg.ElbowEase), glm::radians(cfg.ElbowMaxRate) * dt);
                            glm::vec3 turnAxis = glm::cross(elbowAim[s], aimTo);
                            if (glm::dot(turnAxis, turnAxis) < 1e-10f) turnAxis = glm::cross(elbowAim[s], glm::vec3(0.0f, 1.0f, 0.0f));
                            if (glm::dot(turnAxis, turnAxis) < 1e-10f) turnAxis = glm::vec3(1.0f, 0.0f, 0.0f);
                            elbowAim[s] = glm::normalize(glm::angleAxis(step, glm::normalize(turnAxis)) * elbowAim[s]);
                        }
                    }
                    if (haveElbowAim[s]) aim = glm::inverse(modelToAnchor) * elbowAim[s]; // the solve takes it off the line itself
                    // The body's own elbow must stand off the line for the turn to mean anything (2% .. 8% of the upper arm):
                    // the solved elbow's distance off the shoulder-hand line is the upper arm times the sine of the bend's
                    // angle at the shoulder (the triangle of the two bones and the reach the solve will make).
                    const float lat = std::clamp(glm::length(line), 1e-4f, (lab + lcb) * 0.9999f);
                    const float cosAtShoulder = std::clamp((lab * lab + lat * lat - lcb * lcb) / (2.0f * lab * lat), -1.0f, 1.0f);
                    const float bodyTrust = glm::smoothstep(0.02f, 0.08f, std::sqrt(std::max(0.0f, 1.0f - cosAtShoulder * cosAtShoulder)));
                    if (bodyTrust > 0.0f && glm::dot(aim, aim) > 1e-8f) {
                        hint.HintWeight = steady * bodyTrust;
                        hint.Pole = a + aim;
                        hint.HavePole = true;
                    }
                }
            }
            IK::SolveTwoBone(pose, parents, globals, upper, lower, hand, target, &rot, m_ArmsWeight, 0.0f,
                             hint.HintWeight > 0.0f ? &hint : nullptr);
        }
        // How far each of the hands ended up from its target (off the gun, when the arm can't reach).
        for (int s = 0; s < 2; ++s)
            if (const int hand = m.NodeIndex(Bone(kSides[s].Hand)); t.Have[s] && hand >= 0) {
                const float gap = glm::length(glm::vec3(pieceWorld * glm::vec4(IK::Position(globals[hand]), 1.0f)) - t.Pos[s]);
                if (firstPersonView) {
                    BodyDebug::Info().HandGap[s] = gap;
                } else {
                    m_TwinHandGap[s] = gap;
                    BodyDebug::Info().TwinHandGap[s] = gap;
                }
            }
        m.ApplyLocalPose(pose);

        // The rest of the pieces: the rig's arm shape and fingers (what the solve leaves alone), then the solved bones.
        std::vector<std::string> solved;
        if (leaned) solved.push_back(m.NodeName(chest));
        for (int s = 0; s < 2; ++s)
            for (const char* b : {kSides[s].Clavicle, kSides[s].Upper, kSides[s].Lower, kSides[s].Hand}) solved.push_back(Bone(b));
        float pieceGap = 0.0f;
        for (size_t k = 0; k < models.size() && k < ents.size() && k < m_Pieces.size(); ++k) {
            if (k == source || !models[k] || !reg.valid(ents[k])) continue;
            Model& o = *models[k];
            // A piece skinning nothing the solve moves (legs, feet) draws the same either way. The driver always takes it:
            // its shoulders are read for the camera.
            if (m_Pieces[k] != m_Driver && !SkinsUpperBody(o)) continue;
            IK::Pose other = o.AppliedLocalPose();
            if (other.empty() || (int)other.size() != o.NodeCount()) continue;
            parents.resize(other.size());
            for (int i = 0; i < (int)other.size(); ++i) parents[i] = o.NodeParent(i);
            CopyArmShapeCached(o, *t.Shape, m_ArmsWeight, other, parents, cfg.ClavicleFollow);
            FirstPersonBodyCurlFingers(o, other, fingerCurl * m_ArmsWeight);
            for (const std::string& name : solved)
                if (const int from = m.NodeIndex(name), to = o.NodeIndex(name); from >= 0 && to >= 0) other[to].R = pose[from].R;
            o.ApplyLocalPose(other);
            // Diagnostics: a piece whose skeleton doesn't rest as the source's would leave its hands off the gun.
            if (firstPersonView) {
                const glm::mat4 otherWorld = world.ComposeWorldTransform(ents[k]);
                for (int s = 0; s < 2; ++s)
                    if (const int hand = o.NodeIndex(Bone(kSides[s].Hand)); t.Have[s] && hand >= 0)
                        pieceGap = std::max(pieceGap, glm::length(glm::vec3(otherWorld * o.PoseNodeModelSpace(other, hand)[3]) - t.Pos[s]));
            }
        }
        if (firstPersonView) BodyDebug::Info().PieceHandGap = pieceGap;
    };
    {
    PROFILE_SCOPE("FPB arms: pieces");
    solveArms(m_Models, m_Pieces, firstPerson, m_ShoulderAnchor, m_HaveShoulderAnchor, m_ElbowAim, m_HaveElbowAim, true);
    ArmTwist(m_Models);
    }

    // The world twins (every view but the player's own, and the shadows): the same body and the same solve, onto the world
    // gun's hold - the weapon's third-person clips (arms3p: lower, off the centreline, into the right shoulder). The world gun
    // is the first-person one moved by where the 3P clips hold their gun against where the first-person clips hold theirs
    // (`thirdPersonGun`, the rig's model space), so the rig's recoil and sway still carry it; the 3P clips' hands, elbows and
    // fingers go on it as they hold it. Without 3P clips, the rig's own grip moves with the gun.
    m_WorldGunDelta = glm::mat4(1.0f);
    if (!m_Twins.empty()) {
        PROFILE_SCOPE("FPB arms: twins");
        m_WorldGunDelta = rigWorld * thirdPersonGun * glm::inverse(rigWorld);
        ArmTargets hold = firstPerson;
        glm::mat4 first(1.0f), third(1.0f);
        if (thirdPersonRig && !gunSocket.empty() && (int)thirdPersonRig->AppliedLocalPose().size() == thirdPersonRig->NodeCount() &&
            rig->NodeTransform(gunSocket, first) && thirdPersonRig->NodeTransform(gunSocket, third))
            readTargets(*thirdPersonRig, rigWorld * first * glm::inverse(third), hold); // the 3P clips with their gun on the rig's
        const glm::quat turn = IK::Rotation(m_WorldGunDelta);
        for (int s = 0; s < 2; ++s) {
            hold.Pos[s] = glm::vec3(m_WorldGunDelta * glm::vec4(hold.Pos[s], 1.0f));
            hold.Rot[s] = glm::normalize(turn * hold.Rot[s]);
            hold.Palm[s] = turn * hold.Palm[s];
            hold.Shoulder[s] = glm::vec3(m_WorldGunDelta * glm::vec4(hold.Shoulder[s], 1.0f));
            hold.Elbow[s] = glm::vec3(m_WorldGunDelta * glm::vec4(hold.Elbow[s], 1.0f));
        }
        solveArms(m_TwinModels, m_Twins, hold, m_WorldShoulderAnchor, m_WorldHaveShoulderAnchor, m_WorldElbowAim, m_WorldHaveElbowAim, false);
        ArmTwist(m_TwinModels);
        PlaceHeadAttachedTwins(world);
    }

    // Debug (the Inspector's readout, and the Scene overlay's Gizmos > Player body): the play camera's view
    // and each arm as the arms rig has it (yellow) against the body's (magenta).
    PROFILE_SCOPE("FPB arms debug");
    if (camera) {
        const glm::vec3 eye = camera->Position, f = camera->Front(), r = camera->Right(), u = camera->Up();
        BodyDebug::Info().NearPlane = camera->NearPlane;
        if (BodyDebug::Enabled()) {
            // Near plane (red), then the world's frustum (grey) and the view-model pass's (cyan) out to 40 cm.
            const float aspect = 16.0f / 9.0f;
            auto frustum = [&](float fovDeg, float dist, const glm::vec4& c, bool spokes) {
                const float h = std::tan(glm::radians(fovDeg) * 0.5f) * dist, w = h * aspect;
                const glm::vec3 m = eye + f * dist, p[4] = {m - r * w - u * h, m + r * w - u * h, m + r * w + u * h, m - r * w + u * h};
                for (int i = 0; i < 4; ++i) {
                    BodyDebug::Line(p[i], p[(i + 1) % 4], c);
                    if (spokes) BodyDebug::Line(eye, p[i], c);
                }
            };
            frustum(camera->Fov, camera->NearPlane, glm::vec4(1.0f, 0.1f, 0.1f, 1.0f), false);
            frustum(camera->Fov, 0.4f, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), true);
            if (viewModelFov > 0.0f) {
                frustum(viewModelFov, camera->NearPlane, glm::vec4(1.0f, 0.4f, 0.1f, 1.0f), false);
                frustum(viewModelFov, 0.4f, glm::vec4(0.1f, 0.9f, 1.0f, 1.0f), true);
            }
            BodyDebug::Cross(eye, 0.02f, glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
        }
        auto segDist = [&](const glm::vec3& a, const glm::vec3& b) {
            const glm::vec3 ab = b - a;
            const float t = glm::dot(ab, ab) > 1e-10f ? std::clamp(glm::dot(eye - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
            return glm::length(eye - (a + ab * t));
        };
        // One side's clavicle -> upper arm -> forearm -> hand, world, as `m` stands in `modelWorld`.
        auto chain = [&](const Model& m, const glm::mat4& modelWorld, int s, glm::vec3 (&p)[4]) {
            const IK::Pose& pose = m.AppliedLocalPose();
            if ((int)pose.size() != m.NodeCount()) return false;
            const char* names[4] = {kSides[s].Clavicle, kSides[s].Upper, kSides[s].Lower, kSides[s].Hand};
            for (int j = 0; j < 4; ++j) {
                const int node = m.NodeIndex(Bone(names[j]));
                if (node < 0) return false;
                p[j] = glm::vec3(modelWorld * m.PoseNodeModelSpace(pose, node)[3]);
            }
            return true;
        };
        size_t armsK = m_Models.size();
        for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k) {
            if (m_Models[k] && reg.valid(m_Pieces[k]) && IsArmsPiece(reg, m_Pieces[k])) { armsK = k; break; }
        }
        for (int s = 0; s < 2; ++s) {
            glm::vec3 p[4];
            if (chain(*rig, rigWorld, s, p)) {
                BodyDebug::Info().EyeToUpperArmRig[s] = segDist(p[1], p[2]);
                for (int j = 0; j < 3; ++j) BodyDebug::Line(p[j], p[j + 1], glm::vec4(1.0f, 0.9f, 0.1f, 1.0f)); // the rig's
            }
            if (armsK < m_Models.size() && chain(*m_Models[armsK], world.ComposeWorldTransform(m_Pieces[armsK]), s, p)) {
                BodyDebug::Info().EyeToUpperArmBody[s] = segDist(p[1], p[2]);
                for (int j = 0; j < 3; ++j) BodyDebug::Line(p[j], p[j + 1], glm::vec4(1.0f, 0.3f, 0.9f, 1.0f)); // the body's
            }
        }
        if (BodyDebug::Enabled() || BodyDebug::ProbeRequested()) CameraProbe(world, *camera, viewModelFov, dt);
        if (BodyDebug::ProbeRequested()) InsideProbe(world, *camera);
    }
}

float FirstPersonBody::EyeBodyPush(World& world, const Camera& camera, const glm::vec3& eye, const glm::vec3& flat, float& lookLimitDeg) {
    lookLimitDeg = -89.0f;
    auto& reg = world.Registry;
    if (!m_EyeClearBuilt) {
        // The body's own pieces (not the arms, clothing or a Hidden Part): the vertices mostly on the upper spine and the
        // neck that the camera pass draws (not on a Camera Hidden Bone or the head) - every one, thinned to ~600; and the neck
        // opening's rim: the mesh's open border (its vertices welded by bind position - UV seams split them) within
        // kNeckRimReach of the bind neck_01 -> head segment.
        m_EyeClearBuilt = true;
        m_EyeClearPoints.clear();
        std::vector<EyeClearPoint> rim;
        for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k) {
            const entt::entity e = m_Pieces[k];
            if (!m_Models[k] || !reg.valid(e) || IsArmsPiece(reg, e) || reg.all_of<ViewModelTag>(e)) continue;
            const auto* tag = reg.try_get<PlayerBodyTag>(e);
            if (!tag || tag->CameraHidden || tag->Clothing) continue;
            const Model& m = *m_Models[k];
            int want[4];
            const char* names[4] = {FPBody::kBoneSpine[2], FPBody::kBoneSpine[3], FPBody::kBoneSpine[4], "neck_01"};
            for (int i = 0; i < 4; ++i) want[i] = m.BoneId(Bone(names[i]));
            const int neckNode = m.NodeIndex(Bone("neck_01")), headNode = m.NodeIndex(Bone("head"));
            if (neckNode >= 0 && headNode >= 0) {
                const glm::vec3 n0(m.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, neckNode)[3]);
                const glm::vec3 n1(m.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, headNode)[3]);
                for (int mi = 0; mi < m.MeshCount(); ++mi) {
                    const auto& bind = m.Mesh(mi).LocalPositions();
                    const auto& idx = m.Mesh(mi).LocalIndices();
                    if (bind.size() != m.MeshSkinVertices(mi).size()) continue;
                    std::map<std::tuple<int, int, int>, std::uint32_t> weld;
                    std::vector<std::uint32_t> id(bind.size());
                    for (std::uint32_t vi = 0; vi < (std::uint32_t)bind.size(); ++vi) {
                        const glm::ivec3 q(glm::round(bind[vi] * 1e4f));
                        id[vi] = weld.emplace(std::make_tuple(q.x, q.y, q.z), vi).first->second;
                    }
                    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
                    for (size_t t = 0; t + 2 < idx.size(); t += 3)
                        for (int k3 = 0; k3 < 3; ++k3) {
                            std::uint32_t a = id[idx[t + k3]], b = id[idx[t + (k3 + 1) % 3]];
                            if (a > b) std::swap(a, b);
                            ++edges[{a, b}];
                        }
                    std::vector<char> onRim(bind.size(), 0);
                    for (const auto& [edge, count] : edges)
                        if (count == 1) onRim[edge.first] = onRim[edge.second] = 1;
                    const glm::vec3 axis = n1 - n0;
                    for (std::uint32_t vi = 0; vi < (std::uint32_t)bind.size(); ++vi) {
                        if (!onRim[vi]) continue;
                        const float t = std::clamp(glm::dot(bind[vi] - n0, axis) / std::max(glm::dot(axis, axis), 1e-9f), 0.0f, 1.0f);
                        if (glm::length(bind[vi] - (n0 + axis * t)) < kNeckRimReach) rim.push_back({k, mi, vi, true});
                    }
                }
            }
            for (int mi = 0; mi < m.MeshCount(); ++mi) {
                const auto& verts = m.MeshSkinVertices(mi);
                for (std::uint32_t vi = 0; vi < (std::uint32_t)verts.size(); ++vi) {
                    const ModelMesh::SkinVertex& v = verts[vi];
                    int top = -1;
                    float best = 0.0f, hidden = 0.0f;
                    for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) {
                        const int b = v.BoneIDs[i];
                        if (b < 0) continue;
                        if (v.Weights[i] > best) { best = v.Weights[i]; top = b; }
                        for (int h = 0; h < 8 && tag->CameraHideBones[h] >= 0; ++h)
                            if (tag->CameraHideBones[h] == b) hidden += v.Weights[i];
                    }
                    if (top < 0 || hidden > 0.5f || std::find(std::begin(want), std::end(want), top) == std::end(want)) continue;
                    m_EyeClearPoints.push_back({k, mi, vi, false});
                }
            }
        }
        if (m_EyeClearPoints.size() > 600) {
            std::vector<EyeClearPoint> thin;
            const size_t stride = (m_EyeClearPoints.size() + 599) / 600;
            for (size_t i = 0; i < m_EyeClearPoints.size(); i += stride) thin.push_back(m_EyeClearPoints[i]);
            m_EyeClearPoints.swap(thin);
        }
        m_EyeClearPoints.insert(m_EyeClearPoints.end(), rim.begin(), rim.end());
        if (const Model* drv = DriverModel()) {
            m_NeckNode = drv->NodeIndex(Bone("neck_01"));
            m_HeadRimNode = drv->NodeIndex(Bone("head"));
        }
        BodyDebug::Info().NeckRimPoints = (int)rim.size();
    }
    if (m_EyeClearPoints.empty()) return 0.0f;
    // The points as drawn now (skinned on the CPU, world space).
    thread_local std::vector<glm::vec3> points, rimPoints;
    points.clear();
    rimPoints.clear();
    size_t lastPiece = (size_t)-1;
    glm::mat4 pieceWorld(1.0f);
    for (const EyeClearPoint& c : m_EyeClearPoints) {
        if (c.Piece >= m_Models.size() || !m_Models[c.Piece] || !reg.valid(m_Pieces[c.Piece])) continue;
        if (c.Piece != lastPiece) { pieceWorld = world.ComposeWorldTransform(m_Pieces[c.Piece]); lastPiece = c.Piece; }
        const Model& m = *m_Models[c.Piece];
        const auto& verts = m.MeshSkinVertices(c.Mesh);
        if (c.Vertex >= verts.size()) continue;
        const ModelMesh::SkinVertex& v = verts[c.Vertex];
        glm::mat4 skin(0.0f);
        float total = 0.0f;
        for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
            if (v.BoneIDs[i] >= 0) { skin += m.FinalBoneMatrix(v.BoneIDs[i]) * v.Weights[i]; total += v.Weights[i]; }
        if (total > 1e-4f) (c.Rim ? rimPoints : points).push_back(glm::vec3(pieceWorld * skin * glm::vec4(v.Position, 1.0f)));
    }
    // The opening's outward axis (neck_01 -> head, posed) and centre: the eye sees in only from its outer side.
    glm::vec3 rimAxis(0.0f, 1.0f, 0.0f), rimCentre(0.0f);
    bool rimOpen = false;
    if (!rimPoints.empty()) {
        for (const glm::vec3& p : rimPoints) rimCentre += p;
        rimCentre /= (float)rimPoints.size();
        glm::mat4 a(1.0f), b(1.0f);
        const Model* drv = DriverModel();
        if (drv && m_NeckNode >= 0 && m_HeadRimNode >= 0 && drv->NodeTransform(drv->NodeName(m_NeckNode), a) &&
            drv->NodeTransform(drv->NodeName(m_HeadRimNode), b) && reg.valid(m_Body)) {
            const glm::vec3 axis = glm::mat3(world.ComposeWorldTransform(m_Body)) * (glm::vec3(b[3]) - glm::vec3(a[3]));
            if (glm::length(axis) > 1e-5f) rimAxis = glm::normalize(axis);
        }
        rimOpen = true;
    }
    // A point is in the way when it is nearer than the near plane plus the clearance along the view and inside the view's
    // frustum at that depth (or at the near plane, if nearer): the near plane slices there, and what's behind it shows.
    const glm::vec3 f = camera.Front(), u = camera.Up(), r = camera.Right();
    const float nearPlane = std::max(camera.NearPlane, 0.01f), need = nearPlane + kEyeBodyClearance;
    const float tanHalf = std::tan(glm::radians(camera.Fov) * 0.5f), kAspect = 16.0f / 9.0f;
    const auto blockedAs = [&](const glm::vec3& at, const glm::vec3& vf, const glm::vec3& vu, const glm::vec3& vr) {
        if (rimOpen && glm::dot(at - rimCentre, rimAxis) > 0.0f)
            for (const glm::vec3& p : rimPoints) {
                const glm::vec3 d = p - at;
                const float z = glm::dot(d, vf);
                // (The view widened by kNeckRimMargin: a rim edge can cross its border between two rim vertices outside it.)
                const float w = z * tanHalf * (1.0f + kNeckRimMargin);
                if (z > 0.0f && std::abs(glm::dot(d, vu)) <= w && std::abs(glm::dot(d, vr)) <= w * kAspect) return true;
            }
        for (const glm::vec3& p : points) {
            const glm::vec3 d = p - at;
            const float z = glm::dot(d, vf);
            if (z <= 0.0f || z >= need) continue;
            const float zz = std::max(z, nearPlane);
            if (std::abs(glm::dot(d, vu)) <= zz * tanHalf && std::abs(glm::dot(d, vr)) <= zz * tanHalf * kAspect) return true;
        }
        return false;
    };
    const auto blocked = [&](const glm::vec3& at) { return blockedAs(at, f, u, r); };
    float push = kEyeBodyPushMax;
    for (float s = 0.0f; s <= kEyeBodyPushMax; s += 0.005f)
        if (!blocked(eye + flat * s)) { push = s; break; }
    // The look-down limit: the steepest pitch whose view some push (up to kEyeBodyPushMax, 1 cm steps) clears - the push
    // follows the pitch, so judged from the eye as pushed now it held the view at what an unpushed eye clears (jogging,
    // -66 deg). Bisected (steeper is never clearer), to ~0.5 deg.
    const auto clearAt = [&](float pitch) {
        Camera c = camera;
        c.Pitch = pitch;
        const glm::vec3 cf = c.Front(), cu = c.Up(), cr = c.Right();
        for (float s = 0.0f; s <= kEyeBodyPushMax + 1e-4f; s += 0.01f)
            if (!blockedAs(eye + flat * s, cf, cu, cr)) return true;
        return false;
    };
    if (clearAt(-89.0f)) {
        lookLimitDeg = -89.0f;
    } else {
        float lo = -89.0f, hi = kLookLimitHighest; // lo blocked, hi clear (or the highest the limit goes)
        for (int k = 0; k < 7; ++k) {
            const float mid = 0.5f * (lo + hi);
            (clearAt(mid) ? hi : lo) = mid;
        }
        lookLimitDeg = hi;
    }
    return push;
}

void FirstPersonBody::InsideProbe(World& world, const Camera& camera) {
    auto& info = BodyDebug::Info();
    info.ProbeRays = -1;
    if (++m_InsideProbeTick % 3) return; // every third frame (the grid costs ~0.2 s: test runs only)
    auto& reg = world.Registry;
    const glm::vec3 eye = camera.Position, f = camera.Front(), r = camera.Right(), u = camera.Up();
    glm::vec3 flatRight(r.x, 0.0f, r.z);
    flatRight = glm::dot(flatRight, flatRight) > 1e-8f ? glm::normalize(flatRight) : glm::vec3(1.0f, 0.0f, 0.0f);
    const float nearPlane = std::max(camera.NearPlane, 0.01f), tanHalf = std::tan(glm::radians(camera.Fov) * 0.5f), kAspect = 16.0f / 9.0f;
    auto bit = [](const std::uint32_t (&bits)[16], int b) { return b >= 0 && b < 16 * 32 && ((bits[b >> 5] >> (b & 31)) & 1u) != 0u; };
    struct Tri { glm::vec3 A, B, C; int Piece, Bone; };
    thread_local std::vector<Tri> tris;
    thread_local std::vector<glm::vec3> pos;
    thread_local std::vector<char> drawn;
    thread_local std::vector<int> top;
    tris.clear();
    for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k) {
        const entt::entity e = m_Pieces[k];
        if (!m_Models[k] || !reg.valid(e) || reg.all_of<InactiveTag>(e) || reg.all_of<ViewModelTag>(e)) continue;
        const auto* rc = reg.try_get<RenderableComponent>(e);
        const auto* tag = reg.try_get<PlayerBodyTag>(e);
        if (!rc || !tag || tag->CameraHidden || rc->CastShadows == RenderableComponent::ShadowCasting::ShadowsOnly) continue;
        const Model& m = *m_Models[k];
        const glm::mat4 pieceWorld = world.ComposeWorldTransform(e);
        const std::vector<std::uint8_t>* collar = tag->CollarBits.get();
        size_t vertexIndex = 0;
        for (int mi = 0; mi < m.MeshCount(); ++mi) {
            const auto& verts = m.MeshSkinVertices(mi);
            const auto& idx = m.Mesh(mi).LocalIndices();
            pos.resize(verts.size());
            drawn.assign(verts.size(), 0);
            top.assign(verts.size(), -1);
            for (size_t vi = 0; vi < verts.size(); ++vi, ++vertexIndex) {
                const ModelMesh::SkinVertex& v = verts[vi];
                const bool inCollar = collar && vertexIndex < collar->size() && (*collar)[vertexIndex];
                glm::mat4 skin(0.0f);
                float total = 0.0f, hideBones = 0.0f, head = 0.0f, sleeve = 0.0f, best = 0.0f;
                for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) {
                    const int b = v.BoneIDs[i];
                    if (b < 0) continue;
                    const float w = v.Weights[i];
                    skin += m.FinalBoneMatrix(b) * w;
                    total += w;
                    for (int h = 0; h < 8 && tag->CameraHideBones[h] >= 0; ++h)
                        if (tag->CameraHideBones[h] == b) hideBones += w;
                    if (bit(tag->HeadBones, b)) head += w;
                    if (bit(tag->SleeveBones, b)) sleeve += w;
                    if (w > best) { best = w; top[vi] = b; }
                }
                if (total <= 1e-4f) continue;
                pos[vi] = glm::vec3(pieceWorld * skin * glm::vec4(v.Position, 1.0f));
                if (glm::length(glm::vec3(skin[0])) < 0.5f * total) continue;  // a Camera Hidden Bone's collapse
                if (tag->SleevesInViewModel && sleeve > 0.5f) continue;         // (drawn in the view-model pass)
                if (inCollar || hideBones > 0.5f || (tag->HasHeadBones && head > 0.5f) || (tag->SleevesHidden && sleeve > 0.5f)) continue;
                if (FirstPersonBodyNearHidden(pos[vi], eye, flatRight, tag->NearHide, tag->NearHideWidth)) continue;
                drawn[vi] = 1;
            }
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                const unsigned a = idx[t], b = idx[t + 1], c = idx[t + 2];
                if (a >= verts.size() || b >= verts.size() || c >= verts.size() || !drawn[a] || !drawn[b] || !drawn[c]) continue;
                // Only what can be in the view (its centroid within the frustum widened by 20 %, within 3 m).
                const glm::vec3 d = (pos[a] + pos[b] + pos[c]) / 3.0f - eye;
                const float z = glm::dot(d, f);
                if (z < -0.1f || z > 3.0f) continue;
                const float zz = std::max(z, 0.1f);
                if (std::abs(glm::dot(d, u)) > 1.2f * zz * tanHalf + 0.1f || std::abs(glm::dot(d, r)) > 1.2f * zz * tanHalf * kAspect + 0.1f) continue;
                tris.push_back({pos[a], pos[b], pos[c], (int)k, top[a]});
            }
        }
    }
    constexpr int kW = 24, kH = 14;
    // Each ray tests only the triangles whose screen footprint holds its grid point: a triangle wholly in front of the eye
    // projects to the triangle of its projected corners, so a ray through screen point (sx, sy) can only hit one whose
    // projected box holds it; one reaching behind the eye is tested by every ray. Each cell keeps the triangles' order, so the
    // nearest hit (and a tie) is the brute force's. (Brute force - every ray against every triangle - was two thirds of an
    // mmplayer run's CPU: ~0.2 s a probe on the look-down legs.)
    thread_local std::vector<std::vector<int>> cells;
    cells.resize((size_t)kW * kH);
    for (auto& c : cells) c.clear();
    {
        const float toX = 1.0f / (tanHalf * kAspect), toY = 1.0f / tanHalf;
        for (int ti = 0; ti < (int)tris.size(); ++ti) {
            const Tri& t = tris[(size_t)ti];
            float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
            bool behind = false;
            for (const glm::vec3& v : {t.A, t.B, t.C}) {
                const glm::vec3 d = v - eye;
                const float z = glm::dot(d, f);
                if (z <= 1e-4f) { behind = true; break; }
                const float px = glm::dot(d, r) / z * toX, py = glm::dot(d, u) / z * toY; // -1..1 across the grid
                lo[0] = std::min(lo[0], px); hi[0] = std::max(hi[0], px);
                lo[1] = std::min(lo[1], py); hi[1] = std::max(hi[1], py);
            }
            // Grid point x sits at screen ((x + 0.5) / kW * 2 - 1): the points inside [lo, hi] (widened a hair, so rounding never
            // drops a triangle whose edge runs through a grid point - a wider box only adds rays that miss).
            for (int a = 0; a < 2; ++a) { lo[a] -= 1e-3f; hi[a] += 1e-3f; }
            const int x0 = behind ? 0 : std::max(0, (int)std::ceil((lo[0] + 1.0f) * 0.5f * kW - 0.5f));
            const int x1 = behind ? kW - 1 : std::min(kW - 1, (int)std::floor((hi[0] + 1.0f) * 0.5f * kW - 0.5f));
            const int y0 = behind ? 0 : std::max(0, (int)std::ceil((lo[1] + 1.0f) * 0.5f * kH - 0.5f));
            const int y1 = behind ? kH - 1 : std::min(kH - 1, (int)std::floor((hi[1] + 1.0f) * 0.5f * kH - 0.5f));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) cells[(size_t)y * kW + x].push_back(ti);
        }
    }
    int hits = 0, inside = 0;
    float sumX = 0.0f, sumY = 0.0f; // where on the screen the inside is seen (-1..1, + right / up)
    std::map<std::pair<int, int>, int> what;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const float sx = ((x + 0.5f) / kW * 2.0f - 1.0f) * tanHalf * kAspect, sy = ((y + 0.5f) / kH * 2.0f - 1.0f) * tanHalf;
            const glm::vec3 dir = glm::normalize(f + r * sx + u * sy);
            const float tMin = nearPlane / std::max(glm::dot(dir, f), 1e-3f);
            float bestT = 1e9f;
            const Tri* bestTri = nullptr;
            bool back = false;
            for (const int ti : cells[(size_t)y * kW + x]) {
                const Tri& t = tris[(size_t)ti];
                // Moller-Trumbore, both sides.
                const glm::vec3 e1 = t.B - t.A, e2 = t.C - t.A, p = glm::cross(dir, e2);
                const float det = glm::dot(e1, p);
                if (std::abs(det) < 1e-10f) continue;
                const float inv = 1.0f / det;
                const glm::vec3 s = eye - t.A;
                const float bu = glm::dot(s, p) * inv;
                if (bu < 0.0f || bu > 1.0f) continue;
                const glm::vec3 q = glm::cross(s, e1);
                const float bv = glm::dot(dir, q) * inv;
                if (bv < 0.0f || bu + bv > 1.0f) continue;
                const float tt = glm::dot(e2, q) * inv;
                if (tt < tMin || tt >= bestT) continue;
                bestT = tt;
                bestTri = &t;
                back = glm::dot(glm::cross(e1, e2), dir) > 0.0f; // counter-clockwise front faces: a back face faces along the ray
            }
            if (!bestTri) continue;
            ++hits;
            if (back) {
                ++inside;
                ++what[{bestTri->Piece, bestTri->Bone}];
                sumX += sx / (tanHalf * kAspect);
                sumY += sy / tanHalf;
            }
        }
    info.ProbeRays = kW * kH;
    info.ProbeRayHits = hits;
    info.ProbeInsideRays = inside;
    info.ProbeInsideWhat.clear();
    info.ProbeInsideAt = inside > 0 ? glm::vec2(sumX, sumY) / (float)inside : glm::vec2(0.0f);
    int most = 0;
    for (const auto& [key, n] : what)
        if (n > most) {
            most = n;
            const Model& m = *m_Models[(size_t)key.first];
            std::string bone = "?";
            for (int i = 0; i < m.NodeCount(); ++i)
                if (m.BoneId(m.NodeName(i)) == key.second) { bone = m.NodeName(i); break; }
            const entt::entity pe = m_Pieces[(size_t)key.first];
            info.ProbeInsideWhat = (reg.all_of<NameComponent>(pe) ? reg.get<NameComponent>(pe).Name : std::string("piece")) + "/" + bone;
        }
}

// Which of the body's pieces the camera actually sees nearest the eye, and the bone that moves it there. Every
// piece is skinned on the CPU with this frame's pose and put through the renderer's own hides (Camera Hidden
// Bones, Near Hide, clothing's head, sleeve and collar masks); what's left
// inside the view's frustum (the world's FOV, or the view-model pass's for the arms) is measured. Overlay
// only: the Inspector's readout, and a log line whenever something is within 12 cm of the eye.
void FirstPersonBody::CameraProbe(World& world, const Camera& camera, float viewModelFov, float dt) {
    auto& reg = world.Registry;
    const glm::vec3 eye = camera.Position, f = camera.Front(), r = camera.Right(), u = camera.Up();
    constexpr float kAspect = 16.0f / 9.0f, kClose = 0.12f;
    glm::vec3 flatRight(r.x, 0.0f, r.z);
    flatRight = glm::dot(flatRight, flatRight) > 1e-8f ? glm::normalize(flatRight) : glm::vec3(1.0f, 0.0f, 0.0f);
    auto bit = [](const std::uint32_t (&bits)[16], int b) { return b >= 0 && b < 16 * 32 && ((bits[b >> 5] >> (b & 31)) & 1u) != 0u; };

    float bestDepth = 1e9f, bestDist = 0.0f;
    int bestBone = -1, close = 0;
    size_t bestPiece = 0;
    bool bestViewModel = false;
    int cutNear = 0, cutCollar = 0, cutBones = 0, cutHead = 0;
    const auto inView = [&](const glm::vec3& d, bool viewModel) {
        const float z = glm::dot(d, f);
        if (z <= 0.01f) return false;
        const float tanHalf = std::tan(glm::radians(viewModel ? viewModelFov : camera.Fov) * 0.5f);
        return std::abs(glm::dot(d, u)) <= z * tanHalf && std::abs(glm::dot(d, r)) <= z * tanHalf * kAspect;
    };
    for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k) {
        const entt::entity e = m_Pieces[k];
        if (!m_Models[k] || !reg.valid(e) || reg.all_of<InactiveTag>(e)) continue;
        const auto* rc = reg.try_get<RenderableComponent>(e);
        const auto* tag = reg.try_get<PlayerBodyTag>(e);
        if (!rc || !tag || rc->CastShadows == RenderableComponent::ShadowCasting::ShadowsOnly) continue;
        if (tag->CameraHidden) { // a Hidden Part (the head): what of it is in the view is a cut you see into
            const Model& hm = *m_Models[k];
            const glm::mat4 hw = world.ComposeWorldTransform(e);
            for (int mi = 0; mi < hm.MeshCount(); ++mi)
                for (const ModelMesh::SkinVertex& v : hm.MeshSkinVertices(mi)) {
                    glm::mat4 skin(0.0f);
                    float total = 0.0f;
                    for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
                        if (v.BoneIDs[i] >= 0) { skin += hm.FinalBoneMatrix(v.BoneIDs[i]) * v.Weights[i]; total += v.Weights[i]; }
                    if (total > 1e-4f && inView(glm::vec3(hw * skin * glm::vec4(v.Position, 1.0f)) - eye, false)) ++cutHead;
                }
            continue;
        }
        const Model& m = *m_Models[k];
        const glm::mat4 pieceWorld = world.ComposeWorldTransform(e);
        const bool pieceViewModel = reg.all_of<ViewModelTag>(e) && viewModelFov > 0.0f;
        const std::vector<std::uint8_t>* collar = tag->CollarBits.get();
        size_t vertexIndex = 0; // across the sub-meshes, as the collar's bits count them
        for (int mi = 0; mi < m.MeshCount(); ++mi) {
            for (const ModelMesh::SkinVertex& v : m.MeshSkinVertices(mi)) {
                const bool inCollar = collar && vertexIndex < collar->size() && (*collar)[vertexIndex];
                ++vertexIndex;
                glm::mat4 skin(0.0f);
                float total = 0.0f, hideBones = 0.0f, head = 0.0f, sleeve = 0.0f, top = 0.0f;
                int bone = -1;
                for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) {
                    const int b = v.BoneIDs[i];
                    if (b < 0) continue;
                    const float w = v.Weights[i];
                    skin += m.FinalBoneMatrix(b) * w;
                    total += w;
                    for (int h = 0; h < 8 && tag->CameraHideBones[h] >= 0; ++h)
                        if (tag->CameraHideBones[h] == b) hideBones += w;
                    if (bit(tag->HeadBones, b)) head += w;
                    if (bit(tag->SleeveBones, b)) sleeve += w;
                    if (w > top) { top = w; bone = b; }
                }
                if (total <= 1e-4f) continue;
                const glm::vec3 p = glm::vec3(pieceWorld * skin * glm::vec4(v.Position, 1.0f));
                const glm::vec3 d = p - eye;
                // Skin mostly on a Hidden Bone is collapsed to a point (Model::SetHiddenNodes): nothing is drawn there.
                if (glm::length(glm::vec3(skin[0])) < 0.5f * total) {
                    if (!pieceViewModel && inView(d, false)) ++cutBones;
                    continue;
                }
                // Which pass draws it, and so which hides and which FOV apply (SceneRenderer).
                const bool sleeveInViewModel = tag->SleevesInViewModel && sleeve > 0.5f;
                const bool viewModel = pieceViewModel || sleeveInViewModel;
                if (!viewModel) {
                    if (inCollar || hideBones > 0.5f) { if (inView(d, false)) ++cutCollar; continue; }
                    if (tag->HasHeadBones && head > 0.5f) { if (inView(d, false)) ++cutHead; continue; }
                    if (tag->SleevesHidden && sleeve > 0.5f) continue;
                }
                if (pieceViewModel) {
                    // The arms piece: drawn whole in the view-model pass.
                } else if (sleeveInViewModel) {
                    if (glm::dot(d, d) < tag->NearHide * tag->NearHide) { if (inView(d, true)) ++cutNear; continue; }
                } else if (FirstPersonBodyNearHidden(p, eye, flatRight, tag->NearHide, tag->NearHideWidth)) {
                    if (inView(d, false)) ++cutNear;
                    continue;
                }
                const float z = glm::dot(d, f);
                if (z <= 0.0f) continue;
                const float tanHalf = std::tan(glm::radians(viewModel ? viewModelFov : camera.Fov) * 0.5f);
                if (std::abs(glm::dot(d, u)) > z * tanHalf || std::abs(glm::dot(d, r)) > z * tanHalf * kAspect) continue;
                if (z < kClose) ++close;
                if (z < bestDepth) {
                    bestDepth = z; bestDist = glm::length(d); bestBone = bone; bestPiece = k; bestViewModel = viewModel;
                }
            }
        }
    }

    auto& info = BodyDebug::Info();
    info.ProbeCutNear = cutNear;
    info.ProbeCutCollar = cutCollar;
    info.ProbeCutBones = cutBones;
    info.ProbeCutHead = cutHead;
    info.ProbeDepth = bestDepth < 1e8f ? bestDepth : 0.0f;
    info.ProbePiece.clear();
    info.ProbeBone.clear();
    if (bestDepth >= 1e8f) return;
    const Model& m = *m_Models[bestPiece];
    for (int i = 0; i < m.NodeCount(); ++i)
        if (m.BoneId(m.NodeName(i)) == bestBone) { info.ProbeBone = m.NodeName(i); break; }
    if (reg.all_of<NameComponent>(m_Pieces[bestPiece])) info.ProbePiece = reg.get<NameComponent>(m_Pieces[bestPiece]).Name;
    info.ProbeViewModel = bestViewModel;
    // A line while something is close, at most five a second (and at once for anything nearer than the last).
    m_ProbeLogTimer -= dt;
    if (bestDepth < kClose && (m_ProbeLogTimer <= 0.0f || bestDepth < m_ProbeLogged - 0.01f)) {
        char line[320];
        std::snprintf(line, sizeof(line),
                      "Camera probe: '%s' in view %.1f cm ahead of the eye (%.1f cm away), mostly moved by '%s', %d vertices within %.0f cm - %s pass.",
                      info.ProbePiece.c_str(), bestDepth * 100.0f, bestDist * 100.0f, info.ProbeBone.c_str(), close, kClose * 100.0f,
                      bestViewModel ? "view-model" : "world");
        Log::Info(line);
        m_ProbeLogTimer = 0.2f;
        m_ProbeLogged = bestDepth;
    }
    if (m_ProbeLogTimer <= 0.0f) m_ProbeLogged = 1e9f;
}
