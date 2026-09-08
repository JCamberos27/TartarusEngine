#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>

class Shader;
class World;

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

    void Draw(const glm::mat4& view, const glm::mat4& proj, const World& world);

private:
    unsigned int m_VAO = 0;
    unsigned int m_VBO = 0;
    size_t m_Capacity = 0; // current VBO size in glm::vec3 units
    std::unique_ptr<Shader> m_Shader;
    std::vector<glm::vec3> m_Verts; // interleaved: pos, colour, pos, colour, ... (2 per endpoint)
};
