#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <vector>

#include "Animation.h" // LocalTRS

class Model;
class NpcBody;

// A soldier going down: the body's pose at the moment of death becomes eleven jointed capsules
// (pelvis, chest, head, upper and lower arms, thighs and calves) in the physics world, carrying the
// body's velocity and the round's shove, and from then on the physics drives the pose - every outfit
// piece's bones follow the capsule they belong to (bones in between keep their last animated shape).
//
// The death is a powered blend, not a cut: the joints start with slerp drives holding the pose the body died in
// (so a soldier hit running keeps its stride for a beat and falls as it was), and the drives fade to nothing over
// kDriveFade seconds, after which it is a rag.
class NpcRagdoll {
public:
    ~NpcRagdoll();
    // `velocity`: the body's (m/s); `impulse` (N s) at `point` (world) on part `hitPart` (-1: the part nearest
    // `point`).
    bool Start(NpcBody& body, const glm::vec3& velocity, const glm::vec3& impulse, const glm::vec3& point, int hitPart = -1);
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
    // The drive's strength now, 1 at the moment of death to 0 once faded.
    float DriveLeft() const { return m_Drive; }
    // How the drives fade: seconds from death to limp.
    static constexpr float kDriveFade = 0.25f;

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
