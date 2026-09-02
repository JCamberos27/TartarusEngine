#pragma once
#include <memory>
#include <glm/glm.hpp>

class Shader;

// Shader-based "infinite" ground grid: draws a single full-screen triangle pair with no
// vertex buffer, reconstructs world position per-pixel from the inverse view-projection,
// and rules grid lines + a distance fade directly in the fragment shader. Editor-only —
// never drawn during gameplay.
class Grid {
public:
    Grid();
    ~Grid();

    void Draw(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos,
              float minorSpacing, float majorEvery, float fadeDistance,
              float opacity, bool showAxisLines, float axisThickness);

private:
    unsigned int m_VAO = 0;
    std::unique_ptr<Shader> m_Shader;
};
