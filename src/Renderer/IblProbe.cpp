#include "IblProbe.h"
#include "Shader.h"
#include "Log.h"
#include "gl.h"

#include <string>

namespace {

// Every bake pass is a full-screen triangle pair with no vertex buffer (same attribute-less
// gl_VertexID trick as Sky/Grid). vUV is [0,1] across the target face/mip.
const char* kFullscreenVertexSrc = R"(
#version 460 core
out vec2 vUV;

const vec2 kQuad[6] = vec2[](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0)
);

void main() {
    vec2 p = kQuad[gl_VertexID];
    vUV = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// Shared by the three cube passes: rebuilds the world-space direction for this texel of the
// face being rendered, from the face's orthonormal basis uploaded as three uniforms.
const char* kCubeDirCommon = R"(
uniform vec3 uFaceForward;
uniform vec3 uFaceRight;
uniform vec3 uFaceUp;

vec3 FaceDirection(vec2 uv) {
    vec2 p = uv * 2.0 - 1.0; // [-1,1] across the face
    return normalize(uFaceForward + p.x * uFaceRight + p.y * uFaceUp);
}
)";

// Pass 1 — sky gradient into the environment cube. Mirrors Sky.cpp's fragment shader exactly
// for the upper hemisphere so reflections match the sky you actually see. Below the horizon
// Sky.cpp just clamps to the flat horizon colour (nothing is drawn down there anyway, the
// ground geometry covers it); a probe DOES get sampled by downward normals and by anything
// reflecting the floor, and a full-brightness lower hemisphere makes ambient read like a light
// box, so this darkens toward a dim ground bounce instead.
const char* kEnvFragmentSrc = R"(
#version 460 core
in vec2 vUV;
out vec4 FragColor;

uniform vec3 uHorizonColor;
uniform vec3 uZenithColor;
)"
R"(
void main() {
    vec3 dir = FaceDirection(vUV);
    vec3 color;
    if (dir.y >= 0.0) {
        color = mix(uHorizonColor, uZenithColor, pow(clamp(dir.y, 0.0, 1.0), 0.5));
    } else {
        color = mix(uHorizonColor, uHorizonColor * 0.3, pow(clamp(-dir.y, 0.0, 1.0), 0.5));
    }
    FragColor = vec4(color, 1.0);
}
)";

// Pass 2 — cosine-weighted hemisphere convolution. Fixed-step spherical march (not importance
// sampling): the input is a smooth gradient, so a regular grid converges with no visible noise
// and the whole pass is 6 * 32 * 32 texels.
const char* kIrradianceFragmentSrc = R"(
#version 460 core
in vec2 vUV;
out vec4 FragColor;

uniform samplerCube uEnvMap;

const float PI = 3.14159265359;
)"
R"(
void main() {
    vec3 N = FaceDirection(vUV);

    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    vec3 irradiance = vec3(0.0);
    float sampleCount = 0.0;
    const float kStep = 0.025;
    for (float phi = 0.0; phi < 2.0 * PI; phi += kStep * 4.0) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += kStep) {
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 dir = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            irradiance += texture(uEnvMap, dir).rgb * cos(theta) * sin(theta);
            sampleCount += 1.0;
        }
    }
    // The PI cancels the 1/PI of the Lambert BRDF, so the model shader multiplies this by
    // albedo directly. For a uniform environment of radiance L this returns exactly L.
    irradiance = PI * irradiance / max(sampleCount, 1.0);
    FragColor = vec4(irradiance, 1.0);
}
)";

// Pass 3 — GGX prefilter, one mip per roughness step. Standard split-sum first term.
const char* kPrefilterFragmentSrc = R"(
#version 460 core
in vec2 vUV;
out vec4 FragColor;

uniform samplerCube uEnvMap;
uniform float uRoughness;
uniform float uEnvResolution; // base face size of uEnvMap, for the mip-selection heuristic

const float PI = 3.14159265359;
const uint kSampleCount = 128u;

float RadicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint n) { return vec2(float(i) / float(n), RadicalInverseVdC(i)); }

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float DistributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}
)"
R"(
void main() {
    vec3 N = FaceDirection(vUV);
    vec3 R = N;
    vec3 V = N; // the split-sum approximation's standard N == V == R assumption

    vec3 prefiltered = vec3(0.0);
    float totalWeight = 0.0;

    for (uint i = 0u; i < kSampleCount; ++i) {
        vec2 Xi = Hammersley(i, kSampleCount);
        vec3 H = ImportanceSampleGGX(Xi, N, uRoughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;

        // Sample from a mip chosen by the sample's solid angle vs. a texel's, so sparse
        // high-roughness samples read a blurred mip instead of aliasing the base level.
        float NdotH = max(dot(N, H), 0.0);
        float HdotV = max(dot(H, V), 0.0);
        float D = DistributionGGX(NdotH, uRoughness);
        float pdf = (D * NdotH / (4.0 * max(HdotV, 1e-4))) + 1e-4;
        float saTexel = 4.0 * PI / (6.0 * uEnvResolution * uEnvResolution);
        float saSample = 1.0 / (float(kSampleCount) * pdf);
        float mip = uRoughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

        prefiltered += textureLod(uEnvMap, L, max(mip, 0.0)).rgb * NdotL;
        totalWeight += NdotL;
    }

    FragColor = vec4(prefiltered / max(totalWeight, 1e-4), 1.0);
}
)";

// Pass 4 — the split-sum second term: scale/bias on F0 as a function of (NdotV, roughness).
// Environment-independent, so this is baked once on first use and never rebaked.
const char* kBrdfFragmentSrc = R"(
#version 460 core
in vec2 vUV;
out vec2 FragColor;

const float PI = 3.14159265359;
const uint kSampleCount = 1024u;

float RadicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint n) { return vec2(float(i) / float(n), RadicalInverseVdC(i)); }

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// IBL uses the k = a^2/2 remap, NOT the (r+1)^2/8 direct-lighting one in ModelShaderSource.h.
float GeometrySchlickGGX(float NdotV, float roughness) {
    float a = roughness;
    float k = (a * a) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}
)"
R"(
void main() {
    float NdotV = max(vUV.x, 1e-3);
    float roughness = vUV.y;

    vec3 V = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    vec3 N = vec3(0.0, 0.0, 1.0);

    float A = 0.0;
    float B = 0.0;
    for (uint i = 0u; i < kSampleCount; ++i) {
        vec2 Xi = Hammersley(i, kSampleCount);
        vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        if (NdotL <= 0.0) continue;
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        float G = GeometrySchlickGGX(NdotL, roughness) * GeometrySchlickGGX(NdotV, roughness);
        float GVis = (G * VdotH) / max(NdotH * NdotV, 1e-4);
        float Fc = pow(1.0 - VdotH, 5.0);

        A += (1.0 - Fc) * GVis;
        B += Fc * GVis;
    }
    FragColor = vec2(A, B) / float(kSampleCount);
}
)";

// Face-local bases for GL_TEXTURE_CUBE_MAP layers 0..5 (+X, -X, +Y, -Y, +Z, -Z). The V axis is
// flipped relative to world up on the four side faces, matching OpenGL's (left-handed, +Y-down)
// cube-face convention — get this wrong and reflections come out mirrored.
struct FaceBasis { float forward[3]; float right[3]; float up[3]; };
const FaceBasis kFaces[6] = {
    {{ 1,  0,  0}, { 0,  0, -1}, { 0, -1,  0}}, // +X
    {{-1,  0,  0}, { 0,  0,  1}, { 0, -1,  0}}, // -X
    {{ 0,  1,  0}, { 1,  0,  0}, { 0,  0,  1}}, // +Y
    {{ 0, -1,  0}, { 1,  0,  0}, { 0,  0, -1}}, // -Y
    {{ 0,  0,  1}, { 1,  0,  0}, { 0, -1,  0}}, // +Z
    {{ 0,  0, -1}, {-1,  0,  0}, { 0, -1,  0}}, // -Z
};

std::string CubeFragment(const char* body) { return std::string(kCubeDirCommon) + body; }

