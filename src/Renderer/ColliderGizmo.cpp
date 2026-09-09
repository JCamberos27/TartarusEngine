#include "ColliderGizmo.h"

#include "Shader.h"
#include "gl.h"
#include "World.h"
#include "Components.h"
#include "Model.h"
#include "GLStateCache.h"
#include "PhysicsWorld.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp> // eulerAngleYXZ — must match World::ComposeTransform's order

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace {

const char* kVertexSrc = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uViewProj;
out vec4 vColor;
void main() { vColor = aColor; gl_Position = uViewProj * vec4(aPos, 1.0); }
)";

const char* kFragmentSrc = R"(
#version 460 core
in vec4 vColor;
out vec4 FragColor;
void main() { FragColor = vColor; }
)";

constexpr int kCircleSegs = 28;

const glm::vec3 kSolidColor   (0.35f, 0.90f, 0.35f); // green
const glm::vec3 kTriggerColor (0.90f, 0.85f, 0.25f); // yellow
const glm::vec3 kOccupiedColor(1.00f, 0.55f, 0.15f); // orange — trigger with something inside

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
    const GLsizei stride = 7 * (GLsizei)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
}

ColliderGizmo::~ColliderGizmo() {
    if (m_VBO) glDeleteBuffers(1, &m_VBO);
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
}

