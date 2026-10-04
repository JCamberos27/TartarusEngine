#include "IK.h"

#include "Components.h"
#include "Model.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <limits>

namespace IK {

namespace {

float SafeAngle(const glm::vec3& a, const glm::vec3& b) {
    const float la = glm::length(a), lb = glm::length(b);
    if (la < 1e-8f || lb < 1e-8f) return 0.0f;
    return std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0f, 1.0f));
}

glm::quat ParentRotation(const std::vector<int>& parents, const std::vector<glm::mat4>& globals, int i) {
    const int p = parents[i];
    return p >= 0 ? Rotation(globals[p]) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

// Any unit vector perpendicular to v.
glm::vec3 Perpendicular(const glm::vec3& v) {
    const glm::vec3 other = std::fabs(v.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    return glm::normalize(glm::cross(v, other));
}

bool ValidNode(const Pose& pose, int i) { return i >= 0 && i < (int)pose.size(); }

// The shortest rotation taking unit vector `from` onto unit vector `to`. Not glm::rotation: that
// returns identity whenever the two are within ~5e-4 rad (its FLT_EPSILON cut on the cosine),
// so a reach that only needs a tiny swing - a hand holding the gun through the idle - lands up
// to ~0.3 mm short on some frames and not others, and the hand visibly flickers. The half-angle
// form below stays exact all the way down to parallel.
glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to) {
    const glm::dvec3 f(from), t(to);
    const double d = glm::dot(f, t);
    if (d < -1.0 + 1e-12) { // opposite: half a turn about any perpendicular
        const glm::vec3 axis = Perpendicular(from);
        return glm::quat(0.0f, axis.x, axis.y, axis.z);
    }
    const glm::dvec3 c = glm::cross(f, t);
    const glm::dquat q = glm::normalize(glm::dquat(1.0 + d, c.x, c.y, c.z));
    return glm::quat((float)q.w, (float)q.x, (float)q.y, (float)q.z);
}

} // namespace

void ComputeGlobals(const Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals) {
    globals.resize(pose.size());
    for (size_t i = 0; i < pose.size(); ++i) {
        const glm::mat4 local = pose[i].ToMatrix();
        globals[i] = parents[i] >= 0 ? AffineMul(globals[parents[i]], local) : local;
    }
}

void RefreshGlobals(const Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int from) {
    if (!ValidNode(pose, from)) return;
    // Parents come first, so one forward pass from `from` reaches every descendant.
    thread_local std::vector<unsigned char> dirty;
    dirty.assign(pose.size(), 0);
    dirty[from] = 1;
    for (size_t i = (size_t)from; i < pose.size(); ++i) {
        const int p = parents[i];
        if (!dirty[i] && !(p >= 0 && dirty[p])) continue;
        dirty[i] = 1;
        const glm::mat4 local = pose[i].ToMatrix();
        globals[i] = p >= 0 ? AffineMul(globals[p], local) : local;
    }
}

glm::vec3 Position(const glm::mat4& global) { return glm::vec3(global[3]); }

glm::quat Rotation(const glm::mat4& global) {
    glm::mat3 basis(global);
    for (int c = 0; c < 3; ++c) {
        const float len = glm::length(basis[c]);
        if (len > 1e-12f) basis[c] /= len;
    }
    return glm::normalize(glm::quat_cast(basis));
}

namespace {
// SetGlobal's pose write, without refreshing any global.
void SetLocalFromGlobal(Pose& pose, const std::vector<int>& parents, const std::vector<glm::mat4>& globals, int i,
                        const glm::vec3& pos, const glm::quat& rot) {
    const int p = parents[i];
    if (p >= 0) {
        pose[i].T = glm::vec3(glm::inverse(globals[p]) * glm::vec4(pos, 1.0f));
        pose[i].R = glm::normalize(glm::inverse(Rotation(globals[p])) * rot);
    } else {
        pose[i].T = pos;
        pose[i].R = glm::normalize(rot);
    }
}
} // namespace

void SetGlobal(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int i,
               const glm::vec3& pos, const glm::quat& rot) {
    if (!ValidNode(pose, i)) return;
    SetLocalFromGlobal(pose, parents, globals, i, pos, rot);
    RefreshGlobals(pose, parents, globals, i);
}

