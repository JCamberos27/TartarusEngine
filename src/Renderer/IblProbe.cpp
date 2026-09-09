#include "IblProbe.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "Log.h"
#include "gl.h"

namespace {

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

    m_EnvShader = std::make_unique<Shader>(ShaderLibrary::ReadFile("Ibl.vert.glsl"),
                                           ShaderLibrary::ReadFile("IblEnv.frag.glsl"));
    m_IrradianceShader = std::make_unique<Shader>(ShaderLibrary::ReadFile("Ibl.vert.glsl"),
                                                  ShaderLibrary::ReadFile("IblIrradiance.frag.glsl"));
    m_PrefilterShader = std::make_unique<Shader>(ShaderLibrary::ReadFile("Ibl.vert.glsl"),
                                                 ShaderLibrary::ReadFile("IblPrefilter.frag.glsl"));
    m_BrdfShader = std::make_unique<Shader>(ShaderLibrary::ReadFile("Ibl.vert.glsl"),
                                            ShaderLibrary::ReadFile("IblBrdf.frag.glsl"));

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

void IblProbe::BakeFromCubemap(unsigned int envCube) {
    EnsureCreated();
    if (!m_Fbo) return;

    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    const GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean wasCull      = glIsEnabled(GL_CULL_FACE);
    const GLboolean wasBlend     = glIsEnabled(GL_BLEND);
    GLint prevDepthMask = GL_TRUE;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);

    glBindVertexArray(m_Vao);
    if (!m_BrdfLutBaked) BakeBrdfLut();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, envCube);

    // Pass 2: cosine convolution -> irradiance cube
    m_IrradianceShader->Bind();
    m_IrradianceShader->SetInt("uEnvMap", 0);
    for (int face = 0; face < 6; ++face) {
        BeginCubeFace(m_IrradianceCube, face, 0, kIrradianceSize);
        SetFaceBasis(*m_IrradianceShader, face);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // Pass 3: GGX prefilter -> specular cube
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
    if (wasCull)      glEnable(GL_CULL_FACE);
    if (wasBlend)     glEnable(GL_BLEND);
    glDepthMask((GLboolean)prevDepthMask);

    m_Baked = true;
    // Sentinel so NeedsBake() returns true again when switching back to the procedural sky.
    m_BakedHorizon = glm::vec3(-1.0f);
    m_BakedZenith  = glm::vec3(-1.0f);
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
