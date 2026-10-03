#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include "Animation.h" // LocalTRS

class Model;
class NpcBody;
struct RagdollSettingsComponent;
namespace PhysicsWorld { struct RagdollPart; struct RagdollParams; }

// The muscles of a ragdoll (a dead body that is still powered, Euphoria-style "physical animation"): every joint keeps a slerp drive
// toward a target pose, whose strength decays per region to a small residual (legs first, then spine, neck and arms) and whose target
// goes from the pose the body died in to a procedural collapse (hips and knees fold, the spine curls, the arms brace or drop). The struck
// joint is weak from the start (it takes the round), the legs start at a partial strength and hold it a beat (the body is carried by
// the round and topples along the shot, not straight down). Once its parts slow, the body settles: damping, friction and joint friction
// ramp up and, at rest, it is put to sleep. Pure physics and settings (no GL, no body): the tests drive it on a bare ragdoll.
class NpcRagdollMotor {
public:
    enum class Group { Legs, Spine, Neck, Arms };
    // Starts powering ragdoll `id`, built from `parts` (BuildParts: the joints' frames) in a body whose axes are `root`'s (x left,
    // y up, z forward). `fallDir`: the way the body is going to fall (the round's, or the body's momentum; world, need not be level,
    // zero: forward); `hitPart`: the part the round struck (-1 none). `cfg` is copied.
    void Begin(int id, const PhysicsWorld::RagdollPart* parts, const glm::mat4& root, const glm::vec3& fallDir, int hitPart, const RagdollSettingsComponent& cfg);
    // Once per frame (`dt` seconds): the drives' strengths and targets, the settle, the rest.
    void Update(float dt);
    // Something shoved the body: the settle and the sleep start over.
    void Wake();
    float Time() const { return m_Time; }
    // The settle's progress, 0 (moving) .. 1 (settled values applied).
    float Settle() const { return m_Settle; }
    // The strongest joint's strength now (0..1).
    float Strength() const { return m_Strongest; }
    bool ForcedToRest() const { return m_Resting; }

    static Group GroupOf(int part);
    // The strength (0..1) of the joint that holds `part`, `t` seconds after death (hitPart: the struck part or -1): the group's decay
    // curve from its start strength (the legs: Stagger Leg Strength, held for Stagger Time) to the residual, the struck joint weaker.
    static float PartStrength(const RagdollSettingsComponent& cfg, int part, float t, int hitPart);
    // The collapse pose's flexion of the joint of `part` in degrees (positive: the joint's own flexion direction), for a body falling
    // forward (`forward` true) or back.
    static float CollapseFlexion(const RagdollSettingsComponent& cfg, int part, bool forward);
    // The drive target (the joint frame's rotation from the pose it was built in) of `part` at `t` seconds.
    static glm::quat TargetAt(const RagdollSettingsComponent& cfg, int part, float t, bool forward);

private:
    int m_Id = -1;
    int m_HitPart = -1;
    bool m_Forward = true;
    bool m_Frames = false;            // anatomical joint frames: the collapse pose is available
    bool m_Resting = false;
    float m_Time = 0.0f, m_Settle = 0.0f, m_Still = 0.0f, m_Strongest = 1.0f;
    float m_AppliedSettle = -1.0f;
    float m_Height0 = 1.0f;           // the pelvis's height at death
    std::shared_ptr<RagdollSettingsComponent> m_Cfg; // a copy (the director's may change under a body that is already down)
};

// A soldier going down: the body's pose at the moment of death becomes sixteen jointed capsules
// (pelvis, chest, head, upper and lower arms, thighs and calves, and the ragdoll's own neck, hands and feet) in the physics world, carrying the
// body's velocity and the round's shove, and from then on the physics drives the pose - every outfit
// piece's bones follow the capsule they belong to (bones in between keep their last animated shape).
//
// The death is a powered blend, not a cut (NpcRagdollMotor): the joints keep slerp drives toward the pose the body died in, blending
// into a procedural collapse, their strength decaying per region to a small residual, so a soldier hit running keeps its stride for a
// beat and falls as it was, and goes down along the shot; then it settles and sleeps. (Powered Ragdoll off: the drives fade to nothing
// over kDriveFade seconds, each region on its own clock, after which it is a rag.) Every part starts with its own bone's velocity from the
// last two animated poses, so limbs keep their swing.
class NpcRagdoll {
public:
    ~NpcRagdoll();
    // `velocity`: the body's (m/s); `impulse` (N s) at `point` (world) on part `hitPart` (-1: the part nearest
    // `point`).
    // `cfg`: the scene's Ragdoll Settings (null: the defaults).
    // `prev` / `prevDt`: the bones as they were `prevDt` seconds before the pose the body is in now (Capture), for each part's own
    // velocity (null: all parts move with `velocity`).
    struct BoneSnapshot;
    bool Start(NpcBody& body, const glm::vec3& velocity, const glm::vec3& impulse, const glm::vec3& point, int hitPart = -1,
               const RagdollSettingsComponent* cfg = nullptr, const BoneSnapshot* prev = nullptr, float prevDt = 0.0f);
    void Stop();
    bool Active() const { return m_Id >= 0; }
    // After the physics step (and after the animators, which are off by then): the pieces' poses. `dt` runs the
    // drives' fade.
    void Update(float dt = 0.0f);
    bool Asleep() const;
    // The pelvis part's position (world), for tests.
    glm::vec3 Root() const;
    // A round (or anything) shoves part `part` by `impulse` (N s) at `point` (world): wakes it, and the pose is
    // rewritten from now on.
    void Shove(int part, const glm::vec3& impulse, const glm::vec3& point);
    // Part `part`'s centre (world), as last stepped.
    glm::vec3 PartPosition(int part) const;
    // The drive's strength now (the strongest region's), 1 at the moment of death to 0 once faded.
    float DriveLeft() const { return m_Drive; }
    // How the drives fade: seconds from death to limp (the default; a Ragdoll Settings overrides it, see DriveFadeTime).
    static constexpr float kDriveFade = 0.25f;
    // The longest any region's drive lasts (seconds).
    float DriveFadeTime() const;
    // Seconds region of `part` takes to go limp under `cfg` (null: the default), and the drive's strength (1..0) `t` seconds
    // after death for a fade of `fade`.
    static float PartFade(const RagdollSettingsComponent* cfg, int part);
    static float DriveAt(float t, float fade) { return fade <= 1e-4f ? 0.0f : std::max(0.0f, 1.0f - t / fade); }