void SetGlobals(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, const std::vector<GlobalTarget>& targets) {
    if (targets.empty() || !ValidNode(pose, targets.front().Node)) return;
    thread_local std::vector<unsigned char> dirty;
    dirty.assign(pose.size(), 0);
    size_t next = 0;
    for (size_t i = (size_t)targets.front().Node; i < pose.size(); ++i) {
        const int p = parents[i];
        const bool target = next < targets.size() && targets[next].Node == (int)i;
        if (!target && !(p >= 0 && dirty[p])) continue;
        if (target) {
            SetLocalFromGlobal(pose, parents, globals, (int)i, targets[next].Pos, targets[next].Rot);
            ++next;
        }
        dirty[i] = 1;
        const glm::mat4 local = pose[i].ToMatrix();
        globals[i] = p >= 0 ? AffineMul(globals[p], local) : local;
    }
}

void OffsetBone(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int i,
                const glm::vec3& deltaPos, const glm::quat& deltaRot, const glm::vec3& pivot) {
    if (!ValidNode(pose, i)) return;
    const glm::vec3 pos = Position(globals[i]);
    const glm::quat rot = Rotation(globals[i]);
    SetGlobal(pose, parents, globals, i, pivot + deltaRot * (pos - pivot) + deltaPos, glm::normalize(deltaRot * rot));
}

void OffsetBoneOnly(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int i,
                    const glm::vec3& deltaPos, const glm::quat& deltaRot, const glm::vec3& pivot) {
    if (!ValidNode(pose, i)) return;
    const glm::vec3 pos = Position(globals[i]);
    const glm::quat rot = Rotation(globals[i]);
    SetLocalFromGlobal(pose, parents, globals, i, pivot + deltaRot * (pos - pivot) + deltaPos, glm::normalize(deltaRot * rot));
    const int p = parents[i];
    const glm::mat4 local = pose[i].ToMatrix();
    globals[i] = p >= 0 ? AffineMul(globals[p], local) : local;
}

void RefreshPath(const Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int from, int node) {
    if (!ValidNode(pose, node)) return;
    thread_local std::vector<int> path;
    path.clear();
    for (int n = node; n >= 0 && n != from; n = parents[n]) path.push_back(n);
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const int p = parents[*it];
        const glm::mat4 local = pose[*it].ToMatrix();
        globals[*it] = p >= 0 ? AffineMul(globals[p], local) : local;
    }
}

