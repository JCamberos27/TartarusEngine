#pragma once

#include "NpcRagdoll.h" // kParts, the part table

#include "PhysicsWorld.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

class NpcBody;

// A living soldier's hitboxes: the same eleven capsules the ragdoll is built from (NpcRagdoll's part table), as
// kinematic, query-only bodies in the physics world, posed from the animated skeleton after its late pose. The player's
// rounds hit these (PhysicsWorld::RaycastBodyParts says which) instead of the character's movement capsule.
class NpcHitboxes {
public:
    ~NpcHitboxes();
    bool Start(const NpcBody& body, PhysicsWorld::CharacterId capsule);
    void Stop();
    bool Valid() const { return m_Id >= 0; }
    bool Active() const { return m_Active; }
    // Off: the capsule answers queries again (a soldier too far to be worth the bodies).
    void SetActive(bool active);
    // The skeleton was just posed: re-cut the capsules from it and place them.
    void Pose(const NpcBody& body);
    // The body moved but wasn't re-posed this frame (animation skipped): the last capsules, carried with the root.
    void Follow(const NpcBody& body);
    // Part `part`'s capsule centre (world), as last placed; zero before the first pose.
    glm::vec3 Centre(const NpcBody& body, int part) const;

    // The hitboxes are a little fatter than the ragdoll's bodies: a round that grazes a limb should count.
    static constexpr float kFatten = 1.12f;

private:
    void Place(const NpcBody& body);
    int m_Id = -1;
    bool m_Active = false, m_HavePose = false;
    int m_Node[NpcRagdoll::kParts], m_End[NpcRagdoll::kParts], m_Neck = -1;
    NpcPartShape m_Local[NpcRagdoll::kParts]; // root space
    // The nodes the capsules read (the bones and their ancestors, parents first) and the model's parents, so a pose
    // costs a few dozen matrices, not the whole skeleton's.
    std::vector<int> m_Path, m_Parents;
    std::vector<glm::mat4> m_Globals;
};
