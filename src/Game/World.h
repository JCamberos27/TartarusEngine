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
#include "SceneRenderSettings.h"

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

    // The scene's render/post settings (sky, exposure, AA, SSAO, bloom, grading, DoF, wetness, fog,
    // volumetrics, shadows): SceneRenderSettings.h.
    SceneRenderSettings Render;
    using SkySource = SceneRenderSettings::SkySource;
    using HdriSunMode = SceneRenderSettings::HdriSunMode;
    using AntiAliasingMode = SceneRenderSettings::AntiAliasingMode;
    static constexpr int kTaaMsaaSamples = SceneRenderSettings::kTaaMsaaSamples;
    static AntiAliasingMode LegacyAntiAliasing(int msaaSamples, bool fxaa) {
        return SceneRenderSettings::LegacyAntiAliasing(msaaSamples, fxaa);
    }

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
    // What the latest rebuild changed in the cache's STRUCTURE relative to the previous one, filled by the incremental
    // pass's structure step and cleared by every rebuild (read only). Read this, not ChangedWorldTransforms, for who
    // came, went or moved in the hierarchy: that one lists recomposed matrices and promises nothing about why, so a
    // re-parent that keeps the world matrix (SetParent does) is listed here but need not be there. Valid is false
    // after a rebuild that took the full walk, which keeps no lists: anything derived must be rebuilt in full.
    // An entity that only lost or regained its TransformComponent stays in the cache and is not listed.
    struct TransformStructureDelta {
        bool Valid = false;
        std::vector<entt::entity> Added;                               // new to the cache
        std::vector<std::pair<entt::entity, entt::entity>> Reparented; // (entity, the parent it had)
        std::vector<std::pair<entt::entity, entt::entity>> Dropped;    // (destroyed entity as cached, the parent it had)
    };
    const TransformStructureDelta& TransformStructureChanges() const { return m_StructureDelta; }
    // How many ApplyLod calls rebuilt their records in full (tests: a small structure change patches them).
    std::uint64_t LodFullPasses() const { return m_LodFullPasses; }
    // How many rebuilds took the full walk (tests: a small structure change takes the incremental pass).
    std::uint64_t TransformCacheFullWalks() const { return m_TransformFullWalks; }

    // #163 - picks each LOD Group's level for a view (eye position + projection) and tags the
    // renderers of every other level with LodCulledTag. Call before each view's draw and shadow
    // passes. Returns the number of LOD Groups evaluated.
    int ApplyLod(const glm::vec3& viewPos, const glm::mat4& proj);
    // Smooth LOD (the live views set it; off by default, so a level change is instant): a level holds a little past
    // its switch point before handing over (kLodHysteresis, no flicker while standing at one), and the hand-over is
    // a dithered cross-fade over kLodFadeSeconds - the old level dissolves out as the new one dissolves in, in
    // complementary screen-space patterns (LodFadeOf), so nothing pops.
    bool LodSmooth = false;
    static constexpr float kLodFadeSeconds = 0.6f;
    // The clock the cross-fades run on, in seconds; < 0 (the default) is real time. Tests step it by hand.
    double LodClockOverride = -1.0;
    static constexpr float kLodHysteresis = 0.1f;
    // The cross-fade value of a renderer now: 0 none; t > 0 fading in (drawn where the dither is under t); -t fading
    // out (drawn where it is t or over).
    float LodFadeOf(entt::entity e) const {
        const size_t s = (size_t)entt::to_entity(e);
        return s < m_LodFadeOwner.size() && m_LodFadeOwner[s] == e ? m_LodFadeValue[s] : 0.0f;
    }
    // Octahedral impostors (ImpostorLibrary): a LOD group whose LOD 0 matches a baked set draws its impostor instead of
    // any level once its centre is past ImpostorLibrary::StartDistance() from the view (the screen-height levels still
    // decide nearer, and the group stays culled where they cull it). The impostor is a level of its own to ApplyLod:
    // every child is tagged, and the hand-over cross-fades like any other. Calls f(group, set, fade) for each group
    // the last ApplyLod left showing its impostor, fade as LodFadeOf (0 whole, > 0 fading in, < 0 fading out).
    template <class F>
    void ForEachImpostor(F&& f) const {
        if (!m_LodImpostors) return;
        for (const LodGroupState& st : m_LodState)
            if (st.Impostor >= 0 && (st.Active == kLodImpostor || st.FadeFrom == kLodImpostor))
                f(st.Group, (int)st.Impostor, st.ImpostorFade);
    }
    // Removes every LodCulledTag and forgets the last ApplyLod (its records, impostors and cross-fades).
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
    // Changes whenever a parent link changes (SetParent, a destroy, a cut cycle): what is derived
    // from the hierarchy is good while this stays the same.
    std::uint64_t HierarchyVersion() const { return m_HierarchyVersion; }

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
    // cycle. Collider entities parent like any other (physics and raycasts read world space).
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
    // Cycle guard for the parent-chain walks (the transform cache, LOD groups): no legitimate scene nests this
    // deep, so exceeding it means a malformed hierarchy, not a tall one.
    static constexpr size_t kMaxHierarchyDepth = 1024;

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
    std::vector<std::uint64_t> m_LocalHashes; // LocalHash of each snapshot's transform (0: none), by slot
    std::vector<std::uint32_t> m_RebuildDone;     // the rebuild generation that filled the slot
    std::vector<std::uint8_t> m_RebuildChanged;   // that rebuild recomputed it
    std::uint32_t m_RebuildGeneration = 0;
    std::vector<entt::entity> m_ChangedTransforms;
    bool m_TransformMembershipChanged = true;
    // The hierarchy version and hierarchy/transform counts as of the last full rebuild (see RebuildChangedTransformsOnly).
    std::uint64_t m_TransformStructure[3] = {};
    bool m_TransformStructureValid = false;
    std::vector<entt::entity> m_DirtyScratch, m_PendingScratch, m_ChainScratch;
    TransformStructureDelta m_StructureDelta;
    void TransformStructure(std::uint64_t out[3]);
    bool RebuildChangedTransformsOnly();
    bool ApplyTransformStructureChanges(std::vector<entt::entity>& dirty);
    void EnsureTransformSlots(size_t n);
    std::uint64_t m_TransformFullWalks = 0;
    size_t m_KeyedWithTransform = 0; // cache slots whose entity had a transform when last composed
    void CountKeyedWithTransform();
    // SyncActiveInHierarchy's inputs as of its last run (hierarchy, transforms, Deactivated/InactiveTag sets): it is
    // skipped while they stay the same.
    std::uint64_t m_ActiveSignature[6] = {};
    bool m_ActiveSignatureValid = false;
    void ActiveSignature(std::uint64_t out[6]);
    // ApplyLod's memory of what it tagged, per LOD Group slot (see ApplyLod). Active is the level left untagged, -1 for
    // a culled group (every level tagged), kLodSkipped for one that tagged nothing (inactive, or not seen yet).
    static constexpr int kLodSkipped = -2;
    static constexpr int kLodImpostor = 8; // Active / FadeFrom: the group's impostor (past every child level)
    struct LodGroupState {
        entt::entity Group = entt::null;
        std::int8_t Active = kLodSkipped;
        std::uint8_t Levels = 0;
        entt::entity Kids[4] = {entt::null, entt::null, entt::null, entt::null};
        float AutoSize = 0.0f;     // LOD 0's measured size (for LODGroupComponent::Size 0)
        glm::vec3 Centre{0.0f};    // the group's world position
        std::int8_t FadeFrom = -1; // LodSmooth: the level fading out (left untagged meanwhile), -1 none
        double FadeStart = 0.0;    // when it began (Time::RealtimeSinceStartup)
        double LastChange = -1e9;  // when its level last changed (LodSmooth): a group that keeps flipping doesn't fade
        std::int16_t Impostor = -1; // ImpostorLibrary set drawn past the start distance, -1 none
        float ImpostorFade = 0.0f;  // the impostor's cross-fade, as LodFadeOf
    };
    bool m_LodImpostors = false; // some record has an impostor
    std::vector<float> m_LodFadeValue;        // entity slot -> LodFadeOf
    std::vector<entt::entity> m_LodFadeOwner; // entity slot -> the entity the value is for
    std::vector<entt::entity> m_LodFading;    // groups with a cross-fade under way
    std::vector<LodGroupState> m_LodState;    // dense, in the groups view's order
    std::vector<std::uint32_t> m_LodIndex;    // entity slot -> m_LodState index (~0u: none)
    std::uint64_t m_LodCentreStamp = 0;       // the transform cache stamp the centres were read at
    std::vector<entt::entity> m_LodStack;
    std::vector<const Model*> m_LodModels; // ApplyLod: a LOD 0's models, for ImpostorLibrary::Match
    struct LodMemo { std::uint64_t Stamp = 0; glm::vec3 ViewPos{0.0f}; float YScale = 0.0f; bool Ortho = false;
                     std::uint64_t Inactive = 0; int Evaluated = 0; };
    LodMemo m_LodMemo; // the last ApplyLod's view, so the same view again within a frame is free
    std::vector<entt::entity> m_LodCulledOwner; // entity slot -> the entity tagged there (IsLodCulled)
    void SetLodCulled(entt::entity e, bool culled);
    std::vector<std::pair<entt::entity, bool>> m_LodWalk;
    std::vector<std::int8_t> m_LodNext;       // ApplyLod's incremental pass: each record's new level, picked in parallel
    std::vector<int> m_LodChunkEvaluated;
    std::vector<size_t> m_LodChunkIds;
    std::uint64_t m_LodSignature[4] = {};
    bool m_LodValid = false;  // the tags match m_LodState
    std::uint64_t m_LodFullPasses = 0;
    // Entity slot -> the entity that had a RenderableComponent as of the last ApplyLod (filled by its full pass, kept by
    // its structure patch): tells which of the transform cache's dropped entities took a renderer with them.
    std::vector<entt::entity> m_LodRenderOwner;
    std::vector<entt::entity> m_LodTouched, m_LodRemeasure; // the structure patch's scratch
    std::vector<std::pair<entt::entity, entt::entity>> m_LodDroppedSorted;
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
