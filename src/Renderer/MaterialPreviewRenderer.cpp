#include "MaterialPreviewRenderer.h"
#include "DefaultTextures.h"
#include "GLStateCache.h"
#include "IblProbe.h"
#include "Log.h"
#include "MaterialAsset.h"
#include "Model.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "ShaderVariant.h"
#include "Texture.h"
#include "gl.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {
// Studio sky the IBL is baked from: a bright neutral horizon under a cool zenith, so a chrome
// ball shows a readable horizon line and rough surfaces pick up soft, even ambient.
const glm::vec3 kHorizon(0.62f, 0.60f, 0.58f);
const glm::vec3 kZenith(0.20f, 0.26f, 0.36f);

constexpr GLenum kActiveTexture = 0x84E0; // GL_ACTIVE_TEXTURE (not in the trimmed loader header)

std::weak_ptr<IblProbe>& SharedIbl() {
    static std::weak_ptr<IblProbe> ibl;
    return ibl;
}
} // namespace

const char* MaterialPreviewRenderer::ShapeName(Shape s) {
    switch (s) {
    case Shape::Sphere:   return "Sphere";
    case Shape::Cube:     return "Cube";
    case Shape::Cylinder: return "Cylinder";
    case Shape::Torus:    return "Torus";
    case Shape::Plane:    return "Plane";
    default:              return "?";
    }
}

MaterialPreviewRenderer::MaterialPreviewRenderer() = default;

MaterialPreviewRenderer::~MaterialPreviewRenderer() {
    if (m_ColorTex) glDeleteTextures(1, &m_ColorTex);
    if (m_DepthRBO) glDeleteRenderbuffers(1, &m_DepthRBO);
    if (m_FBO) glDeleteFramebuffers(1, &m_FBO);
    if (m_BackdropTex) glDeleteTextures(1, &m_BackdropTex);
    if (m_BackdropFBO) glDeleteFramebuffers(1, &m_BackdropFBO);
    if (m_OutTex) glDeleteTextures(1, &m_OutTex);
    if (m_OutFBO) glDeleteFramebuffers(1, &m_OutFBO);
    if (m_EmptyVAO) glDeleteVertexArrays(1, &m_EmptyVAO);
}

