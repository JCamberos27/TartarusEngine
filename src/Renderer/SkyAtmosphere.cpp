#include "SkyAtmosphere.h"
#include "TimeOfDay.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "DefaultTextures.h"
#include "GLStateCache.h"
#include "Core/Profiler.h"
#include "gl.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kTransmittanceW = 256, kTransmittanceH = 64;
constexpr int kMultiScatterSize = 32;
constexpr int kSkyViewW = 192, kSkyViewH = 108;
constexpr int kAerialSize = 32;
constexpr int kSkyAmbientUnit = 10; // SkyAmbient.comp.glsl's output (uSkyAmbientLut)
constexpr float kAerialRangeKm = 32.0f;
constexpr float kKmPerWorldUnit = 0.001f; // 1 world unit = 1 m
constexpr float kPi = 3.14159265358979f;

// std140 mirror of AtmosphereBlock in AtmosphereCommon.glsl.
struct AtmosphereUbo {
    glm::vec4 Rayleigh;
    glm::vec4 MieScat;
    glm::vec4 MieExt;
    glm::vec4 Ozone;
    glm::vec4 Ground;
    glm::vec4 Radii;
    glm::vec4 SunDir;
    glm::vec4 SunIllum;
    glm::vec4 MoonDir;
    glm::vec4 MoonIllum;
    glm::vec4 Camera;
    glm::vec4 Params;
    glm::vec4 Params2;
    glm::mat4 StarRotation;
};
static_assert(sizeof(AtmosphereUbo) == 13 * 16 + 64, "AtmosphereUbo must match the std140 AtmosphereBlock");

// Earth's atmosphere (Bruneton 2017 / Hillaire 2020), per km.
const glm::vec3 kRayleighScattering(5.802e-3f, 13.558e-3f, 33.1e-3f);
constexpr float kMieScattering = 3.996e-3f;
constexpr float kMieAbsorption = 0.444e-3f;
const glm::vec3 kOzoneAbsorption(0.650e-3f, 1.881e-3f, 0.085e-3f);
constexpr float kOzoneCenterKm = 25.0f, kOzoneHalfWidthKm = 15.0f;
// Moonlight is sunlight off grey rock: slightly cooler than the sun by the time we see it.
const glm::vec3 kMoonTint(0.80f, 0.88f, 1.0f);

// The medium the settings describe, in the units the shaders use.
struct Medium {
    glm::vec3 Rayleigh;
    float RayleighInvH;
    glm::vec3 MieScat, MieExt;
    float MieInvH;
    glm::vec3 Ozone;
    float BottomRadius, TopRadius;
};

Medium MediumFrom(const SkySettings& s) {
    Medium m;
    m.Rayleigh = kRayleighScattering * std::max(s.RayleighScale, 0.0f) * glm::max(s.RayleighTint, glm::vec3(0.0f));
    m.RayleighInvH = 1.0f / std::max(s.RayleighHeightKm, 0.1f);
    const float mie = kMieScattering * std::max(s.MieScale, 0.0f);
    m.MieScat = glm::vec3(mie);
    m.MieExt = glm::vec3(mie + kMieAbsorption * std::max(s.MieScale, 0.0f) * std::max(s.MieAbsorption, 0.0f));
    m.MieInvH = 1.0f / std::max(s.MieHeightKm, 0.05f);
    m.Ozone = kOzoneAbsorption * std::max(s.OzoneScale, 0.0f);
    m.BottomRadius = std::max(s.PlanetRadiusKm, 10.0f);
    m.TopRadius = m.BottomRadius + std::max(s.AtmosphereHeightKm, 1.0f);
    return m;
}

glm::vec3 Extinction(const Medium& m, float h) {
    h = std::max(h, 0.0f);
    const float dR = std::exp(-h * m.RayleighInvH);
    const float dM = std::exp(-h * m.MieInvH);
    const float dO = std::max(0.0f, 1.0f - std::abs(h - kOzoneCenterKm) / kOzoneHalfWidthKm);
    return m.Rayleigh * dR + m.MieExt * dM + m.Ozone * dO;
}

