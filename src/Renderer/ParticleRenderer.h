#pragma once
#include "Texture.h"
#include <glm/glm.hpp>

#include <future>
#include <memory>
#include <string>
#include <unordered_map>

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

    // Returns the number of particles drawn. Systems tagged ViewModelTag (the player's own muzzle
    // flame) belong to the view-model sub-pass when the view runs one (`viewModelPass`): they're
    // left out of the world pass and drawn by a second call with `drawViewModel` set, at the view
    // model's projection and depth-tested against the arms and gun. OwnerViewOnlyTag /
    // HiddenFromOwnerTag systems are shown only in / kept out of the player's own camera.
    int Draw(const World& world, const RenderFrameContext& ctx, bool viewModelPass = false, bool drawViewModel = false);
    // The programs compiled now (Play's start), not on the first muzzle flame mid-fight, and the textures of the
    // scene's particle systems queued for decoding.
    void WarmUp(const World& world);

private:
    void EnsureCreated();
    // A particle texture, decoded on a worker the first time it's asked for: false while that decode runs (the
    // particle waits a frame or two instead of the frame hitching on a file decode); then true, with `out` the
    // texture or null when it failed to load. `data`: linear (flame / channel-mask textures), else sRGB colour.
    bool ParticleTexture(const std::string& path, bool data, Texture*& out);
    struct TextureSlot {
        std::shared_ptr<Texture> Tex;       // set once the decode is uploaded
        std::future<TextureCpuData> Decode; // valid while the worker runs
    };
    std::unordered_map<std::string, TextureSlot> m_Textures[2]; // [0] colour, [1] data; keyed by project path
    std::unique_ptr<Shader> m_Shader;
    std::unique_ptr<Shader> m_SmokeShader;
    unsigned int m_Vao = 0;
    unsigned int m_Vbo = 0;
    size_t m_Capacity = 0; // instances the VBO can hold
};