    // The bones a ragdoll is built from, in the body's root space, for a velocity from two poses.
    // The last four (ball and middle finger bones, which anchor a foot's and a hand's capsule) are optional: Has says which a rig has.
    static constexpr int kSnapBones = 21;
    struct BoneSnapshot {
        glm::vec3 P[kSnapBones];
        bool Has[kSnapBones] = {};
        bool Valid = false;
    };
    static void Capture(const NpcBody& body, BoneSnapshot& out);
    // Adds each part's own motion to parts[].Velocity / AngularVelocity: the finite difference of its pose over `dt`, from
    // `prevWorld` to `nowWorld` (both in the same, non-moving frame), times cfg.LimbVelocityScale and clamped (a teleport, a bad
    // frame). dt outside 2 ms .. 120 ms adds nothing.
    static void InheritVelocity(const glm::mat4* prevWorld, const glm::mat4* nowWorld, float dt, const RagdollSettingsComponent& cfg,
                                PhysicsWorld::RagdollPart* parts);

    // The physics parts for a body in the pose `bone` gives (a bone's world position by name, false if missing), its root
    // matrix (x left, y up, z forward): capsules, masses and the joints' limits (anatomical ranges of motion from `cfg`, or the
    // old symmetric cones with AnatomicalLimits off). Exposed for tests. `partWorld` (optional) gets each part's pose.
    static bool BuildParts(const std::function<bool(const char*, glm::vec3&)>& bone, const glm::mat4& root, const glm::vec3& velocity,
                           const RagdollSettingsComponent& cfg, PhysicsWorld::RagdollPart* parts, glm::mat4* partWorld = nullptr);
    static PhysicsWorld::RagdollParams BodyParams(const RagdollSettingsComponent& cfg);
    // The death's shove and (with a `motor`: Powered Ragdoll) its muscles, on ragdoll `id` built from `parts` / `partWorld` (BuildParts) in
    // a body of axes `root`: `impulse` (N s) at `point` (world) on part `hitPart` (-1: the nearest), the body going `velocity`. Returns
    // the part struck. Start does this; it is exposed so the tests can kill a bare ragdoll.
    static int Launch(int id, const PhysicsWorld::RagdollPart* parts, const glm::mat4* partWorld, const glm::mat4& root, const glm::vec3& velocity,
                      const glm::vec3& impulse, const glm::vec3& point, int hitPart, const RagdollSettingsComponent& cfg, NpcRagdollMotor* motor);
    // Part `part`'s mass under `cfg` (null: the default).
    static float PartMass(const RagdollSettingsComponent* cfg, int part);

