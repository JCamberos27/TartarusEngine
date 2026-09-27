#pragma once

#include <glm/glm.hpp>
#include <memory>

class Shader;
struct SkySettings;
struct SkyLighting;

// Volumetric clouds for the physical sky (SkyAtmosphere owns one). See CloudsCommon.glsl for
// the density and lighting model.
//
// GPU resources:
//   - noise, generated once at first use: 128^3 Perlin-Worley shape, 32^3 Worley detail, and a
//     512^2 weather map (CloudNoise.comp);
//   - a uniform block (binding 3) with this frame's layer, shape, wind and lighting parameters;
//   - a cloud shadow map (R16F) over the ground around the camera, sampled by the model shader;
//   - per view (ViewState): the reduced-resolution raymarch target, its depth, and two history
//     targets that CloudTemporal.comp ping-pongs between.
//
// Every pass here is a compute dispatch: nothing binds a framebuffer or changes the viewport.
class VolumetricClouds {
public:
    // Resolution divisor, primary and light-march step counts per SkySettings::CloudQuality.
    struct QualityLevel { int Divisor; int Steps; int LightSteps; float Blend; };
    static QualityLevel Quality(int level);

    static constexpr int kShadowMapSize = 384;

    // Per-view temporal state. Owned by the caller (SkyAtmosphere keeps one per view).
    struct ViewState {
        unsigned int Color = 0, Depth = 0; // this frame's raymarch
        unsigned int History[2] = {0, 0};
        int Current = 0;                   // History[Current] holds the latest resolved result
        int Width = 0, Height = 0;
        bool HasHistory = false;
        glm::mat4 PrevViewProj{1.0f};
        glm::vec3 PrevCamera{0.0f};
        void Release();
        ~ViewState() { Release(); }
    };

    VolumetricClouds();
    ~VolumetricClouds();
    VolumetricClouds(const VolumetricClouds&) = delete;
    VolumetricClouds& operator=(const VolumetricClouds&) = delete;

    // Advances the wind (weather drift, shape evolution, cirrus) by dt seconds.
    void Tick(const SkySettings& s, float dt);

    // Uploads the cloud uniform block and binds it at binding 3. Needs the lighting resolved
    // for this frame (the clouds are lit by the sun, or by the moon at night).
    void UpdateUniforms(const SkySettings& s, const SkyLighting& lit);

    // Binds the noise textures to units 4, 5 and 6 (the CloudsCommon.glsl contract).
    void BindNoise();

    // Renders the cloud shadow map centred on `cameraWorld`. The atmosphere's block and LUTs
    // (units 0-3) must already be bound.
    void RenderShadowMap(const SkySettings& s, const glm::vec3& cameraWorld);

    // Raymarches + temporally resolves the clouds for one view into `state`. The atmosphere's
    // block, LUTs and this view's sky-view LUTs must already be bound. Returns the texture the
    // composite should sample (rgb radiance, a transmittance).
    unsigned int RenderView(ViewState& state, const SkySettings& s, const glm::mat4& view,
                            const glm::mat4& proj, const glm::vec3& cameraWorld, int viewWidth,
                            int viewHeight, float frame, float worldUnitsPerKm);

    unsigned int ShadowMap() const { return m_ShadowMap; }
    // xy = world XZ at the shadow map's centre, z = 1 / world extent, w = strength (0 = off).
    glm::vec4 ShadowParams() const { return m_ShadowParams; }

    bool NoiseReady() const { return m_NoiseReady; }

private:
    void EnsureResources();
    void GenerateNoise();

    std::unique_ptr<Shader> m_NoiseShader;
    std::unique_ptr<Shader> m_RaymarchShader;
    std::unique_ptr<Shader> m_TemporalShader;
    std::unique_ptr<Shader> m_ShadowShader;

    unsigned int m_ShapeNoise = 0;
    unsigned int m_DetailNoise = 0;
    unsigned int m_Weather = 0;
    unsigned int m_ShadowMap = 0;
    unsigned int m_Ubo = 0;
    bool m_NoiseReady = false;

    glm::vec2 m_WeatherOffset{0.0f}; // km
    glm::vec2 m_ShapeOffset{0.0f};   // km
    glm::vec2 m_CirrusOffset{0.0f};  // km
    glm::vec4 m_ShadowParams{0.0f};
};