bool SolveTwoBone(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals,
                  int upper, int lower, int end, const glm::vec3& targetPos, const glm::quat* targetRot,
                  float weight, float swivel, const TwoBoneHint* hint) {
    if (!ValidNode(pose, upper) || !ValidNode(pose, lower) || !ValidNode(pose, end)) return false;
    weight = std::clamp(weight, 0.0f, 1.0f);
    if (weight <= 0.0f) return true;

    const glm::vec3 a = Position(globals[upper]);
    glm::vec3 b = Position(globals[lower]);
    glm::vec3 c = Position(globals[end]);
    const glm::vec3 t = glm::mix(c, targetPos, weight);
    float lab = glm::length(b - a);
    float lcb = glm::length(c - b);
    if (lab < 1e-6f || lcb < 1e-6f) return false;
    // Max stretch: past full reach the two bones lengthen toward the target, up to MaxLimbScale x.
    if (hint && hint->MaxLimbScale > 1.0f) {
        const float reach = (lab + lcb) * 0.9999f;
        const float dist = glm::length(t - a);
        if (dist > reach) {
            const float s = 1.0f + (std::min(dist / reach, hint->MaxLimbScale) - 1.0f) * weight;
            pose[lower].T *= s;
            pose[end].T *= s;
            RefreshGlobals(pose, parents, globals, lower);
            b = Position(globals[lower]);
            c = Position(globals[end]);
            lab = glm::length(b - a);
            lcb = glm::length(c - b);
        }
    }
    const glm::vec3 ac = c - a;
    if (glm::length(ac) < 1e-6f) return false;

    // Triangle a-b-t with the bone lengths, a hair short of straight so the elbow never locks.
    const float lat = std::clamp(glm::length(t - a), 1e-4f, (lab + lcb) * 0.9999f);
    const float acAb0 = SafeAngle(ac, b - a);
    const float baBc0 = SafeAngle(a - b, c - b);
    const float acAb1 = std::acos(std::clamp((lcb * lcb - lab * lab - lat * lat) / (-2.0f * lab * lat), -1.0f, 1.0f));
    const float baBc1 = std::acos(std::clamp((lat * lat - lab * lab - lcb * lcb) / (-2.0f * lab * lcb), -1.0f, 1.0f));

    // The bend plane is the one the pose already has; a dead-straight limb picks any.
    glm::vec3 axis = glm::cross(ac, b - a);
    axis = glm::length(axis) > 1e-8f ? glm::normalize(axis) : Perpendicular(glm::normalize(ac));

    const glm::quat ga = Rotation(globals[upper]);
    const glm::quat gb = Rotation(globals[lower]);
    // Both bends turn about the same axis, so they commute: the upper bone's bend leaves the
    // direction a->c unchanged, and one swing about a then carries c onto the target.
    const glm::quat q0 = glm::angleAxis(acAb1 - acAb0, axis);
    const glm::quat q1 = glm::angleAxis(baBc1 - baBc0, axis);
    const glm::vec3 at = t - a;
    glm::quat q2 = glm::length(at) > 1e-8f ? RotationBetween(glm::normalize(ac), glm::normalize(at))
                                           : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    // Hint: the solved elbow turns about the root->target line toward the pole plane (the bend plane the pose
    // had is otherwise all that holds it, and it flips as the target crosses it).
    if (hint && hint->HintWeight > 0.0f && glm::length(at) > 1e-8f) {
        const glm::vec3 n = glm::normalize(at);
        const glm::vec3 elbow = q2 * q0 * (b - a);
        const glm::vec3 pole = (hint->HavePole ? hint->Pole : b) + hint->HintOffset - a;
        const glm::vec3 pe = elbow - n * glm::dot(elbow, n);
        const glm::vec3 pp = pole - n * glm::dot(pole, n);
        if (glm::length(pe) > 1e-5f * lab && glm::length(pp) > 1e-6f) {
            const float turn = std::atan2(glm::dot(glm::cross(pe, pp), n), glm::dot(pe, pp));
            q2 = glm::angleAxis(turn * std::clamp(hint->HintWeight, 0.0f, 1.0f), n) * q2;
        }
    }
    // Swivel: the whole solved limb turns about the root->target line, so the end stays on it.
    if (swivel != 0.0f && glm::length(at) > 1e-8f) q2 = glm::angleAxis(swivel, glm::normalize(at)) * q2;

    const glm::quat endRot = Rotation(globals[end]);
    const glm::quat newGa = glm::normalize(q2 * q0 * ga);
    pose[upper].R = glm::normalize(glm::inverse(ParentRotation(parents, globals, upper)) * newGa);
    // Between the three bone writes only the chain down to the next bone's parent needs fresh
    // globals; the whole limb (twist bones, fingers) is refreshed once at the end, from the same
    // final locals - the result is identical to refreshing every subtree after every write.
    RefreshPath(pose, parents, globals, parents[upper], parents[lower]);
    // Set against the (moved) parent rather than assuming lower hangs straight off upper, so
    // rigs with a twist or roll bone in between solve the same.
    pose[lower].R = glm::normalize(glm::inverse(ParentRotation(parents, globals, lower)) * (q2 * q1 * q0 * gb));
    RefreshPath(pose, parents, globals, parents[lower], parents[end]);

    // The end keeps its model-space rotation unless told otherwise: a hand that the arm swings
    // under should still hold the grip, not spin with the forearm.
    const glm::quat wantEnd = targetRot ? glm::slerp(endRot, *targetRot, weight) : endRot;
    pose[end].R = glm::normalize(glm::inverse(ParentRotation(parents, globals, end)) * wantEnd);
    RefreshGlobals(pose, parents, globals, upper);
    return true;
}

void ChainDivisors(const float* weights, int count, float* out) {
    float sum = 0.0f;
    for (int k = 0; k < count; ++k) sum += std::max(0.0f, weights[k]);
    for (int k = 0; k < count; ++k) {
        const float w = std::max(0.0f, weights[k]);
        out[k] = w > 0.0f ? sum / w : std::numeric_limits<float>::infinity();
    }
}

glm::quat ClampStepAngle(const glm::quat& step, float maxAngleDeg) {
    if (maxAngleDeg <= 0.0f) return step;
    const float angle = 2.0f * std::acos(std::clamp(std::abs(step.w), 0.0f, 1.0f));
    const float limit = glm::radians(maxAngleDeg);
    if (angle <= limit || angle < 1e-6f) return step;
    return glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), step.w < 0.0f ? -step : step, limit / angle));
}

