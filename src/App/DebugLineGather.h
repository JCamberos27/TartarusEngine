#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

class World;

// The debug lines the collider overlay shows (#185), as a world-space GL_LINES list for the Renderer's
// DebugLineRenderer (7 floats per vertex: pos.xyz, rgba). Lives in the App because it reads Physics
// (trigger occupancy, the character capsule, the visual-debugger channels), Audio (reverb zones, probe
// rays) and Game (BodyDebug) state; the Renderer only draws the result. Rebuilt from the live registry
// on every call, so it always reflects the current collider data.
//
// Wireframes: solid colliders green, trigger colliders yellow, a trigger with something inside it
// orange (#185 PR 5), plus the Player's kinematic capsule.
class DebugLineGather {
public:
    // drawShapes false = skip the collider wireframes (Colliders toggle off) but still gather the
    // contact crosses + physics debug-draw channels, which have their own toggles (#185).
    // `only`, when set, limits the wireframes to entities it returns true for (Colliders > selected
    // only); the contact crosses and debug channels are not filtered. Replaces `out`.
    void Gather(const World& world, bool drawShapes, const std::function<bool(entt::entity)>* only,
                std::vector<float>& out);

private:
    // #185 - model-space UNIQUE edges of a convex-hull / triangle-mesh collider, built once per
    // Model from its cooked collision geometry and then just transformed each frame. An empty
    // list means "too dense - draw the bounds box instead". Keyed by the import every instance
    // shares (Model::ImportId); its ImportStamp tells a reimport, or a new import at a freed address, apart.
    struct MeshEdges {
        std::uint64_t Stamp = 0; // 0: not built yet (stamps start at 1)
        std::vector<glm::vec3> Edges;
    };
    std::unordered_map<const void*, MeshEdges> m_MeshEdgeCache;
};