// Whether two settings produce the same transmittance / multi-scattering LUTs (they depend only
// on the medium, never on the sun or the camera).
bool SameMedium(const SkySettings& a, const SkySettings& b) {
    return a.RayleighScale == b.RayleighScale && a.RayleighTint == b.RayleighTint && a.MieScale == b.MieScale &&
           a.MieAnisotropy == b.MieAnisotropy && a.MieAbsorption == b.MieAbsorption && a.OzoneScale == b.OzoneScale &&
           a.GroundAlbedo == b.GroundAlbedo && a.PlanetRadiusKm == b.PlanetRadiusKm &&
           a.AtmosphereHeightKm == b.AtmosphereHeightKm && a.RayleighHeightKm == b.RayleighHeightKm &&
           a.MieHeightKm == b.MieHeightKm;
}

// Settings that change what the environment capture sees, apart from the clock (the sun's
// movement is tracked by angle instead, so a running clock doesn't re-capture every frame).
bool SameLook(SkySettings a, SkySettings b) {
    a.TimeOfDayHours = b.TimeOfDayHours = 0.0f;
    a.DayLengthMinutes = b.DayLengthMinutes = 0.0f;
    a.AnimateInEditor = b.AnimateInEditor = false;
    a.CloudQuality = b.CloudQuality = 0;
    return a == b;
}

float Luminance(const glm::vec3& c) { return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f)); }

float AngleBetween(const glm::vec3& a, const glm::vec3& b) {
    return std::acos(std::clamp(glm::dot(a, b), -1.0f, 1.0f));
}

unsigned int MakeLut2D(int w, int h) {
    unsigned int tex = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureStorage2D(tex, 1, GL_RGBA16F, w, h);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

GLuint Groups(int n, int local) { return (GLuint)((n + local - 1) / local); }

// Mirrors WorldToAtmosphere() in AtmosphereCommon.glsl.
glm::vec3 ToAtmosphere(const SkySettings& s, const glm::vec3& world) {
    glm::vec3 p = world * kKmPerWorldUnit;
    p.y -= s.SeaLevel * kKmPerWorldUnit;
    p.y += std::max(s.PlanetRadiusKm, 10.0f) + s.AltitudeOffsetMeters * 0.001f;
    return p;
}

float WrapHours(float h) { return h - 24.0f * std::floor(h / 24.0f); }
} // namespace

// ------------------------------------------------------------------------------------------

void SkyAtmosphere::ViewResources::Release() {
    unsigned int texs[4] = {SkyViewSun, SkyViewMoon, Aerial, SkyAmbient};
    for (unsigned int t : texs)
        if (t) glDeleteTextures(1, &t);
    SkyViewSun = SkyViewMoon = Aerial = SkyAmbient = 0;
    Clouds.Release();
    CloudTexture = 0;
    CloudsValid = false;
}

SkyAtmosphere::SkyAtmosphere() = default;

SkyAtmosphere::~SkyAtmosphere() {
    m_Views.clear();
    unsigned int texs[3] = {m_TransmittanceLut, m_MultiScatterLut, m_EnvCube};
    for (unsigned int t : texs)
        if (t) glDeleteTextures(1, &t);
    if (m_Ubo) glDeleteBuffers(1, &m_Ubo);
    if (m_Vao) glDeleteVertexArrays(1, &m_Vao);
}