void AimBone(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int bone,
             const glm::vec3& aimAxis, const glm::vec3& targetPos, float maxAngleDeg, float weight) {
    if (!ValidNode(pose, bone) || weight <= 0.0f || glm::length(aimAxis) < 1e-6f) return;
    const glm::vec3 pos = Position(globals[bone]);
    const glm::quat rot = Rotation(globals[bone]);
    const glm::vec3 to = targetPos - pos;
    if (glm::length(to) < 1e-6f) return;
    const glm::vec3 from = glm::normalize(rot * glm::normalize(aimAxis));
    glm::quat delta = RotationBetween(from, glm::normalize(to));
    const float angle = glm::angle(delta);
    const float limit = glm::radians(std::max(0.0f, maxAngleDeg));
    float fraction = std::clamp(weight, 0.0f, 1.0f);
    if (angle > limit && angle > 1e-6f) fraction *= limit / angle;
    delta = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), delta, fraction);
    SetGlobal(pose, parents, globals, bone, pos, glm::normalize(delta * rot));
}

glm::mat4 LimbGoal(const glm::mat4& target, const glm::mat4& relative, bool keepAnimatedOffset, const glm::mat4& goalMove,
                   const glm::vec3& gripPosition, const glm::vec3& gripRotationDeg) {
    // The grip offset (hand-vs-gun) sits between the target and the grip, in the target bone's own frame.
    glm::mat4 base = target;
    if (gripPosition != glm::vec3(0.0f) || gripRotationDeg != glm::vec3(0.0f))
        base = base * glm::translate(glm::mat4(1.0f), gripPosition) * glm::mat4_cast(glm::quat(glm::radians(gripRotationDeg)));
    return goalMove * (keepAnimatedOffset ? base * relative : base);
}

