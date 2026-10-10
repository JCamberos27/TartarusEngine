#pragma once

#include "Animation.h" // LocalTRS

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

class Model;
struct IKRigComponent;
struct FirstPersonBodyComponent;

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

// Twist bones. IK re-rolls a limb (a hand turned to a grip the clip never had) and leaves the roll at one joint: with
// the skin weighted across the limb's twist bones, the wrist then wrings like a sweet wrapper. A limb segment's roll
// beyond its bind pose is measured about the segment's own axis and spread over the segment's twist bones (children
// of the segment), each by its share - how far along the segment it sits.
struct TwistBone {
    int Node = -1;
    float Share = 0.0f; // 0 at the segment's root, 1 at its end
};
// The roll (radians) about `segment`'s axis (toward `end`, its child) of `end` relative to `segment`, beyond the bind
// pose's: the forearm's share of a hand's turn.
float EndRoll(const Pose& pose, const Pose& bind, int segment, int end);
// The roll (radians) of `segment` itself about its axis (toward `end`) relative to its parent, beyond the bind pose's:
// the upper arm's turn in the shoulder.
float SegmentRoll(const Pose& pose, const Pose& bind, int segment, int end);
// Each twist bone at its bind local, turned about the segment's axis by `roll` times its share (`carry`: a forearm's
// twist bones carry the hand's roll) or by `roll` times (1 - share) the other way (`!carry`: an upper arm's undo its own
// roll toward the shoulder).
void SpreadTwist(Pose& pose, const Pose& bind, int segment, int end, const std::vector<TwistBone>& twists, float roll, bool carry);
// The twist bones of `segment` (children whose names contain "twist", not "twistCor"), each with its share along the
// segment toward `end`, from the bind pose.
std::vector<TwistBone> FindTwistBones(const Pose& bind, const std::vector<int>& parents, const std::vector<std::string>& names, int segment, int end);

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
// PreserveBaseGrip keeps that action reference before locomotion layers, including the held
// grip of an ADS reload; the finished target still carries the hand through those layers.
void ApplyHeldPose(Pose& pose,const Pose& held,const std::vector<float>& weights);
void ApplyAdditivePose(Pose& pose,const Pose& layer,const Pose& reference,const std::vector<float>& mask,float weight);
void ApplyRig(const IKRigComponent& rig, const Model& model, Pose& pose, const Pose* gripPose = nullptr,bool holdAlreadyApplied=false);

// The bones an enabled limb or look-at names that `model` doesn't have (an empty name counts),
// for the Inspector's warning. Empty when the rig can run as set up.
std::vector<std::string> MissingBones(const IKRigComponent& rig, const Model& model);

// ---- foot slide correction ----
// Two layers on top of the authored locomotion, both off by default, both weighted 0..1. Stride warping scales the
// pelvis-relative foot offsets along the travel so the feet cover the ground at the capsule's real speed (blend-tree
// speeds, speed multipliers, crouch speed) instead of the clip's authored one, and lowers the pelvis for the longer
// reach; foot pinning then holds each planted foot where it landed (the leg re-solved by the caller) and releases it
// with a short blend. The same step runs for the player's body and for NPCs.
struct FootSlideSettings {
    bool PinEnabled = false;
    float PinWeight = 1.0f;      // 0..1
    float PinRelease = 0.06f;    // seconds the pin takes to let go once the foot lifts (it takes half that to grab)
    float PinMaxDrift = 0.25f;   // metres a pinned foot may lag the body before the pin is dragged along (a leash)
    bool StrideEnabled = false;
    float StrideWeight = 1.0f;   // 0..1
    float StrideMin = 0.75f;     // the stride is never scaled below / above these
    float StrideMax = 1.35f;
    float PelvisAdjust = 1.0f;   // 0..1: how much of the pelvis drop a longer stride needs is applied
    bool Active() const { return PinEnabled || StrideEnabled; }
};
struct FootSlideInput {
    glm::vec3 Feet{0.0f};        // the capsule's foot position (world)
    glm::vec3 Velocity{0.0f};    // the capsule's velocity (world; only the horizontal part is read)
    glm::vec3 Foot[2];           // each animated foot (world)
    float Height[2] = {0, 0};    // each animated foot's height over the capsule's feet (m)
    glm::vec3 Pelvis{0.0f};      // the animated pelvis (world)
    float LegLength = 0.9f;      // thigh + calf (m), for the pelvis drop
    float PlantHeight = 0.05f;   // a foot within this of its own lowest height counts as planted (m)
    float ClipSpeed = -1.0f;     // the playing gait's own ground speed when the animator knows it (m/s); < 0 = estimate it from the feet
    float Dt = 0.0f;
};
struct FootSlideOutput {
    glm::vec3 Shift[2] = {glm::vec3(0.0f), glm::vec3(0.0f)}; // world, horizontal: animated foot -> where the leg should reach
    float PelvisDrop = 0.0f;                                  // metres, >= 0
    float Scale = 1.0f;                                       // the stride scale applied
};

FootSlideOutput SmoothFootSlideOutput(const FootSlideOutput& current,const FootSlideOutput& target,float dt,float ease);
// The planted feet's drift while they are planted, per plant (cm in the log): what the correction is judged by.
struct FootSlideStats {
    int Plants = 0;
    float SumSlide = 0.0f, MaxSlide = 0.0f; // metres
    void Clear() { *this = FootSlideStats{}; }
};
// groundSpeed / clipSpeed clamped to [lo, hi]; 1 when either is too small to mean anything (standing, turning, no estimate).
float StrideScale(float groundSpeed, float clipSpeed, float lo, float hi);
// How far the pelvis sinks (m, >= 0) when a foot at `reach` metres along the travel reaches `scale` times as far on a leg of `legLength`.
float StrideWarpPelvisDrop(float legLength, float reach, float scale, float adjust);

class FootSlide {
public:
    void Reset() { *this = FootSlide{}; }
    // One frame. `measureOnly`: report the stats without changing anything (both layers off).
    FootSlideOutput Step(const FootSlideSettings& set, const FootSlideInput& in, bool measureOnly = false);
    bool Planted(int s) const { return m_Planted[s]; }
    float ClipSpeed() const { return m_ClipSpeed; } // the estimated authored ground speed of the playing clips (m/s; < 0 = none yet)
    float Scale() const { return m_Scale; }
    FootSlideStats Stats;
private:
    bool m_Planted[2] = {false, false};
    float m_PlantTime[2] = {0.0f, 0.0f};
    glm::vec3 m_Lock[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    float m_PinWeight[2] = {0.0f, 0.0f};
    glm::vec2 m_PrevRel[2] = {glm::vec2(0.0f), glm::vec2(0.0f)};
    bool m_HavePrev = false;
    float m_Floor[2] = {0.0f, 0.0f}; // each foot's lowest animated height (the ankle over the ground when flat)
    float m_ClipSpeed = -1.0f;
    float m_Scale = 1.0f;
    glm::vec3 m_PlantStart[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    float m_Slide[2] = {0.0f, 0.0f};
};
FootSlideSettings FootSlideFrom(const FirstPersonBodyComponent& body);
// The measurement hook for --stock-probe / --npc-test: TARTARUS_FOOT_SLIDE_PROBE=0 logs the planted feet's drift with the layers as
// the scene sets them, =1 also forces both layers on at weight 1 (for the before / after); unset = off (-1).
int FootSlideProbeMode();
// Adds `stats` to the running window of `who` and prints it (mean / max drift per plant, cm) every 1.5 s of wall time.
void FootSlideProbeLog(const char* who, FootSlideStats& stats, float speed, float clipSpeed, float scale);

} // namespace IK
