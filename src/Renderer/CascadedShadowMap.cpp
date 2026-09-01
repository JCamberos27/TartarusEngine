#include "CascadedShadowMap.h"
#include "Log.h"
#include "gl.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <string>

CascadedShadowMap::~CascadedShadowMap() { Release(); }

void CascadedShadowMap::Release() {
    if (m_Fbo)        { glDeleteFramebuffers(1, &m_Fbo);   m_Fbo = 0; }
    if (m_DepthArray) { glDeleteTextures(1, &m_DepthArray); m_DepthArray = 0; }
}

void CascadedShadowMap::Configure(int resolution, int count) {
    resolution = std::clamp(resolution, 256, 8192);
    count = std::clamp(count, 1, kMaxCascades);
    if (m_DepthArray != 0 && resolution == m_Resolution && count == m_Count) return;

    Release();
    m_Resolution = resolution;
    m_Count = count;

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_DepthArray);
    glTextureStorage3D(m_DepthArray, 1, GL_DEPTH_COMPONENT32F, resolution, resolution, count);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // outside the map = fully lit
    glTextureParameterfv(m_DepthArray, GL_TEXTURE_BORDER_COLOR, border);
    // Hardware depth comparison so a sampler2DArrayShadow tap returns a filtered 0..1 visibility.
    glTextureParameteri(m_DepthArray, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glCreateFramebuffers(1, &m_Fbo);
    glNamedFramebufferDrawBuffers(m_Fbo, 0, nullptr); // depth only
    // Attachment is (re)pointed at a specific layer in Begin().

    GLenum st = glCheckNamedFramebufferStatus(m_Fbo, GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        // A layered-attachment FBO reports complete only once a layer is attached; re-check in
        // Begin() would be noisy, so just warn if the base object itself failed to create.
        if (!m_Fbo || !m_DepthArray)
            Log::Error("CascadedShadowMap: failed to create depth array (" +
                       std::to_string(resolution) + "x" + std::to_string(count) + ")");
    }
}

void CascadedShadowMap::Update(const glm::mat4& camView, const glm::mat4& camProj,
                               const glm::vec3& lightDir, float shadowDistance) {
    glm::vec3 L = glm::length(lightDir) > 1e-6f ? glm::normalize(lightDir) : glm::vec3(0, -1, 0);

    // Full camera frustum corners in world space, from the inverse view-projection.
    glm::mat4 invVP = glm::inverse(camProj * camView);
    glm::vec3 nearC[4], farC[4];
    const glm::vec2 ndc[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < 4; ++i) {
        glm::vec4 n = invVP * glm::vec4(ndc[i], -1.0f, 1.0f);
        glm::vec4 f = invVP * glm::vec4(ndc[i],  1.0f, 1.0f);
        nearC[i] = glm::vec3(n) / n.w;
        farC[i]  = glm::vec3(f) / f.w;
    }

    // Camera near/far as view-space distances (row-major glm: -z is forward).
    float camNear = -(camView * glm::vec4(nearC[0], 1.0f)).z;
    float camFar  = -(camView * glm::vec4(farC[0],  1.0f)).z;
    camNear = std::max(camNear, 0.01f);
    camFar  = std::min(camFar, camNear + std::max(shadowDistance, 1.0f));

    // Practical split scheme (blend of logarithmic and uniform).
    const float lambda = 0.75f;
    for (int i = 0; i < m_Count; ++i) {
        float p = float(i + 1) / float(m_Count);
        float logSplit = camNear * std::pow(camFar / camNear, p);
        float uniSplit = camNear + (camFar - camNear) * p;
        m_SplitFar[i] = glm::mix(uniSplit, logSplit, lambda);
    }

    float prevFar = camNear;
    for (int c = 0; c < m_Count; ++c) {
        float t0 = (prevFar - camNear) / (camFar - camNear);
        float t1 = (m_SplitFar[c] - camNear) / (camFar - camNear);

        // 8 corners of this cascade's sub-frustum.
        glm::vec3 corners[8];
        for (int i = 0; i < 4; ++i) {
            glm::vec3 edge = farC[i] - nearC[i];
            corners[i]     = nearC[i] + edge * t0;
            corners[i + 4] = nearC[i] + edge * t1;
        }

        glm::vec3 center(0.0f);
        for (auto& p : corners) center += p;
        center /= 8.0f;

        float radius = 0.0f;
        for (auto& p : corners) radius = std::max(radius, glm::length(p - center));
        radius = std::ceil(radius * 16.0f) / 16.0f; // quantize -> less edge shimmer as the camera moves
        m_TexelWorld[c] = (2.0f * radius) / float(std::max(m_Resolution, 1)); // world units / texel (#117)

        glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        float pullback = radius + 50.0f; // capture occluders behind the slice along the light axis
        glm::mat4 lightView = glm::lookAt(center - L * pullback, center, up);
        glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, pullback + radius);

        // Texel-snap the projection so shadow edges don't crawl when the camera moves.
        glm::mat4 vp = lightProj * lightView;
        glm::vec4 origin = vp * glm::vec4(0, 0, 0, 1);
        origin *= float(m_Resolution) * 0.5f;
        glm::vec2 rounded = glm::round(glm::vec2(origin));
        glm::vec2 offset = (rounded - glm::vec2(origin)) * (2.0f / float(m_Resolution));
        lightProj[3][0] += offset.x;
        lightProj[3][1] += offset.y;

        m_LightViewProj[c] = lightProj * lightView;
        prevFar = m_SplitFar[c];
    }
}

void CascadedShadowMap::Begin(int i) const {
    i = std::clamp(i, 0, m_Count - 1);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    glNamedFramebufferTextureLayer(m_Fbo, GL_DEPTH_ATTACHMENT, m_DepthArray, 0, i);
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}

glm::vec4 CascadedShadowMap::SplitDepthsVec4() const {
    glm::vec4 v(m_SplitFar[m_Count - 1]);
    for (int i = 0; i < m_Count; ++i) v[i] = m_SplitFar[i];
    return v;
}

glm::vec4 CascadedShadowMap::TexelWorldSizesVec4() const {
    glm::vec4 v(m_TexelWorld[m_Count - 1]);
    for (int i = 0; i < m_Count; ++i) v[i] = m_TexelWorld[i];
    return v;
}
