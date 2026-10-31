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

void CascadedShadowMap::SetSplitLambda(float lambda) {
    m_SplitLambda = std::isfinite(lambda) ? std::clamp(lambda, 0.0f, 0.98f) : 0.75f;
}

float CascadedShadowMap::SunKernelTexels(float angularDeg, float softness) {
    if (!(softness > 0.0f)) return 0.0f;
    return std::clamp(angularDeg * 3.0f, 1.0f, 14.0f) * softness;
}

void CascadedShadowMap::Update(const glm::mat4& camView, const glm::mat4& camProj,
                               const glm::vec3& lightDir, float shadowDistance,
                               const glm::vec3* casterMin, const glm::vec3* casterMax) {
    glm::vec3 L = glm::length(lightDir) > 1e-6f ? glm::normalize(lightDir) : glm::vec3(0, -1, 0);

    // Full camera frustum corners in world space, from the inverse view-projection.
    glm::mat4 invVP = glm::inverse(camProj * camView);
    m_Eye = glm::vec3(glm::inverse(camView)[3]);
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
    // The corners below run the whole frustum, near plane to the camera's own far plane: a split distance is a
    // fraction of that, not of the shadow distance (which made every cascade 10-50x too big).
    const float frustumFar = std::max(camFar, camNear + 1e-3f);
    camFar  = std::min(camFar, camNear + std::max(shadowDistance, 1.0f));

    // Practical split scheme (blend of logarithmic and uniform).
    const float lambda = m_SplitLambda;
    for (int i = 0; i < m_Count; ++i) {
        float p = float(i + 1) / float(m_Count);
        float logSplit = camNear * std::pow(camFar / camNear, p);
        float uniSplit = camNear + (camFar - camNear) * p;
        m_SplitFar[i] = glm::mix(uniSplit, logSplit, lambda);
    }

    float prevFar = camNear;
    for (int c = 0; c < m_Count; ++c) {
        float t0 = (prevFar - camNear) / (frustumFar - camNear);
        float t1 = (m_SplitFar[c] - camNear) / (frustumFar - camNear);

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
        // The slice's footprint is the convex hull of its corners: SAT against an axis-aligned rectangle
        // needs only the rectangle's axes (the bounds test) and the hull's edge normals.
        const std::array<glm::vec2, 8>& s = m_SliceCorners[c];
        m_SliceMin[c] = m_SliceMax[c] = s[0];
        for (const glm::vec2& p : s) { m_SliceMin[c] = glm::min(m_SliceMin[c], p); m_SliceMax[c] = glm::max(m_SliceMax[c], p); }
        std::array<glm::vec2, 8> pts = s;
        std::sort(pts.begin(), pts.end(), [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
        std::array<glm::vec2, 16> hull{};
        int h = 0;
        const auto cross = [](const glm::vec2& o, const glm::vec2& a, const glm::vec2& b) {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        };
        for (int i = 0; i < 8; ++i) { // monotone chain: lower hull, then upper
            while (h >= 2 && cross(hull[h - 2], hull[h - 1], pts[i]) <= 0.0f) --h;
            hull[h++] = pts[i];
        }
        for (int i = 6, lower = h + 1; i >= 0; --i) {
            while (h >= lower && cross(hull[h - 2], hull[h - 1], pts[i]) <= 0.0f) --h;
            hull[h++] = pts[i];
        }
        int axes = 0;
        for (int i = 0; i + 1 < h && axes < 8; ++i) {
            const glm::vec2 d = hull[i + 1] - hull[i];
            const glm::vec2 n(-d.y, d.x);
            if (glm::dot(n, n) < 1e-12f) continue;
            SliceAxis& ax = m_SliceAxes[c][axes++];
            ax.N = n;
            ax.Min = std::numeric_limits<float>::max();
            ax.Max = -ax.Min;
            for (const glm::vec2& p : s) { const float v = glm::dot(n, p); ax.Min = std::min(ax.Min, v); ax.Max = std::max(ax.Max, v); }
        }
        m_SliceAxisCount[c] = axes;
    }
}

// The light clip-space rectangle cascade c's receivers can read: the slice footprint's bounding box (for cascade 0 also
// everything within kViewModelReach of the eye: the view model draws at its own field of view, so its fragments can sit
// outside the world view's frustum near the eye), grown by what SunShadow reaches from a receiver - the Poisson kernel
// (longest offset ~1.23 radii, radius widened 35% per cascade), the 2x2 compare, the normal offset (at most one texel x
// normalBias) and a texel of slack.
void CascadedShadowMap::ReadRect(int c, float kernelTexels, float normalBias, glm::vec2& lo, glm::vec2& hi) const {
    const float res = (float)std::max(m_Resolution, 1);
    const float texels = (kernelTexels > 0.0f ? std::max(kernelTexels, 0.5f) * (1.0f + 0.35f * (float)c) * 1.25f : 0.0f) +
                         std::max(normalBias, 0.0f) + 3.0f;
    lo = m_SliceMin[c];
    hi = m_SliceMax[c];
    if (c == 0) {
        constexpr float kViewModelReach = 2.0f; // metres
        const glm::vec4 e = m_LightViewProj[0] * glm::vec4(m_Eye, 1.0f);
        const glm::vec2 eye = glm::vec2(e) / e.w;
        const float r = kViewModelReach * 2.0f / std::max(m_TexelWorld[0] * res, 1e-6f); // metres -> light NDC
        lo = glm::min(lo, eye - glm::vec2(r));
        hi = glm::max(hi, eye + glm::vec2(r));
    }
    const float margin = texels * 2.0f / res;
    lo -= glm::vec2(margin);
    hi += glm::vec2(margin);
}

bool CascadedShadowMap::CasterReachesSlice(int c, const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                                           float kernelTexels, float normalBias) const {
    if (c < 0 || c >= m_Count) return true;
    if (c == 0) { // the near slice plus the eye's surroundings (ReadRect): its bounding box is the whole test
        glm::vec2 rlo, rhi;
        ReadRect(0, kernelTexels, normalBias, rlo, rhi);
        const glm::mat4& vp0 = m_LightViewProj[0];
        glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        for (int k = 0; k < 8; ++k) {
            const glm::vec3 p((k & 1) ? boundsMax.x : boundsMin.x, (k & 2) ? boundsMax.y : boundsMin.y,
                              (k & 4) ? boundsMax.z : boundsMin.z);
            const glm::vec4 q = vp0 * glm::vec4(p, 1.0f);
            const glm::vec2 xy = glm::vec2(q) / q.w;
            lo = glm::min(lo, xy);
            hi = glm::max(hi, xy);
        }
        return !(rhi.x < lo.x || rlo.x > hi.x || rhi.y < lo.y || rlo.y > hi.y);
    }
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
    // A hard kernel (0) still reaches the 2x2 compare's neighbour; the 3-texel slack covers it.
    const float texels = (kernelTexels > 0.0f ? std::max(kernelTexels, 0.5f) * (1.0f + 0.35f * (float)c) * 1.25f : 0.0f) +
                         std::max(normalBias, 0.0f) + 3.0f;
    const float margin = texels * 2.0f / (float)std::max(m_Resolution, 1);
    lo -= glm::vec2(margin);
    hi += glm::vec2(margin);

    // Separating axes between that rectangle and the slice's convex footprint: the rectangle's two
    // axes, then the slice hull's edge normals (with the slice's extent along each, from Update).
    if (m_SliceMax[c].x < lo.x || m_SliceMin[c].x > hi.x || m_SliceMax[c].y < lo.y || m_SliceMin[c].y > hi.y) return false;
    const glm::vec2 mid = (lo + hi) * 0.5f, half = (hi - lo) * 0.5f;
    for (int a = 0; a < m_SliceAxisCount[c]; ++a) {
        const SliceAxis& ax = m_SliceAxes[c][a];
        const float centre = glm::dot(ax.N, mid), radius = std::abs(ax.N.x) * half.x + std::abs(ax.N.y) * half.y;
        if (ax.Max < centre - radius || centre + radius < ax.Min) return false;
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

bool CascadedShadowMap::ViewportIndexSupported() {
    static const bool supported = [] {
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; ++i) {
            const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, (GLuint)i));
            if (ext && std::strcmp(ext, "GL_ARB_shader_viewport_layer_array") == 0) return true;
        }
        return false;
    }();
    return supported;
}

void CascadedShadowMap::BeginLayeredScissor(float kernelTexels, float normalBias) const {
    const float res = (float)std::max(m_Resolution, 1);
    for (int c = 0; c < m_Count; ++c) {
        glViewportIndexedf((GLuint)c, 0.0f, 0.0f, res, res);
        glm::vec2 lo, hi;
        ReadRect(c, kernelTexels, normalBias, lo, hi);
        const auto toTexel = [&](float ndc) { return (ndc * 0.5f + 0.5f) * res; };
        const int x0 = std::clamp((int)std::floor(toTexel(lo.x)) - 1, 0, m_Resolution); // a texel more for the rounding
        const int y0 = std::clamp((int)std::floor(toTexel(lo.y)) - 1, 0, m_Resolution);
        const int x1 = std::clamp((int)std::ceil(toTexel(hi.x)) + 1, 0, m_Resolution);
        const int y1 = std::clamp((int)std::ceil(toTexel(hi.y)) + 1, 0, m_Resolution);
        glScissorIndexed((GLuint)c, x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0));
    }
    glEnable(GL_SCISSOR_TEST);
}

void CascadedShadowMap::EndLayeredScissor() {
    glDisable(GL_SCISSOR_TEST);
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
