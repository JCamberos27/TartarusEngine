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
#include "SkySettings.h"

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
    // Atmosphere: the physical sky - atmospheric scattering, volumetric clouds, sun, moon, stars
    // and a time-of-day clock, all set in Sky below (SkyAtmosphere renders it).
    enum class SkySource { Procedural = 0, Hdri = 1, Atmosphere = 2 };
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

    // The physical sky's settings (SkySource::Atmosphere). Kept whatever the source, so
    // switching away and back doesn't lose them.
    SkySettings Sky;

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
    float ChromaticAberration{0.0f}; // #162 - 0 = off
    float FilmGrain{0.0f};           // #162 - 0 = off
    float FilmGrainResponse{0.8f};
    // #162 - depth of field (Game view / player only).
    bool  DepthOfField{false};
    float FocusDistance{10.0f};
    float FocusRange{3.0f};
    float DofMaxBlur{8.0f};

    // #162 - camera motion blur (Game view / player only, like DOF). Velocity is reprojected
    // from depth, so this blurs camera movement, not an object moving under a still camera.
    bool  MotionBlur{false};
    float MotionBlurIntensity{0.5f};
    int   MotionBlurSamples{12};

    // #162 - distance fog (Unity's Lighting > Other Settings > Fog).
    bool  FogEnabled{false};
    int   FogMode{2};               // 1 Linear, 2 Exponential, 3 Exponential Squared
    glm::vec3 FogColor{0.55f, 0.62f, 0.72f}; // linear
    float FogDensity{0.01f};        // Exponential modes
    float FogStart{10.0f};          // Linear mode, metres from the camera
    float FogEnd{300.0f};
    float FogHeightFalloff{0.0f};   // Exponential modes: 0 = uniform, higher = hugs the ground
    float FogBaseHeight{0.0f};      // world Y where height fog is at full density

    // Froxel volumetric fog (VolumetricFog.h): light shafts through the air from the directional
    // light and every point / spot light, shadowed by their shadow maps. Each light's share is
    // LightComponent::VolumetricIntensity. Off by default (older scenes load without it).
    bool  VolumetricEnabled{false};
    float VolumetricDensity{0.02f};        // extinction per metre at the base height (0.02 = light haze)
    float VolumetricAnisotropy{0.6f};      // Henyey-Greenstein g: 0 even, > 0 bright looking toward a light
    float VolumetricHeightFalloff{0.0f};   // 0 = same everywhere; higher = thins out above the base height
    float VolumetricBaseHeight{0.0f};      // world Y below which the density is full
    glm::vec3 VolumetricAlbedo{0.9f};      // how much of what it absorbs it scatters, per channel
    float VolumetricDistance{64.0f};       // metres from the camera the froxel grid reaches
    int   VolumetricQuality{1};            // 0 Low (128x72x48), 1 Medium (160x90x64), 2 High (240x135x96)

    bool  ShadowsEnabled{true};
    int   ShadowResolution{4096};
    int   ShadowCascades{4};
    float ShadowDistance{500.0f};
    // Cascade split blend (CascadedShadowMap::SetSplitLambda): higher = tighter, sharper near
    // cascades. 0.75 matches every scene saved before the setting existed.
    float ShadowCascadeSplit{0.75f};
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
    // AFTER that frame's transform edits and BEFORE the shadow/render passes. Every entity is
    // visited each time, so creation, destruction and re-parenting can never leave a stale or
    // dangling entry; but an entity whose local transform and parent are what they were last
    // time, under an unchanged parent, keeps its matrix instead of recomposing it (a forest of
    // 200k entities that never move was 25 ms a frame).
    void RebuildWorldTransformCache();
    // Changes every time RebuildWorldTransformCache runs, never repeating across worlds: what is derived from the
    // cached transforms (GatherVisible's per-frame candidates) is good for as long as this stays the same.
    uint64_t TransformCacheStamp() const { return m_TransformCacheStamp; }
    // The stamp of the rebuild before the latest one, and what the latest one changed relative to it: the entities
    // whose world matrix it recomputed, and whether any entity entered or left the cache. A cache derived at the
    // previous stamp can patch just those instead of starting over.
    uint64_t PreviousTransformCacheStamp() const { return m_PrevTransformCacheStamp; }
    const std::vector<entt::entity>& ChangedWorldTransforms() const { return m_ChangedTransforms; }
    bool TransformCacheMembershipChanged() const { return m_TransformMembershipChanged; }

    // #163 - picks each LOD Group's level for a view (eye position + projection) and tags the
    // renderers of every other level with LodCulledTag. Call before each view's draw and shadow
    // passes. Returns the number of LOD Groups evaluated.
    int ApplyLod(const glm::vec3& viewPos, const glm::mat4& proj);
    // Removes every LodCulledTag.
    void ClearLod();
    // Whether ApplyLod tagged `e` LodCulledTag: the same answer as the tag, read from a flat per-slot table (the
    // visibility gathers ask for every renderable, several times a frame).
    bool IsLodCulled(entt::entity e) const {
        const size_t s = (size_t)entt::to_entity(e);
        return s < m_LodCulledOwner.size() && m_LodCulledOwner[s] == e;
    }
    // #201 - derives InactiveTag (activeInHierarchy == false) from DeactivatedTag on the entity
    // or any ancestor. RebuildWorldTransformCache() runs it every frame; scene/prefab loads run
    // it on the way out so their callers see a consistent state immediately.
    void SyncActiveInHierarchy();

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

    // #202 - breaks any parent cycle in the hierarchy, returning how many were broken.
    // AttachChildRaw only rejects self-parenting, so a scene file whose parentIds form a loop
    // (A -> B -> A, hand-edited, merged badly, or written by an older build) loads a genuine
    // cycle: those entities are then unreachable from any root, so they never draw, and saving
    // writes the loop straight back out. SetParent already refuses to create one interactively;
    // this is the same guarantee for the load path. Each cycle is broken by detaching the entity
    // that closes it, which becomes a root, and is reported.
    int RepairHierarchyCycles();

    // Destroys `entity` and, recursively, every descendant in its HierarchyComponent — so
    // deleting a parent doesn't leave orphaned children pointing at a dead entt::entity.
    void DestroyEntityAndChildren(entt::entity entity);

    // Next value for OrderComponent (see Components.h). CreateBox/Model/Empty consume one each.
    // SceneSerializer bumps this above the highest value it loads, so entities added after a
    // load still sort after everything that came from the file.
    int AllocateOrder() { return m_NextOrder++; }
    void EnsureNextOrderAtLeast(int value) { if (value > m_NextOrder) m_NextOrder = value; }

