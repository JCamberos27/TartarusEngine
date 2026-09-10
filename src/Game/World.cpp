#include "World.h"
#include "Model.h"
#include "Log.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <algorithm>

namespace {
// Cycle guard for the world-transform cache's parent-chain walk: no legitimate scene nests this
// deep, so exceeding it means a malformed hierarchy, not a tall one.
constexpr size_t kMaxHierarchyDepth = 1024;

// The box/primitive path (World::CreateBox, "Add > Cube/Sphere/...") happens to work with a
// plain Position/Scale AABB because every built-in primitive's native mesh is pre-built to fit
// exactly inside a -0.5..0.5 unit cube — so "size == Scale" is coincidentally exact for them.
// An IMPORTED model's native bounds are whatever the source file's geometry actually spans
// (rarely a unit cube), so that same formula would place the collider nowhere near the visible
// mesh. Using the entity's own RenderableComponent bounds when one exists generalizes correctly
// to any mesh while staying bit-identical for boxes/primitives (verified: BoundsMin/Max for the
// cube/sphere/cylinder/cone/plane generators are exactly -0.5..0.5). Falls back to the old
// Scale-only box for a Collider added to a mesh-less entity (a light or empty), which has no
// bounds to derive from. Still axis-aligned/rotation-ignoring, matching the physics's existing,
// deliberate "simple gameplay collision" simplification (see ColliderComponent).
AABB ColliderWorldBounds(const entt::registry& registry, entt::entity entity, const TransformComponent& transform) {
    if (const auto* renderable = registry.try_get<RenderableComponent>(entity)) {
        glm::vec3 worldMin = transform.Position + renderable->ModelRef->BoundsMin() * transform.Scale;
        glm::vec3 worldMax = transform.Position + renderable->ModelRef->BoundsMax() * transform.Scale;
        return AABB{glm::min(worldMin, worldMax), glm::max(worldMin, worldMax)}; // guards a negative Scale flipping min/max
    }
    return AABB::FromCenterSize(transform.Position, transform.Scale);
}
} // namespace

// A default-constructed World is genuinely empty — no ground, no placeholder crates. Every
// caller that wants a truly blank scene (startup with no scene file, File > New Scene) goes
// through this constructor, so anything spawned here would show up whether they wanted it or
// not. Add starter content, if ever wanted again, as an explicit opt-in action instead (e.g. a
// "New Scene from template" menu item), not as constructor side effects.
World::World() {}

std::string World::NextPrimitivePath() {
    return "primitive://levelgeometry/" + std::to_string(m_NextPrimitiveId++);
}

entt::entity World::CreateBox(const glm::vec3& center, const glm::vec3& size, const glm::vec3& color,
    const glm::vec3& rotationEuler, const std::string& name) {
    entt::entity e = Registry.create();
    Registry.emplace<TransformComponent>(e, center, rotationEuler, size);
    Registry.emplace<NameComponent>(e, name);
    Registry.emplace<OrderComponent>(e, AllocateOrder());

    auto model = Model::CreatePrimitive("cube", NextPrimitivePath());
    model->MeshMaterial(0).BaseColor = color;
    Registry.emplace<RenderableComponent>(e, std::move(model));

    Registry.emplace<ColliderComponent>(e);
    Registry.emplace<LevelGeometryTag>(e);
    return e;
}

entt::entity World::CreateModelEntity(std::shared_ptr<Model> modelRef, const glm::vec3& position,
    const glm::vec3& rotationEuler, const glm::vec3& scale, const std::string& name) {
    entt::entity e = Registry.create();
    Registry.emplace<TransformComponent>(e, position, rotationEuler, scale);
    Registry.emplace<NameComponent>(e, name);
    Registry.emplace<OrderComponent>(e, AllocateOrder());
    Registry.emplace<RenderableComponent>(e, std::move(modelRef));
    return e;
}

entt::entity World::CreateEmptyEntity(const glm::vec3& position, const glm::vec3& rotationEuler,
    const glm::vec3& scale, const std::string& name) {
    entt::entity e = Registry.create();
    Registry.emplace<TransformComponent>(e, position, rotationEuler, scale);
    Registry.emplace<NameComponent>(e, name);
    Registry.emplace<OrderComponent>(e, AllocateOrder());
    return e;
}

