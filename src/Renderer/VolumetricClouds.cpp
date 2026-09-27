#include "VolumetricClouds.h"
#include "SkyAtmosphere.h"
#include "SkySettings.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "Log.h"
#include "gl.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kShapeSize = 128;
constexpr int kDetailSize = 32;
// 2048 texels over the weather map's ~45 km: ~22 m each. At 512 (~88 m) the cloud outlines, which
// threshold this map, showed its bilinear texels as straight segments. Mipmapped, so distant
// clouds read a level that matches their footprint. CloudsCommon.glsl's kWeatherTexels mirrors it.
constexpr int kWeatherSize = 2048;
// How far (km, at CloudScale / CirrusScale 1) each texture repeats. The wind offsets wrap at
// these exact periods, so wrapping never moves the pattern.
constexpr float kWeatherTileKm = 45.0f;
constexpr float kShapeTileKm = 7.0f;   // the detail noise (0.7 km) repeats 10x inside it
constexpr float kDetailTileKm = 0.7f;
constexpr float kCirrusTileKm = 25.0f;

float CloudScale(const SkySettings& s) { return std::max(s.CloudScale, 0.05f); }
float CirrusScale(const SkySettings& s) { return std::max(s.CirrusScale, 0.05f); }

// v mod period, per component, in [0, period).
void WrapOffset(glm::vec2& v, float period) {
    v -= glm::floor(v / period) * period;
    for (int i = 0; i < 2; ++i)
        if (v[i] >= period || v[i] < 0.0f) v[i] = 0.0f; // rounding at the ends
}

// std140 mirror of CloudBlock in CloudsCommon.glsl.
struct CloudUbo {
    glm::vec4 Layer;
    glm::vec4 Shape;
    glm::vec4 Wind;
    glm::vec4 LightDir;
    glm::vec4 LightIllum;
    glm::vec4 Ambient;
    glm::vec4 Misc;
    glm::vec4 Misc2;
    glm::vec4 WindDir;
};
static_assert(sizeof(CloudUbo) == 9 * 16, "CloudUbo must match the std140 CloudBlock");

unsigned int MakeTexture2D(GLenum format, int w, int h, GLenum wrap) {
    unsigned int tex = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureStorage2D(tex, 1, format, w, h);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, (GLint)wrap);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, (GLint)wrap);
    return tex;
}

unsigned int MakeNoise3D(int size) {
    unsigned int tex = 0;
    glCreateTextures(GL_TEXTURE_3D, 1, &tex);
    const int mips = 1 + (int)std::floor(std::log2((float)size));
    glTextureStorage3D(tex, mips, GL_RGBA8, size, size, size);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_R, GL_REPEAT);
    return tex;
}

// Compass bearing (degrees, clockwise from north) -> world XZ unit vector, with the scene's
// compass rotation applied. North is -Z and east +X before the rotation (TimeOfDay.h).
glm::vec2 BearingToXZ(float bearingDeg, float northOffsetDeg) {
    const float b = glm::radians(bearingDeg);
    const glm::vec2 local(std::sin(b), -std::cos(b));
    const float a = glm::radians(northOffsetDeg);
    const float c = std::cos(a), s = std::sin(a);
    return glm::vec2(c * local.x + s * local.y, -s * local.x + c * local.y);
}

GLuint Groups(int n, int local) { return (GLuint)((n + local - 1) / local); }
} // namespace

void VolumetricClouds::ViewState::Release() {
    unsigned int texs[4] = {Color, Depth, History[0], History[1]};
    for (unsigned int t : texs)
        if (t) glDeleteTextures(1, &t);
    Color = Depth = History[0] = History[1] = 0;
    Width = Height = 0;
    HasHistory = false;
}

VolumetricClouds::QualityLevel VolumetricClouds::Quality(int level) {
    switch (std::clamp(level, 0, 3)) {
    case 0:  return {4, 40, 4, 0.15f};  // Low: quarter resolution
    case 2:  return {2, 96, 6, 0.08f};  // High
    case 3:  return {1, 128, 6, 0.1f};  // Ultra: full resolution
    default: return {2, 56, 5, 0.1f};   // Medium: half resolution
    }
}

VolumetricClouds::VolumetricClouds() = default;