void ApplyRig(const IKRigComponent& rig, const Model& model, Pose& pose, const Pose* gripPose) {
    const float w = std::clamp(rig.Weight, 0.0f, 1.0f);
    if (!rig.Enabled || w <= 0.0f || pose.empty() || (int)pose.size() != model.NodeCount()) return;

    // Scratch reused across calls: this runs every frame for every rigged object.
    thread_local std::vector<int> parents;
    thread_local std::vector<glm::mat4> globals;
    parents.resize(pose.size());
    for (int i = 0; i < (int)pose.size(); ++i) parents[i] = model.NodeParent(i);
    // The held pose goes in first; the grips of limbs that aren't held are read from the pose
    // as animated (or the grip pose), before it.
    const bool hold = rig.HoldPose.size() == pose.size() && rig.HoldWeights.size() == pose.size();
    thread_local std::vector<glm::mat4> animated;
    if (hold) {
        ComputeGlobals(gripPose && gripPose->size() == pose.size() ? *gripPose : pose, parents, animated);
        for (size_t i = 0; i < pose.size(); ++i)
            if (rig.HoldWeights[i] > 0.0f) pose[i] = LocalTRS::Blend(pose[i], rig.HoldPose[i], std::min(rig.HoldWeights[i], 1.0f));
    }
    for (const auto& [bone, rot] : rig.LocalRotations) {
        const int i = model.NodeIndex(bone);
        if (i >= 0)
            pose[i].R = glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::normalize(rot), w) * pose[i].R);
    }
    ComputeGlobals(pose, parents, globals);
    thread_local std::vector<glm::mat4> gripGlobals;
    const bool ownGrip = gripPose && gripPose->size() == pose.size();
    if (ownGrip) ComputeGlobals(*gripPose, parents, gripGlobals);
    const std::vector<glm::mat4>& grip = ownGrip ? gripGlobals : globals;

    // Limbs that keep their animated grip remember where the end sat relative to its target
    // BEFORE any offset moves the target.
    struct Limb {
        const IKLimb* Settings;
        int Upper, Lower, End, Target;
        glm::mat4 Relative{1.0f};
    };
    Limb limbs[2];
    int limbCount = 0;
    for (const IKLimb* l : {&rig.LimbA, &rig.LimbB}) {
        if (!l->Enabled || l->Weight <= 0.0f || l->CurveWeight <= 0.0f) continue;
        Limb limb{l, model.NodeIndex(l->Upper), model.NodeIndex(l->Lower), model.NodeIndex(l->End),
                  model.NodeIndex(l->Target)};
        if (limb.Upper < 0 || limb.Lower < 0 || limb.End < 0 || limb.Target < 0) continue;
        if (l->KeepAnimatedOffset) {
            limb.Relative = glm::inverse(grip[limb.Target]) * grip[limb.End];
            if (hold) {
                // Held hand: the held pose's grip; free hand: its animated grip; in between, a mix.
                const float h = std::clamp(rig.HoldWeights[limb.End], 0.0f, 1.0f);
                const glm::mat4 free = glm::inverse(animated[limb.Target]) * animated[limb.End];
                const glm::mat4 held = glm::inverse(globals[limb.Target]) * globals[limb.End];
                const glm::quat r = glm::slerp(Rotation(free), Rotation(held), h);
                limb.Relative = glm::translate(glm::mat4(1.0f), glm::mix(Position(free), Position(held), h)) * glm::mat4_cast(r);
            }
        }
        limbs[limbCount++] = limb;
    }

    for (const IKBoneOffset& off : rig.Offsets) {
        const int i = model.NodeIndex(off.Bone);
        if (i < 0) continue;
        const int pivotBone = off.PivotBone.empty() ? i : model.NodeIndex(off.PivotBone);
        const glm::quat rot = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::normalize(off.Rotation), w);
        OffsetBone(pose, parents, globals, i, off.Position * w, rot,
                   Position(globals[pivotBone >= 0 ? pivotBone : i]) + off.Pivot);
    }

    // The look-at goes before the limbs: aiming a spine or head bone swings the arms hanging off
    // it, and the limbs then reach for their targets from wherever that left them.
    if (rig.LookAtEnabled) {
        const int bone = model.NodeIndex(rig.LookAtBone);
        const int target = model.NodeIndex(rig.LookAtTarget);
        if (bone >= 0 && target >= 0)
            AimBone(pose, parents, globals, bone, rig.LookAtAxis, Position(globals[target]),
                    rig.LookAtMaxAngle, rig.LookAtWeight * std::clamp(rig.LookCurveWeight, 0.0f, 1.0f) * w);
    }

    for (int n = 0; n < limbCount; ++n) {
        const Limb& limb = limbs[n];
        const glm::mat4 goal = LimbGoal(globals[limb.Target], limb.Relative, limb.Settings->KeepAnimatedOffset, limb.Settings->GoalMove,
                                        limb.Settings->GripPosition, limb.Settings->GripRotation);
        const glm::quat goalRot = Rotation(goal);
        const IKLimb& ls = *limb.Settings;
        TwoBoneHint hint;
        const bool hinted = ls.HintWeight > 0.0f || ls.MaxLimbScale > 1.0f;
        if (hinted) {
            hint.HintWeight = ls.HintWeight * w;
            hint.HintOffset = ls.HintOffset;
            hint.MaxLimbScale = ls.MaxLimbScale;
            if (const int pole = ls.PoleBone.empty() ? -1 : model.NodeIndex(ls.PoleBone); pole >= 0) {
                hint.Pole = Position(globals[pole]);
                hint.HavePole = true;
            }
        }
        SolveTwoBone(pose, parents, globals, limb.Upper, limb.Lower, limb.End, Position(goal),
                     ls.MatchRotation ? &goalRot : nullptr, ls.Weight * std::clamp(ls.CurveWeight, 0.0f, 1.0f) * w, ls.Swivel * w, hinted ? &hint : nullptr);
    }
}

std::vector<std::string> MissingBones(const IKRigComponent& rig, const Model& model) {
    std::vector<std::string> missing;
    const auto need = [&](const std::string& name) {
        if (!name.empty() && model.NodeIndex(name) >= 0) return;
        const std::string label = name.empty() ? "(a bone left empty)" : name;
        if (std::find(missing.begin(), missing.end(), label) == missing.end()) missing.push_back(label);
    };
    for (const IKLimb* l : {&rig.LimbA, &rig.LimbB}) {
        if (!l->Enabled) continue;
        need(l->Upper); need(l->Lower); need(l->End); need(l->Target);
    }
    if (rig.LookAtEnabled) { need(rig.LookAtBone); need(rig.LookAtTarget); }
    return missing;
}

// ---- foot slide correction ----
namespace {
float FollowK(float dt, float seconds) { return seconds > 1e-4f ? 1.0f - std::exp(-dt / seconds) : 1.0f; }
} // namespace

