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
class NpcRagdoll {
public:
    ~NpcRagdoll();
    // `velocity`: the body's (m/s); `impulse` (N s) at `point` (world) on the nearest part.
    bool Start(NpcBody& body, const glm::vec3& velocity, const glm::vec3& impulse, const glm::vec3& point);
    void Stop();
    bool Active() const { return m_Id >= 0; }
    // After the physics step (and after the animators, which are off by then): the pieces' poses.
    void Update();
    bool Asleep() const;
    // The pelvis part's position (world), for tests.
    glm::vec3 Root() const;

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
    // Scratch, reused every frame.
    std::vector<glm::mat4> m_Globals;
    std::vector<std::pair<int, int>> m_Order;
    std::vector<LocalTRS> m_Pose;
};
