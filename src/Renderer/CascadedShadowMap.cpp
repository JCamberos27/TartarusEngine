#include "CascadedShadowMap.h"
#include "GLFramebufferCheck.h"
#include "Log.h"
#include "gl.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

CascadedShadowMap::~CascadedShadowMap() { Release(); }

void CascadedShadowMap::Release() {
    for (unsigned int& fbo : m_Fbos)
        if (fbo) { glDeleteFramebuffers(1, &fbo); fbo = 0; }
    if (m_LayeredFbo) { glDeleteFramebuffers(1, &m_LayeredFbo); m_LayeredFbo = 0; }
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

    glCreateFramebuffers(count, m_Fbos.data());
    for (int i = 0; i < count; ++i) {
        glNamedFramebufferDrawBuffers(m_Fbos[i], 0, nullptr); // depth only
        glNamedFramebufferTextureLayer(m_Fbos[i], GL_DEPTH_ATTACHMENT, m_DepthArray, 0, i);
    }
    glCreateFramebuffers(1, &m_LayeredFbo);
    glNamedFramebufferDrawBuffers(m_LayeredFbo, 0, nullptr);
    glNamedFramebufferTexture(m_LayeredFbo, GL_DEPTH_ATTACHMENT, m_DepthArray, 0);
    // The completeness check stays in Begin() (audit GL-204), once per Configure().
    m_CompleteChecked = false; // re-check after a resolution/count change

    if (!m_Fbos[0] || !m_DepthArray)
        Log::Error("CascadedShadowMap: failed to create depth array (" +
                   std::to_string(resolution) + "x" + std::to_string(count) + ")");
}

void CascadedShadowMap::Update(const glm::mat4& camView, const glm::mat4& camProj,
                               const glm::vec3& lightDir, float shadowDistance,
                               const glm::vec3* casterMin, const glm::vec3* casterMax) {
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
        // Capture occluders behind the slice along the light axis. #160: far enough back to take
        // in every caster (the furthest AABB corner toward the light), not a fixed 50 m that
        // clipped tall/distant casters; capped so a stray far-off object can't wreck depth precision.
        float pullback = radius + 50.0f;
        if (casterMin && casterMax && casterMin->x <= casterMax->x) {
            float reach = 0.0f;
            for (int k = 0; k < 8; ++k) {
                const glm::vec3 p((k & 1) ? casterMax->x : casterMin->x, (k & 2) ? casterMax->y : casterMin->y,
                                  (k & 4) ? casterMax->z : casterMin->z);
                reach = std::max(reach, glm::dot(p - center, -L));
            }
            pullback = std::clamp(reach + 1.0f, radius + 1.0f, radius + 2000.0f);
        }
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

    // Receiver slices (CasterReachesSlice). Unlike the fit above, these place each view depth on
    // the frustum edges exactly: a point's view depth runs linearly from the near to the far plane
    // along each corner edge. Cascade c is read from 0.88 x the previous split (the cross-fade
    // band) to its own split; the last one also by everything past the final split, to the far plane.
    const float realFar = std::max(-(camView * glm::vec4(farC[0], 1.0f)).z, camNear + 1e-3f);
    for (int c = 0; c < m_Count; ++c) {
        const float lo = c == 0 ? camNear : std::max(camNear, m_SplitFar[c - 1] * 0.88f);
        const float hi = c == m_Count - 1 ? realFar : std::min(m_SplitFar[c], realFar);
        const float t0 = (lo - camNear) / (realFar - camNear), t1 = (hi - camNear) / (realFar - camNear);
        for (int i = 0; i < 4; ++i) {
            const glm::vec3 edge = farC[i] - nearC[i];
            for (int k = 0; k < 2; ++k) {
                const glm::vec4 p = m_LightViewProj[c] * glm::vec4(nearC[i] + edge * (k ? t1 : t0), 1.0f);
                m_SliceCorners[c][i + 4 * k] = glm::vec2(p) / p.w;
            }
        }
    }
}