VolumetricClouds::~VolumetricClouds() {
    unsigned int texs[4] = {m_ShapeNoise, m_DetailNoise, m_Weather, m_ShadowMap};
    for (unsigned int t : texs)
        if (t) glDeleteTextures(1, &t);
    if (m_Ubo) glDeleteBuffers(1, &m_Ubo);
}

void VolumetricClouds::EnsureResources() {
    if (m_RaymarchShader) return;
    m_NoiseShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("CloudNoise.comp.glsl"), "CloudNoise");
    m_RaymarchShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("CloudRaymarch.comp.glsl"), "CloudRaymarch");
    m_TemporalShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("CloudTemporal.comp.glsl"), "CloudTemporal");
    m_ShadowShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("CloudShadow.comp.glsl"), "CloudShadow");

    glCreateBuffers(1, &m_Ubo);
    glNamedBufferStorage(m_Ubo, sizeof(CloudUbo), nullptr, GL_DYNAMIC_STORAGE_BIT);

    m_ShapeNoise = MakeNoise3D(kShapeSize);
    m_DetailNoise = MakeNoise3D(kDetailSize);
    glCreateTextures(GL_TEXTURE_2D, 1, &m_Weather);
    glTextureStorage2D(m_Weather, 1 + (int)std::floor(std::log2((float)kWeatherSize)), GL_RGBA8, kWeatherSize, kWeatherSize);
    glTextureParameteri(m_Weather, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(m_Weather, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_Weather, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(m_Weather, GL_TEXTURE_WRAP_T, GL_REPEAT);
    m_ShadowMap = MakeTexture2D(GL_R16F, kShadowMapSize, kShadowMapSize, GL_CLAMP_TO_EDGE);
    GenerateNoise();
}

void VolumetricClouds::GenerateNoise() {
    // One-off (~tens of ms on the GPU): three dispatches, then mip chains for the 3D noise so
    // distant/cheap samples can read a blurrier level.
    m_NoiseShader->Bind();
    const int modes[3] = {0, 1, 2};
    const int sizes[3] = {kShapeSize, kDetailSize, kWeatherSize};
    for (int i = 0; i < 3; ++i) {
        const int mode = modes[i], size = sizes[i];
        // Both image units hold a real image of the right format every dispatch, whichever
        // one the mode writes.
        glBindImageTexture(0, mode == 1 ? m_DetailNoise : m_ShapeNoise, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glBindImageTexture(1, m_Weather, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        m_NoiseShader->SetInt("uMode", mode);
        m_NoiseShader->SetInt("uSize", size);
        m_NoiseShader->SetFloat("uSeed", 0.0f);
        if (mode == 2)
            m_NoiseShader->DispatchCompute(Groups(size, 4), Groups(size, 4), 1);
        else
            m_NoiseShader->DispatchCompute(Groups(size, 4), Groups(size, 4), Groups(size, 4));
    }
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glGenerateTextureMipmap(m_ShapeNoise);
    glGenerateTextureMipmap(m_DetailNoise);
    glGenerateTextureMipmap(m_Weather);
    m_NoiseReady = true;
}

void VolumetricClouds::Tick(const SkySettings& s, float dt) {
    if (dt <= 0.0f || dt > 1.0f) return; // a hitch or a pause shouldn't teleport the sky
    const glm::vec2 dir = BearingToXZ(s.CloudWindDirectionDegrees, s.NorthOffsetDegrees);
    const glm::vec2 velKm = dir * (s.CloudWindSpeed * 0.001f); // km/s
    // Texture offsets move opposite to the air so the pattern travels with the wind. The shape
    // noise drifts a little faster than the weather so clouds evolve while they travel, and the
    // cirrus rides the faster winds aloft.
    m_WeatherOffset -= velKm * dt;
    m_ShapeOffset -= velKm * dt * 1.3f;
    m_CirrusOffset -= glm::vec2(glm::length(velKm) * 2.5f, 0.0f) * dt;
    // Keep the offsets small, wrapping each at a whole number of its texture's tiles so the wrap
    // itself never shows. (They used to wrap at thousands of km, which isn't a whole number of
    // tiles: a frame's tiny step then rounded away against the large value, the offset flipped
    // between 0 and the period every frame, and the whole cloud field jumped back and forth
    // between two layouts instead of drifting.) The cirrus's second octave is 3.1x its first:
    // ten tiles keep both whole.
    WrapOffset(m_WeatherOffset, kWeatherTileKm * CloudScale(s));
    WrapOffset(m_ShapeOffset, kShapeTileKm * CloudScale(s));
    WrapOffset(m_CirrusOffset, kCirrusTileKm * 10.0f * CirrusScale(s));
}

void VolumetricClouds::UpdateUniforms(const SkySettings& s, const SkyLighting& lit) {
    EnsureResources();
    const float bottomRadius = std::max(s.PlanetRadiusKm, 10.0f);
    const float baseKm = std::max(s.CloudBaseMeters, 0.0f) * 0.001f;
    const float thicknessKm = std::max(s.CloudThicknessMeters, 50.0f) * 0.001f;
    const float scale = CloudScale(s);
    const glm::vec2 windDir = BearingToXZ(s.CloudWindDirectionDegrees, s.NorthOffsetDegrees);

    CloudUbo u{};
    u.Layer = glm::vec4(bottomRadius + baseKm, bottomRadius + baseKm + thicknessKm, thicknessKm,
                        s.CloudsEnabled ? std::clamp(s.CloudCoverage, 0.0f, 1.0f) : 0.0f);
    // Shape noise tiles every ~7 km (puffs of ~1-2 km), detail every ~0.7 km, weather every ~45 km.
    u.Shape = glm::vec4(1.0f / (kShapeTileKm * scale), 1.0f / (kDetailTileKm * scale), std::clamp(s.CloudDetail, 0.0f, 1.0f),
                        40.0f * std::max(s.CloudDensity, 0.0f));
    // Wind offsets in km (the shader adds them before scaling by each texture's frequency).
    u.Wind = glm::vec4(m_WeatherOffset, m_ShapeOffset);
    u.LightDir = glm::vec4(lit.CloudLightDir, std::clamp(s.CloudType, 0.0f, 1.0f));
    u.LightIllum = glm::vec4(lit.CloudLightIlluminance, std::clamp(s.CloudForwardScattering, 0.0f, 0.95f));
    u.Ambient = glm::vec4(std::max(s.CloudAmbient, 0.0f), std::clamp(s.CloudPowder, 0.0f, 1.0f),
                          1.0f / (kWeatherTileKm * scale), std::clamp(s.CloudMultiScattering, 0.0f, 1.5f));
    u.Misc = glm::vec4(60.0f, s.CloudsEnabled ? std::clamp(s.CirrusCoverage, 0.0f, 1.0f) : 0.0f,
                       std::max(s.CirrusAltitudeMeters, s.CloudBaseMeters + s.CloudThicknessMeters + 100.0f) * 0.001f,
                       1.0f / (kCirrusTileKm * CirrusScale(s)));
    u.Misc2 = glm::vec4(m_CirrusOffset, std::max(s.CloudWindShear, 0.0f), s.CloudSeed * 0.1317f);
    u.WindDir = glm::vec4(windDir, 0.5f, std::max(s.CloudHorizonHaze, 0.0f));
    glNamedBufferSubData(m_Ubo, 0, sizeof(u), &u);
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, m_Ubo);
}

void VolumetricClouds::BindNoise() {
    EnsureResources();
    glBindTextureUnit(4, m_ShapeNoise);
    glBindTextureUnit(5, m_DetailNoise);
    glBindTextureUnit(6, m_Weather);
}

void VolumetricClouds::RenderShadowMap(const SkySettings& s, const glm::vec3& cameraWorld) {
    EnsureResources();
    const float strength = (s.CloudsEnabled && s.CloudShadows) ? std::clamp(s.CloudShadowStrength, 0.0f, 1.0f) : 0.0f;
    // 16 km of ground in world units (1 unit = 1 m), centred on the camera and snapped to whole
    // texels so the shadows don't crawl as it moves.
    const float extent = 16000.0f;
    const float texel = extent / (float)kShadowMapSize;
    const glm::vec2 center(std::floor(cameraWorld.x / texel) * texel, std::floor(cameraWorld.z / texel) * texel);
    m_ShadowParams = glm::vec4(center, 1.0f / extent, strength);
    if (strength <= 0.0f) return;

    BindNoise();
    m_ShadowShader->Bind();
    m_ShadowShader->SetVec2("uCenterWorld", center);
    m_ShadowShader->SetFloat("uExtentWorld", extent);
    m_ShadowShader->SetFloat("uStrength", strength);
    glUniform2i(m_ShadowShader->Loc("uSize"), kShadowMapSize, kShadowMapSize);
    glBindImageTexture(0, m_ShadowMap, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    m_ShadowShader->DispatchCompute(Groups(kShadowMapSize, 8), Groups(kShadowMapSize, 8), 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
}

unsigned int VolumetricClouds::RenderView(ViewState& st, const SkySettings& s, const glm::mat4& view,
                                          const glm::mat4& proj, const glm::vec3& cameraWorld,
                                          int viewWidth, int viewHeight, float frame, float worldUnitsPerKm) {
    EnsureResources();
    const QualityLevel q = Quality(s.CloudQuality);
    const int w = std::max(1, viewWidth / q.Divisor), h = std::max(1, viewHeight / q.Divisor);
    if (st.Width != w || st.Height != h || !st.Color) {
        st.Release();
        st.Width = w;
        st.Height = h;
        st.Color = MakeTexture2D(GL_RGBA16F, w, h, GL_CLAMP_TO_EDGE);
        st.Depth = MakeTexture2D(GL_R32F, w, h, GL_CLAMP_TO_EDGE);
        st.History[0] = MakeTexture2D(GL_RGBA16F, w, h, GL_CLAMP_TO_EDGE);
        st.History[1] = MakeTexture2D(GL_RGBA16F, w, h, GL_CLAMP_TO_EDGE);
        glTextureParameteri(st.Depth, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(st.Depth, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        st.HasHistory = false;
    }
    const glm::mat4 viewProj = proj * view;
    const glm::mat4 invViewProj = glm::inverse(viewProj);

    // --- Raymarch ---
    BindNoise();
    m_RaymarchShader->Bind();
    m_RaymarchShader->SetMat4("uInvViewProj", invViewProj);
    m_RaymarchShader->SetVec3("uCameraWorld", cameraWorld);
    glUniform2i(m_RaymarchShader->Loc("uSize"), w, h);
    m_RaymarchShader->SetFloat("uFrame", frame);
    m_RaymarchShader->SetInt("uSteps", q.Steps);
    m_RaymarchShader->SetInt("uLightSteps", q.LightSteps);
    // The angle one target pixel spans (the vertical field of view over its height), which
    // picks the noise mips for distant clouds.
    m_RaymarchShader->SetFloat("uPixelAngle", 2.0f / (std::max(std::abs(proj[1][1]), 1e-3f) * (float)h));
    glBindImageTexture(0, st.Color, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(1, st.Depth, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    m_RaymarchShader->DispatchCompute(Groups(w, 8), Groups(h, 8), 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // --- Temporal resolve into the other history target ---
    // A big camera jump (teleport, switching views) invalidates the history.
    const bool cut = glm::length(cameraWorld - st.PrevCamera) > worldUnitsPerKm * 0.5f;
    const int dst = st.Current ^ 1;
    m_TemporalShader->Bind();
    glBindTextureUnit(7, st.Color);
    glBindTextureUnit(8, st.Depth);
    glBindTextureUnit(9, st.History[st.Current]);
    m_TemporalShader->SetMat4("uInvViewProj", invViewProj);
    m_TemporalShader->SetMat4("uPrevViewProj", st.PrevViewProj);
    m_TemporalShader->SetVec3("uCameraWorld", cameraWorld);
    m_TemporalShader->SetFloat("uKmToWorld", worldUnitsPerKm);
    glUniform2i(m_TemporalShader->Loc("uSize"), w, h);
    m_TemporalShader->SetFloat("uBlend", q.Blend);
    m_TemporalShader->SetInt("uReset", (!st.HasHistory || cut) ? 1 : 0);
    glBindImageTexture(0, st.History[dst], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    m_TemporalShader->DispatchCompute(Groups(w, 8), Groups(h, 8), 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    st.Current = dst;
    st.HasHistory = true;
    st.PrevViewProj = viewProj;
    st.PrevCamera = cameraWorld;
    return st.History[dst];
}