// The cube passes need FaceDirection() declared before use but AFTER the #version line, so the
// shared block is spliced in right after the fragment source's own header rather than prepended.
std::string SpliceCubeCommon(const char* src) {
    std::string s(src);
    const std::string versionLine = "#version 460 core\n";
    size_t at = s.find(versionLine);
    if (at == std::string::npos) return CubeFragment(src); // shouldn't happen; still compiles
    at += versionLine.size();
    return s.substr(0, at) + kCubeDirCommon + s.substr(at);
}

} // namespace

IblProbe::~IblProbe() { Release(); }

void IblProbe::Release() {
    if (m_Fbo)            { glDeleteFramebuffers(1, &m_Fbo);          m_Fbo = 0; }
    if (m_Vao)            { glDeleteVertexArrays(1, &m_Vao);          m_Vao = 0; }
    if (m_EnvCube)        { glDeleteTextures(1, &m_EnvCube);          m_EnvCube = 0; }
    if (m_IrradianceCube) { glDeleteTextures(1, &m_IrradianceCube);   m_IrradianceCube = 0; }
    if (m_SpecularCube)   { glDeleteTextures(1, &m_SpecularCube);     m_SpecularCube = 0; }
    if (m_BrdfLut)        { glDeleteTextures(1, &m_BrdfLut);          m_BrdfLut = 0; }
    m_EnvShader.reset();
    m_IrradianceShader.reset();
    m_PrefilterShader.reset();
    m_BrdfShader.reset();
    m_Baked = false;
}

