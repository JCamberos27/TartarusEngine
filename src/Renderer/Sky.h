#pragma once
#include <memory>
#include <glm/glm.hpp>

class Shader;

// Shader-based vertical gradient sky: draws a single full-screen triangle pair with no
// vertex buffer (same "unproject near/far per-pixel" technique as Grid), so it's resolution-
// and camera-independent. Draw after the opaque geometry: it tests GL_LEQUAL at the far plane,
// so only uncovered pixels shade, and never writes depth.
class Sky {
public:
    Sky();
    ~Sky();

    void Draw(const glm::mat4& view, const glm::mat4& proj,
              const glm::vec3& horizonColor, const glm::vec3& zenithColor);

    // PR13: renders the HDRI cubemap as the sky background.
    // rotationRadians rotates the environment around the world Y axis.
    void DrawHdri(unsigned int cubeTex, float rotationRadians,
                  const glm::mat4& view, const glm::mat4& proj);

private:
    unsigned int m_VAO = 0;
    std::unique_ptr<Shader> m_Shader;
    std::unique_ptr<Shader> m_HdriShader; // PR13
};
