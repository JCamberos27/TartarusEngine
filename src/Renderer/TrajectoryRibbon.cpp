#include "TrajectoryRibbon.h"

#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"

#include <cmath>

namespace {
// Half-width as a fraction of the distance to the camera: about 3 px on a 1080p screen at 75 deg.
constexpr float kHalfWidthPerMetre = 0.0022f;
constexpr float kMinHalfWidth = 0.004f;
constexpr int kRingSegs = 32;
} // namespace

TrajectoryRibbon::TrajectoryRibbon() {
    m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("ColliderGizmo.vert.glsl"),
                                        ShaderLibrary::ReadFile("ColliderGizmo.frag.glsl"));
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    const GLsizei stride = 7 * (GLsizei)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
}

TrajectoryRibbon::~TrajectoryRibbon() {
    if (m_VBO) glDeleteBuffers(1, &m_VBO);
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
}

void TrajectoryRibbon::Draw(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye,
                            const std::vector<glm::vec3>& points, const glm::vec4& color,
                            const glm::vec3& landNormal, float ringRadius) {
    if (points.size() < 2) return;
    std::vector<float>& V = m_Verts;
    V.clear();
    auto vert = [&](const glm::vec3& p, float a) {
        V.insert(V.end(), {p.x, p.y, p.z, color.r, color.g, color.b, color.a * a});
    };
    auto halfWidth = [&](const glm::vec3& p) {
        return std::max(kMinHalfWidth, glm::length(p - eye) * kHalfWidthPerMetre);
    };
    // One quad per segment, widened across the view direction. The first stretch fades in so the
    // ribbon doesn't start as a blob right in front of the held object.
    const size_t n = points.size();
    for (size_t i = 0; i + 1 < n; ++i) {
        const glm::vec3 a = points[i], b = points[i + 1];
        const glm::vec3 d = b - a;
        glm::vec3 side = glm::cross(d, eye - 0.5f * (a + b));
        const float len = glm::length(side);
        if (len < 1e-6f) continue;
        side /= len;
        const glm::vec3 sa = side * halfWidth(a), sb = side * halfWidth(b);
        const float fa = std::min(1.0f, (float)i / 4.0f), fb = std::min(1.0f, (float)(i + 1) / 4.0f);
        vert(a - sa, fa); vert(a + sa, fa); vert(b + sb, fb);
        vert(a - sa, fa); vert(b + sb, fb); vert(b - sb, fb);
    }
    // Landing ring: a flat annulus on the surface the arc hits.
    if (glm::length(landNormal) > 0.5f) {
        const glm::vec3 nrm = glm::normalize(landNormal);
        const glm::vec3 c = points.back() + nrm * 0.01f;
        const glm::vec3 t = glm::normalize(std::abs(nrm.y) < 0.9f ? glm::cross(nrm, glm::vec3(0, 1, 0))
                                                                 : glm::cross(nrm, glm::vec3(1, 0, 0)));
        const glm::vec3 bt = glm::cross(nrm, t);
        const float w = halfWidth(c) * 1.5f;
        for (int k = 0; k < kRingSegs; ++k) {
            const float a0 = 6.2831853f * k / kRingSegs, a1 = 6.2831853f * (k + 1) / kRingSegs;
            const glm::vec3 d0 = t * std::cos(a0) + bt * std::sin(a0), d1 = t * std::cos(a1) + bt * std::sin(a1);
            const glm::vec3 i0 = c + d0 * (ringRadius - w), o0 = c + d0 * (ringRadius + w);
            const glm::vec3 i1 = c + d1 * (ringRadius - w), o1 = c + d1 * (ringRadius + w);
            vert(i0, 1.0f); vert(o0, 1.0f); vert(o1, 1.0f);
            vert(i0, 1.0f); vert(o1, 1.0f); vert(i1, 1.0f);
        }
    }
    if (V.empty()) return;

    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    if (V.size() > m_Capacity) {
        glBufferData(GL_ARRAY_BUFFER, V.size() * sizeof(float), V.data(), GL_DYNAMIC_DRAW);
        m_Capacity = V.size();
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, V.size() * sizeof(float), V.data());
    }
    m_Shader->Bind();
    m_Shader->SetMat4("uViewProj", proj * view);

    const GLboolean cullWasOn = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE); // the ribbon's winding flips with the view side
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(V.size() / 7));
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    if (cullWasOn) glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    GLStateCache::Invalidate(); // raw program/VAO/buffer binds above bypass the cache
}
