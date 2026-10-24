#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The spider's locomotion, free of the ECS and of PhysX so the unit tests can run it against boxes: a body that sticks
// to whatever surface it is on (floor, wall, ceiling) and climbs from one onto the next, and eight legs that plant on
// that surface and step in two alternating groups of four. SpiderSystem (SpiderSystem.h) feeds both from the scene and
// puts the legs onto the rig with IK::SolveChain.
//
// Frames: the body's origin sits on the surface between its feet; its local +Y is the surface's normal (its "up") and
// its local -Z its forward, the model's own facing.
namespace Spider {

struct SurfaceHit {
    bool Hit = false;
    float Distance = 0.0f;
    glm::vec3 Point{0.0f};
    glm::vec3 Normal{0.0f, 1.0f, 0.0f};
};

// What the spider can stand on: the solid world in the game (PhysicsWorld::RaycastSolid), boxes in the unit tests.
class SurfaceQuery {
public:
    virtual ~SurfaceQuery() = default;
    // Nearest surface along `dir` (unit) from `from` within `maxDistance`.
    virtual bool Ray(const glm::vec3& from, const glm::vec3& dir, float maxDistance, SurfaceHit& out) const = 0;
    // Whether a sphere there would touch anything solid (the surface graph's room for the body).
    virtual bool Occupied(const glm::vec3& centre, float radius) const { (void)centre; (void)radius; return false; }
};

struct MoverSettings {
    float Speed = 0.9f;          // m/s along the surface when it has somewhere to go
    float Accel = 6.0f;          // m/s^2 up to that speed and back down
    float TurnRate = 260.0f;     // deg/s about its up
    float UpRate = 10.0f;        // 1/s: how fast its up follows the surface's (a time constant's inverse)
    float ProbeLift = 0.25f;     // the down probe starts this far above the body's origin ...
    float ProbeDepth = 0.6f;     // ... and looks this far down
    float ProbeHeight = 0.12f;   // the forward probe runs this high off the surface ...
    float Reach = 0.45f;         // ... this far ahead: a wall inside it is climbed onto
    float Clearance = 0.16f;     // the body never gets closer to a wall ahead than this (it turns onto it instead)
    float Gravity = 9.81f;       // m/s^2 while it has let go
    float LetGoAfter = 0.12f;    // seconds with no surface under it before it falls
    float Arrive = 0.12f;        // metres: within this of the goal it stops
};

struct MoveInput {
    bool HasGoal = false;
    glm::vec3 Goal{0.0f};        // world: where it heads, over whatever surfaces lie between
    float SpeedScale = 1.0f;     // x Speed (a run)
    glm::vec3 LegUp{0.0f};       // the planted legs' mean surface normal (zero: none), pulled into its up
};

class Mover {
public:
    void Reset(const glm::vec3& pos, const glm::quat& rot);
    // Lets go of the surface with `velocity` (a leap, a drop from the ceiling, a hit that knocks it off): it flies
    // under gravity, rights itself and catches the first surface it meets.
    void Detach(const glm::vec3& velocity);
    void Step(const SurfaceQuery& q, const MoverSettings& s, const MoveInput& in, float dt);

    glm::vec3 Position() const { return m_Pos; }
    glm::quat Rotation() const { return m_Rot; }
    glm::vec3 Up() const { return m_Rot * glm::vec3(0.0f, 1.0f, 0.0f); }
    glm::vec3 Forward() const { return m_Rot * glm::vec3(0.0f, 0.0f, -1.0f); }
    glm::vec3 Velocity() const { return m_Vel; }  // world, m/s (along the surface, or the fall)
    float Speed() const { return m_Speed; }       // along the surface, m/s
    bool Attached() const { return m_Attached; }
    bool Arrived() const { return m_Arrived; }
    // Knocked off whatever it holds (a heavy round into it on a wall or a ceiling): it lets go at once, moving at
    // `velocity` (world, m/s), falls, lands and rights itself. It is lifted a little clear of the surface first, so the
    // fall's ray doesn't start inside what it just let go of.
    void Detach(const glm::vec3& velocity);
    // The surface it stands on: its normal and how far the origin is off it (m; < 0: no surface found this frame).
    glm::vec3 SurfaceNormal() const { return m_SurfaceNormal; }
    float SurfaceGap() const { return m_SurfaceGap; }

private:
    void Fall(const SurfaceQuery& q, const MoverSettings& s, float dt);
    void TurnUpToward(const glm::vec3& want, float rate, float dt);

