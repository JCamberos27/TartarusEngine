#pragma once
#include <memory>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

class Shader;
class World;
class Model;

// Scene-viewport wireframe overlay of every ColliderComponent's PhysX shape — box, sphere or
// capsule (#185 PR 2). Immediate-mode: each Draw() rebuilds a world-space line list from the
// live registry and uploads it in one call, so it always reflects the current collider data
// with no per-frame bookkeeping. Editor-only, gated on EditorSettings::ShowColliders; drawn in
// both edit and Play mode. Depth-tested so geometry occludes it like a real gizmo.
//
// Colour is per-vertex: solid colliders draw green, trigger colliders yellow, and a trigger
// currently occupied (something inside it, per PhysicsWorld) draws orange (#185 PR 5).
class ColliderGizmo {
public:
    ColliderGizmo();
    ~ColliderGizmo();

    // drawShapes false = skip the collider wireframes (Colliders toggle off) but still draw the
    // contact crosses + physics debug-draw channels, which have their own toggles (#185).
    void Draw(const glm::mat4& view, const glm::mat4& proj, const World& world, bool drawShapes = true);

private:
    unsigned int m_VAO = 0;
    unsigned int m_VBO = 0;
    size_t m_Capacity = 0; // current VBO size in floats
    std::unique_ptr<Shader> m_Shader;
    std::vector<float> m_Verts; // interleaved per vertex: pos.xyz, colour.rgba (7 floats)

    // #185 — model-space UNIQUE edges of a convex-hull / triangle-mesh collider, built once per
    // Model from its cooked collision geometry and then just transformed each frame. An empty
    // list means "too dense — draw the bounds box instead". Keyed by raw Model* (stable for the
    // life of a loaded asset); entries are cheap and never invalidate.
    std::unordered_map<const Model*, std::vector<glm::vec3>> m_MeshEdgeCache;
};
