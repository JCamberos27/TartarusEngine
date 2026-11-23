#pragma once
// A scene's render and post settings: sky source, exposure and tone curve, anti-aliasing, SSAO,
// bloom, grading, depth of field, wetness, fog, volumetrics and shadows. Scene-authored (saved
// in the scene file); World::Render holds the live scene's.
#include <string>
#include <glm/glm.hpp>
#include "SkySettings.h"

struct SceneRenderSettings {
    // PR13: sky source — default Procedural (gradient) or Hdri (equirectangular .hdr file).
    // Procedural is the backwards-compatible default; existing scenes are unaffected.
    // Atmosphere: the physical sky - atmospheric scattering, volumetric clouds, sun, moon, stars
    // and a time-of-day clock, all set in Sky below (SkyAtmosphere renders it).
    enum class SkySource { Procedural = 0, Hdri = 1, Atmosphere = 2 };
    SkySource   SkySourceMode{SkySource::Procedural};
    std::string SkyHdriPath;               // absolute or project-relative path to a .hdr file
    float       SkyRotationDegrees{0.0f};  // Y-axis rotation of the HDRI in degrees [0, 360)
    // #277 - an HDRI that contains the sun also bakes it into ambient + reflections, so a scene
    // that ALSO has a directional Sun counts it twice and washes out. Auto removes the HDRI's
    // sun from the IBL bake (radiance clamped to SkyHdriSunThreshold) whenever a directional
    // light is lit; Keep never clamps; Remove always does. The visible sky is never affected.
    enum class HdriSunMode { Auto = 0, Keep = 1, Remove = 2 };
    HdriSunMode SkyHdriSun{HdriSunMode::Auto};
    float       SkyHdriSunThreshold{50.0f}; // IBL radiance ceiling; open sky is ~1-10, a sun ~1e4-1e5

    // Vertical gradient sky (see Sky.h) — horizon at the world's XZ plane, zenith straight up.
    // Default is pure black: a fresh scene reads as a dark stage, and the sky never washes out
    // the HDR tonemapper or fights a scene's own lighting until it's set deliberately.
    glm::vec3 SkyHorizonColor{0.0f, 0.0f, 0.0f};
    glm::vec3 SkyZenithColor{0.0f, 0.0f, 0.0f};

    // Multiplier on the image-based ambient term the sky probes produce (#196). 1.0 is the
    // physically-consistent value: a surface fully open to a sky of radiance L receives L.
    // Exposed because the sky colours are authored by eye, not measured, so a scene lit mostly
    // by a bright authored sky can want the ambient pulled back without darkening the sky
    // itself. Purely a shader uniform — changing it does NOT rebake the probes.
    float SkyAmbientIntensity{1.0f};

    // The physical sky's settings (SkySource::Atmosphere). Kept whatever the source, so
    // switching away and back doesn't lose them.
    SkySettings Sky;

    // Post-processing / shadow settings (#9, Phase M item 1) — scene-authored content, moved off
    // EditorSettings/editor_prefs.json onto the scene itself: these are look choices the scene's
    // author makes, not per-user editor preferences, so they belong in the file that travels with
    // the scene. See EditorLayer::DrawPostProcessSettings/DrawShadowSettings for the editing UI.
    float ExposureEV{0.0f};        // photographic stops applied before the tone curve, 0 = neutral
    int   TonemapOperator{1};      // 0 Reinhard, 1 ACES, 2 AgX
    int   MsaaSamples{4};          // 1 / 2 / 4 / 8 for the HDR target

    bool  SsaoEnabled{false};
    // #160 — SSAO tuning (were hard-coded): sample radius in view-space units, depth bias against
    // self-occlusion, and an exponent on the result (1 = as computed, 2 = darker contact shadows).
    float SsaoRadius{0.5f};
    float SsaoBias{0.025f};
    float SsaoIntensity{1.0f};

    bool  BloomEnabled{false};
    float BloomThreshold{1.0f};
    float BloomKnee{0.5f};
    float BloomIntensity{0.25f};

    // #162 - anti-aliasing and colour grading, applied in the final tonemap pass.
    bool  FxaaEnabled{false};
    bool  AutoExposure{false};          // meter the scene and ease exposure toward mid grey
    float AutoExposureMinEV{-4.0f};     // how far it may brighten (negative) / darken (positive)
    float AutoExposureMaxEV{4.0f};
    float AutoExposureSpeedUp{2.0f};    // adapting to a brighter scene, per second
    float AutoExposureSpeedDown{1.0f};  // adapting to a darker scene
    float GradeTemperature{0.0f};  // -100..100, 0 = neutral (Unity's White Balance)
    float GradeTint{0.0f};
    float GradeContrast{0.0f};
    float GradeSaturation{0.0f};
    glm::vec3 GradeColorFilter{1.0f};
    // #162 - depth of field (Game view / player only).
    bool  DepthOfField{false};
    float FocusDistance{10.0f};
    float FocusRange{3.0f};
    float DofMaxBlur{8.0f};