void IblProbe::EnsureCreated() {
    if (m_Fbo != 0) return;

    glCreateVertexArrays(1, &m_Vao);
    glCreateFramebuffers(1, &m_Fbo);

    const int envMips = 8; // 128 -> 1; the prefilter pass reads well down the chain
    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_EnvCube);
    glTextureStorage2D(m_EnvCube, envMips, GL_RGB16F, kEnvSize, kEnvSize);
    glTextureParameteri(m_EnvCube, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(m_EnvCube, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_EnvCube, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_EnvCube, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_EnvCube, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_IrradianceCube);
    glTextureStorage2D(m_IrradianceCube, 1, GL_RGB16F, kIrradianceSize, kIrradianceSize);
    glTextureParameteri(m_IrradianceCube, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_IrradianceCube, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_IrradianceCube, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_IrradianceCube, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_IrradianceCube, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_SpecularCube);
    glTextureStorage2D(m_SpecularCube, kSpecularMips, GL_RGB16F, kSpecularSize, kSpecularSize);
    glTextureParameteri(m_SpecularCube, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(m_SpecularCube, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_SpecularCube, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_SpecularCube, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_SpecularCube, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glCreateTextures(GL_TEXTURE_2D, 1, &m_BrdfLut);
    glTextureStorage2D(m_BrdfLut, 1, GL_RG16F, kBrdfLutSize, kBrdfLutSize);
    glTextureParameteri(m_BrdfLut, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_BrdfLut, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_BrdfLut, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_BrdfLut, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_EnvShader = std::make_unique<Shader>(kFullscreenVertexSrc, SpliceCubeCommon(kEnvFragmentSrc));
    m_IrradianceShader = std::make_unique<Shader>(kFullscreenVertexSrc, SpliceCubeCommon(kIrradianceFragmentSrc));
    m_PrefilterShader = std::make_unique<Shader>(kFullscreenVertexSrc, SpliceCubeCommon(kPrefilterFragmentSrc));
    m_BrdfShader = std::make_unique<Shader>(kFullscreenVertexSrc, kBrdfFragmentSrc);

    if (!m_EnvCube || !m_IrradianceCube || !m_SpecularCube || !m_BrdfLut)
        Log::Error("IblProbe: failed to create one or more probe textures");
}

void IblProbe::BeginCubeFace(unsigned int tex, int face, int mip, int size) const {
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    glNamedFramebufferTextureLayer(m_Fbo, GL_COLOR_ATTACHMENT0, tex, mip, face);
    const GLenum draw[1] = {GL_COLOR_ATTACHMENT0};
    glNamedFramebufferDrawBuffers(m_Fbo, 1, draw);
    glViewport(0, 0, size, size);
}

void IblProbe::SetFaceBasis(const Shader& shader, int face) {
    const FaceBasis& f = kFaces[face];
    shader.SetVec3("uFaceForward", glm::vec3(f.forward[0], f.forward[1], f.forward[2]));
    shader.SetVec3("uFaceRight",   glm::vec3(f.right[0],   f.right[1],   f.right[2]));
    shader.SetVec3("uFaceUp",      glm::vec3(f.up[0],      f.up[1],      f.up[2]));
}

bool IblProbe::NeedsBake(const glm::vec3& horizonColor, const glm::vec3& zenithColor) const {
    return !m_Baked || horizonColor != m_BakedHorizon || zenithColor != m_BakedZenith;
}

bool IblProbe::BakeIfDirty(const glm::vec3& horizonColor, const glm::vec3& zenithColor) {
    if (!NeedsBake(horizonColor, zenithColor)) return false;
    Bake(horizonColor, zenithColor);
    return true;
}

void IblProbe::Bake(const glm::vec3& horizonColor, const glm::vec3& zenithColor) {
    EnsureCreated();
    if (!m_Fbo) return;

    // Filtering across cube-face seams — without this the irradiance/prefilter passes read
    // clamped edge texels and every face boundary shows as a visible crease in reflections.
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    const GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean wasCull = glIsEnabled(GL_CULL_FACE);
    const GLboolean wasBlend = glIsEnabled(GL_BLEND);
    GLint prevDepthMask = GL_TRUE;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);

    glBindVertexArray(m_Vao);

    if (!m_BrdfLutBaked) BakeBrdfLut(); // once ever: environment-independent

    // --- Pass 1: sky gradient -> environment cube, then its mip chain --------------------
    m_EnvShader->Bind();
    m_EnvShader->SetVec3("uHorizonColor", horizonColor);
    m_EnvShader->SetVec3("uZenithColor", zenithColor);
    for (int face = 0; face < 6; ++face) {
        BeginCubeFace(m_EnvCube, face, 0, kEnvSize);
        SetFaceBasis(*m_EnvShader, face);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glGenerateTextureMipmap(m_EnvCube);

    // --- Pass 2: cosine convolution -> irradiance cube ------------------------------------
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_EnvCube);
    m_IrradianceShader->Bind();
    m_IrradianceShader->SetInt("uEnvMap", 0);
    for (int face = 0; face < 6; ++face) {
        BeginCubeFace(m_IrradianceCube, face, 0, kIrradianceSize);
        SetFaceBasis(*m_IrradianceShader, face);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // --- Pass 3: GGX prefilter -> specular cube, one mip per roughness step ---------------
    m_PrefilterShader->Bind();
    m_PrefilterShader->SetInt("uEnvMap", 0);
    m_PrefilterShader->SetFloat("uEnvResolution", (float)kEnvSize);
    for (int mip = 0; mip < kSpecularMips; ++mip) {
        int size = kSpecularSize >> mip;
        float roughness = (float)mip / (float)(kSpecularMips - 1);
        m_PrefilterShader->SetFloat("uRoughness", roughness);
        for (int face = 0; face < 6; ++face) {
            BeginCubeFace(m_SpecularCube, face, mip, size);
            SetFaceBasis(*m_PrefilterShader, face);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
    }

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (wasDepthTest) glEnable(GL_DEPTH_TEST);
    if (wasCull) glEnable(GL_CULL_FACE);
    if (wasBlend) glEnable(GL_BLEND);
    glDepthMask((GLboolean)prevDepthMask);

    m_Baked = true;
    m_BakedHorizon = horizonColor;
    m_BakedZenith = zenithColor;
}

void IblProbe::BakeBrdfLut() {
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    glNamedFramebufferTexture(m_Fbo, GL_COLOR_ATTACHMENT0, m_BrdfLut, 0);
    const GLenum draw[1] = {GL_COLOR_ATTACHMENT0};
    glNamedFramebufferDrawBuffers(m_Fbo, 1, draw);
    glViewport(0, 0, kBrdfLutSize, kBrdfLutSize);

    m_BrdfShader->Bind();
    glDrawArrays(GL_TRIANGLES, 0, 6);
    m_BrdfLutBaked = true;
}
