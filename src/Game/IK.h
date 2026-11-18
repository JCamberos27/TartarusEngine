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
    // Model-space bend axis (the way cross(root->end, root->mid) turns for a correctly bent limb) taken in place of the
    // pose's own by BendAxisWeight: near straight the pose's bend plane is rounding noise, and a knee bent out of a straight
    // leg would go a different way each frame. Before the solve the limb swings rigidly about its root->end line by
    // BendAxisWeight of the angle from its own bend plane to this one (the end stays put, so the result is continuous in
    // the weight and still lands on the target). 0 = the pose's own.
    glm::vec3 BendAxis{0.0f};
    float BendAxisWeight = 0.0f;
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

// ---- foot locking ----
// A planted foot stays where it landed in the world, whatever the clips and the capsule disagree on (a blend's stride
// against the capsule's speed, a body turning on the spot, a start or stop's travel, a crossfade between two gaits).
// A foot plants when it is down - within Height of its own lowest point - and its animated foot barely moves over the
// ground (under Speed m/s: a swinging foot passing low never plants). That speed leaves the body's own turn out: it is the
// root's travel plus the foot's move in the body (the clip's), so a foot the clip holds still plants however fast the body
// is swung round (its world speed is the turn rate times its distance from the root - 4 m/s at 600 degrees a second).
// It lets go when it lifts, and the lag it built up is eased out over Release seconds (smoothstep), so the foot catches up
// through its swing instead of popping. When holding it would take more than Reach of the leg (Overreach: the hips have
// walked or climbed on from it), or - a safety - the animation wanders MaxDrift metres from it while it is still down, the
// foot steps there: it lifts in an arc (StepHeight a metre of the way, at most StepMax) while it catches up, and plants
// again only once it has. The player's body and the soldiers alike.
struct FootLockSettings {
    bool Enabled = true;
    float Height = 0.02f;   // m over the foot's own lowest height that still counts as down
    float Lift = 0.045f;    // m over it that lets go (the heel rolling up onto the toe stays planted under this)
    float Speed = 2.0f;     // m/s the animated foot may move over the ground (the body's turn left out) and still plant
    float FlatHeight = 0.008f; // m over its lowest under which a foot the clip slides plants at any speed (the root's under Speed)
    float Reach = 0.98f;    // x the leg's length (hip to held foot) past which a planted foot lets go and steps
    float MaxDrift = 0.25f; // m the animation may wander from a planted foot before it lets go (a safety: Reach goes first)
    float Release = 0.2f;   // s a released foot takes to catch up with the animation
    float StepHeight = 0.3f; // a let-go foot that was still down lifts this much per metre it has to go...
    float StepMax = 0.08f;   // ... at most this (m)
    // The metres it has to go: its lag when let go, plus - with StepForTravel - the way the animation carries the foot over
    // the ground through the step (its speed, the body's turn included, x Release). Without it a foot let go as the body
    // turned over it measured a 5 cm lag, lifted 1.5 cm, and was carried 24 cm round with the turn, skimming the floor
    // (the motion matching body's 180 deg walk flick, 2026-11-17).
    bool StepForTravel = false;
    // A foot at rest near the ground - under Lift over its floor, moving under RestSpeed over the ground (the body's turn left
    // out) and up or down under RestRise, for RestTime - is down where it is: its floor (and its ball's) is raised to it.
    // Without it the floor only creeps up (1 cm/s) from the lowest the foot has been: after the sprint (the legacy jog set
    // plants its ankle 3 cm lower than the pack's idles) a standing foot read 2.6 cm up, wasn't held for 1.5 s and crept 4.5 cm
    // (mmplayer, M sprint back, 2026-11-18).
    bool RestFloor = false;
    static constexpr float RestSpeed = 0.3f, RestRise = 0.05f, RestTime = 0.1f;
};
FootLockSettings FootLockFrom(const FirstPersonBodyComponent& body);
// The leg's goal never asks for more than `most` x `length` from `root` (a two-bone chain asked past its reach snaps
// straight and its knee flips through): up to `knee` x length it is left alone; past that it is drawn in along the
// root->target line, easing (continuous, no corner) toward `most` without ever reaching it. The player's legs and the
// soldiers' alike.
glm::vec3 ClampReach(const glm::vec3& root, const glm::vec3& target, float length, float knee = 0.98f, float most = 0.995f);
class FootLock {
public:
    void Reset() { *this = FootLock{}; }
    void ResetFoot(int s) { m_Foot[s] = Foot{}; }
    // One foot, one frame. The animated foot is `root` + `facing` * `local`: `root` the body's root on the ground (world),
    // `facing` its heading, `local` the clip's foot relative to it (the model's space, scaled to metres). `height`: the
    // foot's height over the capsule's ground (m). Returns the world offset that puts the foot where it should be drawn:
    // horizontal, plus a step's lift (zero for a free foot).
    // `ball` (optional, the motion matching body): the forefoot, as `local`, and its height. A planted foot is held at the
    // heel (the ankle) from the strike, as without it; once the ball is down too the hold passes to the ball - at the offset
    // it has, so nothing moves - and the foot lets go when the ball lifts, not the heel. The body turning over a held foot
    // then swivels it on its ball, as a stance foot pivots, instead of sweeping its toes over the floor; the heel rising
    // onto the toe stays held until the toe leaves. A foot is down when its heel or its ball is: one planted on its ball (the
    // heel up - a jog's start, a stance shifting weight) is held at the ball from the first frame.
    glm::vec3 Step(int s, const FootLockSettings& set, const glm::vec3& root, const glm::quat& facing, const glm::vec3& local,
                   float height, float dt, const glm::vec3* ball = nullptr, float ballHeight = 0.0f);
    bool OnBall(int s) const { return m_Foot[s].OnBall; }
    // After Step, once the leg's goal is known: `reach` is the hip-to-goal distance with the foot held, `own` without
    // (the clip's own), both x the leg's length, the hip where it stands after the pelvis has dropped. Past Reach - and
    // past what the clip itself asks, so a leg the clip has near straight is not let go for nothing - a planted foot lets
    // go and steps (its lag eased out from the next Step). Returns whether it let go.
    bool Overreach(int s, const FootLockSettings& set, float reach, float own);
    // Lets a planted foot go now (it steps to catch up): the body turning faster than the lock can follow.
    void Release(int s, const FootLockSettings& set) { if (m_Foot[s].Locked) LetGo(m_Foot[s], set, true); }
    bool Locked(int s) const { return m_Foot[s].Locked; }
    glm::vec3 Pinned(int s) const { return m_Foot[s].Pin; }
    float LockedTime(int s) const { return m_Foot[s].Time; }
    glm::vec3 Offset(int s) const { return m_Foot[s].Offset; }
private:
    struct Foot {
        bool Have = false, Locked = false;
        glm::vec3 Prev{0.0f};      // last frame's animated foot (world)
        glm::vec3 PrevRoot{0.0f};  // ... its body's root
        glm::quat PrevFacing{1.0f, 0.0f, 0.0f, 0.0f}; // ... and heading
        glm::vec3 PrevLocal{0.0f}; // ... and the foot in the body
        float Floor = 0.0f;        // its lowest height (creeping up)
        glm::vec3 Pin{0.0f};       // where a locked foot is held
        glm::vec3 Offset{0.0f};    // what is applied (horizontal)
        glm::vec3 ReleaseFrom{0.0f};
        float ReleaseTime = 1e9f;  // seconds since the last let-go
        float Time = 0.0f;         // seconds locked
        float Lift = 0.0f;         // a step's arc height (m; 0 = the foot lifted on its own)
        float AnimSpeed = 0.0f;    // m/s the animated foot moves over the ground, the body's turn included (last Step)
        bool OnBall = false;       // held at the ball (Step's `ball`), not the heel
        glm::vec3 BallPin{0.0f};   // where the ball is held
        float BallFloor = 0.0f;    // the ball's lowest height (creeping up, as Floor)
        bool HaveBall = false;
        glm::vec3 PrevBallLocal{0.0f}; // last frame's ball in the body
        float PrevHeight = 0.0f;   // last frame's height (FootLockSettings::RestFloor)
        float Rest = 0.0f;         // seconds at rest near the ground
    };
    static void LetGo(Foot& f, const FootLockSettings& set, bool step);
    Foot m_Foot[2];
};

