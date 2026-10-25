#pragma once

#include "SpiderLocomotion.h" // PartDef

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>

namespace PhysicsWorld { struct RagdollPart; struct RagdollParams; }

// A dead spider's body: its rig's part table (Spider.rig.json "parts") as one PhysX articulation of up to 32 capsules,
// powered for a moment by a limp motor that folds the legs in under the body - the curl of a dead spider - and then lets
// them go slack, settling and sleeping where it lies. Pure physics: the skin write-back is SpiderDamage's.
namespace SpiderRagdoll {

// Where each part's bones stand at death (world): the joint it turns about (Anchor), and its capsule's two ends.
struct PartPoint {
    glm::vec3 Anchor{0.0f}, A{0.0f}, B{0.0f};
};

// The capsule over `a` .. `b` of radius `radius`: centre, rotation (+X along it, +Y as near `up` as it can be) and the
// cylinder's half length (the caps reach a little past the ends: the parts overlap at their joints, never gap).
void CapsuleOver(const glm::vec3& a, const glm::vec3& b, float radius, const glm::vec3& up, glm::vec3& centre, glm::quat& rot,
                 float& halfLength);

// The physics parts for `count` parts `defs` posed at `points`, in a body whose up is `bodyUp` (the surface's normal it
// stood on: down on a ceiling). Each joint's cone is wide enough to hold its curl. A leg's parts collide with the
// thorax and abdomen but not with the other legs' (eight legs folding in at once would lock into each other and kick).
// `partWorld` (optional) gets each capsule's pose. False on a bad table.
bool BuildParts(const Spider::PartDef* defs, int count, const PartPoint* points, const glm::vec3& bodyUp,
                PhysicsWorld::RagdollPart* out, glm::mat4* partWorld = nullptr);
// The body's tuning (solver, damping, friction, skin).
PhysicsWorld::RagdollParams BodyParams();

// The death curl's drive target of part `def` (its joint's rotation from the death pose, in the joint's frame: the
// capsule's own at build, `partRot`): each leg joint folding by its Curl degrees in the plane of its leg, the far end
// down toward the body's underside and in. `outward`: the leg's direction away from the body (world, across `bodyUp`).
glm::quat CurlTarget(const Spider::PartDef& def, const glm::quat& partRot, const glm::vec3& bodyUp, const glm::vec3& outward);

// A limp motor for any ragdoll: every powered joint driven toward its own target, its strength rising in RiseTime to
// full, held until HoldTime, then fading to Residual by FadeTime (a dead body's last tone), each part weighted.
class LimpMotor {
public:
    struct Settings {
        float Stiffness = 600.0f, Damping = 40.0f; // the drives' (acceleration units: x what each joint turns)
        float RiseTime = 0.08f, HoldTime = 0.9f, FadeTime = 2.4f, Residual = 0.12f;
    };
    // Drives ragdoll `id`'s `count` parts toward `targets` (joint frame, from the built pose), part i at `weight[i]` (0: limp).
    void Begin(int id, int count, const glm::quat* targets, const float* weight, const Settings& s);
    void Update(float dt);
    float Strength() const { return m_Strength; } // 0..1 now
    float Time() const { return m_Time; }
    static float StrengthAt(const Settings& s, float t);

private:
    int m_Id = -1, m_Count = 0;
    std::array<float, Spider::kMaxParts> m_Weight{};
    Settings m_Set;
    float m_Time = 0.0f, m_Strength = 0.0f, m_Applied = -1.0f;
};

// The corpse coming to rest: once its parts have slowed it heavies up (damping), and when it has been still a moment it is
// put to sleep (PhysicsWorld freezes it later). A shove (a round into it) starts that over.
class Settler {
public:
    void Begin(int id) { *this = Settler(); m_Id = id; }
    // rootPart / upInRoot: the body's up, in its root part's frame - a corpse come to rest on its side is rolled gently
    // onto its belly or its back (whichever is nearer), and never put to sleep on a side.
    void Update(float dt, int rootPart = -1, const glm::vec3& upInRoot = glm::vec3(0.0f, 1.0f, 0.0f));
    void Wake() { m_Still = 0.0f; m_Asleep = false; m_Heavy = -1.0f; m_Time = std::min(m_Time, 0.6f); }
    bool Asleep() const { return m_Asleep; }
    float AsleepAt() const { return m_AsleepAt; } // seconds from its start it first slept (-1: not yet)

private:
    int m_Id = -1;
    float m_Time = 0.0f, m_Still = 0.0f, m_Heavy = -1.0f, m_AsleepAt = -1.0f;
    bool m_Asleep = false;
    bool m_OnSide = false, m_Reached = false; // on its side; rolled flat (left to settle)
    float m_Age = 0.0f; // (since Begin - Wake winds m_Time back)
    float m_SideFor = 0.0f; // seconds it has been pushed over off its side
    float m_Target = 0.0f, m_Best = -2.0f, m_BestAt = 0.0f, m_PrevOff = -1.0f; // the flat it is rolled to (+1 belly, -1 back), its best yet, when
};

} // namespace SpiderRagdoll