    // The hitboxes (NpcHitboxes) and the first eleven parts of the ragdoll, by bone, in order:
    // pelvis, spine_03, head, upperarm_l, lowerarm_l, upperarm_r, lowerarm_r, thigh_l, calf_l, thigh_r, calf_r.
    // Hitboxes are exactly these (their shapes are gameplay and don't follow the ragdoll's tuning); the ragdoll has its own table
    // (RagdollDefOf) that keeps those indices and adds neck_01, hand_l, hand_r, foot_l, foot_r after them, so a hit part's index
    // names the same body part in both.
    static constexpr int kParts = 11;
    static constexpr int kNeck = 11, kHandL = 12, kHandR = 13, kFootL = 14, kFootR = 15;
    static constexpr int kRagParts = 16;

private:
    struct PieceBones {
        std::shared_ptr<Model> M;
        std::vector<int> Node;       // per part: the piece's node (-1 = none)
        std::vector<glm::mat4> Off;  // per part: node world = part world * Off
        // Bones between two parts (spine_01/02 between pelvis and chest) or riding one (clavicles on the chest): written back as
        // the blend of where each part's frame would put them, so the skin between parts doesn't stretch.
        struct Link { int Node, A, B; float T; glm::mat4 OffA, OffB; };
        std::vector<Link> Links;
        std::vector<int> Parents;    // the model's node parents (filled on first use)
    };
    int m_Id = -1;
    glm::mat4 m_RootInv{1.0f};       // the body's root (frozen at death), inverted
    std::vector<PieceBones> m_Pieces;
    bool m_Settled = false;          // asleep, and the pieces already show the resting pose
    float m_Drive = 0.0f;
    NpcRagdollMotor m_Motor;         // the powered body (cfg.PoweredRagdoll); else the legacy fade below
    bool m_Powered = false;
    float m_Time = 0.0f;             // seconds since death
    float m_PartFade[kRagParts];        // per part: seconds to limp
    float m_DistalDamping = 0.0f;    // the hands' and feet's joint damping floor
    float m_Stiffness = 700.0f, m_Damping = 60.0f; // the drives' strength, from the settings
    // Scratch, reused every frame.
    std::vector<glm::mat4> m_Globals;
    std::vector<LocalTRS> m_Pose;
};

// One of a soldier's eleven body parts: where its capsule runs from and to, and how it hangs on the rest. The
// ragdoll's joints and the per-bone hitboxes (NpcHitboxes) are cut from the same table.
struct NpcPartDef {
    const char* Bone;
    const char* End;     // the bone the capsule reaches to (null = along the parent direction)
    float Extend;        // metres past End (a hand, a skull)
    float Radius, Mass;
    int Parent;
    float Swing, Twist;
};
const NpcPartDef& NpcPartDefOf(int part); // the hitbox table: the first kParts only
// The ragdoll's table: the same eleven (forearms and calves stop at the wrist and ankle, the head hangs off the neck) plus
// neck, hands and feet. `part` 0 .. NpcRagdoll::kRagParts - 1. A part whose End bone a rig lacks (middle_01 / ball) is shaped
// from a fallback direction (see BuildParts).
const NpcPartDef& NpcRagdollDefOf(int part);

// The capsule of part `def` between bone positions `a` and `b` (`neck`: the head's reference, for the part with no End):
// its centre, orientation (+X along it) and shape, in whatever space the points are in.
struct NpcPartShape {
    glm::vec3 Centre{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float Radius = 0.05f, HalfLength = 0.1f;
    glm::vec3 Anchor{0.0f}; // where it joins its parent
};
NpcPartShape NpcPartShapeOf(const NpcPartDef& def, const glm::vec3& a, const glm::vec3& b, const glm::vec3& neck);

// A soldier hit but not killed: the struck region's bones are kicked along the round (a rotation about each bone's own joint,
// axis = bone x round direction, so its far end goes where the round did) and a critically damped spring brings them back within
// the Ragdoll Settings' Flinch Duration. It is added to the animated pose after the late pose and the hitboxes (so aim, eye and
// hitboxes never see it), on top of the animator's own hit reaction; scaled by damage and by the region (a head or an arm kicks
// further than the chest), and several rounds in a row add up to a cap. Pure maths (Hit, Rotations) apart from Apply.
class NpcFlinch {
public:
    static constexpr int kBones = 13;
    static const char* BoneName(int bone); // spine_01, spine_02, spine_03, neck_01, head, upperarm_l/r, lowerarm_l/r, thigh_l/r, calf_l/r
    struct Kick { int Bone; glm::vec3 Axis; float Peak, Start, Duration; };
    // `part`: a hitbox part (0 .. NpcRagdoll::kParts - 1); `boneDir`, `dir`: the part's bone direction and the round's travel, both in
    // the body's root space (need not be unit); `now`: the director's clock. Nothing is added with cfg.HitFlinch off.
    void Hit(int part, const glm::vec3& boneDir, const glm::vec3& dir, float damage, float now, const RagdollSettingsComponent& cfg);
    // The same from the body: the bone direction and the round's world direction `dirWorld`.
    void Hit(const NpcBody& body, int part, const glm::vec3& dirWorld, float damage, float now, const RagdollSettingsComponent& cfg);
    bool Active(float now) const;
    // Each bone's rotation vector (rad, root space) at `now`.
    void Rotations(float now, glm::vec3 out[kBones]) const;
    // Adds them to every piece's applied pose (once per fresh pose: a held pose already has the last frame's).
    void Apply(NpcBody& body, float now) const;
    int Count() const { return (int)m_Kicks.size(); }
    // 0 at the hit, 1 at the peak (a fifth of the duration in), ~0.06 at `duration`, ~0 well after.
    static float Curve(float age, float duration);
    // The kick a round of `damage` gives bone weight `weight` of part `part`'s region (radians, before the cap).
    static float PeakAngle(const RagdollSettingsComponent& cfg, float damage, int part, float weight);
    // The bones a hit on `part` kicks, with their shares: out[i] = {bone, weight}; returns the count.
    static int PartBones(int part, int outBone[3], float outWeight[3]);

private:
    std::vector<Kick> m_Kicks;
    float m_MaxAngle = 0.5f;
};