void SkyAtmosphere::EnsureResources() {
    if (m_Ubo) return;
    m_TransmittanceShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("AtmosphereTransmittance.comp.glsl"), "AtmosphereTransmittance");
    m_MultiScatterShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("AtmosphereMultiScatter.comp.glsl"), "AtmosphereMultiScatter");
    m_SkyViewShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("AtmosphereSkyView.comp.glsl"), "AtmosphereSkyView");
    m_SkyAmbientShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("SkyAmbient.comp.glsl"), "SkyAmbient");
    m_AerialShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("AtmosphereAerial.comp.glsl"), "AtmosphereAerial");
    m_EnvShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("SkyEnvCapture.comp.glsl"), "SkyEnvCapture");
    m_CompositeShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("Sky.vert.glsl"),
                                                 ShaderLibrary::ReadFileRequired("SkyAtmosphere.frag.glsl"), "SkyAtmosphere");

    glCreateBuffers(1, &m_Ubo);
    glNamedBufferStorage(m_Ubo, sizeof(AtmosphereUbo), nullptr, GL_DYNAMIC_STORAGE_BIT);
    m_TransmittanceLut = MakeLut2D(kTransmittanceW, kTransmittanceH);
    m_MultiScatterLut = MakeLut2D(kMultiScatterSize, kMultiScatterSize);

    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_EnvCube);
    const int mips = 1 + (int)std::floor(std::log2((float)kEnvSize));
    glTextureStorage2D(m_EnvCube, mips, GL_RGBA16F, kEnvSize, kEnvSize);
    glTextureParameteri(m_EnvCube, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(m_EnvCube, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_EnvCube, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_EnvCube, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_EnvCube, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glCreateVertexArrays(1, &m_Vao); // attribute-less full-screen quad, like Sky
}

float SkyAtmosphere::CameraAltitudeKm(const SkySettings& s, const glm::vec3& cameraWorld) {
    return std::max((cameraWorld.y - s.SeaLevel) * kKmPerWorldUnit + s.AltitudeOffsetMeters * 0.001f, 0.001f);
}

glm::vec3 SkyAtmosphere::Transmittance(const SkySettings& s, float altitudeKm, const glm::vec3& dirIn) {
    const Medium m = MediumFrom(s);
    const glm::vec3 dir = glm::normalize(dirIn);
    const float r = m.BottomRadius + std::clamp(altitudeKm, 0.0f, m.TopRadius - m.BottomRadius - 0.001f);
    const float mu = dir.y; // the observer stands at the "north pole" of this frame

    // The planet: the same soft cut-off as TransmittanceToSpace() in the shaders.
    const float sinHorizon = m.BottomRadius / r;
    const float cosHorizon = -std::sqrt(std::max(1.0f - sinHorizon * sinHorizon, 0.0f));
    const float t = std::clamp((mu - (cosHorizon - 0.01f)) / 0.02f, 0.0f, 1.0f);
    const float fade = t * t * (3.0f - 2.0f * t);
    if (fade <= 0.0f) return glm::vec3(0.0f);

    // Distance to the top of the atmosphere, then a midpoint integration of the extinction.
    const float b = r * mu;
    const float disc = b * b - (r * r - m.TopRadius * m.TopRadius);
    const float tTop = -b + std::sqrt(std::max(disc, 0.0f));
    constexpr int kSteps = 64;
    const float dt = tTop / (float)kSteps;
    glm::vec3 od(0.0f);
    for (int i = 0; i < kSteps; ++i) {
        const float ti = ((float)i + 0.5f) * dt;
        const glm::vec3 p(dir.x * ti, r + dir.y * ti, dir.z * ti);
        od += Extinction(m, glm::length(p) - m.BottomRadius) * dt;
    }
    return glm::exp(-od) * fade;
}

void SkyAtmosphere::Tick(const SkySettings& s, float dt, bool playing) {
    dt = std::clamp(dt, 0.0f, 0.25f);
    m_RealSeconds += dt;
    const bool clockRuns = s.TimeOfDayEnabled && s.DayLengthMinutes > 0.0f && (playing || s.AnimateInEditor);
    if (clockRuns)
        m_ClockOffsetHours = WrapHours(m_ClockOffsetHours + dt / (s.DayLengthMinutes * 60.0f) * 24.0f);
    // Stop -> back to the authored time, like every other Play-mode change.
    if ((m_WasPlaying && !playing) || (!playing && !s.AnimateInEditor)) m_ClockOffsetHours = 0.0f;
    m_WasPlaying = playing;
    m_Clouds.Tick(s, dt);
}

SkyLighting SkyAtmosphere::ResolveLighting(const SkySettings& s, const glm::vec3& authoredAim,
                                           const glm::vec3& sunIlluminance, float angularSizeDeg,
                                           const glm::vec3& cameraWorld) const {
    SkyLighting L;
    const float hours = WrapHours(s.TimeOfDayHours + m_ClockOffsetHours);
    L.ClockHours = hours;
    const float phase = TimeOfDay::MoonPhase(s.DayOfYear, hours, s.MoonPhaseOffset);
    if (s.TimeOfDayEnabled) {
        L.SunDir = TimeOfDay::SunDirection(hours, s.DayOfYear, s.LatitudeDegrees, s.NorthOffsetDegrees);
        L.MoonDir = TimeOfDay::MoonDirection(hours, s.DayOfYear, s.LatitudeDegrees, s.NorthOffsetDegrees, phase);
    } else {
        const float len = glm::length(authoredAim);
        L.SunDir = len > 1e-6f ? -authoredAim / len : glm::vec3(0.0f, 1.0f, 0.0f);
        // No clock: put the moon opposite the sun, swung round by its phase, so it rises as the
        // light is aimed below the horizon.
        const float a = (phase - 0.5f) * 2.0f * kPi;
        const glm::vec3 opp = -L.SunDir;
        L.MoonDir = glm::normalize(glm::vec3(std::cos(a) * opp.x + std::sin(a) * opp.z, opp.y,
                                             -std::sin(a) * opp.x + std::cos(a) * opp.z));
    }
    L.StarRotation = TimeOfDay::StarRotation(hours, s.DayOfYear, s.LatitudeDegrees, s.NorthOffsetDegrees);

    L.SunIlluminance = glm::max(sunIlluminance, glm::vec3(0.0f));
    L.SunAngularRadius = glm::radians(std::max(angularSizeDeg, 0.05f) * 0.5f) * std::max(s.SunDiscScale, 0.05f);
    L.MoonAngularRadius = 0.0045f * std::max(s.MoonDiscScale, 0.05f);
    L.MoonIlluminance = s.MoonEnabled ? kMoonTint * (Luminance(L.SunIlluminance) * std::max(s.MoonBrightness, 0.0f))
                                      : glm::vec3(0.0f);
    L.MoonLitFraction = TimeOfDay::MoonIllumination(phase);

    const float alt = CameraAltitudeKm(s, cameraWorld);
    auto throughAir = [&](const glm::vec3& dir) {
        if (s.AtmosphereTintsSun) return Transmittance(s, alt, dir);
        const float t = std::clamp((dir.y + 0.01f) / 0.02f, 0.0f, 1.0f);
        return glm::vec3(t * t * (3.0f - 2.0f * t));
    };
    const glm::vec3 sunRadiance = L.SunIlluminance * throughAir(L.SunDir);
    const glm::vec3 moonRadiance = L.MoonIlluminance * L.MoonLitFraction * throughAir(L.MoonDir);

    // The scene light is whichever is brighter where the camera is: the sun, or the moon once
    // the sun has gone down. The switch happens when both are dim, so the jump in shadow
    // direction lands in near-darkness.
    L.LightIsMoon = s.MoonEnabled && Luminance(moonRadiance) > Luminance(sunRadiance);
    L.LightDir = L.LightIsMoon ? L.MoonDir : L.SunDir;
    L.NightBoost = std::exp2(NightBrightnessStops(s, L.SunDir));
    L.LightRadiance = (L.LightIsMoon ? moonRadiance : sunRadiance) * L.NightBoost;

    // Clouds stay sunlit well after sunset at ground level (their undersides glow red); only
    // hand them to the moon once the sun is far enough down to be out of reach even at altitude.
    const bool cloudsMoonlit = s.MoonEnabled && L.SunDir.y < -0.17f;
    L.CloudLightDir = cloudsMoonlit ? L.MoonDir : L.SunDir;
    L.CloudLightIlluminance = cloudsMoonlit ? L.MoonIlluminance * L.MoonLitFraction : L.SunIlluminance;
    return L;
}

float SkyAtmosphere::NightBrightnessStops(const SkySettings& s, const glm::vec3& sunDir) {
    // Sun 2 deg above the horizon -> none; 10 deg below (well into nautical twilight) -> all.
    // A sunset is still bright enough as it is; it's the dusk after it that needs the help.
    const float t = std::clamp((0.035f - sunDir.y) / (0.035f + 0.17f), 0.0f, 1.0f);
    return std::max(s.NightBrightness, 0.0f) * t * t * (3.0f - 2.0f * t);
}

void SkyAtmosphere::UpdateAtmosphereUniforms(const SkySettings& s, const SkyLighting& lit, const glm::vec3& cameraWorld) {
    const Medium m = MediumFrom(s);
    AtmosphereUbo u{};
    u.Rayleigh = glm::vec4(m.Rayleigh, -m.RayleighInvH);
    u.MieScat = glm::vec4(m.MieScat, -m.MieInvH);
    u.MieExt = glm::vec4(m.MieExt, std::clamp(s.MieAnisotropy, 0.0f, 0.99f));
    u.Ozone = glm::vec4(m.Ozone, kOzoneCenterKm);
    u.Ground = glm::vec4(glm::clamp(s.GroundAlbedo, glm::vec3(0.0f), glm::vec3(1.0f)), kOzoneHalfWidthKm);
    // Sky intensity carries the night brightening, so the visible sky, the haze, the clouds and
    // the environment capture (hence the ambient light) all rise together.
    u.Radii = glm::vec4(m.BottomRadius, m.TopRadius, std::max(s.SkyIntensity, 0.0f) * lit.NightBoost,
                        std::max(s.MultipleScattering, 0.0f));
    u.SunDir = glm::vec4(lit.SunDir, lit.SunAngularRadius);
    u.SunIllum = glm::vec4(lit.SunIlluminance, std::max(s.SunDiscBrightness, 0.0f));
    u.MoonDir = glm::vec4(lit.MoonDir, lit.MoonAngularRadius);
    u.MoonIllum = glm::vec4(lit.MoonIlluminance, lit.MoonLitFraction);
    u.Camera = glm::vec4(ToAtmosphere(s, cameraWorld), m_RealSeconds);
    u.Params = glm::vec4(std::max(s.AerialPerspectiveScale, 0.0f), std::max(s.StarsBrightness, 0.0f),
                         std::max(s.NightGlow, 0.0f), kKmPerWorldUnit);
    u.Params2 = glm::vec4(kAerialRangeKm, s.SeaLevel, s.AltitudeOffsetMeters * 0.001f, s.MoonEnabled ? 1.0f : 0.0f);
    u.StarRotation = glm::mat4(lit.StarRotation);
    glNamedBufferSubData(m_Ubo, 0, sizeof(u), &u);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_Ubo);
}