// World::ResolveCollisions (the AABB push-out that moved the Play-mode Player) was removed in
// #185 PR 3 — the Player now sweeps a PxCapsuleController through PhysicsWorld's scene. The
// AABB Raycast below stays: the editor still uses it for drop-to-surface / snap queries.
entt::entity World::Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist, float& outDist) const {
    entt::entity best = entt::null;
    float bestT = maxDist;
    auto view = Registry.view<const TransformComponent, const ColliderComponent>(entt::exclude<InactiveTag>);
    for (auto entity : view) {
        const auto& [transform, collider] = view.get<const TransformComponent, const ColliderComponent>(entity);
        if (collider.IsTrigger) continue;
        AABB b = ColliderWorldBounds(Registry, entity, transform);
        float t;
        if (b.RayIntersect(origin, dir, t) && t < bestT) {
            bestT = t;
            best = entity;
        }
    }
    outDist = bestT;
    return best;
}

glm::mat4 World::ComposeWorldTransform(entt::entity entity) const {
    if (!Registry.valid(entity)) return glm::mat4(1.0f);

    glm::mat4 world = ComposeTransform(Registry.get<TransformComponent>(entity));

    // Walk up the parent chain iteratively instead of recursing. AttachChildRaw (used by scene
    // loading) doesn't reject parentId cycles, so a malformed file used to recurse straight
    // through here into a stack overflow with no diagnostic. Cap the walk exactly as
    // RebuildWorldTransformCache does for the same reason, and log once if it trips.
    entt::entity walk = entity;
    for (size_t depth = 0; ; ++depth) {
        const auto* hier = Registry.try_get<HierarchyComponent>(walk);
        if (!hier || hier->Parent == entt::null || !Registry.valid(hier->Parent)) break;
        if (depth >= kMaxHierarchyDepth) {
            Log::Error("World::ComposeWorldTransform: parent chain exceeds " +
                       std::to_string(kMaxHierarchyDepth) +
                       " levels (cycle in parentId?); world transform truncated.");
            break;
        }
        walk = hier->Parent;
        if (const auto* t = Registry.try_get<TransformComponent>(walk))
            world = ComposeTransform(*t) * world;
    }
    return world;
}

void World::RebuildWorldTransformCache() {
    m_WorldTransformCache.clear();
    auto view = Registry.view<TransformComponent>();
    m_WorldTransformCache.reserve(view.size());

    // Chain scratch, reused across entities so a deep hierarchy doesn't allocate per entity.
    std::vector<entt::entity> chain;
    for (entt::entity entity : view) {
        if (m_WorldTransformCache.count(entity)) continue; // already filled as some ancestor

        // Walk UP collecting the uncached part of the parent chain, then compose DOWN from the
        // topmost one. Every entity is therefore composed exactly once per rebuild, no matter
        // how many of its descendants the view visits afterwards.
        chain.clear();
        glm::mat4 base(1.0f);
        for (entt::entity walk = entity; walk != entt::null; ) {
            auto cached = m_WorldTransformCache.find(walk);
            if (cached != m_WorldTransformCache.end()) { base = cached->second; break; }
            chain.push_back(walk);
            const auto* h = Registry.try_get<HierarchyComponent>(walk);
            entt::entity next = h ? h->Parent : entt::null;
            if (next == entt::null || !Registry.valid(next)) break;
            // SetParent rejects cycles, but AttachChildRaw (used by scene loading) doesn't
            // validate — bail rather than spin forever on a malformed file.
            if (chain.size() > kMaxHierarchyDepth) break;
            walk = next;
        }

        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            // An ancestor with no TransformComponent contributes identity rather than throwing:
            // every entity the editor can create has one, but the cache must not be the thing
            // that crashes on a hand-edited or future component layout.
            if (const auto* t = Registry.try_get<TransformComponent>(*it)) base = base * ComposeTransform(*t);
            m_WorldTransformCache[*it] = base;
        }
    }
}