std::uint64_t MaterialPreviewRenderer::ContentKey(const MaterialAsset& m) {
    std::uint64_t h = m.Mat.Hash();
    auto mix = [&h](const void* p, std::size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    auto mixTex = [&](const std::shared_ptr<Texture>& t) {
        const unsigned int id = t ? t->GLHandle() : 0u;
        mix(&id, sizeof(id));
    };
    const Material& mat = m.Mat;
    for (const auto* t : { &mat.AlbedoMap, &mat.NormalMap, &mat.MetallicRoughnessMap, &mat.MetallicMap,
                           &mat.RoughnessMap, &mat.AOMap, &mat.EmissiveMap, &mat.ClearCoatMap,
                           &mat.ThicknessMap, &mat.HeightMap, &mat.DetailAlbedoMap, &mat.DetailNormalMap })
        mixTex(*t);
    const int queue = (int)m.RenderQueue;
    mix(&queue, sizeof(queue));
    mix(&m.Opacity, sizeof(m.Opacity));
    const void* shader = m.Shader.get();
    mix(&shader, sizeof(shader));
    mix(&m.Missing, sizeof(m.Missing));
    // Custom shader properties. An unordered_map's order is stable while it isn't modified, but
    // two maps with the same contents can iterate differently, so combine order-independently.
    std::uint64_t props = 0;
    for (const auto& [name, p] : mat.ExtraProps) {
        std::uint64_t e = 1469598103934665603ull;
        auto emix = [&e](const void* d, std::size_t n) {
            const unsigned char* b = static_cast<const unsigned char*>(d);
            for (std::size_t i = 0; i < n; ++i) { e ^= b[i]; e *= 1099511628211ull; }
        };
        emix(name.data(), name.size());
        const int type = (int)p.Type;
        emix(&type, sizeof(type));
        emix(&p.F, sizeof(p.F));
        emix(&p.V, sizeof(p.V));
        emix(&p.B, sizeof(p.B));
        emix(&p.I, sizeof(p.I));
        const unsigned int tex = p.Tex ? p.Tex->GLHandle() : 0u;
        emix(&tex, sizeof(tex));
        props += e * 0x9E3779B97F4A7C15ull;
    }
    mix(&props, sizeof(props));
    for (const std::string& k : mat.ShaderKeywords) { mix(k.data(), k.size()); mix("|", 1); }
    return h;
}

void MaterialPreviewRenderer::EnsureResources(int width, int height) {
    if (!m_Fallback) {
        m_Fallback = std::make_unique<Shader>(ShaderLibrary::ReadFile("ModelVertex.glsl"),
                                              ShaderLibrary::ReadFile("ModelFragment.glsl"),
                                              "MaterialPreview");
        m_Backdrop = std::make_unique<Shader>(ShaderLibrary::ReadFile("ChannelPreview.vert.glsl"),
                                              ShaderLibrary::ReadFile("MaterialPreviewBackdrop.frag.glsl"),
                                              "MaterialPreviewBackdrop");
        static const char* kKinds[] = { "sphere", "cube", "cylinder", "donut", "plane" };
        for (size_t i = 0; i < m_Shapes.size(); ++i)
            m_Shapes[i] = Model::CreatePrimitive(kKinds[i], std::string("<material preview ") + kKinds[i] + ">");
        glGenFramebuffers(1, &m_FBO);
        glGenFramebuffers(1, &m_BackdropFBO);
        glGenFramebuffers(1, &m_OutFBO);
        glGenVertexArrays(1, &m_EmptyVAO);
    }
    if (!m_Ibl) {
        m_Ibl = SharedIbl().lock();
        if (!m_Ibl) {
            m_Ibl = std::make_shared<IblProbe>();
            SharedIbl() = m_Ibl;
        }
    }
    if (width == m_W && height == m_H && m_ColorTex) return;
    m_W = width;
    m_H = height;
    m_BackdropChecker = -1;

    if (m_ColorTex) glDeleteTextures(1, &m_ColorTex);
    if (m_DepthRBO) glDeleteRenderbuffers(1, &m_DepthRBO);
    if (m_BackdropTex) glDeleteTextures(1, &m_BackdropTex);
    if (m_OutTex) glDeleteTextures(1, &m_OutTex);

    // The output, at the requested size; everything is drawn at kSupersample x and filtered
    // down into it (smooth silhouettes and specular highlights without an MSAA target).
    glGenTextures(1, &m_OutTex);
    glBindTexture(GL_TEXTURE_2D, m_OutTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    width *= kSupersample;
    height *= kSupersample;

    glGenTextures(1, &m_ColorTex);
    glBindTexture(GL_TEXTURE_2D, m_ColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &m_DepthRBO);
    glBindRenderbuffer(GL_RENDERBUFFER, m_DepthRBO);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    // Mipped, like the scene's opaque-colour capture: frosted (rough) transmission samples blur.
    // (glGenerateTextureMipmap after each backdrop draw allocates and fills the chain.)
    glGenTextures(1, &m_BackdropTex);
    glBindTexture(GL_TEXTURE_2D, m_BackdropTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void MaterialPreviewRenderer::DrawBackdrop(bool checker, bool tonemap, int width, int height) {
    m_Backdrop->Bind();
    m_Backdrop->SetInt("uChecker", checker ? 1 : 0);
    m_Backdrop->SetInt("uTonemap", tonemap ? 1 : 0);
    m_Backdrop->SetVec2("uSize", glm::vec2((float)width, (float)height));
    glBindVertexArray(m_EmptyVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

unsigned int MaterialPreviewRenderer::Render(const std::shared_ptr<MaterialAsset>& material, Shape shape,
                                             float yaw, float pitch, int width, int height) {
    if (!material || width <= 0 || height <= 0) return 0;
    width = std::min(width, 4096 / kSupersample);
    height = std::min(height, 4096 / kSupersample);
    const int outW = width, outH = height;
    const int shapeIndex = std::clamp((int)shape, 0, (int)Shape::Count - 1);

    // Everything below runs nested inside the editor's ImGui pass: put back every piece of GL
    // state it touches.
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevReadFBO = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
    const GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    const GLboolean prevCull = glIsEnabled(GL_CULL_FACE);
    const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    GLint prevBlendSrcRgb = 0, prevBlendDstRgb = 0, prevBlendSrcA = 0, prevBlendDstA = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstA);
    GLint prevDepthFunc = GL_LESS, prevCullMode = GL_BACK;
    glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    glGetIntegerv(GL_CULL_FACE_MODE, &prevCullMode);
    GLint prevActiveTex = GL_TEXTURE0;
    glGetIntegerv(kActiveTexture, &prevActiveTex);
    auto restore = [&]() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned int)prevFBO);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int)prevReadFBO);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        prevDepthTest ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
        prevBlend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
        prevCull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
        prevScissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
        glDepthMask(prevDepthMask);
        glDepthFunc((GLenum)prevDepthFunc);
        glCullFace((GLenum)prevCullMode);
        glBlendFuncSeparate((GLenum)prevBlendSrcRgb, (GLenum)prevBlendDstRgb, (GLenum)prevBlendSrcA, (GLenum)prevBlendDstA);
        glActiveTexture((GLenum)prevActiveTex);
        // Raw binds below bypassed the cache: the scene's next Texture::Bind / Shader::Bind
        // must not be skipped as redundant.
        GLStateCache::Invalidate();
    };

    try {
        EnsureResources(outW, outH);
    } catch (const std::exception& e) {
        Log::Error(std::string("Material preview: ") + e.what());
        restore();
        return 0;
    }
    GLStateCache::Invalidate();
    width = outW * kSupersample; // the internal render size from here on
    height = outH * kSupersample;

    // IBL: baked once (the studio sky never changes); Bake leaves FBO 0 bound and its own
    // viewport, both restored by the binds below.
    if (m_Ibl->NeedsBake(kHorizon, kZenith)) m_Ibl->Bake(kHorizon, kZenith);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glViewport(0, 0, width, height);

    const bool seeThrough = material->RenderQueue != MaterialAsset::Queue::Opaque || material->Mat.AlphaClip ||
                            material->Mat.TransmissionStrength > 0.0f;

    // 1. Linear backdrop -> refraction source (only re-rendered when it would differ).
    if (m_BackdropChecker != (seeThrough ? 1 : 0)) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_BackdropFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_BackdropTex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            DrawBackdrop(seeThrough, false, width, height);
            glGenerateTextureMipmap(m_BackdropTex);
            m_BackdropChecker = seeThrough ? 1 : 0;
        }
    }

    // 2. Visible target: the tonemapped backdrop, then the shape.
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ColorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_DepthRBO);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Log::Error("Material preview: framebuffer incomplete (0x" + std::to_string(status) + ")");
        restore();
        return 0;
    }
    glClear(GL_DEPTH_BUFFER_BIT);
    DrawBackdrop(seeThrough, true, width, height);

    Model& model = *m_Shapes[(size_t)shapeIndex];
    const glm::vec3 bmin = model.BoundsMin(), bmax = model.BoundsMax();
    const glm::vec3 centre = (bmin + bmax) * 0.5f;
    // Radius of the sphere around the shape: the box corner for the cube, the widest extent for
    // round shapes (their box corners are empty space), a little over it for the rest.
    const glm::vec3 half = (bmax - bmin) * 0.5f;
    const float widest = std::max({ half.x, half.y, half.z });
    const float radius = std::max(0.01f, shape == Shape::Cube   ? glm::length(half) * 0.92f
                                       : shape == Shape::Sphere ? widest
                                       : shape == Shape::Plane  ? widest * 1.05f
                                                                : widest * 1.12f);
    // Fill most of the frame (a material preview is about the surface, not the silhouette).
    const float fov = glm::radians(30.0f);
    const float distance = radius / std::sin(fov * 0.5f) * 1.22f;
    const float clampedPitch = std::clamp(pitch, -1.45f, 1.45f);
    const glm::vec3 eye = centre + distance * glm::vec3(std::cos(clampedPitch) * std::sin(yaw),
                                                        std::sin(clampedPitch),
                                                        std::cos(clampedPitch) * std::cos(yaw));
    const glm::mat4 view = glm::lookAt(eye, centre, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(fov, (float)width / (float)height,
                                            std::max(0.01f, distance - radius * 2.0f), distance + radius * 2.0f);

    // Lights fixed to the camera, so orbiting shows the surface from every side under the same
    // studio lighting (Unity's preview behaves the same way).
    const glm::mat3 camToWorld = glm::transpose(glm::mat3(view));
    m_Lights.Clear();
    m_Lights.AddDirectional(glm::normalize(camToWorld * glm::vec3(0.55f, -0.6f, -0.6f)), glm::vec3(1.0f, 0.97f, 0.92f), 2.6f, false);
    m_Lights.AddDirectional(glm::normalize(camToWorld * glm::vec3(-0.7f, 0.25f, -0.4f)), glm::vec3(0.75f, 0.85f, 1.0f), 0.6f, false);
    m_Lights.Upload();
    m_Lights.Bind(0);

    const bool transparent = material->RenderQueue == MaterialAsset::Queue::Transparent;
    std::vector<Shader*> prepared;
    auto prepare = [&](Shader& p) {
        p.Bind();
        p.SetMat4("uView", view);
        p.SetMat4("uProj", proj);
        p.SetVec3("uViewPos", eye);
        p.SetInt("uDebugView", 0);
        p.SetInt("uUnlit", 0);
        p.SetInt("uApplyTonemap", 1);
        p.SetInt("uAlphaBlend", transparent ? 1 : 0);
        p.SetInt("uShadowEnabled", 0);
        p.SetInt("uShadowCascadeCount", 0);
        p.SetInt("uSpotShadowCount", 0);
        p.SetInt("uPointShadowCount", 0);
        p.SetInt("uNoReceiveShadows", 1);
        p.SetInt("uObjectLayerBit", 0);
        p.SetInt("uClusterEnabled", 0);
        p.SetInt("uSSAOEnabled", 0);
        p.SetInt("uFogMode", 0);
        p.SetInt("uProbeCount", 0);
        p.SetInt("uShadowMap", 8);
        p.SetInt("uSpotShadowMap", 9);
        p.SetInt("uPointShadowMap", 10);
        p.SetInt("uSSAOMap", 15);
        p.SetInt("uIBLEnabled", m_Ibl->IsValid() ? 1 : 0);
        p.SetFloat("uIBLIntensity", 1.0f);
        p.SetFloat("uIBLSpecularMaxLod", (float)(IblProbe::kSpecularMips - 1));
        p.SetInt("uOpaqueColor", 14);
        p.SetVec2("uScreenSize", glm::vec2((float)width, (float)height));
    };
    auto selectProgram = [&](const MaterialAsset* ma) -> Shader* {
        Shader* prog = m_Fallback.get();
        if (ma && ma->Shader)
            if (Shader* v = ma->Shader->Variant(ShaderVariantKeyFor(ma->Mat, *ma->Shader))) prog = v;
        if (std::find(prepared.begin(), prepared.end(), prog) == prepared.end()) {
            prepare(*prog);
            prepared.push_back(prog);
        }
        return prog;
    };

    // Texture units the model shader samples besides the material maps: valid, correctly-typed
    // textures on each, as in the scene (a sampler with no texture of its type is undefined).
    glActiveTexture(GL_TEXTURE0 + 8);
    glBindTexture(GL_TEXTURE_2D_ARRAY, DefaultTextures::DepthArray());
    glActiveTexture(GL_TEXTURE0 + 9);
    glBindTexture(GL_TEXTURE_2D_ARRAY, DefaultTextures::DepthArray());
    glActiveTexture(GL_TEXTURE0 + 10);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, DefaultTextures::DepthCubeArray());
    glActiveTexture(GL_TEXTURE0 + 11);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_Ibl->IrradianceMap());
    glActiveTexture(GL_TEXTURE0 + 12);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_Ibl->SpecularMap());
    glActiveTexture(GL_TEXTURE0 + 13);
    glBindTexture(GL_TEXTURE_2D, m_Ibl->BrdfLut());
    glActiveTexture(GL_TEXTURE0 + 14);
    glBindTexture(GL_TEXTURE_2D, m_BackdropTex);
    glActiveTexture(GL_TEXTURE0 + 15);
    glBindTexture(GL_TEXTURE_2D, DefaultTextures::White());
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    if (transparent) { // the scene's transparent pass state
        glEnable(GL_BLEND);
        glDepthMask(GL_FALSE);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    const std::vector<std::shared_ptr<MaterialAsset>> slots{ material };
    const Model::MeshPass pass = transparent ? Model::MeshPass::Transparent : Model::MeshPass::Opaque;
    try {
        model.DrawSelected(*m_Fallback, glm::mat4(1.0f), slots, selectProgram, 1.0f, {}, pass);
    } catch (const std::exception& e) {
        Log::Error(std::string("Material preview: ") + e.what());
    }

    // Filter the supersampled render down into the output (an exact 2x2 box at kSupersample 2).
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_FBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_OutFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_OutTex, 0);
    glBlitFramebuffer(0, 0, width, height, 0, 0, outW, outH, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    restore();
    return m_OutTex;
}
