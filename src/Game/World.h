#pragma once
#include "Log.h" // LogContext
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
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
// #182 - how a log line names an entity: "entity #<OrderComponent value>". The Order value
// survives undo/redo, Play->Stop and scene reload (raw entt ids are recycled by all three), so
// the Console's double-click can still find the right entity later. An entity without one
// (shouldn't happen for scene entities) prints "entity id <raw>", which the Console
// deliberately doesn't link.
std::string EntityLogRef(const entt::registry& registry, entt::entity e);
// #146 — the structured counterpart of EntityLogRef, for Log::Warn(msg, ctx): lets the Console
// select the entity without parsing the text. No entity link when it has no OrderComponent.
LogContext EntityLogContext(const entt::registry& registry, entt::entity e);

class World {
public:
    World();

    entt::registry Registry;

    // PR13: sky source — default Procedural (gradient) or Hdri (equirectangular .hdr file).
    // Procedural is the backwards-compatible default; existing scenes are unaffected.
    enum class SkySource { Procedural = 0, Hdri = 1 };
    SkySource   SkySourceMode{SkySource::Procedural};
    std::string SkyHdriPath;               // absolute or project-relative path to a .hdr file
    float       SkyRotationDegrees{0.0f};  // Y-axis rotation of the HDRI in degrees [0, 360)
    // #277 - an HDRI that contains the sun also bakes it into ambient + reflections, so a scene
    // that ALSO has a directional Sun counts it twice and washes out. Auto removes the HDRI's
    // sun from the IBL bake (radiance clamped to SkyHdriSunThreshold) whenever a directional
    // light is lit; Keep never clamps; Remove always does. The visible sky is never affected.
    enum class HdriSunMode { Auto = 0, Keep = 1, Remove = 2 };
    HdriSunMode SkyHdriSun{HdriSunMode::Auto};
    float       SkyHdriSunThreshold{50.0f}; // IBL radiance ceiling; open sky is ~1-10, a sun ~1e4-1e5

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

    // Post-processing / shadow settings (#9, Phase M item 1) — scene-authored content, moved off
    // EditorSettings/editor_prefs.json onto the scene itself: these are look choices the scene's
    // author makes, not per-user editor preferences, so they belong in the file that travels with
    // the scene. See EditorLayer::DrawPostProcessSettings/DrawShadowSettings for the editing UI.
    float ExposureEV{0.0f};        // photographic stops applied before the tone curve, 0 = neutral
    int   TonemapOperator{1};      // 0 Reinhard, 1 ACES, 2 AgX
    int   MsaaSamples{4};          // 1 / 2 / 4 / 8 for the HDR target

    bool  SsaoEnabled{false};
    // #160 — SSAO tuning (were hard-coded): sample radius in view-space units, depth bias against
    // self-occlusion, and an exponent on the result (1 = as computed, 2 = darker contact shadows).
    float SsaoRadius{0.5f};
    float SsaoBias{0.025f};
    float SsaoIntensity{1.0f};

    bool  BloomEnabled{false};
    float BloomThreshold{1.0f};
    float BloomKnee{0.5f};
    float BloomIntensity{0.25f};

    // #162 - anti-aliasing and colour grading, applied in the final tonemap pass.
    bool  FxaaEnabled{false};
    bool  AutoExposure{false};          // meter the scene and ease exposure toward mid grey
    float AutoExposureMinEV{-4.0f};     // how far it may brighten (negative) / darken (positive)
    float AutoExposureMaxEV{4.0f};
    float AutoExposureSpeedUp{2.0f};    // adapting to a brighter scene, per second
    float AutoExposureSpeedDown{1.0f};  // adapting to a darker scene
    float GradeTemperature{0.0f};  // -100..100, 0 = neutral (Unity's White Balance)
    float GradeTint{0.0f};
    float GradeContrast{0.0f};
    float GradeSaturation{0.0f};
    glm::vec3 GradeColorFilter{1.0f};
    float VignetteIntensity{0.0f}; // 0 = off
    float VignetteSmoothness{0.4f};

    // #162 - distance fog (Unity's Lighting > Other Settings > Fog).
    bool  FogEnabled{false};
    int   FogMode{2};               // 1 Linear, 2 Exponential, 3 Exponential Squared
    glm::vec3 FogColor{0.55f, 0.62f, 0.72f}; // linear
    float FogDensity{0.01f};        // Exponential modes
    float FogStart{10.0f};          // Linear mode, metres from the camera
    float FogEnd{300.0f};
    float FogHeightFalloff{0.0f};   // Exponential modes: 0 = uniform, higher = hugs the ground
    float FogBaseHeight{0.0f};      // world Y where height fog is at full density

    bool  ShadowsEnabled{true};
    int   ShadowResolution{4096};
    int   ShadowCascades{4};
    float ShadowDistance{500.0f};
    // #110 - shadow budgets for local lights: how many shadowed spot / point lights get a map
    // each frame (the most important ones, scored in main.cpp). Clamped to 0..SpotShadowMap::
    // kMaxSpots / PointShadowMap::kMaxPoints on load and in the UI.
    int   MaxSpotShadows{4};
    int   MaxPointShadows{2};

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

    // #114 — the entity's transform expressed in WORLD space (position / YXZ Euler degrees /
    // absolute scale decomposed from ComposeWorldTransform). Identical to its TransformComponent
    // when it has no parent. For systems that must work in world space (physics actors, collider
    // gizmos, editor raycasts) — reading TransformComponent directly misplaces any child entity.
    TransformComponent WorldSpaceTransform(entt::entity entity) const;

    // #150 — Unity's tag queries, for game code (the game module gets this World directly).
    // Tags are TagComponent::Tag ("Untagged" when absent); inactive entities are skipped, like
    // GameObject.FindWithTag. Exact, case-sensitive match.
    bool CompareTag(entt::entity entity, const std::string& tag) const;
    entt::entity FindWithTag(const std::string& tag) const;                // entt::null if none
    std::vector<entt::entity> FindAllWithTag(const std::string& tag) const;
    // #114 — the inverse: write a WORLD-space position + rotation into the entity's (local)
    // TransformComponent, converting through its parent's world matrix. Scale is left untouched.
    void SetWorldPose(entt::entity entity, const glm::vec3& worldPosition, const glm::quat& worldRotation);

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
    // Entities ComposeWorldTransform/RebuildWorldTransformCache have already logged a
    // parentId-cycle warning for (audit #74) — each one gets exactly one Console line, ever, not
    // one per frame. Never explicitly cleared on scene load: it only ever holds ids that were
    // once genuinely cyclic, so the sole cost of staleness across a load is a vanishingly
    // unlikely false negative (a brand-new entity reusing an old numeric id that also happens to
    // be cyclic again) — not worth wiring a clear-on-load hook for. Mutable because the compose
    // side of this is logically read-only (const).
    mutable std::unordered_set<entt::entity> m_CycleWarned;
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
