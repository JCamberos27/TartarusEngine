#pragma once
#include <cstddef>
#include <memory>
#include <glm/glm.hpp>

class Shader;

// Draws a world-space line list (GL_LINES, 7 floats per vertex: pos.xyz, rgba) in one call, straight
// alpha-over so a pure-colour line renders as that colour and alpha carries any fade. It knows nothing
// about what the lines show: the App's DebugLineGather builds them (colliders, physics and audio debug
// channels) and ViewPipeline hands them here. The caller sets depth test / depth writes.
class DebugLineRenderer {
public:
    DebugLineRenderer();
    ~DebugLineRenderer();
    DebugLineRenderer(const DebugLineRenderer&) = delete;
    DebugLineRenderer& operator=(const DebugLineRenderer&) = delete;

    void Draw(const glm::mat4& viewProj, const float* verts, size_t floatCount);

private:
    unsigned int m_VAO = 0;
    unsigned int m_VBO = 0;
    size_t m_Capacity = 0; // current VBO size in floats
    std::unique_ptr<Shader> m_Shader;
};
