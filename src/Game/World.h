#pragma once
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
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
    // Default is pure black: a fresh scene reads as a dark stage, and the sky never washes out
    // the HDR tonemapper or fights a scene's own lighting until it's set deliberately.
    glm::vec3 SkyHorizonColor{0.0f, 0.0f, 0.0f};
    glm::vec3 SkyZenithColor{0.0f, 0.0f, 0.0f};

    // Multiplier on the image-based ambient term the sky probes produce (#196). 1.0 is the
    // physically-consistent value: a surface fully open to a sky of radiance L receives L.
    // Exposed because the sky colours are authored by eye, not measured, so a scene lit mostly
    // by a bright authored sky can want the ambient pulled back without darkening the sky
    // itself. Purely a shader uniform — changing it does NOT rebake the probes.
    float SkyAmbientIntensity{1.0f};

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

    // Casts a ray against every Collider entity; returns the closest hit, or entt::null.
    // Editor-only now (drop-to-surface / snap); Play-mode collision moved to PhysX (#185 PR 3).
    entt::entity Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist, float& outDist) const;

    // Walks the HierarchyComponent parent chain (if any) and composes local transforms
    // top-down: an unparented entity's world transform is just ComposeTransform(its own
    // TransformComponent), matching every entity's behavior before parenting existed.
    glm::mat4 ComposeWorldTransform(entt::entity entity) const;

    // --- Per-frame world-transform cache (#173) -------------------------------------------
    // ComposeWorldTransform re-derives the whole parent chain on every call, so a frame that
    // asks for the same entity's world matrix from 4 cascades + N spots + 6 point faces + 2
    // viewport draws pays for that chain that many times. Worse, it made "world transform" the
    // expensive path, which is why several editor features reach for TransformComponent (the
    // LOCAL transform) and quietly break for parented entities (#224, #185).
    //
    // RebuildWorldTransformCache() computes every entity's world matrix ONCE, top-down (a
    // child's world = its parent's already-cached world * its own local), and
    // GetCachedWorldTransform() then hands it back for free. Call the rebuild once per frame
    // AFTER that frame's transform edits and BEFORE the shadow/render passes; the whole cache
    // is thrown away and rebuilt each time, so creation, destruction and re-parenting can never
    // leave a stale or dangling entry.
    void RebuildWorldTransformCache();

    // World matrix of `entity` from the cache built by the last RebuildWorldTransformCache().
    // Falls back to ComposeWorldTransform for anything the cache doesn't know about — an entity
    // spawned after the rebuild, or a caller running before the first one — so it is always
    // correct, never merely fast.
    glm::mat4 GetCachedWorldTransform(entt::entity entity) const;

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

    // Next value for OrderComponent (see Components.h). CreateBox/Model/Empty consume one each.
    // SceneSerializer bumps this above the highest value it loads, so entities added after a
    // load still sort after everything that came from the file.
    int AllocateOrder() { return m_NextOrder++; }
    void EnsureNextOrderAtLeast(int value) { if (value > m_NextOrder) m_NextOrder = value; }

private:
    std::unordered_map<entt::entity, glm::mat4> m_WorldTransformCache;
    int m_NextPrimitiveId = 0;
    int m_NextOrder = 0;
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
