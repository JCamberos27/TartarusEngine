#pragma once
#include <memory>
#include <glm/glm.hpp>

class Shader;

// Shader-based vertical gradient sky: draws a single full-screen triangle pair with no
// vertex buffer (same "unproject near/far per-pixel" technique as Grid), so it's resolution-
// and camera-independent. Draw first, before any scene geometry, with depth writes off.
class Sky {
public:
    Sky();
    ~Sky();

    void Draw(const glm::mat4& view, const glm::mat4& proj,
              const glm::vec3& horizonColor, const glm::vec3& zenithColor);

private:
    unsigned int m_VAO = 0;
    std::unique_ptr<Shader> m_Shader;
};