bool CascadedShadowMap::CasterReachesSlice(int c, const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                                           float kernelTexels, float normalBias) const {
    if (c <= 0 || c >= m_Count) return true;
    // The caster's footprint: its bounds' light clip-space x/y extent, grown by what the shader can
    // reach from a receiver - the Poisson kernel (longest offset ~1.23 radii, radius widened 35% per
    // cascade), the 2x2 compare, the normal offset (at most one texel x normalBias) and a texel of slack.
    const glm::mat4& vp = m_LightViewProj[c];
    glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (int k = 0; k < 8; ++k) {
        const glm::vec3 p((k & 1) ? boundsMax.x : boundsMin.x, (k & 2) ? boundsMax.y : boundsMin.y,
                          (k & 4) ? boundsMax.z : boundsMin.z);
        const glm::vec4 q = vp * glm::vec4(p, 1.0f);
        const glm::vec2 xy = glm::vec2(q) / q.w;
        lo = glm::min(lo, xy);
        hi = glm::max(hi, xy);
    }
    const float texels = std::max(kernelTexels, 0.5f) * (1.0f + 0.35f * (float)c) * 1.25f + std::max(normalBias, 0.0f) + 3.0f;
    const float margin = texels * 2.0f / (float)std::max(m_Resolution, 1);
    lo -= glm::vec2(margin);
    hi += glm::vec2(margin);

    // Separating axes between that rectangle and the slice's convex footprint: the rectangle's two
    // axes, then the direction between every pair of slice corners (a superset of the hull's edges,
    // so a separation found is real and every hull edge is tried).
    const std::array<glm::vec2, 8>& s = m_SliceCorners[c];
    glm::vec2 sMin = s[0], sMax = s[0];
    for (const glm::vec2& p : s) { sMin = glm::min(sMin, p); sMax = glm::max(sMax, p); }
    if (sMax.x < lo.x || sMin.x > hi.x || sMax.y < lo.y || sMin.y > hi.y) return false;
    const glm::vec2 rect[4] = {{lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
    for (int a = 0; a < 8; ++a)
        for (int b = a + 1; b < 8; ++b) {
            const glm::vec2 d = s[b] - s[a];
            const glm::vec2 n(-d.y, d.x);
            if (glm::dot(n, n) < 1e-12f) continue;
            float s0 = std::numeric_limits<float>::max(), s1 = -s0, r0 = s0, r1 = -s0;
            for (const glm::vec2& p : s) { const float v = glm::dot(n, p); s0 = std::min(s0, v); s1 = std::max(s1, v); }
            for (const glm::vec2& p : rect) { const float v = glm::dot(n, p); r0 = std::min(r0, v); r1 = std::max(r1, v); }
            if (s1 < r0 || r1 < s0) return false;
        }
    return true;
}

void CascadedShadowMap::Begin(int i) const {
    i = std::clamp(i, 0, m_Count - 1);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbos[i]);
    // audit GL-204 — one-shot completeness check of every cascade's FBO.
    if (!m_CompleteChecked) {
        m_CompleteChecked = true;
        for (int c = 0; c < m_Count; ++c)
            GLFramebufferCheck::Complete("CascadedShadowMap", m_Fbos[c], m_Resolution, m_Resolution);
    }
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void CascadedShadowMap::BeginLayered() const {
    glBindFramebuffer(GL_FRAMEBUFFER, m_LayeredFbo);
    if (!m_CompleteChecked) {
        m_CompleteChecked = true;
        GLFramebufferCheck::Complete("CascadedShadowMap (layered)", m_LayeredFbo, m_Resolution, m_Resolution);
    }
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT); // every layer of a layered attachment
}

bool CascadedShadowMap::LayeredSupported() {
    static const bool supported = [] {
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; ++i) {
            const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, (GLuint)i));
            if (!ext) continue;
            if (std::strcmp(ext, "GL_ARB_shader_viewport_layer_array") == 0 ||
                std::strcmp(ext, "GL_AMD_vertex_shader_layer") == 0)
                return true;
        }
        return false;
    }();
    return supported;
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
