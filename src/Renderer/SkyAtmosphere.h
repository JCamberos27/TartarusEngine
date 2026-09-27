#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "SkySettings.h"
#include "VolumetricClouds.h"

class Shader;

// This frame's sun, moon and scene light for the physical sky, resolved on the CPU by
// SkyAtmosphere::ResolveLighting before the light buffer is built.
struct SkyLighting {
    glm::vec3 SunDir{0.0f, 1.0f, 0.0f};  // toward the sun
    glm::vec3 MoonDir{0.0f, -1.0f, 0.0f}; // toward the moon
    glm::vec3 SunIlluminance{5.0f};      // above the atmosphere
    glm::vec3 MoonIlluminance{0.0f};     // full-moon illuminance above the atmosphere
    float MoonLitFraction = 0.0f;        // phase: 0 new .. 1 full
    float SunAngularRadius = 0.00462f;   // radians, for the disc
    float MoonAngularRadius = 0.0045f;
    glm::mat3 StarRotation{1.0f};

    // The scene's directional light this frame: the sun by day, the moon when it outshines the
    // (set) sun. Direction TOWARD the light; radiance = what reaches the camera's altitude.
    glm::vec3 LightDir{0.0f, 1.0f, 0.0f};
    glm::vec3 LightRadiance{0.0f};
    bool LightIsMoon = false;

    // What lights the clouds (same body as the scene light), above the atmosphere; the clouds
    // apply the atmosphere's transmittance themselves at their own altitude.
    glm::vec3 CloudLightDir{0.0f, 1.0f, 0.0f};
    glm::vec3 CloudLightIlluminance{0.0f};

    float ClockHours = 0.0f; // the time of day actually shown (authored + running clock)
    // 2^NightBrightnessStops: applied to the sky's radiance and to LightRadiance.
    float NightBoost = 1.0f;
};

// The physical sky (World::SkySource::Atmosphere): a Hillaire-2020 atmosphere, volumetric
// clouds, sun/moon/stars, the time-of-day clock, aerial perspective on the scene, cloud shadows,
// and the environment cube the IBL probes are baked from. main.cpp owns one for the process.
//
// Per frame, in order:
//   1. Tick()             - clock and wind.
//   2. ResolveLighting()  - sun/moon directions, the directional light's colour and aim.
//   3. PrepareFrame()     - LUTs when the atmosphere changed, the cloud shadow map, and the
//                           environment capture when the sky has changed enough to re-bake.
//   4. per view: RenderView() (inside the scene pass, in place of the gradient sky) and
//      ApplyToProgram() for each model program.
//
// GL contract: every pass is compute except RenderView's final full-screen draw, which renders
// into whatever framebuffer + viewport the caller has bound and restores the depth state it
// touches. Texture units 0-9 and image units 0-1 are clobbered; GLStateCache is invalidated.
class SkyAtmosphere {
public:
    static constexpr int kEnvSize = 128;
    // Model-shader texture units: the top two of the 32 every GL 4.6 GPU has, which material
    // texture allocation stops short of (ShaderAsset::BuildBindings, Model.cpp's fixed 16-18).
    static constexpr int kAerialUnit = 30;
    static constexpr int kCloudShadowUnit = 31;

    SkyAtmosphere();
    ~SkyAtmosphere();
    SkyAtmosphere(const SkyAtmosphere&) = delete;
    SkyAtmosphere& operator=(const SkyAtmosphere&) = delete;

    // Advances the clock (while Play runs, or always with AnimateInEditor) and the wind.
    // Leaving Play resets the clock to the authored time.
    void Tick(const SkySettings& s, float dt, bool playing);

    // `authoredAim` = the scene's directional light's travel direction (its -Z), used as the sun
    // when the clock is off; `sunIlluminance` = its colour * intensity (the sun above the
    // atmosphere); `angularSizeDeg` = its Angular Size. `cameraWorld` sets the altitude the
    // light is filtered to.
    SkyLighting ResolveLighting(const SkySettings& s, const glm::vec3& authoredAim,
                                const glm::vec3& sunIlluminance, float angularSizeDeg,
                                const glm::vec3& cameraWorld) const;

    // Once per frame, before any view: LUTs, cloud shadows, environment capture.
    void PrepareFrame(const SkySettings& s, const SkyLighting& lit, const glm::vec3& cameraWorld);

    // True when PrepareFrame captured a new environment cube this frame: the caller convolves
    // it (IblProbe::BakeFromCubemap(EnvironmentCube(), kEnvSize, 0)) and calls
    // MarkEnvironmentBaked(). ForceEnvironmentCapture() re-captures next frame (switching into
    // this sky mode, scene load).
    bool EnvironmentDirty() const { return m_EnvDirty; }
    unsigned int EnvironmentCube() const { return m_EnvCube; }
    void MarkEnvironmentBaked() { m_EnvDirty = false; }
    void ForceEnvironmentCapture() { m_ForceCapture = true; }

