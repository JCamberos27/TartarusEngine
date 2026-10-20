#include "Openable.h"
#include "World.h"
#include "Components.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "GameModuleAPI.h" // RaycastHit

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace Openable {
namespace {
using Kind = OpenableComponent::MotionKind;

float Ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The depth (local Z, metres) of the model this entity hangs under, or 0 without one.
float ParentDepth(World& world, entt::entity e) {
    const auto* h = world.Registry.try_get<HierarchyComponent>(e);
    if (!h || h->Parent == entt::null || !world.Registry.valid(h->Parent)) return 0.0f;
    const auto* r = world.Registry.try_get<RenderableComponent>(h->Parent);
    const auto* t = world.Registry.try_get<TransformComponent>(h->Parent);
    if (!r || !r->ModelRef) return 0.0f;
    return (r->ModelRef->BoundsMax().z - r->ModelRef->BoundsMin().z) * (t ? std::abs(t->Scale.z) : 1.0f);
}

void Apply(OpenableComponent& o, TransformComponent& t, const Model& m) {
    glm::vec3 pos;
    glm::quat rot;
    Pose(o.Motion, o.ResolvedAmount, m.BoundsMin(), m.BoundsMax(), t.Scale, o.BasePosition, o.BaseRotation, o.Progress,
         pos, rot);
    t.Position = pos;
    t.Rotation = rot;
}

void Start(World& world, entt::entity e, OpenableComponent& o, const TransformComponent& t, const Model& m) {
    o.Started = true;
    o.BasePosition = t.Position;
    o.BaseRotation = t.Rotation;
    o.ResolvedAmount = o.Amount > 0.0f ? o.Amount : DefaultAmount(o.Motion, m.BoundsMin(), m.BoundsMax(), ParentDepth(world, e));
    o.Progress = o.Target = o.StartOpen ? 1.0f : 0.0f;
}

bool IsSelfOrAncestor(World& world, entt::entity of, entt::entity candidate) {
    for (entt::entity e = of; e != entt::null && world.Registry.valid(e);) {
        if (e == candidate) return true;
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        e = h ? h->Parent : entt::null;
    }
    return false;
}
} // namespace

float DefaultAmount(int motion, const glm::vec3& lo, const glm::vec3& hi, float parentDepth) {
    switch ((Kind)motion) {
        case Kind::HingeLeft:
        case Kind::HingeRight: return 100.0f;
        case Kind::Lid: return 95.0f;
        case Kind::FlapDown: return 85.0f;
        case Kind::FlapUp: return 90.0f;
        case Kind::SlideOut: return parentDepth > 0.05f ? 0.7f * parentDepth : 0.3f;
        case Kind::SlideLeft:
        case Kind::SlideRight: return 0.9f * (hi.x - lo.x);
    }
    return 0.0f;
}

void Pose(int motion, float amount, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& scale,
          const glm::vec3& basePos, const glm::quat& baseRot, float t, glm::vec3& outPos, glm::quat& outRot) {
    outPos = basePos;
    outRot = baseRot;
    const float s = Ease(t);
    if (s <= 0.0f) return;
    switch ((Kind)motion) {
        case Kind::SlideOut: outPos = basePos + baseRot * glm::vec3(0.0f, 0.0f, amount * s); return;
        case Kind::SlideLeft: outPos = basePos + baseRot * glm::vec3(-amount * s, 0.0f, 0.0f); return;
        case Kind::SlideRight: outPos = basePos + baseRot * glm::vec3(amount * s, 0.0f, 0.0f); return;
        default: break;
    }
    // The hinge line and which way round it opens, so the free edge always comes out toward local +Z.
    const glm::vec3 c = (lo + hi) * 0.5f;
    glm::vec3 pivot, axis;
    float sign;
    switch ((Kind)motion) {
        case Kind::HingeLeft: pivot = {lo.x, c.y, lo.z}; axis = {0.0f, 1.0f, 0.0f}; sign = -1.0f; break;
        case Kind::HingeRight: pivot = {hi.x, c.y, lo.z}; axis = {0.0f, 1.0f, 0.0f}; sign = 1.0f; break;
        case Kind::Lid: pivot = {c.x, lo.y, lo.z}; axis = {1.0f, 0.0f, 0.0f}; sign = -1.0f; break;
        case Kind::FlapDown: pivot = {c.x, lo.y, hi.z}; axis = {1.0f, 0.0f, 0.0f}; sign = 1.0f; break;
        case Kind::FlapUp: pivot = {c.x, hi.y, hi.z}; axis = {1.0f, 0.0f, 0.0f}; sign = -1.0f; break;
        default: return;
    }
    const glm::quat q = glm::angleAxis(glm::radians(sign * amount * s), axis);
    // Turned about the hinge in the entity's scaled space: the hinge line itself stays put.
    const glm::vec3 p = scale * pivot;
    outRot = baseRot * q;
    outPos = basePos + baseRot * (p - q * p);
}

float RayBox(const glm::mat4& xform, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& origin,
             const glm::vec3& dir) {
    const glm::mat4 inv = glm::inverse(xform);
    const glm::vec3 o = glm::vec3(inv * glm::vec4(origin, 1.0f));
    const glm::vec3 d = glm::vec3(inv * glm::vec4(dir, 0.0f));
    float t0 = -1e30f, t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-9f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return -1.0f;
            continue;
        }
        float a = (lo[i] - o[i]) / d[i], b = (hi[i] - o[i]) / d[i];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return -1.0f;
    }
    if (t1 < 0.0f) return -1.0f;
    return std::max(t0, 0.0f); // from inside the box: right here
}

