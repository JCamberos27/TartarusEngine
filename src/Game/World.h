#pragma once
#include <string>
#include <memory>
#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include "AABB.h"
#include "Components.h"

class Model;

// Every placed thing — level geometry and placed models alike — is an entity in Registry built
// from the components in Components.h. See World.h's own comment history (and the ECS migration
// plan) for why: the old split between a flat-shaded, collidable "WorldBox" and a PBR-textured,
// non-collidable "PlacedModel" made level geometry unable to have real materials and placed
// models unable to block the player or take hits. One entity representation fixes both.
class World {
public:
    World();

    entt::registry Registry;

    // Vertical gradient sky (see Sky.h) — horizon at the world's XZ plane, zenith straight up.
    glm::vec3 SkyHorizonColor{0.53f, 0.72f, 0.86f};
    glm::vec3 SkyZenithColor{0.20f, 0.40f, 0.75f};

    // Creates a level-geometry entity: a fresh (unshared) cube-primitive Model tinted `color`,
    // a Collider, and LevelGeometryTag. `size` becomes the entity's Transform Scale, matching
    // the old WorldBox::Size (the native cube primitive is -0.5..0.5, so scale == world-space
    // size directly).
    entt::entity CreateBox(const glm::vec3& center, const glm::vec3& size, const glm::vec3& color,
        const glm::vec3& rotationEuler, const std::string& name);

    // Creates a placed-model entity (imported file or procedural primitive) — Transform +
    // Renderable only, no Collider (models opting into collision is a follow-up feature; this
    // matches today's behavior of models never blocking the player).
    entt::entity CreateModelEntity(std::shared_ptr<Model> modelRef, const glm::vec3& position,
        const glm::vec3& rotationEuler, const glm::vec3& scale, const std::string& name);

    // Creates a Transform + Name entity with no mesh — the base every "Add Component" build-up
    // starts from (Unity's Create Empty), and what a light or a pure grouping pivot is.
    entt::entity CreateEmptyEntity(const glm::vec3& position, const glm::vec3& rotationEuler,
        const glm::vec3& scale, const std::string& name);

    // Resolves a moving AABB against every solid (non-trigger) Collider entity, in place.
    // Returns true if the mover was grounded (resting on something) this call.
    bool ResolveCollisions(AABB& mover, glm::vec3& velocity) const;

    // Casts a ray against every Collider entity; returns the closest hit, or entt::null.
    entt::entity Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist, float& outDist) const;

    // Walks the HierarchyComponent parent chain (if any) and composes local transforms
    // top-down: an unparented entity's world transform is just ComposeTransform(its own
    // TransformComponent), matching every entity's behavior before parenting existed.
    glm::mat4 ComposeWorldTransform(entt::entity entity) const;

    // Re-parents `child` under `parent` (or detaches it if `parent` is entt::null), converting
    // TransformComponent in place so the entity doesn't visually jump: its world-space transform
    // is preserved, only the frame it's expressed in changes. Fails (returns false, no change)
    // if `parent` is `child` itself or one of `child`'s own descendants, which would create a
    // cycle. Deliberately does not support parenting a Collider entity — collision/raycast keep
    // reading TransformComponent directly as world space (see ColliderComponent), so a parented
    // entity with a Collider would collide using stale local coordinates.
    bool SetParent(entt::entity child, entt::entity parent);

    // Sets the Parent/Children links directly, WITHOUT touching TransformComponent — unlike
    // SetParent, which re-expresses it to preserve the entity's visual position (the right
    // behavior for an interactive editor re-parent, the wrong one here). Used only by
    // SceneSerializer, where a loaded entity's TransformComponent is already correct for
    // whichever frame (local or world) it belongs in.
    void AttachChildRaw(entt::entity child, entt::entity parent);

    // Destroys `entity` and, recursively, every descendant in its HierarchyComponent — so
    // deleting a parent doesn't leave orphaned children pointing at a dead entt::entity.
    void DestroyEntityAndChildren(entt::entity entity);

private:
    int m_NextPrimitiveId = 0;
    // Level-geometry entities own an unshared cube Model (never looked up by path elsewhere,
    // unlike imported/library models), so each just needs a unique synthetic path to satisfy
    // Model's constructor bookkeeping — this generates one.
    std::string NextPrimitivePath();
};

// Position * RotationY * RotationX * RotationZ * Scale — the one true transform composition
// order used everywhere an entity is turned into a world matrix (rendering, viewport picking,
// drag-drop placement, vertex snapping). Keeping it in one place means all of those stay
// consistent with each other by construction instead of by copy-paste diligence.
glm::mat4 ComposeTransform(const glm::vec3& position, const glm::vec3& rotationEulerDegrees, const glm::vec3& scale);
inline glm::mat4 ComposeTransform(const TransformComponent& t) {
    return ComposeTransform(t.Position, t.RotationEuler, t.Scale);
}