    glm::vec3 m_Pos{0.0f}, m_Vel{0.0f};
    glm::quat m_Rot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 m_SurfaceNormal{0.0f, 1.0f, 0.0f};
    float m_SurfaceGap = 0.0f;
    float m_Speed = 0.0f;
    float m_Unsupported = 0.0f;
    bool m_Attached = true, m_Arrived = false;
};

// ---- legs ----

constexpr int kLegs = 8;

struct GaitSettings {
    float StepTime = 0.2f;       // seconds a step takes at a walk (shorter as it speeds up)
    float MinStepTime = 0.07f;   // ... never shorter than this
    float StepHeight = 0.07f;    // metres the foot lifts off the surface at mid-step
    float StepDistance = 0.11f;  // a planted foot this far from where it wants to be steps
    float Lead = 0.6f;           // the foothold is picked this many step-times ahead of the body's travel
    float ProbeLift = 0.3f;      // a foothold's down probe starts this far above the rest spot ...
    float ProbeDepth = 0.65f;    // ... and looks this far down
    float StanceScale = 0.92f;   // rest spots pulled toward the body (1 = the rig's bind stance)
    int ProbesPerFrame = 4;      // legs whose foothold is looked for each frame (round robin; a stepping leg always is)
};

// One leg as the rig gives it, in the model's space.
struct LegRest {
    glm::vec3 Hip{0.0f};         // the chain's root standing
    glm::vec3 Foot{0.0f};        // the tip standing
    float Lift = 0.0f;           // how far the tip stands off the surface (the claw's mesh reaches below the bone's end)
    float Reach = 0.0f;          // the chain's full length
    int Group = 0;               // 0 or 1: which half of the alternating gait
};

struct LegState {
    glm::vec3 Foot{0.0f};        // world: where the tip is now
    glm::vec3 Normal{0.0f, 1.0f, 0.0f};
    glm::vec3 From{0.0f}, To{0.0f}, FromNormal{0.0f, 1.0f, 0.0f}, ToNormal{0.0f, 1.0f, 0.0f};
    glm::vec3 Want{0.0f}, WantNormal{0.0f, 1.0f, 0.0f}; // the last foothold found
    bool WantGround = false;     // false: nothing under the rest spot (it dangles there)
    bool Planted = true, Stepping = false, Grounded = false;
    float T = 0.0f, Duration = 0.2f;
};

class Gait {
public:
    void Init(const std::array<LegRest, kLegs>& rest, const glm::mat4& model);
    // One frame: footholds looked for, steps started and advanced. `model` is the model's world matrix now, `bodyPos`
    // and `up` the mover's, `velocity` its travel (world, m/s).
    void Step(const SurfaceQuery& q, const GaitSettings& s, const glm::mat4& model, const glm::vec3& bodyPos,
              const glm::vec3& up, const glm::vec3& velocity, bool attached, float dt);

    const LegState& Leg(int i) const { return m_Legs[(size_t)i]; }
    const LegRest& Rest(int i) const { return m_Rest[(size_t)i]; }
    // The planted legs' mean surface normal (zero when none is planted on anything).
    glm::vec3 MeanNormal() const;
    // Legs of `group` mid-step now.
    int Stepping(int group) const;
    // Steps finished since the last call (for footsteps), and how many frames ever had both groups lifted at once.
    int TakeLanded() { const int n = m_Landed; m_Landed = 0; return n; }
    int BothGroupsLifted() const { return m_BothLifted; }

private:
    void FindFoothold(const SurfaceQuery& q, const GaitSettings& s, const glm::mat4& model, const glm::vec3& bodyPos,
                      const glm::vec3& up, const glm::vec3& lead, int leg);

    std::array<LegRest, kLegs> m_Rest{};
    std::array<LegState, kLegs> m_Legs{};
    int m_NextProbe = 0, m_Landed = 0, m_BothLifted = 0;
    int m_LastGroup = 1; // the group that last lifted (the other gets the next turn)
};

// ---- rig ----

// ---- body parts ----

// Where a round lands on a spider, for its damage: the head (the cephalothorax: critical), the soft abdomen, a leg.
enum class PartZone : std::uint8_t { Head = 0, Abdomen = 1, Leg = 2 };
constexpr int kZones = 3;
constexpr int kMaxParts = 32; // PhysicsWorld::kMaxRagdollParts

// One body part (the rig's "parts"): a capsule over a run of bones that is both a hitbox while the spider lives and a
// ragdoll link once it is dead. Bone is the joint it turns about (and the bone it drives); the capsule runs from Start
// (empty: Bone) to End (empty: Length metres on along the bone).
struct PartDef {
    std::string Name, Bone, Start, End;
    float Length = 0.0f, Radius = 0.03f, Mass = 0.2f;
    PartZone Zone = PartZone::Leg;
    int Parent = -1;            // index into the parts (-1: the root, exactly one)
    int Leg = -1, Segment = 0;  // a leg's part: which leg (the rig's order) and which of its segments from the hip
    float Swing = 30.0f, Twist = 15.0f; // the ragdoll joint's cone and twist (degrees)
    float Curl = 0.0f;          // degrees the joint folds in the death curl (positive: the far end down and in)
};

// Spider.rig.json: which of the model's bones are which (the pack's are only numbered).
struct RigDef {
    std::string Thorax, Abdomen;
    std::string StanceClip;      // the clip whose first frame is the standing stance (empty: the bind pose)
    std::array<std::string, kLegs> LegNames{};
    std::array<std::vector<std::string>, kLegs> LegBones{};
    std::array<int, kLegs> LegGroup{};
    std::vector<PartDef> Parts; // empty: the rig names none (no hitboxes, no ragdoll)
    // Damage x by zone, and the flinch's peak (degrees) by zone and its length (seconds).
    float ZoneMultiplier[kZones] = {2.5f, 1.2f, 0.5f};
    float FlinchDeg[kZones] = {10.0f, 8.0f, 28.0f};
    float FlinchTime = 0.35f;
};
const char* ZoneName(PartZone z);
// Parses the file's text. False (with `error`) when it is not a rig of eight legs in two gait groups.
bool ParseRig(const std::string& json, RigDef& out, std::string* error = nullptr);

} // namespace Spider
