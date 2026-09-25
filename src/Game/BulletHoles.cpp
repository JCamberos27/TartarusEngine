#include "BulletHoles.h"

#include "World.h"

#include <cmath>

namespace {
// Any unit vector across the surface.
glm::vec3 SurfaceTangent(const glm::vec3& n) {
    const glm::vec3 t = std::abs(n.y) < 0.9f ? glm::cross(n, glm::vec3(0.0f, 1.0f, 0.0f))
                                             : glm::cross(n, glm::vec3(1.0f, 0.0f, 0.0f));
    return glm::normalize(t);
}

glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
    const float len = glm::length(v);
    return len > 1e-8f ? v / len : fallback;
}
} // namespace

void BulletHoleList::Add(const World& world, entt::entity entity, const glm::vec3& point, const glm::vec3& normal,
                         float radius) {
    Hole h;
    h.Radius = radius;
    const glm::vec3 n = SafeNormalize(normal, glm::vec3(0.0f, 1.0f, 0.0f));
    // Each hole turned its own way (a golden-ratio walk), so their ragged edges don't line up.
    ++m_Count;
    h.Seed = std::fmod((float)m_Count * 0.6180340f, 1.0f);
    const glm::vec3 t0 = SurfaceTangent(n), b0 = glm::cross(n, t0);
    const float spin = h.Seed * 6.2831853f;
    const glm::vec3 t = t0 * std::cos(spin) + b0 * std::sin(spin);
    h.Position = point;
    h.Normal = n;
    h.Tangent = t;
    if (entity != entt::null && world.Registry.valid(entity)) {
        const glm::mat4 M = world.ComposeWorldTransform(entity);
        const glm::mat4 inv = glm::inverse(M);
        h.Entity = entity;
        h.Position = glm::vec3(inv * glm::vec4(point, 1.0f));
        // Normals go back through the inverse transpose; this is its inverse.
        h.Normal = glm::transpose(glm::mat3(M)) * n;
        h.Tangent = glm::mat3(inv) * t;
    }
    if (m_Holes.size() < kCapacity) {
        m_Holes.push_back(h);
    } else {
        m_Holes[m_Next] = h;
        m_Next = (m_Next + 1) % kCapacity;
    }
}

void BulletHoleList::Resolve(const World& world, std::vector<Placed>& out) {
    out.clear();
    out.reserve(m_Holes.size());
    bool dropped = false;
    for (Hole& h : m_Holes) {
        Placed p;
        p.Seed = h.Seed;
        p.Radius = h.Radius;
        if (h.Entity == entt::null) {
            p.Position = h.Position;
            p.Normal = h.Normal;
            p.Tangent = h.Tangent;
        } else if (!world.Registry.valid(h.Entity)) {
            dropped = true;
            continue;
        } else {
            const glm::mat4 M = world.ComposeWorldTransform(h.Entity);
            const glm::mat3 R(M);
            p.Position = glm::vec3(M * glm::vec4(h.Position, 1.0f));
            p.Normal = SafeNormalize(glm::transpose(glm::inverse(R)) * h.Normal, glm::vec3(0.0f, 1.0f, 0.0f));
            glm::vec3 t = R * h.Tangent;
            t -= p.Normal * glm::dot(t, p.Normal);
            p.Tangent = SafeNormalize(t, SurfaceTangent(p.Normal));
        }
        out.push_back(p);
    }
    if (dropped) {
        // Compact once the holes of destroyed entities are gone (the ring restarts: the order
        // no longer matters much once some are missing).
        std::vector<Hole> kept;
        for (const Hole& h : m_Holes)
            if (h.Entity == entt::null || world.Registry.valid(h.Entity)) kept.push_back(h);
        m_Holes.swap(kept);
        m_Next = 0;
    }
}
