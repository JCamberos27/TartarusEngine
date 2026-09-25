#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <vector>

class World;

// The holes rounds leave where they strike: kept in the struck entity's own space, so a hole in
// a crate goes with the crate when it's knocked over and one in a wall stays put. The oldest are
// dropped past Capacity, and a hole goes with its entity when that's destroyed. Play-only state:
// the caller clears it when Play ends.
class BulletHoleList {
public:
    static constexpr std::size_t kCapacity = 512;

    struct Placed {
        glm::vec3 Position{0.0f}, Normal{0.0f, 1.0f, 0.0f}, Tangent{1.0f, 0.0f, 0.0f};
        float Seed = 0.0f; // 0..1, for each hole's own ragged edge
        float Radius = 0.0045f;
    };

    // `entity` is what the round hit (entt::null or an invalid one: the hole stays in world space).
    void Add(const World& world, entt::entity entity, const glm::vec3& point, const glm::vec3& normal,
             float radius = 0.0045f);
    void Clear() { m_Holes.clear(); m_Next = 0; }
    std::size_t Size() const { return m_Holes.size(); }
    // This frame's world-space holes, dropping any whose entity is gone.
    void Resolve(const World& world, std::vector<Placed>& out);

private:
    struct Hole {
        entt::entity Entity = entt::null;
        glm::vec3 Position{0.0f}, Normal{0.0f, 1.0f, 0.0f}, Tangent{1.0f, 0.0f, 0.0f}; // entity space
        float Seed = 0.0f;
        float Radius = 0.0045f;
    };
    std::vector<Hole> m_Holes;
    std::size_t m_Next = 0; // the slot the next hole overwrites once full
    unsigned m_Count = 0;   // holes ever added: the seed sequence
};
