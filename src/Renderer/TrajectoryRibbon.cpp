#include "TrajectoryRibbon.h"

#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"

#include <algorithm>
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

void TrajectoryRibbon::AddPath(const std::vector<glm::vec3>& points, const glm::vec4& color, int fadeInSegments) {
    if (points.size() >= 2) m_Paths.push_back({points, color, fadeInSegments});
}

void TrajectoryRibbon::AddRing(const glm::vec3& center, const glm::vec3& normal, float radius, const glm::vec4& color) {
    if (glm::length(normal) > 0.5f && radius > 0.0f) m_Rings.push_back({center, glm::normalize(normal), radius, color});
}

void TrajectoryRibbon::Draw(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye) {
    std::vector<float>& V = m_Verts;
    V.clear();
    auto vert = [&](const glm::vec3& p, const glm::vec4& c, float a) {
        V.insert(V.end(), {p.x, p.y, p.z, c.r, c.g, c.b, c.a * a});
    };
    auto halfWidth = [&](const glm::vec3& p) {
        return std::max(kMinHalfWidth, glm::length(p - eye) * kHalfWidthPerMetre);
    };
    // One quad per segment, widened across the view direction.
    for (const Path& path : m_Paths) {
        const std::vector<glm::vec3>& P = path.Points;
        auto fade = [&](size_t i) {
            return path.FadeIn > 0 ? std::min(1.0f, (float)i / (float)path.FadeIn) : 1.0f;
        };
        for (size_t i = 0; i + 1 < P.size(); ++i) {
            const glm::vec3 a = P[i], b = P[i + 1];
            glm::vec3 side = glm::cross(b - a, eye - 0.5f * (a + b));
            const float len = glm::length(side);
            if (len < 1e-7f) continue;
            side /= len;
            const glm::vec3 sa = side * halfWidth(a), sb = side * halfWidth(b);
            const float fa = fade(i), fb = fade(i + 1);
            vert(a - sa, path.Color, fa); vert(a + sa, path.Color, fa); vert(b + sb, path.Color, fb);
            vert(a - sa, path.Color, fa); vert(b + sb, path.Color, fb); vert(b - sb, path.Color, fb);
        }
    }
    // Rings: flat annuli on the surface, lifted a hair off it.
    for (const Ring& r : m_Rings) {
        const glm::vec3 c = r.Center + r.Normal * 0.01f;
        const glm::vec3 t = glm::normalize(std::abs(r.Normal.y) < 0.9f ? glm::cross(r.Normal, glm::vec3(0, 1, 0))
                                                                       : glm::cross(r.Normal, glm::vec3(1, 0, 0)));
        const glm::vec3 bt = glm::cross(r.Normal, t);
        const float w = halfWidth(c) * 1.5f;
        for (int k = 0; k < kRingSegs; ++k) {
            const float a0 = 6.2831853f * k / kRingSegs, a1 = 6.2831853f * (k + 1) / kRingSegs;
            const glm::vec3 d0 = t * std::cos(a0) + bt * std::sin(a0), d1 = t * std::cos(a1) + bt * std::sin(a1);
            const glm::vec3 i0 = c + d0 * std::max(0.0f, r.Radius - w), o0 = c + d0 * (r.Radius + w);
            const glm::vec3 i1 = c + d1 * std::max(0.0f, r.Radius - w), o1 = c + d1 * (r.Radius + w);
            vert(i0, r.Color, 1.0f); vert(o0, r.Color, 1.0f); vert(o1, r.Color, 1.0f);
            vert(i0, r.Color, 1.0f); vert(o1, r.Color, 1.0f); vert(i1, r.Color, 1.0f);
        }
    }
    m_Paths.clear();
    m_Rings.clear();
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
