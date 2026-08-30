#include "World.h"
#include "Model.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <algorithm>

namespace {
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

World::World() {
    // Ground.
    CreateBox({0, -0.5f, 0}, {60.0f, 1.0f, 60.0f}, {0.25f, 0.28f, 0.25f}, {0, 0, 0}, "");

    // A scattering of crate-like placeholder geometry, just so a brand-new scene isn't a bare
    // plane.
    struct P { float x, z, s, h; };
    P layout[] = {
        {5, 5, 2, 2}, {-6, 4, 2, 3}, {8, -4, 2, 1.5f}, {-4, -6, 2, 2.5f},
        {0, 10, 2, 2}, {12, 0, 2, 4}, {-12, 0, 2, 2}, {3, -10, 2, 2},
        {-8, -8, 1.5f, 1.5f}, {10, 8, 2, 2},
    };
    glm::vec3 colors[] = {
        {0.8f,0.3f,0.2f}, {0.2f,0.5f,0.8f}, {0.8f,0.7f,0.2f}, {0.4f,0.8f,0.3f},
        {0.7f,0.3f,0.7f}, {0.3f,0.8f,0.8f}, {0.9f,0.5f,0.2f}, {0.5f,0.5f,0.9f},
        {0.9f,0.2f,0.4f}, {0.4f,0.9f,0.6f},
    };
    for (int i = 0; i < 10; ++i) {
        const P& p = layout[i];
        CreateBox({p.x, p.h * 0.5f, p.z}, {p.s, p.h, p.s}, colors[i], {0, 0, 0}, "");
    }
}

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

bool World::ResolveCollisions(AABB& mover, glm::vec3& velocity) const {
    bool grounded = false;
    auto view = Registry.view<const TransformComponent, const ColliderComponent>(entt::exclude<InactiveTag>);
    for (auto entity : view) {
        const auto& [transform, collider] = view.get<const TransformComponent, const ColliderComponent>(entity);
        if (collider.IsTrigger) continue;
        AABB b = ColliderWorldBounds(Registry, entity, transform);
        if (!mover.Intersects(b)) continue;

        // --- Stand / land on top --------------------------------------------------------
        // If the mover isn't heading upward and its feet sit within a short reach of this
        // collider's top face, treat it as ground: snap the feet exactly onto the surface and
        // kill downward speed. Handled separately from the min-translation push-out below
        // because that push moves along the *smallest* overlap axis — and for a thin or
        // single-quad floor collider (the BuildingKit floor tiles are basically flat slabs)
        // the vertical overlap is ~0, so MTV barely pushes back and the player sinks straight
        // through. kGroundGrab only needs to cover one integration substep of penetration
        // (Player::Update caps a step at a few cm) plus feet resting a hair above the surface.
        const float kGroundGrab = 0.35f;
        if (velocity.y <= 0.0f && mover.Min.y >= b.Max.y - kGroundGrab) {
            float lift = b.Max.y - mover.Min.y; // Intersects() guarantees this is >= 0
            mover.Min.y += lift;
            mover.Max.y += lift;
            velocity.y = 0.0f;
            grounded = true;
            continue; // resolved vertically — don't also shove it sideways this frame
        }

        // --- Generic push-out (walls, ceilings, box sides) ----------------------------
        glm::vec3 mtv = mover.MTV(b);
        mover.Min += mtv;
        mover.Max += mtv;
        if (mtv.y > 0.0f) {
            grounded = true;
            if (velocity.y < 0.0f) velocity.y = 0.0f;
        } else if (mtv.y < 0.0f) {
            if (velocity.y > 0.0f) velocity.y = 0.0f;
        }
        if (mtv.x != 0.0f) velocity.x = 0.0f;
        if (mtv.z != 0.0f) velocity.z = 0.0f;
    }
    return grounded;
}

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
    glm::mat4 local = ComposeTransform(Registry.get<TransformComponent>(entity));
    const auto* hier = Registry.try_get<HierarchyComponent>(entity);
    if (hier && hier->Parent != entt::null && Registry.valid(hier->Parent)) {
        return ComposeWorldTransform(hier->Parent) * local;
    }
    return local;
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