void ColliderGizmo::Draw(const glm::mat4& view, const glm::mat4& proj, const World& world, bool drawShapes) {
    m_Verts.clear();
    auto& V = m_Verts;
    glm::vec3 col = kSolidColor;
    auto seg = [&](const glm::vec3& a, const glm::vec3& b) {
        V.insert(V.end(), { a.x, a.y, a.z, col.r, col.g, col.b, 1.0f,
                            b.x, b.y, b.z, col.r, col.g, col.b, 1.0f });
    };

    auto ring = [&](const glm::vec3& c, const glm::vec3& u, const glm::vec3& v, int segs) {
        glm::vec3 prev = c + u;
        for (int i = 1; i <= segs; ++i) {
            float a = (float)i / segs * 6.28318530718f;
            glm::vec3 p = c + std::cos(a) * u + std::sin(a) * v;
            seg(prev, p);
            prev = p;
        }
    };
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
      if (!drawShapes) break;
        const auto& t = vw.get<const TransformComponent>(e);
        const auto& c = vw.get<const ColliderComponent>(e);

        col = !c.IsTrigger ? kSolidColor
            : (PhysicsWorld::IsTriggerOccupied(entt::to_integral(e)) ? kOccupiedColor : kTriggerColor);

        const bool meshKind = (c.Kind == ColliderComponent::Shape::ConvexHull ||
                               c.Kind == ColliderComponent::Shape::Mesh);

        if (meshKind) {
            // Draw the actual cooked collision geometry (the same mesh PhysicsWorld hands PhysX)
            // as triangle edges in the entity's world frame. The model-space UNIQUE-edge list is
            // built once per Model and cached; each frame just transforms it. A too-dense mesh
            // caches an empty list and falls back to a bounds box.
            const auto* rc = world.Registry.try_get<const RenderableComponent>(e);
            const Model* mkey = (rc && rc->ModelRef) ? rc->ModelRef.get() : nullptr;
            if (!mkey) continue;

            auto it = m_MeshEdgeCache.find(mkey);
            if (it == m_MeshEdgeCache.end()) {
                std::vector<glm::vec3> mv; std::vector<unsigned int> mi;
                rc->ModelRef->CollisionGeometry(mv, mi);
                std::vector<glm::vec3> edges;
                const size_t tris = mi.size() / 3;
                if (tris > 0 && tris <= 20000) {
                    std::unordered_set<std::uint64_t> seen;
                    seen.reserve(mi.size());
                    auto add = [&](unsigned a, unsigned b) {
                        const std::uint64_t key = a < b ? ((std::uint64_t)a << 32 | b)
                                                        : ((std::uint64_t)b << 32 | a);
                        if (seen.insert(key).second) { edges.push_back(mv[a]); edges.push_back(mv[b]); }
                    };
                    for (size_t k = 0; k + 2 < mi.size(); k += 3) {
                        add(mi[k], mi[k+1]); add(mi[k+1], mi[k+2]); add(mi[k+2], mi[k]);
                    }
                }
                it = m_MeshEdgeCache.emplace(mkey, std::move(edges)).first;
            }

            const std::vector<glm::vec3>& edges = it->second;
            if (!edges.empty()) {
                const glm::mat4 M = ComposeTransform(t);
                for (size_t k = 0; k + 1 < edges.size(); k += 2)
                    seg(glm::vec3(M * glm::vec4(edges[k], 1.0f)),
                        glm::vec3(M * glm::vec4(edges[k+1], 1.0f)));
            } else {
                glm::vec3 center, half;
                AutoBoxWorld(world.Registry, e, t, center, half);
                if (half.x > 0.0f && half.y > 0.0f && half.z > 0.0f) box(center, half, glm::mat3(1.0f));
            }
            continue;
        }

        // Rotation built the SAME way World::ComposeTransform does (Ry * Rx * Rz), so a tumbling
        // body's wireframe tracks its true orientation — glm::quat(euler) uses a different order.
        const glm::mat3 R = glm::mat3(glm::eulerAngleYXZ(glm::radians(t.RotationEuler.y),
                                                         glm::radians(t.RotationEuler.x),
                                                         glm::radians(t.RotationEuler.z)));
        const glm::vec3 s = glm::abs(t.Scale);

        if (c.HalfExtents == glm::vec3(0.0f)) {
            // Auto-fit box. A simulated body (has a Rigidbody) is built as an ORIENTED box in
            // PhysX and rotates with the actor; a lone static collider is a plain world AABB.
            glm::vec3 halfL, offL(0.0f);
            if (const auto* rr = world.Registry.try_get<const RenderableComponent>(e); rr && rr->ModelRef) {
                const glm::vec3 lo = rr->ModelRef->BoundsMin() * t.Scale;
                const glm::vec3 hi = rr->ModelRef->BoundsMax() * t.Scale;
                halfL = 0.5f * glm::abs(hi - lo);
                offL  = 0.5f * (lo + hi);
            } else {
                halfL = 0.5f * s;
            }
            if (halfL.x > 0.0f && halfL.y > 0.0f && halfL.z > 0.0f) {
                if (world.Registry.all_of<RigidbodyComponent>(e)) {
                    box(t.Position + R * offL, halfL, R);
                } else {
                    glm::vec3 center, half;
                    AutoBoxWorld(world.Registry, e, t, center, half);
                    box(center, half, glm::mat3(1.0f));
                }
            }
            continue;
        }

        const glm::vec3 centerW = t.Position + R * c.Center;

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
            default: break; // ConvexHull / Mesh drawn as their render-bounds box above
        }
    }

    // The Player's capsule (#185): it's a PxCapsuleController, not an ECS entity with a
    // ColliderComponent, so the loop above never draws it. Same green as a solid collider.
    if (drawShapes) {
        float foot[3], r = 0.0f, hh = 0.0f;
        if (PhysicsWorld::GetCharacterCapsule(foot, &r, &hh) && r > 0.0f) {
            col = kSolidColor;
            const glm::vec3 c(foot[0], foot[1] + r + hh, foot[2]); // foot -> capsule centre
            const glm::vec3 X(r, 0, 0), Z(0, 0, r), UR(0, r, 0), Y(0, hh, 0);
            const glm::vec3 top = c + Y, bot = c - Y;
            ring(top, X, Z, kCircleSegs);
            ring(bot, X, Z, kCircleSegs);
            for (glm::vec3 d : { X, -X, Z, -Z }) seg(top + d, bot + d);
            arc(top, X,  UR, kCircleSegs / 2);
            arc(top, Z,  UR, kCircleSegs / 2);
            arc(bot, X, -UR, kCircleSegs / 2);
            arc(bot, Z, -UR, kCircleSegs / 2);
        }
    }

    // The opaque wireframe part ends here; everything after is additive-blended glow.
    const GLsizei wireVerts = (GLsizei)(V.size() / 7);

    // Physics visual debugger (#185): fading contact sparks, raycasts, velocity + sleep markers,
    // pre-coloured with alpha carrying the fade. 7 floats per vertex (pos.xyz, rgba); 14/line.
    if (PhysicsWorld::GetDebugDrawFlags() != 0u) {
        const int probe = PhysicsWorld::CopyDebugLines(nullptr, 0);
        if (probe > 0) {
            const size_t base = V.size();
            V.resize(base + (size_t)probe * 14);
            const int n = PhysicsWorld::CopyDebugLines(V.data() + base, probe);
            V.resize(base + (size_t)n * 14);
        }
    }
    const GLsizei glowVerts = (GLsizei)(V.size() / 7) - wireVerts;

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

    glEnable(GL_BLEND);
    // Straight alpha-over for everything so a pure-colour line renders as that exact colour
    // (additive would tint it toward whatever is behind it). Alpha carries the fade.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_LINES, 0, wireVerts + glowVerts);
    glDisable(GL_BLEND);
    glBindVertexArray(0);

    GLStateCache::Invalidate(); // raw program/VAO/buffer binds above bypass the cache
}
