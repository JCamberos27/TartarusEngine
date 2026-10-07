#pragma once
#include <memory>
#include <glm/glm.hpp>

class Shader;

// The grid's colours, passed in by the editor so the Renderer stays free of Editor headers. The
// defaults are the Enhancers palette's red / green / blue (EditorTheme::AxisX/Y/Z) and a calm grey.
struct GridColors {
    glm::vec3 AxisX = glm::vec3(0xE5, 0x48, 0x4D) / 255.0f;
    glm::vec3 AxisY = glm::vec3(0x4C, 0xC3, 0x8A) / 255.0f;
    glm::vec3 AxisZ = glm::vec3(0x4C, 0x8D, 0xF6) / 255.0f;
    glm::vec3 Lines = glm::vec3(0.42f);
    float AxisAlpha = 0.7f; // the axis lines' peak opacity, so they sit in the view rather than on it
};

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
              float opacity, bool showAxisLines, float axisThickness,
              const GridColors& colors = GridColors());

private:
    unsigned int m_VAO = 0;
    std::unique_ptr<Shader> m_Shader;
};
