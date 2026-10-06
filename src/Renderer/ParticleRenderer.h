#pragma once
#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <unordered_map>

class World;
class Shader;
class Texture;
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

    // Returns the number of particles drawn. Systems tagged ViewModelTag (the player's own muzzle
    // flame) belong to the view-model sub-pass when the view runs one (`viewModelPass`): they're
    // left out of the world pass and drawn by a second call with `drawViewModel` set, at the view
    // model's projection and depth-tested against the arms and gun. OwnerViewOnlyTag /
    // HiddenFromOwnerTag systems are shown only in / kept out of the player's own camera.
    int Draw(const World& world, const RenderFrameContext& ctx, bool viewModelPass = false, bool drawViewModel = false);

private:
    void EnsureCreated();
    Texture* ParticleTexture(const std::string& path, bool flame);
    std::unordered_map<std::string, std::shared_ptr<Texture>> m_Textures;
    Shader* m_Shader = nullptr;
    unsigned int m_Vao = 0;
    unsigned int m_Vbo = 0;
    size_t m_Capacity = 0; // instances the VBO can hold
};
