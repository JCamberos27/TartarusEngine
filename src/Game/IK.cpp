#include "IK.h"

#include "Components.h"
#include "Model.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>

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
        globals[i] = parents[i] >= 0 ? globals[parents[i]] * local : local;
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
        globals[i] = p >= 0 ? globals[p] * local : local;
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

void SetGlobal(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int i,
               const glm::vec3& pos, const glm::quat& rot) {
    if (!ValidNode(pose, i)) return;
    const int p = parents[i];
    if (p >= 0) {
        pose[i].T = glm::vec3(glm::inverse(globals[p]) * glm::vec4(pos, 1.0f));
        pose[i].R = glm::normalize(glm::inverse(Rotation(globals[p])) * rot);
    } else {
        pose[i].T = pos;
        pose[i].R = glm::normalize(rot);
    }
    RefreshGlobals(pose, parents, globals, i);
}

void OffsetBone(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int i,
                const glm::vec3& deltaPos, const glm::quat& deltaRot, const glm::vec3& pivot) {
    if (!ValidNode(pose, i)) return;
    const glm::vec3 pos = Position(globals[i]);
    const glm::quat rot = Rotation(globals[i]);
    SetGlobal(pose, parents, globals, i, pivot + deltaRot * (pos - pivot) + deltaPos, glm::normalize(deltaRot * rot));
}

bool SolveTwoBone(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals,
                  int upper, int lower, int end, const glm::vec3& targetPos, const glm::quat* targetRot,
                  float weight, float swivel) {
    if (!ValidNode(pose, upper) || !ValidNode(pose, lower) || !ValidNode(pose, end)) return false;
    weight = std::clamp(weight, 0.0f, 1.0f);
    if (weight <= 0.0f) return true;

    const glm::vec3 a = Position(globals[upper]);
    const glm::vec3 b = Position(globals[lower]);
    const glm::vec3 c = Position(globals[end]);
    const glm::vec3 t = glm::mix(c, targetPos, weight);
    const float lab = glm::length(b - a);
    const float lcb = glm::length(c - b);
    if (lab < 1e-6f || lcb < 1e-6f) return false;
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
    // Swivel: the whole solved limb turns about the root->target line, so the end stays on it.
    if (swivel != 0.0f && glm::length(at) > 1e-8f) q2 = glm::angleAxis(swivel, glm::normalize(at)) * q2;

    const glm::quat endRot = Rotation(globals[end]);
    const glm::quat newGa = glm::normalize(q2 * q0 * ga);
    pose[upper].R = glm::normalize(glm::inverse(ParentRotation(parents, globals, upper)) * newGa);
    RefreshGlobals(pose, parents, globals, upper);
    // Set against the (moved) parent rather than assuming lower hangs straight off upper, so
    // rigs with a twist or roll bone in between solve the same.
    pose[lower].R = glm::normalize(glm::inverse(ParentRotation(parents, globals, lower)) * (q2 * q1 * q0 * gb));
    RefreshGlobals(pose, parents, globals, lower);

    // The end keeps its model-space rotation unless told otherwise: a hand that the arm swings
    // under should still hold the grip, not spin with the forearm.
    const glm::quat wantEnd = targetRot ? glm::slerp(endRot, *targetRot, weight) : endRot;
    pose[end].R = glm::normalize(glm::inverse(ParentRotation(parents, globals, end)) * wantEnd);
    RefreshGlobals(pose, parents, globals, end);
    return true;
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

void ApplyRig(const IKRigComponent& rig, const Model& model, Pose& pose, const Pose* gripPose) {
    const float w = std::clamp(rig.Weight, 0.0f, 1.0f);
    if (!rig.Enabled || w <= 0.0f || pose.empty() || (int)pose.size() != model.NodeCount()) return;

    // Scratch reused across calls: this runs every frame for every rigged object.
    thread_local std::vector<int> parents;
    thread_local std::vector<glm::mat4> globals;
    parents.resize(pose.size());
    for (int i = 0; i < (int)pose.size(); ++i) parents[i] = model.NodeParent(i);
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
        if (!l->Enabled || l->Weight <= 0.0f) continue;
        Limb limb{l, model.NodeIndex(l->Upper), model.NodeIndex(l->Lower), model.NodeIndex(l->End),
                  model.NodeIndex(l->Target)};
        if (limb.Upper < 0 || limb.Lower < 0 || limb.End < 0 || limb.Target < 0) continue;
        if (l->KeepAnimatedOffset) limb.Relative = glm::inverse(grip[limb.Target]) * grip[limb.End];
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
                    rig.LookAtMaxAngle, rig.LookAtWeight * w);
    }

    for (int n = 0; n < limbCount; ++n) {
        const Limb& limb = limbs[n];
        const glm::mat4 goal = limb.Settings->KeepAnimatedOffset ? globals[limb.Target] * limb.Relative
                                                                 : globals[limb.Target];
        const glm::quat goalRot = Rotation(goal);
        SolveTwoBone(pose, parents, globals, limb.Upper, limb.Lower, limb.End, Position(goal),
                     limb.Settings->MatchRotation ? &goalRot : nullptr, limb.Settings->Weight * w,
                     limb.Settings->Swivel * w);
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

} // namespace IK
