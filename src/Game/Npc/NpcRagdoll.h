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

// A soldier going down: the body's pose at the moment of death becomes eleven jointed capsules
// (pelvis, chest, head, upper and lower arms, thighs and calves) in the physics world, carrying the
// body's velocity and the round's shove, and from then on the physics drives the pose - every outfit
// piece's bones follow the capsule they belong to (bones in between keep their last animated shape).
//
// The death is a powered blend, not a cut: the joints start with slerp drives holding the pose the body died in
// (so a soldier hit running keeps its stride for a beat and falls as it was), and the drives fade to nothing over
// kDriveFade seconds (each region on its own clock: the Ragdoll Settings' fade scales), after which it is a rag. Every part
// starts with its own bone's velocity from the last two animated poses, so limbs keep their swing.
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
    static constexpr int kSnapBones = 17;
    struct BoneSnapshot {
        glm::vec3 P[kSnapBones];
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
    // Part `part`'s mass under `cfg` (null: the default).
    static float PartMass(const RagdollSettingsComponent* cfg, int part);

    // The parts (pure: from bone positions), exposed for tests. Bones by part, in order:
    // pelvis, spine_03, head, upperarm_l, lowerarm_l, upperarm_r, lowerarm_r, thigh_l, calf_l, thigh_r, calf_r.
    static constexpr int kParts = 11;

private:
    struct PieceBones {
        std::shared_ptr<Model> M;
        std::vector<int> Node;       // per part: the piece's node (-1 = none)
        std::vector<glm::mat4> Off;  // per part: node world = part world * Off
        std::vector<int> Parents;    // the model's node parents (filled on first use)
    };
    int m_Id = -1;
    glm::mat4 m_RootInv{1.0f};       // the body's root (frozen at death), inverted
    std::vector<PieceBones> m_Pieces;
    bool m_Settled = false;          // asleep, and the pieces already show the resting pose
    float m_Drive = 0.0f;
    float m_Time = 0.0f;             // seconds since death
    float m_PartFade[kParts];        // per part: seconds to limp
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
const NpcPartDef& NpcPartDefOf(int part);

// The capsule of part `def` between bone positions `a` and `b` (`neck`: the head's reference, for the part with no End):
// its centre, orientation (+X along it) and shape, in whatever space the points are in.
struct NpcPartShape {
    glm::vec3 Centre{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float Radius = 0.05f, HalfLength = 0.1f;
    glm::vec3 Anchor{0.0f}; // where it joins its parent
};
NpcPartShape NpcPartShapeOf(const NpcPartDef& def, const glm::vec3& a, const glm::vec3& b, const glm::vec3& neck);