void SkyAtmosphere::RenderLuts() {
    PROFILE_SCOPE("Atmosphere LUTs");
    m_TransmittanceShader->Bind();
    glBindImageTexture(0, m_TransmittanceLut, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    m_TransmittanceShader->DispatchCompute(Groups(kTransmittanceW, 8), Groups(kTransmittanceH, 8), 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    m_MultiScatterShader->Bind();
    glBindTextureUnit(0, m_TransmittanceLut);
    glBindTextureUnit(1, DefaultTextures::Black()); // declared by the include, not read here
    glBindImageTexture(0, m_MultiScatterLut, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    m_MultiScatterShader->DispatchCompute(Groups(kMultiScatterSize, 8), Groups(kMultiScatterSize, 8), 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

void SkyAtmosphere::BindLuts(const ViewResources& vr) {
    glBindTextureUnit(0, m_TransmittanceLut);
    glBindTextureUnit(1, m_MultiScatterLut);
    glBindTextureUnit(2, vr.SkyViewSun);
    glBindTextureUnit(3, vr.SkyViewMoon);
    glBindTextureUnit(kSkyAmbientUnit, vr.SkyAmbient);
}

void SkyAtmosphere::RenderSkyViewLuts(ViewResources& vr) {
    if (!vr.SkyViewSun) {
        vr.SkyViewSun = MakeLut2D(kSkyViewW, kSkyViewH);
        vr.SkyViewMoon = MakeLut2D(kSkyViewW, kSkyViewH);
        vr.SkyAmbient = MakeLut2D(3, 1);
        glTextureParameteri(vr.SkyAmbient, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(vr.SkyAmbient, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    m_SkyViewShader->Bind();
    glBindTextureUnit(0, m_TransmittanceLut);
    glBindTextureUnit(1, m_MultiScatterLut);
    const SkyLighting& lit = m_Lighting;
    m_SkyViewShader->SetVec3("uLightDir", lit.SunDir);
    m_SkyViewShader->SetVec3("uLightIllum", lit.SunIlluminance);
    glBindImageTexture(0, vr.SkyViewSun, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    m_SkyViewShader->DispatchCompute(Groups(kSkyViewW, 8), Groups(kSkyViewH, 8), 1);
    // The moon's LUT is always written (black when the moon is off): the cloud and environment
    // passes read it unconditionally.
    m_SkyViewShader->SetVec3("uLightDir", lit.MoonDir);
    m_SkyViewShader->SetVec3("uLightIllum", m_Settings.MoonEnabled ? lit.MoonIlluminance * lit.MoonLitFraction : glm::vec3(0.0f));
    glBindImageTexture(0, vr.SkyViewMoon, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    m_SkyViewShader->DispatchCompute(Groups(kSkyViewW, 8), Groups(kSkyViewH, 8), 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // The sky's ambient terms, from the LUTs just written (see SkyAmbient.comp.glsl).
    m_SkyAmbientShader->Bind();
    glBindTextureUnit(2, vr.SkyViewSun);
    glBindTextureUnit(3, vr.SkyViewMoon);
    glBindImageTexture(0, vr.SkyAmbient, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    m_SkyAmbientShader->DispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

SkyAtmosphere::ViewResources& SkyAtmosphere::View(const void* key) {
    auto& slot = m_Views[key];
    if (!slot) slot = std::make_unique<ViewResources>();
    slot->LastUsedFrame = m_Frame;
    return *slot;
}

void SkyAtmosphere::PrepareFrame(const SkySettings& s, const SkyLighting& lit, const glm::vec3& cameraWorld) {
    PROFILE_SCOPE("Sky Atmosphere (frame)");
    PROFILE_GPU_SCOPE("Sky Atmosphere (frame)");
    EnsureResources();
    ++m_Frame;
    m_Settings = s;
    m_Lighting = lit;

    // Views nobody has drawn for a while (a closed Game tab) give their targets back.
    for (auto it = m_Views.begin(); it != m_Views.end();) {
        if (it->first != this && m_Frame - it->second->LastUsedFrame > 600) it = m_Views.erase(it);
        else ++it;
    }

    UpdateAtmosphereUniforms(s, lit, cameraWorld);
    if (!m_LutsValid || !SameMedium(s, m_LutSettings)) {
        RenderLuts();
        m_LutsValid = true;
        m_LutSettings = s;
    }

    // The frame's own sky-view LUTs (primary camera), for the shadow map and the capture.
    ViewResources& fv = View(this);
    m_FrameView = &fv;
    RenderSkyViewLuts(fv);
    BindLuts(fv);

    m_Clouds.UpdateUniforms(s, lit);
    if (s.CloudsEnabled) {
        PROFILE_SCOPE("Cloud Shadows");
        m_Clouds.RenderShadowMap(s, cameraWorld);
    }

    // Re-capture the environment when the sky has visibly changed: settings edited, the sun or
    // moon moved a little, or (with clouds drifting) every few seconds. Rate-limited so dragging
    // a slider doesn't convolve a cube every frame.
    const float since = m_RealSeconds - m_LastCaptureSeconds;
    const bool lookChanged = !SameLook(s, m_CapturedSettings);
    const bool bodiesMoved = AngleBetween(lit.SunDir, m_CapturedSunDir) > glm::radians(0.35f) ||
                             AngleBetween(lit.MoonDir, m_CapturedMoonDir) > glm::radians(1.0f) ||
                             glm::length(lit.SunIlluminance - m_CapturedLightIllum) >
                                 0.02f * std::max(glm::length(m_CapturedLightIllum), 1e-3f);
    const bool cloudsDrift = s.CloudsEnabled && s.CloudWindSpeed > 0.0f && since > 4.0f;
    if (m_ForceCapture || ((lookChanged || bodiesMoved) && since > 0.1f) || cloudsDrift) {
        CaptureEnvironment(s, fv);
        m_ForceCapture = false;
        m_LastCaptureSeconds = m_RealSeconds;
        m_CapturedSettings = s;
        m_CapturedSunDir = lit.SunDir;
        m_CapturedMoonDir = lit.MoonDir;
        m_CapturedLightIllum = lit.SunIlluminance;
        m_EnvDirty = true;
    }
    m_Prepared = true;
    GLStateCache::Invalidate();
}

void SkyAtmosphere::CaptureEnvironment(const SkySettings& s, ViewResources& fv) {
    PROFILE_SCOPE("Sky Environment Capture");
    BindLuts(fv);
    m_Clouds.BindNoise();
    m_EnvShader->Bind();
    m_EnvShader->SetInt("uSize", kEnvSize);
    m_EnvShader->SetInt("uClouds", s.CloudsEnabled && s.CloudCoverage > 0.0f ? 1 : 0);
    glBindImageTexture(0, m_EnvCube, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    m_EnvShader->DispatchCompute(Groups(kEnvSize, 8), Groups(kEnvSize, 8), 6);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glGenerateTextureMipmap(m_EnvCube);
}

void SkyAtmosphere::RenderView(const void* viewKey, const glm::mat4& view, const glm::mat4& proj,
                               const glm::vec3& cameraWorld) {
    if (!m_Prepared) return;
    PROFILE_SCOPE("Sky Atmosphere");
    PROFILE_GPU_SCOPE("Sky Atmosphere");
    const SkySettings& s = m_Settings;
    ViewResources& vr = View(viewKey);

    GLint vp[4] = {0, 0, 1, 1};
    glGetIntegerv(GL_VIEWPORT, vp);
    const glm::mat4 viewProj = proj * view;
    const glm::mat4 invViewProj = glm::inverse(viewProj);

    // This view's camera altitude drives its sky-view LUTs and aerial perspective.
    UpdateAtmosphereUniforms(s, m_Lighting, cameraWorld);
    RenderSkyViewLuts(vr);

    if (!vr.Aerial) {
        glCreateTextures(GL_TEXTURE_3D, 1, &vr.Aerial);
        glTextureStorage3D(vr.Aerial, 1, GL_RGBA16F, kAerialSize, kAerialSize, kAerialSize);
        glTextureParameteri(vr.Aerial, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(vr.Aerial, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(vr.Aerial, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(vr.Aerial, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(vr.Aerial, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }
    if (s.AerialPerspectiveScale > 0.0f) {
        m_AerialShader->Bind();
        BindLuts(vr);
        m_AerialShader->SetMat4("uInvViewProj", invViewProj);
        m_AerialShader->SetVec3("uCameraWorld", cameraWorld);
        glBindImageTexture(0, vr.Aerial, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        m_AerialShader->DispatchCompute(Groups(kAerialSize, 8), Groups(kAerialSize, 8), 1);
    }

    vr.CloudsValid = false;
    if (s.CloudsEnabled && (s.CloudCoverage > 0.0f || s.CirrusCoverage > 0.0f)) {
        PROFILE_SCOPE("Volumetric Clouds");
        PROFILE_GPU_SCOPE("Volumetric Clouds");
        BindLuts(vr);
        m_Clouds.UpdateUniforms(s, m_Lighting);
        vr.CloudTexture = m_Clouds.RenderView(vr.Clouds, s, view, proj, cameraWorld, vp[2], vp[3],
                                              (float)(m_Frame % 1024), 1.0f / kKmPerWorldUnit);
        vr.CloudsValid = vr.CloudTexture != 0;
    }
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    vr.InvViewProj = invViewProj;
    vr.CameraWorld = cameraWorld;
    for (int i = 0; i < 4; ++i) vr.Viewport[i] = vp[i];
    vr.Rendered = true;
    GLStateCache::Invalidate();
}

void SkyAtmosphere::DrawSky(const void* viewKey) {
    if (!m_Prepared) return;
    const auto it = m_Views.find(viewKey);
    if (it == m_Views.end() || !it->second->Rendered) return;
    ViewResources& vr = *it->second;
    PROFILE_SCOPE("Sky Draw");
    PROFILE_GPU_SCOPE("Sky Draw");

    // Another view may have filled the shared atmosphere UBO with its own camera altitude since.
    UpdateAtmosphereUniforms(m_Settings, m_Lighting, vr.CameraWorld);

    // Only where nothing opaque wrote depth: the quad sits exactly on the far plane (Sky.vert).
    const GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint prevDepthMask = GL_TRUE, prevDepthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    glDepthMask(GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    const int* vp = vr.Viewport;
    m_CompositeShader->Bind();
    BindLuts(vr);
    glBindTextureUnit(4, vr.CloudsValid ? vr.CloudTexture : DefaultTextures::Black());
    m_CompositeShader->SetMat4("uInvViewProj", vr.InvViewProj);
    m_CompositeShader->SetInt("uCloudsOn", vr.CloudsValid ? 1 : 0);
    m_CompositeShader->SetVec4("uViewportRect", glm::vec4((float)vp[0], (float)vp[1], (float)vp[2], (float)vp[3]));
    m_CompositeShader->SetFloat("uFrame", (float)(m_Frame % 64));
    glBindVertexArray(m_Vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDepthFunc((GLenum)prevDepthFunc);
    if (!wasDepthTest) glDisable(GL_DEPTH_TEST);
    glDepthMask((GLboolean)prevDepthMask);
    glActiveTexture(GL_TEXTURE0);
    GLStateCache::Invalidate();
}

void SkyAtmosphere::ApplyToProgram(Shader& program, const void* viewKey, const int viewport[4]) const {
    const auto it = m_Views.find(viewKey);
    const ViewResources* vr = it != m_Views.end() ? it->second.get() : nullptr;
    const bool aerial = m_Prepared && vr && vr->Aerial && m_Settings.AerialPerspectiveScale > 0.0f;
    const glm::vec4 shadow = m_Clouds.ShadowParams();
    const bool cloudShadows = m_Prepared && m_Settings.CloudsEnabled && shadow.w > 0.0f && m_Clouds.ShadowMap();

    glBindTextureUnit(SkyAtmosphere::kAerialUnit, aerial ? vr->Aerial : DefaultTextures::Volume());
    glBindTextureUnit(SkyAtmosphere::kCloudShadowUnit, cloudShadows ? m_Clouds.ShadowMap() : DefaultTextures::White());
    program.SetInt("uAerialOn", aerial ? 1 : 0);
    program.SetVec4("uAerialViewport", glm::vec4((float)viewport[0], (float)viewport[1],
                                                 (float)std::max(viewport[2], 1), (float)std::max(viewport[3], 1)));
    program.SetFloat("uAerialInvRange", kKmPerWorldUnit / kAerialRangeKm);
    program.SetInt("uCloudShadowOn", cloudShadows ? 1 : 0);
    program.SetVec4("uCloudShadowParams", glm::vec4(shadow.x, shadow.y, shadow.z, m_Settings.SeaLevel));
    program.SetVec3("uCloudShadowLightDir", m_Lighting.CloudLightDir);
}

void SkyAtmosphere::ApplyDisabled(Shader& program) {
    glBindTextureUnit(SkyAtmosphere::kAerialUnit, DefaultTextures::Volume());
    glBindTextureUnit(SkyAtmosphere::kCloudShadowUnit, DefaultTextures::White());
    program.SetInt("uAerialOn", 0);
    program.SetInt("uCloudShadowOn", 0);
}