    // Anti-aliasing mode (Unity's camera Anti-aliasing). Off / FXAA / MSAA only summarise the older
    // MsaaSamples + FxaaEnabled fields, which still drive those modes exactly as before (so a scene saved
    // before this existed renders the same). TAA is the new one: jittered projection, per-pixel motion
    // vectors and a history resolve (TemporalAA.h); it renders the HDR target at kTaaMsaaSamples and skips FXAA.
    enum class AntiAliasingMode { Off = 0, Fxaa = 1, Msaa = 2, Taa = 3 };
    AntiAliasingMode AntiAliasing{AntiAliasingMode::Msaa};
    float TaaSharpness{0.35f};          // 0..1, the post-resolve sharpen (TAA only)
    // TAA asked for 1 sample, but the driver rounds a 1-sample multisample texture up to 2: TAA has always rendered
    // at 2x MSAA, resolved before the history pass. Asking for 2 says so, and keeps TAA's edges as they look (a true
    // 1x target is a plain 2D texture since HdrTarget's single-sample path, and its edges come out more jagged).
    static constexpr int kTaaMsaaSamples = 2;
    int  EffectiveMsaaSamples() const { return AntiAliasing == AntiAliasingMode::Taa ? kTaaMsaaSamples : MsaaSamples; }
    bool EffectiveFxaa() const { return AntiAliasing != AntiAliasingMode::Taa && FxaaEnabled; }
    bool TaaEnabled() const { return AntiAliasing == AntiAliasingMode::Taa; }
    // What Off / FXAA / MSAA a scene without the key reads as (derived from the two older fields).
    static AntiAliasingMode LegacyAntiAliasing(int msaaSamples, bool fxaa) {
        return fxaa ? AntiAliasingMode::Fxaa : msaaSamples > 1 ? AntiAliasingMode::Msaa : AntiAliasingMode::Off;
    }

    // Global rain wetness (ModelFragment.glsl ApplyWetness). 0 = dry, the default: nothing changes.
    // Wetness darkens porous albedo and lowers roughness on upward-facing surfaces; PuddleAmount
    // floods a world-space noise mask on near-flat ground (flat normal, roughness ~0.02, water tint).
    // Materials with NoWetness (_NoWetness) stay dry - interiors are opted out that way.
    float Wetness{0.0f};       // 0..1
    float PuddleAmount{0.0f};  // 0..1
    float PuddleScale{4.0f};   // metres per noise cell (bigger = larger puddles)
    // Screen-space reflections on the puddles only (PuddleSsr): lamps and facades show in the standing water. Off by
    // default (the converter's night look turns it on); only does anything while PuddleAmount > 0.
    bool PuddleSsr{false};

    // #162 - distance fog (Unity's Lighting > Other Settings > Fog).
    bool  FogEnabled{false};
    int   FogMode{2};               // 1 Linear, 2 Exponential, 3 Exponential Squared
    glm::vec3 FogColor{0.55f, 0.62f, 0.72f}; // linear
    float FogDensity{0.01f};        // Exponential modes
    float FogStart{10.0f};          // Linear mode, metres from the camera
    float FogEnd{300.0f};
    float FogHeightFalloff{0.0f};   // Exponential modes: 0 = uniform, higher = hugs the ground
    float FogBaseHeight{0.0f};      // world Y where height fog is at full density

    // Froxel volumetric fog (VolumetricFog.h): light shafts through the air from the directional
    // light and every point / spot light, shadowed by their shadow maps. Each light's share is
    // LightComponent::VolumetricIntensity. Off by default (older scenes load without it).
    bool  VolumetricEnabled{false};
    float VolumetricDensity{0.02f};        // extinction per metre at the base height (0.02 = light haze)
    float VolumetricAnisotropy{0.6f};      // Henyey-Greenstein g: 0 even, > 0 bright looking toward a light
    float VolumetricHeightFalloff{0.0f};   // 0 = same everywhere; higher = thins out above the base height
    float VolumetricBaseHeight{0.0f};      // world Y below which the density is full
    glm::vec3 VolumetricAlbedo{0.9f};      // how much of what it absorbs it scatters, per channel
    float VolumetricDistance{64.0f};       // metres from the camera the froxel grid reaches
    int   VolumetricQuality{1};            // 0 Low (128x72x48), 1 Medium (160x90x64), 2 High (240x135x96)

    bool  ShadowsEnabled{true};
    int   ShadowResolution{4096};
    int   ShadowCascades{4};
    float ShadowDistance{500.0f};
    // Cascade split blend (CascadedShadowMap::SetSplitLambda): higher = tighter, sharper near
    // cascades. 0.75 matches every scene saved before the setting existed.
    float ShadowCascadeSplit{0.75f};
    // #110 - shadow budgets for local lights: how many shadowed spot / point lights get a map
    // each frame (the most important ones, scored in LightGather.cpp). Clamped to 0..SpotShadowMap::
    // kMaxSpots / PointShadowMap::kMaxPoints on load and in the UI.
    int   MaxSpotShadows{4};
    int   MaxPointShadows{2};
};
