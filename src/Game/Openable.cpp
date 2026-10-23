#include "Openable.h"
#include "World.h"
#include "Components.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "GameModuleAPI.h" // RaycastHit
#include "Audio/WeaponAudio.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace Openable {
namespace {
using Kind = OpenableComponent::MotionKind;

// One of `files` (';'-separated audio assets) at `pos`, pitch jittered a little so repeats don't sound stamped.
void PlaySound(const std::string& files, const glm::vec3& pos) {
    if (files.empty()) return;
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return;
    SoundSet set;
    set.Key = "openable." + files;
    set.FilesExplicit = true;
    std::istringstream in(files);
    for (std::string f; std::getline(in, f, ';');)
        if (!f.empty()) set.Files.push_back(f);
    set.PitchMin = 0.96f;
    set.PitchMax = 1.04f;
    set.VolumeJitterDb = 1.0f;
    set.MinDistance = 1.0f;
    set.MaxDistance = 25.0f;
    SoundPlayer::Request r;
    r.Position = pos;
    wa.Player().Play(set, r);
}

constexpr float kBounceBack = 0.15f; // of the full travel: how far a part that meets somebody eases back

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

void Apply(OpenableComponent& o, TransformComponent& t) {
    glm::vec3 pos;
    glm::quat rot;
    Pose(o.Motion, o.ResolvedAmount, o.PoseMin, o.PoseMax, t.Scale, o.BasePosition, o.BaseRotation, o.Progress, pos, rot, o.SwingBack);
    t.Position = pos;
    t.Rotation = rot;
}

void Start(World& world, entt::entity e, OpenableComponent& o, const TransformComponent& t, const Model& m) {
    o.Started = true;
    o.BasePosition = t.Position;
    o.BaseRotation = t.Rotation;
    // The part's own box collider, when it has one, is the solid panel; the model's bounds also take in the handles
    // sticking out either face, which would put a hinge out in the air beside the door.
    o.PoseMin = m.BoundsMin();
    o.PoseMax = m.BoundsMax();
    const auto* c = world.Registry.try_get<ColliderComponent>(e);
    if (c && c->Kind == ColliderComponent::Shape::Box && glm::all(glm::greaterThan(c->HalfExtents, glm::vec3(0.0f)))) {
        o.PoseMin = c->Center - c->HalfExtents;
        o.PoseMax = c->Center + c->HalfExtents;
    }
    o.ResolvedAmount = o.Amount > 0.0f ? o.Amount : DefaultAmount(o.Motion, m.BoundsMin(), m.BoundsMax(), ParentDepth(world, e));
    o.Progress = o.Target = o.StartOpen ? 1.0f : 0.0f;
}

// The part's box collider in the world with the part posed at `progress` (false: it has no box collider).
bool LeafBoxAt(World& world, entt::entity e, const OpenableComponent& o, const TransformComponent& t, float progress,
               glm::vec3& centre, glm::vec3& half, glm::quat& rot) {
    const auto* c = world.Registry.try_get<ColliderComponent>(e);
    if (!c || c->Kind != ColliderComponent::Shape::Box) return false;
    glm::vec3 pos;
    glm::quat q;
    Pose(o.Motion, o.ResolvedAmount, o.PoseMin, o.PoseMax, t.Scale, o.BasePosition, o.BaseRotation, progress, pos, q, o.SwingBack);
    glm::mat4 parent(1.0f);
    if (const auto* h = world.Registry.try_get<HierarchyComponent>(e); h && h->Parent != entt::null && world.Registry.valid(h->Parent))
        parent = world.GetCachedWorldTransform(h->Parent);
    const glm::mat4 m = parent * glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(q) * glm::scale(glm::mat4(1.0f), t.Scale);
    centre = glm::vec3(m * glm::vec4(c->Center, 1.0f));
    glm::mat3 axes(1.0f);
    for (int i = 0; i < 3; ++i) {
        const float l = glm::length(glm::vec3(m[i]));
        if (l > 1e-6f) axes[i] = glm::vec3(m[i]) / l;
        half[i] = std::abs(c->HalfExtents[i]) * l;
    }
    rot = glm::quat_cast(axes);
    return true;
}

// Whether the part, posed at `progress`, would be in a character (a hair smaller: only a real overlap, not a graze).
bool InSomebody(World& world, entt::entity e, const OpenableComponent& o, const TransformComponent& t, float progress) {
    glm::vec3 centre, half;
    glm::quat rot;
    if (!LeafBoxAt(world, e, o, t, progress, centre, half, rot)) return false;
    half = glm::max(half - glm::vec3(0.005f), glm::vec3(0.001f));
    const float q[4] = {rot.x, rot.y, rot.z, rot.w};
    return PhysicsWorld::BoxOverlapsCharacters(&centre.x, &half.x, q) > 0;
}

// Whether moving the part from `from` to `to` would run it into a character on the way (a hinge checked every 10 degrees).
bool SwingsIntoSomebody(World& world, entt::entity e, const OpenableComponent& o, const TransformComponent& t, float from, float to) {
    const bool hinge = (Kind)o.Motion != Kind::SlideOut && (Kind)o.Motion != Kind::SlideLeft && (Kind)o.Motion != Kind::SlideRight;
    const float degrees = hinge ? o.ResolvedAmount * std::abs(Ease(to) - Ease(from)) : 0.0f;
    const int n = std::max(1, (int)std::ceil(degrees / 10.0f));
    for (int i = 1; i <= n; ++i)
        if (InSomebody(world, e, o, t, from + (to - from) * (float)i / (float)n)) return true;
    return false;
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
          const glm::vec3& basePos, const glm::quat& baseRot, float t, glm::vec3& outPos, glm::quat& outRot, bool back) {
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
    // The hinge line and which way round it opens, so the free edge always comes out toward local +Z. A side hinge's
    // pin is on the front face, the side it swings to, so the open door stands in the doorway clear of the jamb.
    const glm::vec3 c = (lo + hi) * 0.5f;
    glm::vec3 pivot, axis;
    float sign;
    switch ((Kind)motion) {
        case Kind::HingeLeft: pivot = {lo.x, c.y, hi.z}; axis = {0.0f, 1.0f, 0.0f}; sign = -1.0f; break;
        case Kind::HingeRight: pivot = {hi.x, c.y, hi.z}; axis = {0.0f, 1.0f, 0.0f}; sign = 1.0f; break;
        case Kind::Lid: pivot = {c.x, lo.y, lo.z}; axis = {1.0f, 0.0f, 0.0f}; sign = -1.0f; break;
        case Kind::FlapDown: pivot = {c.x, lo.y, hi.z}; axis = {1.0f, 0.0f, 0.0f}; sign = 1.0f; break;
        case Kind::FlapUp: pivot = {c.x, hi.y, hi.z}; axis = {1.0f, 0.0f, 0.0f}; sign = -1.0f; break;
        default: return;
    }
    // Swing Back: the mirror image through the door's plane - the pin on the back face, the free edge out toward -Z.
    if (back && (Kind)motion != Kind::Lid) {
        pivot.z = lo.z;
        sign = -sign;
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
                if (o.Target > 0.5f) PlaySound(o.OpenSound, glm::vec3(world.GetCachedWorldTransform(best)[3]));
            }
        }
    }
    for (auto e : view) {
        auto& o = view.get<OpenableComponent>(e);
        const auto& r = view.get<RenderableComponent>(e);
        if (!o.Started || !r.ModelRef || o.Progress == o.Target) continue;
        const float step = dt / std::max(o.OpenTime, 0.01f);
        const float next = o.Target > o.Progress ? std::min(o.Target, o.Progress + step) : std::max(o.Target, o.Progress - step);
        auto& t = view.get<TransformComponent>(e);
        // Swinging into somebody (the player, a soldier, either way it moves): it stops short, eases back a little the way it
        // came, and stays there until it's used again. (Already in someone - spawned there - it carries on, not to trap them.)
        if (SwingsIntoSomebody(world, e, o, t, o.Progress, next) && !InSomebody(world, e, o, t, o.Progress)) {
            const float back = std::clamp(o.Progress + (next > o.Progress ? -kBounceBack : kBounceBack), 0.0f, 1.0f);
            o.Target = SwingsIntoSomebody(world, e, o, t, o.Progress, back) ? o.Progress : back; // boxed in: just stops
            continue;
        }
        o.Progress = next;
        Apply(o, t);
        if (o.Progress == 0.0f) { // landed shut
            const glm::vec3 at(world.GetCachedWorldTransform(e)[3]);
            PlaySound(o.CloseSound, at);
            PlaySound(o.LatchSound, at);
        }
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
        Apply(o, view.get<TransformComponent>(e));
    }
}

} // namespace Openable
