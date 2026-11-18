#include "IK.h"

#include "Components.h"
#include "Model.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
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
    const glm::vec3 acDir = glm::normalize(ac);
    glm::vec3 axis = glm::cross(ac, b - a);
    const bool bent = glm::length(axis) > 1e-8f;
    axis = bent ? glm::normalize(axis) : Perpendicular(acDir);
    glm::quat ga = Rotation(globals[upper]);
    glm::quat gb = Rotation(globals[lower]);
    if (hint && hint->BendAxisWeight > 0.0f) {
        // Kept square to the limb (a bend about anything else isn't a bend). The limb is first swung rigidly about its
        // root->end line, so its own bend plane turns BendAxisWeight of the way to the wanted one (the end stays put, the
        // triangle keeps its angles), and the solve below then bends it in that plane as in any other pose. (Bending by
        // the pose's angles about some other axis neither reaches the target nor keeps the knee's angle.)
        glm::vec3 want = hint->BendAxis - acDir * glm::dot(hint->BendAxis, acDir);
        if (glm::length(want) > 1e-6f) {
            want = glm::normalize(want);
            if (bent) {
                const float turn = std::atan2(glm::dot(glm::cross(axis, want), acDir), glm::dot(axis, want)) *
                                   std::clamp(hint->BendAxisWeight, 0.0f, 1.0f);
                const glm::quat qs = glm::angleAxis(turn, acDir);
                b = a + qs * (b - a);
                ga = glm::normalize(qs * ga);
                gb = glm::normalize(qs * gb);
                axis = glm::normalize(qs * axis);
            } else {
                axis = want; // dead straight: no plane of its own to turn from (and no swing needed to change it)
            }
        }
    }

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

void ApplyHeldPose(Pose& pose,const Pose& held,const std::vector<float>& weights) {
    if(held.size()!=pose.size() || weights.size()!=pose.size()) return;
    for(size_t i=0;i<pose.size();++i)
        if(weights[i]>0) pose[i]=LocalTRS::Blend(pose[i],held[i],std::clamp(weights[i],0.0f,1.0f));
}

void ApplyAdditivePose(Pose& pose,const Pose& layer,const Pose& reference,const std::vector<float>& mask,float weight) {
    for(size_t i=0;i<pose.size() && i<layer.size() && i<reference.size();++i) {
        const float w=weight*(i<mask.size()?mask[i]:1.0f);
        if(w<=0) continue;
        pose[i].T+=(layer[i].T-reference[i].T)*w;
        const glm::quat delta=layer[i].R*glm::inverse(reference[i].R);
        pose[i].R=glm::normalize(glm::slerp(glm::quat(1,0,0,0),delta,w)*pose[i].R);
        pose[i].S*=glm::mix(glm::vec3(1),layer[i].S/glm::max(reference[i].S,glm::vec3(1e-6f)),w);
    }
}

