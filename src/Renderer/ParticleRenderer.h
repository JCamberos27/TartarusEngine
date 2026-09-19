#pragma once
#include <glm/glm.hpp>

class World;
class Shader;
struct RenderFrameContext;

// #177 - draws every ParticleSystemComponent's live particles as camera-facing soft discs, one
// instanced draw per blend mode (alpha-blended ones sorted back to front). Depth-tested against
// the scene, no depth write. Call after the transparent pass with the HDR target bound.
class ParticleRenderer {
public:
    ParticleRenderer() = default;
    ~ParticleRenderer();
    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // Returns the number of particles drawn.
    int Draw(const World& world, const RenderFrameContext& ctx);

private:
    void EnsureCreated();
    Shader* m_Shader = nullptr;
    unsigned int m_Vao = 0;
    unsigned int m_Vbo = 0;
    size_t m_Capacity = 0; // instances the VBO can hold
};