glm::mat4 World::GetCachedWorldTransform(entt::entity entity) const {
    auto it = m_WorldTransformCache.find(entity);
    if (it != m_WorldTransformCache.end()) return it->second;
    return ComposeWorldTransform(entity); // spawned since the rebuild, or none has run yet
}

bool World::SetParent(entt::entity child, entt::entity parent) {
    if (!Registry.valid(child) || child == parent) return false;
    if (parent != entt::null && !Registry.valid(parent)) return false;
    // ColliderComponent-bearing entities (all current level geometry) collide/raycast against
    // TransformComponent read directly as world space — parenting one would silently break
    // physics for it, so refuse rather than produce a subtly-wrong collider.
    if (parent != entt::null && Registry.all_of<ColliderComponent>(child)) return false;

    // Reject if `parent` is `child` or one of its own descendants — that would create a cycle.
    for (entt::entity walk = parent; walk != entt::null; ) {
        if (walk == child) return false;
        const auto* h = Registry.try_get<HierarchyComponent>(walk);
        walk = h ? h->Parent : entt::null;
    }

    glm::mat4 worldBefore = ComposeWorldTransform(child);

    if (auto* childHier = Registry.try_get<HierarchyComponent>(child)) {
        if (childHier->Parent != entt::null) {
            if (auto* oldParentHier = Registry.try_get<HierarchyComponent>(childHier->Parent)) {
                auto& kids = oldParentHier->Children;
                kids.erase(std::remove(kids.begin(), kids.end(), child), kids.end());
            }
        }
    }

    auto& childHier = Registry.get_or_emplace<HierarchyComponent>(child);
    childHier.Parent = parent;
    if (parent != entt::null) {
        Registry.get_or_emplace<HierarchyComponent>(parent).Children.push_back(child);
    }

    // Re-express TransformComponent in the new frame (local if parented, world if detached) so
    // the entity doesn't visually jump when the parent link changes.
    glm::mat4 newParentWorld = parent != entt::null ? ComposeWorldTransform(parent) : glm::mat4(1.0f);
    glm::mat4 newLocal = glm::inverse(newParentWorld) * worldBefore;

    glm::vec3 pos, scale, skew; glm::vec4 persp; glm::quat rot;
    glm::decompose(newLocal, scale, rot, pos, skew, persp);

    float ex, ey, ez;
    glm::extractEulerAngleYXZ(glm::mat4_cast(rot), ey, ex, ez);

    auto& t = Registry.get<TransformComponent>(child);
    t.Position = pos;
    t.Scale = scale;
    t.RotationEuler = glm::degrees(glm::vec3(ex, ey, ez));
    return true;
}

void World::AttachChildRaw(entt::entity child, entt::entity parent) {
    if (!Registry.valid(child) || !Registry.valid(parent) || child == parent) return;
    auto& childHier = Registry.get_or_emplace<HierarchyComponent>(child);
    childHier.Parent = parent;
    Registry.get_or_emplace<HierarchyComponent>(parent).Children.push_back(child);
}

void World::DestroyEntityAndChildren(entt::entity entity) {
    if (!Registry.valid(entity)) return;
    if (auto* hier = Registry.try_get<HierarchyComponent>(entity)) {
        // Copy first — destroying a child mutates its parent's HierarchyComponent (this one),
        // so recursing over the live vector would invalidate the iteration.
        std::vector<entt::entity> children = hier->Children;
        for (entt::entity c : children) DestroyEntityAndChildren(c);
        if (hier->Parent != entt::null) {
            if (auto* parentHier = Registry.try_get<HierarchyComponent>(hier->Parent)) {
                auto& kids = parentHier->Children;
                kids.erase(std::remove(kids.begin(), kids.end(), entity), kids.end());
            }
        }
    }
    Registry.destroy(entity);
}

glm::mat4 ComposeTransform(const glm::vec3& position, const glm::vec3& rotationEulerDegrees, const glm::vec3& scale) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m = glm::rotate(m, glm::radians(rotationEulerDegrees.y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(rotationEulerDegrees.x), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(rotationEulerDegrees.z), glm::vec3(0, 0, 1));
    m = glm::scale(m, scale);
    return m;
}