// The foot slide the eye sees: each frame the FINAL feet (after every IK pass, in the world) are watched; a foot within
// ContactHeight of its own lowest height is on the ground, and every centimetre it moves sideways while there is slide.
// It runs in every state - turns on the spot, starts, stops, crossfades - and never changes the pose.
struct FootSlideMeterTotals {
    int Plants = 0;             // contacts that lasted 0.1 s or more
    float SumDrift = 0.0f;      // metres: each plant's furthest drift from where it settled, summed
    float MaxDrift = 0.0f;      // metres: the worst plant's
    // With the balls given (Step's `ball`): per plant the lesser of the ankle's drift and the ball's - a foot pivoting on its
    // heel or on its ball is turning, not sliding (a slide moves both). 0 when no balls were given.
    float SumContactDrift = 0.0f, MaxContactDrift = 0.0f;
    float MeanContactCm() const { return Plants ? SumContactDrift * 100.0f / (float)Plants : 0.0f; }
    float ContactTime = 0.0f;   // seconds of foot-on-ground (both feet summed)
    // The worst plants measured from the contact's third frame instead (before 2026-11-18): the swing's end counted too.
    struct { float MaxDrift = 0.0f, MaxContactDrift = 0.0f; } FromContact;
    float MeanCm() const { return Plants ? SumDrift * 100.0f / (float)Plants : 0.0f; }
};
class FootSlideMeter {
public:
    float ContactHeight = 0.015f; // m over the foot's own lowest height
    float SettleHeight = 0.005f;  // m over the contact's lowest: where a plant has settled (its drift is measured from there)
    // `ground`: the capsule's feet height (world y). `foot`: each final foot bone (world); `ball` (optional): each final ball.
    void Step(const glm::vec3 (&foot)[2], float ground, float dt, const glm::vec3 (*ball)[2] = nullptr);
    void Reset() { *this = FootSlideMeter{}; }
    FootSlideMeterTotals Total; // since the last TakeTotals
    FootSlideMeterTotals TakeTotals() { FootSlideMeterTotals t = Total; Total = {}; return t; }
private:
    bool m_Have = false;
    float m_Floor[2] = {0.0f, 0.0f};
    int m_Frames[2] = {0, 0};      // frames on the ground this contact
    float m_Time[2] = {0.0f, 0.0f};
    struct RunFrame { glm::vec2 At, Ball; float H; };
    std::vector<RunFrame> m_Run[2]; // this contact's frames from the third
    bool m_Balls = false;
};
// The meters the --npc-test scenarios read (one per body: "player", or an NPC's name). Only fed while a test asks for them.
bool FootSlideMeterOn();
void SetFootSlideMeterOn(bool on);
FootSlideMeter& FootSlideMeterFor(const std::string& who);

} // namespace IK