void Update(World& world, const glm::vec3& eye, const glm::vec3& forward, bool interact, float dt) {
    auto view = world.Registry.view<OpenableComponent, TransformComponent, RenderableComponent>(entt::exclude<InactiveTag>);
    for (auto e : view) {
        auto& o = view.get<OpenableComponent>(e);
        const auto& r = view.get<RenderableComponent>(e);
        if (!o.Started && r.ModelRef) Start(world, e, o, view.get<TransformComponent>(e), *r.ModelRef);
    }
    if (interact) {
        const glm::vec3 dir = glm::normalize(forward);
        entt::entity best = entt::null;
        float bestDist = kReach;
        for (auto e : view) {
            const auto& r = view.get<RenderableComponent>(e);
            if (!r.ModelRef) continue;
            const float d = RayBox(world.GetCachedWorldTransform(e), r.ModelRef->BoundsMin(), r.ModelRef->BoundsMax(), eye, dir);
            if (d >= 0.0f && d < bestDist) {
                best = e;
                bestDist = d;
            }
        }
        if (best != entt::null) {
            // Not through a wall: whatever solid the ray meets first is the part, its body, or no nearer than it
            // (a cabinet's collider is a box around its doors).
            const float o3[3] = {eye.x, eye.y, eye.z}, d3[3] = {dir.x, dir.y, dir.z};
            RaycastHit hit;
            const bool blocked = PhysicsWorld::Raycast(o3, d3, bestDist, hit) && hit.Distance + 0.15f < bestDist &&
                                 !IsSelfOrAncestor(world, best, static_cast<entt::entity>(hit.Entity));
            if (!blocked) {
                auto& o = view.get<OpenableComponent>(best);
                o.Target = o.Target > 0.5f ? 0.0f : 1.0f;
            }
        }
    }
    for (auto e : view) {
        auto& o = view.get<OpenableComponent>(e);
        const auto& r = view.get<RenderableComponent>(e);
        if (!o.Started || !r.ModelRef || o.Progress == o.Target) continue;
        const float step = dt / std::max(o.OpenTime, 0.01f);
        o.Progress = o.Target > o.Progress ? std::min(o.Target, o.Progress + step) : std::max(o.Target, o.Progress - step);
        Apply(o, view.get<TransformComponent>(e), *r.ModelRef);
    }
}

void SetAll(World& world, float t) {
    auto view = world.Registry.view<OpenableComponent, TransformComponent, RenderableComponent>();
    for (auto e : view) {
        auto& o = view.get<OpenableComponent>(e);
        const auto& r = view.get<RenderableComponent>(e);
        if (!r.ModelRef) continue;
        if (!o.Started) Start(world, e, o, view.get<TransformComponent>(e), *r.ModelRef);
        o.Progress = o.Target = std::clamp(t, 0.0f, 1.0f);
        Apply(o, view.get<TransformComponent>(e), *r.ModelRef);
    }
}

} // namespace Openable
