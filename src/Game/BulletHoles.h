#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <functional>
#include <vector>

class World;
struct RuntimeDecal;

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
        int Decal = -1;     // a textured hole (KnifeFxLibrary id, ImpactFx) instead of the procedural one
        float Size = 0.1f;  // its decal's size across, metres
        float Rim = 0.0f;   // how far out the decal's chipped rim shows (cell half-widths; 0 = all of it)
    };

    // `entity` is what the round hit (entt::null or an invalid one: the hole stays in world space). `decal` >= 0: draw
    // that textured hole, `size` metres across, instead of the procedural one.
    void Add(const World& world, entt::entity entity, const glm::vec3& point, const glm::vec3& normal,
             float radius = 0.0045f, int decal = -1, float size = 0.1f, float rim = 0.0f);
    void Clear() { m_Holes.clear(); m_Next = 0; }
    std::size_t Size() const { return m_Holes.size(); }
    // This frame's world-space holes, dropping any whose entity is gone.
    void Resolve(const World& world, std::vector<Placed>& out);

    // This frame's holes as decals: the box placed from the hole (+Z out of the surface, its spin from the seed), the
    // cell from the seed, the rim cut. `look` fills the maps and the response for a Knife hole (`decal` >= 0) or the
    // procedural one (-1); false skips that hole this frame (its maps are still being made). Returns how many.
    using LookFn = std::function<bool(int decal, RuntimeDecal& r)>;
    int Emit(const World& world, std::vector<RuntimeDecal>& out, const LookFn& look);
    // Emit through BloodRenderer::HoleLook into the current DecalSystem's runtime list (after BloodRenderer::BeginFrame).
    int Submit(const World& world);
    // A box's projection depth: a Knife hole's 6 cm (as the old blood decal pass drew it), the procedural one's 3 cm.
    static constexpr float kKnifeDepth = 0.06f, kProceduralDepth = 0.03f;

private:
    struct Hole {
        entt::entity Entity = entt::null;
        glm::vec3 Position{0.0f}, Normal{0.0f, 1.0f, 0.0f}, Tangent{1.0f, 0.0f, 0.0f}; // entity space
        float Seed = 0.0f;
        float Radius = 0.0045f;
        int Decal = -1;
        float Size = 0.1f;
        float Rim = 0.0f;
    };
    std::vector<Hole> m_Holes;
    std::size_t m_Next = 0; // the slot the next hole overwrites once full
    unsigned m_Count = 0;   // holes ever added: the seed sequence
};
