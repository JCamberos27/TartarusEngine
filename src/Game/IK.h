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

// Rigidly moves node `i` (and so everything under it) in model space: rotation `deltaRot` about
// `pivot`, then translation `deltaPos`.
void OffsetBone(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int i,
                const glm::vec3& deltaPos, const glm::quat& deltaRot, const glm::vec3& pivot);

// Two-bone IK (shoulder-elbow-hand, hip-knee-foot). Bends `upper` and `lower` so `end` reaches
// `targetPos`, keeping the bend plane the pose already has (the animated elbow is the pole).
// Out-of-reach targets are reached for along the straightened chain. With `targetRot`, the
// end bone's model-space rotation is set too. `weight` blends from the input pose (0) to the
// solved one (1). Returns false when the chain is degenerate (zero-length bones) or invalid.
bool SolveTwoBone(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals,
                  int upper, int lower, int end, const glm::vec3& targetPos, const glm::quat* targetRot,
                  float weight);

// Rotates `bone` so its local axis `aimAxis` points at `targetPos`, by at most `maxAngleDeg`,
// blended by `weight`.
void AimBone(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int bone,
             const glm::vec3& aimAxis, const glm::vec3& targetPos, float maxAngleDeg, float weight);

// Moves `move` rigidly so the line rear -> front (points fixed to `ref`, in its local space)
// lies on the line through `eye` along `forward`: turned to face along it (the smallest turn,
// so the roll is kept) and slid onto it. `distance` > 0 also sets how far along it the rear
// point sits; 0 keeps its current depth. `weight` blends from no change (0).
void AlignLine(Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int move, int ref,
               const glm::vec3& rearLocal, const glm::vec3& frontLocal, const glm::vec3& eye,
               const glm::vec3& forward, float distance, float weight);

// Runs an IK Rig component on `pose` for `model`: its line alignment and bone offsets (runtime,
// code), then its limbs and look-at. No-op when the rig is disabled or its weight is 0.
void ApplyRig(const IKRigComponent& rig, const Model& model, Pose& pose);

// The bones an enabled limb or look-at names that `model` doesn't have (an empty name counts),
// for the Inspector's warning. Empty when the rig can run as set up.
std::vector<std::string> MissingBones(const IKRigComponent& rig, const Model& model);

} // namespace IK
