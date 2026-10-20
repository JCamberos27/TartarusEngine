#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

class Shader;
class World;
struct RenderFrameContext;
struct FrameState;

// Froxel volumetric fog: light shafts and glowing air from the sun / moon and every point and spot
// light, shadowed by their shadow maps. Scene settings live on World (Volumetric*), each light's
// share on LightComponent::VolumetricIntensity.
//
// Per view, once per frame, before the opaque pass (SceneRenderer::RenderScene):
//   1. VolumetricInject.comp: one thread per froxel of a W x H x D grid aligned to the view
//      frustum (depth sliced exponentially from kNear to World::VolumetricDistance). Each places
//      a blue-noise-jittered sample in its froxel, evaluates density / albedo and in-scatter from
//      the lights (directional + its cascades, the clustered point/spot lists with their shadow
//      maps, the sky ambient) through a Henyey-Greenstein phase, and blends with the previous
//      frame's volume, reprojected.
//   2. VolumetricIntegrate.comp: front-to-back along each froxel column, into a volume whose
//      texel k holds (in-scatter, transmittance) from the eye to slice k's far boundary.
// Then every lit fragment (opaque and transparent - ModelShading.glsl's ApplyVolumetricFog) applies
// it as c * transmittance + in-scatter, and DrawSky() does the same for the sky's pixels with a
// far-plane triangle. Applying it in the forward shading rather than as one full-screen pass keeps
// it per MSAA sample and lets the transparent pass, which blends over the fogged opaque scene,
// fog its own surfaces at their own depth.
//
// Every pass here is a compute dispatch or a draw into the bound framebuffer; nothing changes the
// framebuffer binding or the viewport.
class VolumetricFog {
public:
    struct QualityLevel { int W, H, D; float Blend; };
    // World::VolumetricQuality 0 Low, 1 Medium, 2 High.
    static QualityLevel Quality(int level);
    // The first slice's start: closer than this the fog fades in from nothing.
    static constexpr float kNear = 0.15f;
    // The integrated volume's texture unit, read by ModelShading.glsl (kept out of the material
    // units: ShaderAsset stops at 26).
    static constexpr int kVolumeUnit = 27;

    VolumetricFog();
    ~VolumetricFog();
    VolumetricFog(const VolumetricFog&) = delete;
    VolumetricFog& operator=(const VolumetricFog&) = delete;

    // Whether this view gets volumetrics: the world has them on, the view is a perspective,
    // lit one with a viewport, and it has an HDR target to key its history on.
    static bool WantedFor(const World& world, const RenderFrameContext& ctx, const int viewport[4]);

    // Runs both compute passes for the view `fs` describes. Needs the light SSBO (binding 0) and,
    // when fs.clusterOn, this view's cluster lists (bindings 3, 4) already filled. Leaves texture
    // units 6-11 changed: invalidate GLStateCache afterwards.
    void Render(const FrameState& fs);

    // Binds this view's integrated volume on kVolumeUnit and sets the program's uVol* uniforms;
    // `enabled` false (or no volume for this view) switches the shader's apply off.
    void ApplyToProgram(Shader& program, const void* viewKey, bool enabled) const;
    // For a program that never draws with the fog: switch its apply off and keep a valid 3D texture on the unit.
    static void ApplyDisabled(Shader& program);

    // The fog over the sky: a far-plane triangle blended over whatever the sky drew. Call right
    // after the sky, with the scene's depth bound.
    void DrawSky(const void* viewKey, const int viewport[4]);

    // Compiles the programs and builds the blue-noise texture now (load-time warm-up).
    void WarmUp() { EnsureResources(); }

    // How many times any view's froxel passes have run (the editor smoke test checks they do).
    static unsigned long long RenderCount();

    // The 32 x 32 blue-noise ranks (0..1023, each once): exposed for the unit tests.
    static void BuildBlueNoise(unsigned short out[32 * 32]);

private:
    struct ViewState {
        unsigned int Scatter[2] = {0, 0}; // pass 1 output, ping-ponged as each other's history
        unsigned int Integrated = 0;      // pass 2 output, what the shading reads
        int Current = 0;
        int W = 0, H = 0, D = 0;
        float Far = 0.0f;
        float PixelsPerFroxel = 1.0f;     // the viewport width over W
        bool HasHistory = false;
        bool Rendered = false;            // this view has a volume from this frame
        unsigned Frame = 0;
        unsigned LastUsed = 0;
        glm::mat4 ViewProj{1.0f};         // this frame's, what the apply projects with
        glm::mat4 PrevViewProj{1.0f};
        glm::vec3 PrevCamera{0.0f};
        void Release();
        ~ViewState() { Release(); }
    };

    void EnsureResources();
    ViewState& View(const void* key);

    std::unique_ptr<Shader> m_InjectShader;
    std::unique_ptr<Shader> m_IntegrateShader;
    std::unique_ptr<Shader> m_SkyShader;
    unsigned int m_BlueNoise = 0;
    unsigned int m_EmptyVao = 0;
    unsigned m_Tick = 0;
    std::unordered_map<const void*, std::unique_ptr<ViewState>> m_Views;
};