void ApplyRig(const IKRigComponent& rig, const Model& model, Pose& pose, const Pose* gripPose,bool holdAlreadyApplied) {
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
    thread_local std::vector<glm::mat4> heldGripGlobals;
    if (hold) {
        ComputeGlobals(gripPose && gripPose->size() == pose.size() ? *gripPose : pose, parents, animated);
        if (rig.PreserveBaseGrip) {
            // Held and free grips must share the same pre-locomotion reference.
            // Reading the held grip from the final blend would reintroduce hand drift.
            thread_local Pose heldGripPose;
            heldGripPose = gripPose && gripPose->size() == pose.size() ? *gripPose : pose;
            ApplyHeldPose(heldGripPose,rig.HoldPose,rig.HoldWeights);
            ComputeGlobals(heldGripPose,parents,heldGripGlobals);
        }
        if(!holdAlreadyApplied) ApplyHeldPose(pose,rig.HoldPose,rig.HoldWeights);
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
        glm::mat4 Carried{1.0f}; // the end relative to its target in this pose, before the offsets: where it rides along
    };
    Limb limbs[2];
    int limbCount = 0;
    for (const IKLimb* l : {&rig.LimbA, &rig.LimbB}) {
        if (!l->Enabled || l->Weight <= 0.0f || l->CurveWeight <= 0.0f) continue;
        Limb limb{l, model.NodeIndex(l->Upper), model.NodeIndex(l->Lower), model.NodeIndex(l->End),
                  model.NodeIndex(l->Target)};
        if (limb.Upper < 0 || limb.Lower < 0 || limb.End < 0 || limb.Target < 0) continue;
        if (l->KeepAnimatedOffset) {
            limb.Carried = glm::inverse(globals[limb.Target]) * globals[limb.End];
            limb.Relative = glm::inverse(grip[limb.Target]) * grip[limb.End];
            if (hold) {
                // Held hand: the held pose's grip; free hand: its animated grip; in between, a mix.
                const float h = std::clamp(rig.HoldWeights[limb.End], 0.0f, 1.0f);
                const glm::mat4 free = glm::inverse(animated[limb.Target]) * animated[limb.End];
                const auto& heldGlobals = rig.PreserveBaseGrip ? heldGripGlobals : globals;
                const glm::mat4 held = glm::inverse(heldGlobals[limb.Target]) * heldGlobals[limb.End];
                const glm::quat r = glm::slerp(Rotation(free), Rotation(held), h);
                limb.Relative = glm::translate(glm::mat4(1.0f), glm::mix(Position(free), Position(held), h)) * glm::mat4_cast(r);
            }
        }
        limbs[limbCount++] = limb;
    }

    for (size_t offset=0;offset<=rig.Offsets.size();++offset) {
        if (rig.SwayEnabled && offset==static_cast<size_t>(std::max(0,rig.SwayBeforeOffset)))
            ApplySwayModifier(rig.SwaySettings,rig.SwayPose,pose,parents,globals,
                              model.NodeIndex(rig.SwaySettings.WeaponBone),model.NodeIndex(rig.SwaySettings.WeaponAdditiveBone),
                              rig.SwayModelToComponent,rig.SwayModelToWorld,rig.SwayMetresPerUnit,w);
        if (offset==rig.Offsets.size()) break;
        const IKBoneOffset& off=rig.Offsets[offset];
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
        glm::vec3 goalPos = Position(goal);
        glm::quat goalRot = Rotation(goal);
        const IKLimb& ls = *limb.Settings;
        float limbWeight = ls.Weight * std::clamp(ls.CurveWeight, 0.0f, 1.0f);
        if (ls.KeepAnimatedOffset) {
            // The offsets above already moved the target by the rig's weight; a hand that keeps its grip goes with all of
            // that move (a solve weighted by it again took the hand only w*w of the way: off the gun by w(1-w) of the
            // move). What the rig weight blends is the goal: from the end riding rigidly with its target (exactly the
            // animated end at weight 0) to the full goal - the grip offset, GoalMove and a grip read from another pose.
            if (w < 1.0f) {
                const glm::mat4 carried = globals[limb.Target] * limb.Carried;
                goalPos = glm::mix(Position(carried), goalPos, w);
                goalRot = glm::normalize(glm::slerp(Rotation(carried), goalRot, w));
            }
        } else {
            limbWeight *= w; // reaching for the target itself: the rig weight is how far
        }
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
        SolveTwoBone(pose, parents, globals, limb.Upper, limb.Lower, limb.End, goalPos,
                     ls.MatchRotation ? &goalRot : nullptr, limbWeight, ls.Swivel * w, hinted ? &hint : nullptr);
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

void FootSlideMeter::Step(const glm::vec3 (&foot)[2], float ground, float dt, const glm::vec3 (*ball)[2]) {
    if (dt <= 1e-5f) return;
    m_Balls = ball != nullptr;
    for (int s = 0; s < 2; ++s) {
        const float h = foot[s].y - ground;
        // Each foot's own lowest height (the ankle stands a few cm up when the foot is flat), creeping up slowly so a crouch
        // or a first frame in the air does not stick - slowly enough that a swinging foot passing low is not taken for a plant.
        m_Floor[s] = m_Have ? std::min(m_Floor[s] + 0.01f * dt, h) : h;
        const bool contact = h - m_Floor[s] < ContactHeight;
        const glm::vec2 at(foot[s].x, foot[s].z);
        if (contact) {
            // The first two frames are the heel coming down; the plant is judged from where it settles.
            const glm::vec2 ballAt = ball ? glm::vec2((*ball)[s].x, (*ball)[s].z) : at;
            if (++m_Frames[s] == 3) { m_Start[s] = at; m_Drift[s] = 0.0f; m_Time[s] = 0.0f; m_BallStart[s] = ballAt; m_BallDrift[s] = 0.0f; }
            if (m_Frames[s] >= 3) {
                m_Drift[s] = std::max(m_Drift[s], glm::length(at - m_Start[s]));
                m_BallDrift[s] = std::max(m_BallDrift[s], glm::length(ballAt - m_BallStart[s]));
                m_Time[s] += dt;
                Total.ContactTime += dt;
            }
        } else {
            if (m_Frames[s] >= 3 && m_Time[s] >= 0.1f && m_Drift[s] < 1.0f) { // (a teleport is not a slide)
                ++Total.Plants;
                Total.SumDrift += m_Drift[s];
                Total.MaxDrift = std::max(Total.MaxDrift, m_Drift[s]);
                if (m_Balls) {
                    const float least = std::min(m_Drift[s], m_BallDrift[s]);
                    Total.SumContactDrift += least;
                    Total.MaxContactDrift = std::max(Total.MaxContactDrift, least);
                }
            }
            m_Frames[s] = 0;
        }
    }
    m_Have = true;
}

namespace {
bool& MeterFlag() { static bool on = false; return on; }
} // namespace
bool FootSlideMeterOn() { return MeterFlag(); }
void SetFootSlideMeterOn(bool on) { MeterFlag() = on; }
FootSlideMeter& FootSlideMeterFor(const std::string& who) {
    static std::map<std::string, FootSlideMeter> meters;
    return meters[who];
}

void FootLock::LetGo(Foot& f, const FootLockSettings& set, bool step) {
    f.Locked = false;
    f.OnBall = false;
    f.ReleaseFrom = glm::vec3(f.Offset.x, 0.0f, f.Offset.z);
    f.ReleaseTime = 0.0f;
    const float travel = glm::length(f.ReleaseFrom) + (set.StepForTravel ? f.AnimSpeed * set.Release : 0.0f);
    f.Lift = step ? std::min(set.StepHeight * travel, set.StepMax) : 0.0f;
}

glm::vec3 FootLock::Step(int s, const FootLockSettings& set, const glm::vec3& root, const glm::quat& facing, const glm::vec3& local,
                         float height, float dt, const glm::vec3* ball, float ballHeight) {
    Foot& f = m_Foot[s];
    if (dt <= 1e-5f) return f.Offset;
    const glm::vec3 anim = root + facing * local;
    if (f.Have && glm::length(glm::vec2(anim.x - f.Prev.x, anim.z - f.Prev.z)) > 1.0f) f = Foot{}; // a teleport: start over
    const bool seen = f.Have;
    // How fast the animated foot moves over the ground, the body's own turn left out: the root's travel, plus the foot's
    // move in the body at last frame's heading. A foot the clip holds still reads the capsule's travel against the clip's
    // (about nothing) however fast the body swings round; a swinging foot reads its swing.
    const glm::vec3 moved = (root - f.PrevRoot) + f.PrevFacing * (local - f.PrevLocal);
    const float speed = seen ? glm::length(glm::vec2(moved.x, moved.z)) / dt : 0.0f;
    const float rootSpeed = seen ? glm::length(glm::vec2(root.x - f.PrevRoot.x, root.z - f.PrevRoot.z)) / dt : 0.0f;
    f.AnimSpeed = seen ? glm::length(glm::vec2(anim.x - f.Prev.x, anim.z - f.Prev.z)) / dt : 0.0f;
    f.Floor = seen ? std::min(f.Floor + 0.01f * dt, height) : height;
    const float up = height - f.Floor;
    const glm::vec3 animBall = ball ? root + facing * *ball : anim;
    f.BallFloor = ball && f.HaveBall && seen ? std::min(f.BallFloor + 0.01f * dt, ballHeight) : ballHeight;
    f.HaveBall = ball != nullptr;
    const float ballUp = ball ? ballHeight - f.BallFloor : up;
    // ... and the ball's own speed over the ground (the same way), for a foot planted on its ball.
    float ballSpeed = speed;
    if (ball) {
        const glm::vec3 ballMoved = (root - f.PrevRoot) + f.PrevFacing * (*ball - f.PrevBallLocal);
        ballSpeed = seen ? glm::length(glm::vec2(ballMoved.x, ballMoved.z)) / dt : 0.0f;
        f.PrevBallLocal = *ball;
    }
    f.Prev = anim;
    f.PrevRoot = root;
    f.PrevFacing = facing;
    f.PrevLocal = local;
    f.Have = true;
    const bool stepping = f.Lift > 0.0f && f.ReleaseTime < set.Release; // a step plants again only once it is over
    if (!set.Enabled) {
        if (f.Locked) LetGo(f, set, false);
    } else if (!f.Locked) {
        // Flat on the floor (within FlatHeight), a foot the clip itself slides plants however fast it slides: a turn on
        // the spot lands its step sliding at ~2 m/s. Only the clip's slide - a foot the capsule carries that fast is
        // a swing, held to Speed.
        const bool flat = up < set.FlatHeight && rootSpeed < set.Speed;
        const bool heel = up < set.Height && (speed < set.Speed || flat);
        const bool onBall = ball && !heel && ballUp < set.Height && (ballSpeed < set.Speed || (ballUp < set.FlatHeight && rootSpeed < set.Speed));
        if (seen && !stepping && (heel || onBall)) { // (a first frame has no speed yet)
            // Held where it is drawn now (a foot still catching up plants there, not where the animation is).
            f.Locked = true;
            f.OnBall = onBall;
            f.Pin = glm::vec3(anim.x + f.Offset.x, anim.y, anim.z + f.Offset.z);
            f.BallPin = glm::vec3(animBall.x + f.Offset.x, animBall.y, animBall.z + f.Offset.z);
            f.Time = 0.0f;
        }
    } else {
        // The ball down too: the hold passes to it, where it is drawn now (the offset unchanged). (Waiting until it is flat -
        // FlatHeight - measured no better: the straight legs' drift the same, the turns' a little worse.)
        if (ball && !f.OnBall && ballUp < set.Height) {
            f.OnBall = true;
            f.BallPin = glm::vec3(animBall.x + f.Offset.x, animBall.y, animBall.z + f.Offset.z);
        }
        const float drift = f.OnBall ? glm::length(glm::vec2(f.BallPin.x - animBall.x, f.BallPin.z - animBall.z))
                                     : glm::length(glm::vec2(f.Pin.x - anim.x, f.Pin.z - anim.z));
        if ((f.OnBall ? ballUp : up) > set.Lift) LetGo(f, set, false);
        else if (drift > set.MaxDrift) LetGo(f, set, true);
    }
    if (f.Locked) {
        f.Time += dt;
        f.Offset = f.OnBall ? glm::vec3(f.BallPin.x - animBall.x, 0.0f, f.BallPin.z - animBall.z)
                            : glm::vec3(f.Pin.x - anim.x, 0.0f, f.Pin.z - anim.z);
    } else {
        f.ReleaseTime += dt;
        const float k = std::clamp(f.ReleaseTime / std::max(set.Release, 1e-3f), 0.0f, 1.0f);
        f.Offset = f.ReleaseFrom * (1.0f - k * k * (3.0f - 2.0f * k));
        if (f.Lift > 0.0f) f.Offset.y = f.Lift * std::sin(3.14159265f * k); // the step's arc
        if (k >= 1.0f) f.Lift = 0.0f;
    }
    return f.Offset;
}

bool FootLock::Overreach(int s, const FootLockSettings& set, float reach, float own) {
    Foot& f = m_Foot[s];
    // (A hundredth over the clip's own: a held foot that has not moved off its animation is not what stretches the leg -
    // but never past the whole leg, where the reach clamp would have to drag it.)
    if (!f.Locked || reach <= std::max(set.Reach, std::min(own + 0.01f, 1.0f))) return false;
    LetGo(f, set, true);
    return true;
}

glm::vec3 ClampReach(const glm::vec3& root, const glm::vec3& target, float length, float knee, float most) {
    const glm::vec3 d = target - root;
    const float len = glm::length(d);
    const float from = knee * length;
    if (length <= 1e-5f || len <= from || len <= 1e-6f) return target;
    // Past `knee` the reach follows from + room * (1 - e^-(over / room)): slope 1 where it starts, flattening onto from + room.
    const float room = std::max((most - knee) * length, 0.0f);
    const float r = room > 1e-6f ? from + room * (1.0f - std::exp(-(len - from) / room)) : from;
    return root + d * (r / len);
}

FootLockSettings FootLockFrom(const FirstPersonBodyComponent& b) {
    FootLockSettings s;
    s.Enabled = b.FootLockEnabled;
    s.Height = b.FootLockHeight;
    s.Lift = b.FootLockLift;
    s.Speed = b.FootLockSpeed;
    s.MaxDrift = b.FootLockDrift;
    s.Release = b.FootLockRelease;
    return s;
}

namespace {
// The angle (radians) of `q`'s twist about unit `axis` (swing-twist decomposition), in (-pi, pi].
float TwistAngle(const glm::quat& q, const glm::vec3& axis) {
    const float along = glm::dot(glm::vec3(q.x, q.y, q.z), axis);
    float a = 2.0f * std::atan2(along, q.w);
    if (a > glm::pi<float>()) a -= glm::two_pi<float>();
    else if (a <= -glm::pi<float>()) a += glm::two_pi<float>();
    return a;
}
glm::vec3 SegmentAxis(const Pose& bind, int end) {
    const glm::vec3 t = bind[end].T;
    return glm::dot(t, t) > 1e-12f ? glm::normalize(t) : glm::vec3(1.0f, 0.0f, 0.0f);
}
} // namespace

float EndRoll(const Pose& pose, const Pose& bind, int segment, int end) {
    if (segment < 0 || end < 0 || end >= (int)pose.size() || end >= (int)bind.size()) return 0.0f;
    // The end's turn off its bind, in the segment's frame.
    const glm::quat delta = glm::normalize(pose[end].R * glm::inverse(bind[end].R));
    return TwistAngle(delta, SegmentAxis(bind, end));
}

float SegmentRoll(const Pose& pose, const Pose& bind, int segment, int end) {
    if (segment < 0 || end < 0 || segment >= (int)pose.size() || end >= (int)bind.size()) return 0.0f;
    // The segment's turn off its bind, in its own bind frame.
    const glm::quat delta = glm::normalize(glm::inverse(bind[segment].R) * pose[segment].R);
    return TwistAngle(delta, SegmentAxis(bind, end));
}

void SpreadTwist(Pose& pose, const Pose& bind, int segment, int end, const std::vector<TwistBone>& twists, float roll, bool carry) {
    if (segment < 0 || end < 0 || end >= (int)bind.size()) return;
    const glm::vec3 axis = SegmentAxis(bind, end);
    for (const TwistBone& t : twists) {
        if (t.Node < 0 || t.Node >= (int)pose.size() || t.Node >= (int)bind.size()) continue;
        const float amount = carry ? roll * t.Share : -roll * (1.0f - t.Share);
        pose[t.Node].R = glm::normalize(glm::angleAxis(amount, axis) * bind[t.Node].R);
    }
}

std::vector<TwistBone> FindTwistBones(const Pose& bind, const std::vector<int>& parents, const std::vector<std::string>& names, int segment, int end) {
    std::vector<TwistBone> out;
    if (segment < 0 || end < 0 || end >= (int)bind.size()) return out;
    const glm::vec3 axis = SegmentAxis(bind, end);
    const float length = glm::length(bind[end].T);
    for (int i = 0; i < (int)parents.size() && i < (int)names.size() && i < (int)bind.size(); ++i) {
        if (parents[i] != segment) continue;
        std::string n = names[i];
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        if (n.find("twist") == std::string::npos || n.find("twistcor") != std::string::npos) continue;
        const float along = length > 1e-6f ? std::clamp(glm::dot(bind[i].T, axis) / length, 0.0f, 1.0f) : 0.5f;
        out.push_back({i, along});
    }
    return out;
}

} // namespace IK
