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
class ColliderGizmo {
public:
    ColliderGizmo();
    ~ColliderGizmo();

    void Draw(const glm::mat4& view, const glm::mat4& proj, const World& world,
              const glm::vec3& color = glm::vec3(0.35f, 0.9f, 0.35f));

private:
    unsigned int m_VAO = 0;
    unsigned int m_VBO = 0;
    size_t m_Capacity = 0; // current VBO size in vertices
    std::unique_ptr<Shader> m_Shader;
    std::vector<glm::vec3> m_Lines; // reused across frames; pairs of endpoints
};