float StrideScale(float groundSpeed, float clipSpeed, float lo, float hi) {
    if (groundSpeed < 0.25f || clipSpeed < 0.3f) return 1.0f;
    if (lo > hi) std::swap(lo, hi);
    return std::clamp(groundSpeed / clipSpeed, lo, hi);
}

float StrideWarpPelvisDrop(float legLength, float reach, float scale, float adjust) {
    if (scale <= 1.0f || legLength < 1e-3f) return 0.0f;
    const float L2 = legLength * legLength;
    const float d0 = std::min(std::abs(reach), legLength * 0.98f);
    const float d1 = std::min(d0 * scale, legLength * 0.98f);
    const float drop = std::sqrt(std::max(L2 - d0 * d0, 0.0f)) - std::sqrt(std::max(L2 - d1 * d1, 0.0f));
    return std::clamp(drop, 0.0f, 0.15f) * std::clamp(adjust, 0.0f, 1.0f);
}

FootSlideOutput FootSlide::Step(const FootSlideSettings& set, const FootSlideInput& in, bool measureOnly) {
    FootSlideOutput out;
    const float dt = in.Dt;
    if (dt <= 1e-5f) return out;
    const glm::vec2 vel(in.Velocity.x, in.Velocity.z);
    const float speed = glm::length(vel);
    const glm::vec2 dir = speed > 1e-3f ? vel / speed : glm::vec2(0.0f, 1.0f);
    glm::vec2 rel[2];
    for (int s = 0; s < 2; ++s) rel[s] = glm::vec2(in.Foot[s].x - in.Feet.x, in.Foot[s].z - in.Feet.z);

    // The ankle bone sits a few cm over the ground even when the foot is flat on it, so a plant is judged against each foot's own
    // lowest height (it creeps up slowly so a rig change or a first frame in the air does not stick).
    float height[2];
    for (int s = 0; s < 2; ++s) {
        m_Floor[s] = m_HavePrev ? std::min(m_Floor[s] + 0.03f * dt, in.Height[s]) : in.Height[s];
        height[s] = in.Height[s] - m_Floor[s];
    }
    // The clips' own ground speed: a planted foot slides back along the travel at it, in the capsule's frame.
    if (m_HavePrev) {
        float sample = 0.0f;
        int n = 0;
        for (int s = 0; s < 2; ++s)
            if (m_Planted[s] && height[s] < in.PlantHeight && m_PlantTime[s] > 0.04f) {
                sample += -glm::dot((rel[s] - m_PrevRel[s]) / dt, dir);
                ++n;
            }
        if (n > 0 && speed > 0.25f) {
            sample /= (float)n;
            if (sample > 0.2f && sample < 12.0f) m_ClipSpeed = m_ClipSpeed < 0.0f ? sample : m_ClipSpeed + (sample - m_ClipSpeed) * FollowK(dt, 0.12f);
        }
    }
    for (int s = 0; s < 2; ++s) m_PrevRel[s] = rel[s];
    m_HavePrev = true;

    const bool warp = set.StrideEnabled && !measureOnly;
    const float target = warp ? StrideScale(speed, m_ClipSpeed, set.StrideMin, set.StrideMax) : 1.0f;
    m_Scale += (target - m_Scale) * FollowK(dt, 0.1f);
    const float k = 1.0f + (m_Scale - 1.0f) * std::clamp(set.StrideWeight, 0.0f, 1.0f);
    out.Scale = k;

    float reach = 0.0f;
    glm::vec3 warped[2];
    for (int s = 0; s < 2; ++s) {
        warped[s] = in.Foot[s];
        if (std::abs(k - 1.0f) < 1e-5f) continue;
        const glm::vec2 relP(in.Foot[s].x - in.Pelvis.x, in.Foot[s].z - in.Pelvis.z);
        const float along = glm::dot(relP, dir);
        reach = std::max(reach, std::abs(along));
        warped[s].x += dir.x * along * (k - 1.0f);
        warped[s].z += dir.y * along * (k - 1.0f);
    }
    if (warp) out.PelvisDrop = StrideWarpPelvisDrop(in.LegLength, reach, k, set.PelvisAdjust);

    const bool pin = set.PinEnabled && !measureOnly;
    for (int s = 0; s < 2; ++s) {
        const bool planted = height[s] < in.PlantHeight;
        glm::vec3 fin = warped[s];
        if (planted) {
            if (!m_Planted[s]) { m_Planted[s] = true; m_PlantTime[s] = 0.0f; m_Lock[s] = warped[s]; m_Slide[s] = 0.0f; m_PlantStart[s] = glm::vec3(-1e9f); }
            m_PlantTime[s] += dt;
            // The leash: a pin that lags too far is dragged along rather than snapping.
            const glm::vec2 lag(m_Lock[s].x - warped[s].x, m_Lock[s].z - warped[s].z);
            const float len = glm::length(lag);
            const float maxDrift = std::max(set.PinMaxDrift, 0.01f);
            if (len > maxDrift) m_Lock[s] = glm::vec3(warped[s].x + lag.x / len * maxDrift, warped[s].y, warped[s].z + lag.y / len * maxDrift);
        } else if (m_Planted[s]) {
            m_Planted[s] = false;
            if (m_PlantTime[s] > 0.08f) { // a real plant, not a chatter at the threshold
                ++Stats.Plants;
                Stats.SumSlide += m_Slide[s];
                Stats.MaxSlide = std::max(Stats.MaxSlide, m_Slide[s]);
            }
        }
        if (pin) {
            m_PinWeight[s] += ((planted ? 1.0f : 0.0f) - m_PinWeight[s]) * FollowK(dt, planted ? set.PinRelease * 0.5f : set.PinRelease);
            // Lifting off, the pin lets go with the foot's height too, so a swinging foot is never dragged back.
            const float lift = 1.0f - std::clamp((height[s] - in.PlantHeight) / std::max(in.PlantHeight, 1e-3f), 0.0f, 1.0f);
            const float w = m_PinWeight[s] * lift * std::clamp(set.PinWeight, 0.0f, 1.0f);
            fin.x += (m_Lock[s].x - warped[s].x) * w;
            fin.z += (m_Lock[s].z - warped[s].z) * w;
        } else {
            m_PinWeight[s] = 0.0f;
        }
        out.Shift[s] = glm::vec3(fin.x - in.Foot[s].x, 0.0f, fin.z - in.Foot[s].z);
        if (planted && m_PlantTime[s] > 0.04f) { // measured once the pin has taken hold
            if (m_PlantStart[s].x < -1e8f) m_PlantStart[s] = fin;
            m_Slide[s] = std::max(m_Slide[s], std::hypot(fin.x - m_PlantStart[s].x, fin.z - m_PlantStart[s].z));
        }
    }
    return out;
}