private:
    // Flat, indexed by entity slot (entt::to_entity) rather than hashed: every render pass reads
    // it once per drawable, so a hash lookup per read added up. m_WorldTransformKeys holds the
    // full entity (slot + version) each slot was filled for; a mismatch (entt::null, or a slot
    // since recycled by a newer entity) means "not cached" and falls back to composing.
    std::vector<glm::mat4> m_WorldTransformCache;
    std::vector<entt::entity> m_WorldTransformKeys;
    uint64_t m_TransformCacheStamp = 0;
    uint64_t m_PrevTransformCacheStamp = 0;
    // What each slot's matrix was composed from, so the next rebuild can tell it is unchanged.
    struct LocalSnapshot {
        glm::vec3 Position{0.0f};
        glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 Scale{1.0f};
        entt::entity Parent = entt::null;
        bool HasTransform = false;
    };
    std::vector<LocalSnapshot> m_LocalSnapshots;
    std::vector<std::uint32_t> m_RebuildDone;     // the rebuild generation that filled the slot
    std::vector<std::uint8_t> m_RebuildChanged;   // that rebuild recomputed it
    std::uint32_t m_RebuildGeneration = 0;
    std::vector<entt::entity> m_ChangedTransforms;
    bool m_TransformMembershipChanged = true;
    // The hierarchy version and hierarchy/transform counts as of the last full rebuild (see RebuildChangedTransformsOnly).
    std::uint64_t m_TransformStructure[3] = {};
    bool m_TransformStructureValid = false;
    std::vector<entt::entity> m_DirtyScratch, m_PendingScratch, m_ChainScratch;
    void TransformStructure(std::uint64_t out[3]);
    bool RebuildChangedTransformsOnly();
    // SyncActiveInHierarchy's inputs as of its last run (hierarchy, transforms, Deactivated/InactiveTag sets): it is
    // skipped while they stay the same.
    std::uint64_t m_ActiveSignature[6] = {};
    bool m_ActiveSignatureValid = false;
    void ActiveSignature(std::uint64_t out[6]);
    // ApplyLod's memory of what it tagged, per LOD Group slot (see ApplyLod). Active is the level left untagged, -1 for
    // a culled group (every level tagged), kLodSkipped for one that tagged nothing (inactive, or not seen yet).
    static constexpr int kLodSkipped = -2;
    struct LodGroupState {
        entt::entity Group = entt::null;
        std::int8_t Active = kLodSkipped;
        std::uint8_t Levels = 0;
        entt::entity Kids[4] = {entt::null, entt::null, entt::null, entt::null};
        float AutoSize = 0.0f;     // LOD 0's measured size (for LODGroupComponent::Size 0)
        glm::vec3 Centre{0.0f};    // the group's world position
    };
    std::vector<LodGroupState> m_LodState;    // dense, in the groups view's order
    std::vector<std::uint32_t> m_LodIndex;    // entity slot -> m_LodState index (~0u: none)
    std::uint64_t m_LodCentreStamp = 0;       // the transform cache stamp the centres were read at
    std::vector<entt::entity> m_LodStack;
    std::vector<entt::entity> m_LodCulledOwner; // entity slot -> the entity tagged there (IsLodCulled)
    void SetLodCulled(entt::entity e, bool culled);
    std::vector<std::pair<entt::entity, bool>> m_LodWalk;
    std::uint64_t m_LodSignature[4] = {};
    bool m_LodValid = false;  // the tags match m_LodState
    // Bumped by every re-parent, attach and hierarchy destroy: ApplyLod's records are only good while it stays put.
    std::uint64_t m_HierarchyVersion = 0;
    bool IsWorldTransformCached(entt::entity e) const {
        const auto i = (size_t)entt::to_entity(e);
        return i < m_WorldTransformKeys.size() && m_WorldTransformKeys[i] == e;
    }
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

// Position * Rotation * Scale — the one true transform composition
// order used everywhere an entity is turned into a world matrix (rendering, viewport picking,
// drag-drop placement, vertex snapping). Keeping it in one place means all of those stay
// consistent with each other by construction instead of by copy-paste diligence.
glm::mat4 ComposeTransform(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale);
inline glm::mat4 ComposeTransform(const glm::vec3& position, const glm::vec3& rotationEulerDegrees,
                                  const glm::vec3& scale) {
    return ComposeTransform(position, QuaternionFromEulerYXZ(rotationEulerDegrees), scale);
}
inline glm::mat4 ComposeTransform(const TransformComponent& t) {
    return ComposeTransform(t.Position, t.Rotation, t.Scale);
}
