#include "ColliderGizmo.h"

#include "Shader.h"
#include "gl.h"
#include "World.h"
#include "Components.h"
#include "Model.h"
#include "GLStateCache.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>

namespace {

const char* kVertexSrc = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
void main() { gl_Position = uViewProj * vec4(aPos, 1.0); }
)";

const char* kFragmentSrc = R"(
#version 460 core
out vec4 FragColor;
uniform vec3 uColor;
void main() { FragColor = vec4(uColor, 1.0); }
)";

constexpr int kCircleSegs = 28;

// Same axis-aligned box the legacy collider path uses (World.cpp's ColliderWorldBounds): render
// bounds scaled by the transform, or a unit box for a mesh-less entity.
void AutoBoxWorld(const entt::registry& reg, entt::entity e, const TransformComponent& t,
                  glm::vec3& outCenter, glm::vec3& outHalf) {
    if (const auto* r = reg.try_get<RenderableComponent>(e); r && r->ModelRef) {
        glm::vec3 a = t.Position + r->ModelRef->BoundsMin() * t.Scale;
        glm::vec3 b = t.Position + r->ModelRef->BoundsMax() * t.Scale;
        outCenter = 0.5f * (a + b);
        outHalf   = 0.5f * glm::abs(b - a);
    } else {
        outCenter = t.Position;
        outHalf   = 0.5f * glm::abs(t.Scale);
    }
}

} // namespace

ColliderGizmo::ColliderGizmo() {
    m_Shader = std::make_unique<Shader>(kVertexSrc, kFragmentSrc);
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glBindVertexArray(0);
}

ColliderGizmo::~ColliderGizmo() {
    if (m_VBO) glDeleteBuffers(1, &m_VBO);
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
}

void ColliderGizmo::Draw(const glm::mat4& view, const glm::mat4& proj, const World& world,
                         const glm::vec3& color) {
    m_Lines.clear();
    auto& L = m_Lines;
    auto seg = [&](const glm::vec3& a, const glm::vec3& b) { L.push_back(a); L.push_back(b); };

    // Ring of `segs` chords in the plane spanned by u,v (both length `radius`), centred at c.
    auto ring = [&](const glm::vec3& c, const glm::vec3& u, const glm::vec3& v, int segs) {
        glm::vec3 prev = c + u;
        for (int i = 1; i <= segs; ++i) {
            float a = (float)i / segs * 6.28318530718f;
            glm::vec3 p = c + std::cos(a) * u + std::sin(a) * v;
            seg(prev, p);
            prev = p;
        }
    };
    // Half-ring from angle 0..pi (used for capsule end caps).
    auto arc = [&](const glm::vec3& c, const glm::vec3& u, const glm::vec3& v, int segs) {
        glm::vec3 prev = c + u;
        for (int i = 1; i <= segs; ++i) {
            float a = (float)i / segs * 3.14159265359f;
            glm::vec3 p = c + std::cos(a) * u + std::sin(a) * v;
            seg(prev, p);
            prev = p;
        }
    };
    auto box = [&](const glm::vec3& c, const glm::vec3& half, const glm::mat3& R) {
        glm::vec3 x = R[0] * half.x, y = R[1] * half.y, z = R[2] * half.z;
        glm::vec3 k[8];
        for (int i = 0; i < 8; ++i)
            k[i] = c + ((i & 1) ? x : -x) + ((i & 2) ? y : -y) + ((i & 4) ? z : -z);
        const int e[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
        for (auto& pr : e) seg(k[pr[0]], k[pr[1]]);
    };

    auto vw = world.Registry.view<const TransformComponent, const ColliderComponent>(entt::exclude<InactiveTag>);
    for (entt::entity e : vw) {
        const auto& t = vw.get<const TransformComponent>(e);
        const auto& c = vw.get<const ColliderComponent>(e);

        if (c.HalfExtents == glm::vec3(0.0f)) {
            glm::vec3 center, half;
            AutoBoxWorld(world.Registry, e, t, center, half);
            if (half.x > 0.0f && half.y > 0.0f && half.z > 0.0f)
                box(center, half, glm::mat3(1.0f));
            continue;
        }

        glm::mat3 R = glm::mat3_cast(glm::quat(glm::radians(t.RotationEuler)));
        glm::vec3 s = glm::abs(t.Scale);
        glm::vec3 centerW = t.Position + R * c.Center;

        switch (c.Kind) {
            case ColliderComponent::Shape::Box: {
                glm::vec3 h = c.HalfExtents * s;
                if (h.x > 0.0f && h.y > 0.0f && h.z > 0.0f) box(centerW, h, R);
                break;
            }
            case ColliderComponent::Shape::Sphere: {
                float r = c.HalfExtents.x * std::max({s.x, s.y, s.z});
                if (r > 0.0f) {
                    ring(centerW, R[0] * r, R[1] * r, kCircleSegs);
                    ring(centerW, R[1] * r, R[2] * r, kCircleSegs);
                    ring(centerW, R[2] * r, R[0] * r, kCircleSegs);
                }
                break;
            }
            case ColliderComponent::Shape::Capsule: {
                float r  = c.HalfExtents.x * std::max(s.x, s.z);
                float hh = c.HalfExtents.y * s.y;
                if (r <= 0.0f || hh <= 0.0f) break;
                glm::vec3 up = R[1] * hh, xr = R[0] * r, zr = R[2] * r, ur = R[1] * r;
                glm::vec3 top = centerW + up, bot = centerW - up;
                ring(top, xr, zr, kCircleSegs);
                ring(bot, xr, zr, kCircleSegs);
                for (glm::vec3 d : {xr, -xr, zr, -zr}) seg(top + d, bot + d);
                arc(top,  xr, ur, kCircleSegs / 2);
                arc(top,  zr, ur, kCircleSegs / 2);
                arc(bot,  xr, -ur, kCircleSegs / 2);
                arc(bot,  zr, -ur, kCircleSegs / 2);
                break;
            }
        }
    }

    if (L.empty()) return;

    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    if (L.size() > m_Capacity) {
        glBufferData(GL_ARRAY_BUFFER, L.size() * sizeof(glm::vec3), L.data(), GL_DYNAMIC_DRAW);
        m_Capacity = L.size();
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, L.size() * sizeof(glm::vec3), L.data());
    }

    m_Shader->Bind();
    m_Shader->SetMat4("uViewProj", proj * view);
    m_Shader->SetVec3("uColor", color);
    glDrawArrays(GL_LINES, 0, (GLsizei)L.size());
    glBindVertexArray(0);

    GLStateCache::Invalidate(); // raw program/VAO/buffer binds above bypass the cache
}
