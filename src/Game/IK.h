#pragma once

#include "Animation.h" // LocalTRS

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

class Model;
struct IKRigComponent;

// Inverse kinematics and procedural bone edits on a sampled pose - the post-process stage that
// runs after the Animator Controller has blended its layers and before the pose is applied to
// the model (AnimatorController.cpp, PoseModel).
//
// A pose is one LocalTRS per node, parents-first (Model::NodeParent). Everything here works in
// MODEL space: the node globals Model::NodeTransform reports, before the entity's transform.
// Parent scale is assumed uniform (true for imported rigs, whose only scale is a unit factor).
namespace IK {

using Pose = std::vector<LocalTRS>;

// Model-space global of every node.
void ComputeGlobals(const Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals);
// Recomputes `from` and every node below it after pose[from] changed.
void RefreshGlobals(const Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int from);

glm::vec3 Position(const glm::mat4& global);
glm::quat Rotation(const glm::mat4& global); // scale removed

// Sets node `i`'s local translation/rotation so its model-space global lands on (pos, rot);
// its local scale is kept. Refreshes the globals below it.
void SetGlobal(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int i,
               const glm::vec3& pos, const glm::quat& rot);

// SetGlobal for several nodes in one pass. `nodes` ascending (parents first); wanted[k] = {position, rotation} for
// nodes[k]. The pose and globals come out the same as SetGlobal on each in turn, at one refresh instead of one each.
struct GlobalTarget { int Node; glm::vec3 Pos; glm::quat Rot; };
void SetGlobals(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, const std::vector<GlobalTarget>& targets);

// Rigidly moves node `i` (and so everything under it) in model space: rotation `deltaRot` about
// `pivot`, then translation `deltaPos`.
void OffsetBone(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int i,
                const glm::vec3& deltaPos, const glm::quat& deltaRot, const glm::vec3& pivot);

// OffsetBone that refreshes only node `i`'s own global and leaves everything below it stale -
// for offsetting several bones down one chain (the spine), where re-deriving every descendant
// (arms, fingers) after each one was most of the cost. Follow each with RefreshPath to the next
// bone to be read; the pose itself comes out identical to calling OffsetBone each time.
void OffsetBoneOnly(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int i,
                    const glm::vec3& deltaPos, const glm::quat& deltaRot, const glm::vec3& pivot);
// Recomputes the globals on the parent path down to `node`, starting below `from` when `from` is
// one of its ancestors (whose own global is current), else from the root.
void RefreshPath(const Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int from, int node);

// How a rotation spread over the spine (look pitch, view twist, shoulder line) is shared out. The defaults are the
// even spread: every bone takes 1/n of the turn, the pelvis none, no bone limited.
struct SpineDistribution {
    float Weight[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // spine_01..spine_05, relative (a bone the model lacks is skipped)
    float MaxAngle[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f}; // degrees one bone may turn from one spread; 0 = no limit
    float PelvisAlpha = 0.0f;                           // 0..1: share of the turn the pelvis takes (the spine takes the rest)
};

// Two-bone IK (shoulder-elbow-hand, hip-knee-foot). Bends `upper` and `lower` so `end` reaches
// `targetPos`, keeping the bend plane the pose already has (the animated elbow is the pole).
// Out-of-reach targets are reached for along the straightened chain. With `targetRot`, the
// end bone's model-space rotation is set too. `weight` blends from the input pose (0) to the
// solved one (1). Returns false when the chain is degenerate (zero-length bones) or invalid.
//
// `hint` (optional) steers the elbow and allows a stretch; null, or the default hint, solves exactly as
// without it. See TwoBoneHint.
struct TwoBoneHint {
    // 0 = keep the animated bend plane (no change); 1 = the elbow sits in the plane that holds the pole
    // point and the root->target line, which stops it flipping when the target crosses the bend plane.
    float HintWeight = 0.0f;
    // Model-space pole point. With HavePole false the animated elbow's position stands in for it.
    glm::vec3 Pole{0.0f};
    bool HavePole = false;
    // Model-space offset added to the pole point (pushes the elbow out, in, up).
    glm::vec3 HintOffset{0.0f};
    // The longest the limb may stretch toward an out-of-reach target, x its authored length. 1 = no stretch.
    float MaxLimbScale = 1.0f;
};
bool SolveTwoBone(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals,
                  int upper, int lower, int end, const glm::vec3& targetPos, const glm::quat* targetRot,
                  float weight, float swivel = 0.0f, const TwoBoneHint* hint = nullptr);

// Per-bone share of a spread rotation over a chain of `count` bones (spine, neck). weights[0..count) are the
// bones' relative weights (all equal = the old even spread); out[k] is the DIVISOR that bone applies to the whole
// rotation, so an even spread gives `count` exactly (angle / out[k] = angle / count, bit for bit). A zero weight
// gives infinity (the bone takes nothing).
void ChainDivisors(const float* weights, int count, float* out);
// Largest angle (degrees) a bone's share may reach: `step` is slerped toward identity if it turns further.
// maxAngleDeg <= 0 means no limit.
glm::quat ClampStepAngle(const glm::quat& step, float maxAngleDeg);

// Where a limb's end is sent: `goalMove` * target * grip offset * (relative, when the animated grip is kept). The grip
// offset (position, rotation in degrees pitch/yaw/roll) is in the target bone's own frame; zero = no change.
glm::mat4 LimbGoal(const glm::mat4& target, const glm::mat4& relative, bool keepAnimatedOffset, const glm::mat4& goalMove,
                   const glm::vec3& gripPosition, const glm::vec3& gripRotationDeg);

// Rotates `bone` so its local axis `aimAxis` points at `targetPos`, by at most `maxAngleDeg`,
// blended by `weight`.
void AimBone(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int bone,
             const glm::vec3& aimAxis, const glm::vec3& targetPos, float maxAngleDeg, float weight);

// Runs an IK Rig component on `pose` for `model`: its bone offsets (runtime, written by game
// code), then its limbs and look-at. No-op when the rig is disabled or its weight is 0.
// `gripPose`, when given, is where limbs that keep their animated offset read that offset from
// instead of `pose`: a crossfade blends two arm shapes joint by joint, and even when both hold
// the gun the same way the blend doesn't, so the animator passes the pose it's fading into.
void ApplyRig(const IKRigComponent& rig, const Model& model, Pose& pose, const Pose* gripPose = nullptr);

// The bones an enabled limb or look-at names that `model` doesn't have (an empty name counts),
// for the Inspector's warning. Empty when the rig can run as set up.
std::vector<std::string> MissingBones(const IKRigComponent& rig, const Model& model);

} // namespace IK