    // Renders one view's sky: its sky-view LUTs, aerial-perspective volume and clouds, then the
    // full-screen sky into the bound framebuffer. `viewKey` identifies the view across frames
    // (for the clouds' temporal history); any stable pointer, e.g. the view's HDR target.
    void RenderView(const void* viewKey, const glm::mat4& view, const glm::mat4& proj,
                    const glm::vec3& cameraWorld);

    // Model-shader state for the view rendered by the last RenderView(viewKey): the aerial
    // perspective volume and the cloud shadow map (kAerialUnit / kCloudShadowUnit), plus their
    // uniforms.
    void ApplyToProgram(Shader& program, const void* viewKey, const int viewport[4]) const;
    // The same units with neutral textures and the effects switched off (other sky modes).
    static void ApplyDisabled(Shader& program);

    // Direct transmittance of the atmosphere from `altitudeKm` above sea level toward `dir`
    // (CPU, for the sun light's colour and for tests). Zero once the planet is in the way.
    static glm::vec3 Transmittance(const SkySettings& s, float altitudeKm, const glm::vec3& dir);

    // Camera altitude above sea level in km for the settings' sea level and altitude offset.
    static float CameraAltitudeKm(const SkySettings& s, const glm::vec3& cameraWorld);

    // Stops of night brightening for a sun direction (SkySettings::NightBrightness, eased in as
    // the sun sets through twilight).
    static float NightBrightnessStops(const SkySettings& s, const glm::vec3& sunDir);

    const VolumetricClouds& Clouds() const { return m_Clouds; }
    float ClockOffsetHours() const { return m_ClockOffsetHours; }

private:
    struct ViewResources {
        unsigned int SkyViewSun = 0, SkyViewMoon = 0, Aerial = 0;
        unsigned int SkyAmbient = 0; // 3 x 1, SkyAmbient.comp.glsl
        VolumetricClouds::ViewState Clouds;
        unsigned int CloudTexture = 0; // this frame's resolved clouds (owned by Clouds)
        bool CloudsValid = false;
        std::uint64_t LastUsedFrame = 0;
        void Release();
        ~ViewResources() { Release(); }
    };

    void EnsureResources();
    void UpdateAtmosphereUniforms(const SkySettings& s, const SkyLighting& lit, const glm::vec3& cameraWorld);
    void RenderLuts();
    void RenderSkyViewLuts(ViewResources& vr);
    void BindLuts(const ViewResources& vr);
    void CaptureEnvironment(const SkySettings& s, ViewResources& frameView);
    ViewResources& View(const void* key);

    std::unique_ptr<Shader> m_TransmittanceShader;
    std::unique_ptr<Shader> m_MultiScatterShader;
    std::unique_ptr<Shader> m_SkyViewShader;
    std::unique_ptr<Shader> m_SkyAmbientShader;
    std::unique_ptr<Shader> m_AerialShader;
    std::unique_ptr<Shader> m_EnvShader;
    std::unique_ptr<Shader> m_CompositeShader;

    unsigned int m_Ubo = 0;
    unsigned int m_TransmittanceLut = 0;
    unsigned int m_MultiScatterLut = 0;
    unsigned int m_EnvCube = 0;
    unsigned int m_Vao = 0;

    VolumetricClouds m_Clouds;
    std::unordered_map<const void*, std::unique_ptr<ViewResources>> m_Views;
    ViewResources* m_FrameView = nullptr; // the primary camera's, used by PrepareFrame

    // What the LUTs were last built from (they only depend on the medium, not the sun).
    bool m_LutsValid = false;
    SkySettings m_LutSettings;

    // Frame state carried from PrepareFrame into the views.
    SkySettings m_Settings;
    SkyLighting m_Lighting;
    bool m_Prepared = false;
    std::uint64_t m_Frame = 0;
    float m_RealSeconds = 0.0f; // accumulated by Tick: star twinkle, capture scheduling

    // Environment capture scheduling.
    bool m_EnvDirty = false;
    bool m_ForceCapture = true;
    float m_LastCaptureSeconds = -1e9f;
    SkySettings m_CapturedSettings;
    glm::vec3 m_CapturedSunDir{0.0f}, m_CapturedMoonDir{0.0f};
    glm::vec3 m_CapturedLightIllum{0.0f};

    float m_ClockOffsetHours = 0.0f;
    bool m_WasPlaying = false;
};