int FootSlideProbeMode() {
    static const int mode = [] {
        char* e = nullptr;
        size_t n = 0;
        _dupenv_s(&e, &n, "TARTARUS_FOOT_SLIDE_PROBE");
        const int m = e && *e ? (std::atoi(e) != 0 ? 1 : 0) : -1;
        std::free(e);
        return m;
    }();
    return mode;
}

void FootSlideProbeLog(const char* who, FootSlideStats& stats, float speed, float clipSpeed, float scale) {
    struct Window { FootSlideStats Sum; std::chrono::steady_clock::time_point Last = std::chrono::steady_clock::now(); };
    static std::map<std::string, Window> windows;
    Window& w = windows[who];
    w.Sum.Plants += stats.Plants;
    w.Sum.SumSlide += stats.SumSlide;
    w.Sum.MaxSlide = std::max(w.Sum.MaxSlide, stats.MaxSlide);
    stats.Clear();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<float>(now - w.Last).count() < 1.5f) return;
    w.Last = now;
    if (w.Sum.Plants > 0)
        std::printf("[FootSlide] %s probe=%d speed %.2f m/s clip %.2f scale %.2f: %d plants, drift mean %.1f cm max %.1f cm\n", who, FootSlideProbeMode(), speed, clipSpeed, scale,
                    w.Sum.Plants, w.Sum.SumSlide / w.Sum.Plants * 100.0f, w.Sum.MaxSlide * 100.0f);
    w.Sum.Clear();
}

FootSlideSettings FootSlideFrom(const FirstPersonBodyComponent& b) {
    FootSlideSettings s;
    s.PinEnabled = b.FootPinEnabled;
    s.PinWeight = b.FootPinWeight;
    s.PinRelease = b.FootPinRelease;
    s.PinMaxDrift = b.FootPinMaxDrift;
    s.StrideEnabled = b.StrideWarpEnabled;
    s.StrideWeight = b.StrideWarpWeight;
    s.StrideMin = b.StrideScaleMin;
    s.StrideMax = b.StrideScaleMax;
    s.PelvisAdjust = b.StridePelvisAdjust;
    return s;
}

} // namespace IK
