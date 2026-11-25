#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "AABB.h"
#include "CascadedShadowMap.h"
#include "Frustum.h"
#include "GpuScene.h"
#include "InstanceBatch.h"
#include "PointShadowMap.h"
#include "SceneRenderer.h"
#include "SpotShadowMap.h"

class World;
class Shader;
class Model;
struct MaterialAsset;
class VisibleIndexBuffer;
struct FrameLights;

// A local (spot / point) caster's change signature: its id, model, every material slot and what each mesh's depth draw
// reads from it (Model::DepthMeshState: queue, cull, cutout, sway), transform, hidden-triangle set (VisibleIndexBuffer
// id, 0 = none) and Two Sided. A still light keeps its static depth while the signatures hold.
std::uint64_t LocalCasterHash(std::uint32_t id, const Model* model, const std::vector<std::shared_ptr<MaterialAsset>>& materials,
                              const glm::mat4& xform, unsigned visibleId, bool twoSided);

// G4-c: which local lights may keep their maps this frame without gathering their casters or hashing them. A light
// turns quiet when a full look at it found nothing in it moving, animated or settling, its static cache built and its
// live layer equal to that cache. It stays quiet until a logged slot change (GpuScene::TakeChangeLog: a moved, posed,
// swapped, added or removed renderable, where it was and where it is) touches its reach, its own inputs (key: map,
// resolution, matrices, position, range) change, it goes dark, a reported render-settings write lands
// (RenderSettingsEpoch: a material or Cast Shadows edit), or the log says anything may have changed.
struct LocalShadowQuiet {
    struct Light {
        bool Quiet = false;
        std::uint64_t Key = 0;
    };
    Light Spots[SpotShadowMap::kMaxSpots];
    Light Points[PointShadowMap::kMaxPoints];
    std::uint32_t Epoch = 0;
    bool EpochSeen = false;
    struct Lights {
        int SpotCount = 0, PointCount = 0;
        const bool* SpotLive = nullptr;
        const std::uint64_t* SpotKey = nullptr;
        const Frustum* SpotReach = nullptr;
        const bool* PointLive = nullptr;
        const std::uint64_t* PointKey = nullptr;
        const AABB* PointRange = nullptr;
    };
    // Wakes the lights this frame's changes reach and takes their keys.
    void Wake(const GpuScene::ChangeLog& log, std::uint32_t epoch, const Lights& lights);
    void Reset(); // every light awake (the next full look decides again)
};

// G4-c: LOD level changes since the last look, as slot changes where each flipped renderer stands. A level change swaps
// which renderers a light gathers without moving any slot. A renderer flipped and back between two looks (another
// view's levels applied in between, as the editor's Scene and Game views do each frame) nets out against what the last
// look saw.
struct LodToggleWake {
    // Entity slot -> the entity the last look saw LOD-culled there (null: none), as World::IsLodCulled.
    std::vector<entt::entity> Seen;
    const World* For = nullptr;
    std::vector<entt::entity> Toggled; // scratch
    void Append(World& world, GpuScene::ChangeLog& log);
};

struct ShadowFrameInputs {
    World* world = nullptr;
    const FrameLights* lights = nullptr;
    Shader* sunShader = nullptr;   // ShadowDepth: the cascades (and the SSAO pre-pass)
    Shader* localShader = nullptr; // ShadowDepthLocal: linear distance-to-light depth for spot/point maps
    CascadedShadowMap* sunMap = nullptr;
    SpotShadowMap* spotMap = nullptr;
    PointShadowMap* pointMap = nullptr;
    std::uint64_t frame = 0;
};

// The frame's shadow maps: the sun's cascades (per rendered view, #109) and the spot / point maps (once per frame, #119),
// with the static-depth caches that keep a still light's map from being redrawn.
class ShadowFrame {
public:
    // Fits and renders the cascades to this view; a second view with the same matrices in the same frame reuses them.
    // False when the sun casts no shadows this frame. Restores the caller's framebuffer and viewport.
    bool RenderSun(const ShadowFrameInputs& in, const glm::mat4& fitView, const glm::mat4& fitProj);
    // Spot and point maps. Call once per frame after the characters are pre-skinned. Restores the caller's
    // framebuffer and viewport.
    void RenderLocal(const ShadowFrameInputs& in);

    // The sun's caster list (GatherSunCasters' last step): m_Casting -> one SunCaster each, and the G4-d motion check
    // that returns the held far cascades a moved or re-posed caster forces. The full loop refills every caster; when the
    // gathered set is the one the list already holds (caster for caster: slot, entity, renderable), the render settings
    // epoch and the outfit-hidden entities are unchanged, and `log` is this frame's GPU scene change log taken right
    // after the one the list was last brought up to date with, the delta path refills and re-checks only the casters
    // that can differ - those the log names, the skinned ones, the outfit-hidden ones - with the same per-caster code.
    // Every kSunFullEvery frames the full loop runs anyway. TARTARUS_NO_SUN_CASTER_DELTA=1: always the full loop.
    struct SunCasterProbe { // what the loop asks a model (tests stand in a fake pose for a skinned one)
        bool (*Skinned)(const Model*) = nullptr;
        std::uint64_t (*Pose)(const Model*) = nullptr;
    };
    struct SunListIn {
        World* world = nullptr;
        std::uint64_t frame = 0;      // this frame (ShadowFrameInputs::frame)
        unsigned held = 0;            // cascades held this frame (not in the render mask)
        int cascadeCount = 0;
        // Whether cascade c's receivers read a shadow from a caster in [lo, hi] (CasterReachesSlice, no hold margin).
        std::function<bool(int c, const glm::vec3& lo, const glm::vec3& hi)> reaches;
        const GpuScene::ChangeLog* log = nullptr; // this frame's change log, or null when none was taken
        std::uint64_t logPrevFrame = ~0ull;       // the frame the change log before it was taken
        bool allowDelta = true;
        SunCasterProbe probe;                     // unset: the model's own HasBones / PoseVersion
    };
    static constexpr std::uint64_t kSunFullEvery = 16;
    unsigned FillSunCasters(const SunListIn& in); // returns the cascades to force (G4-d)
    // Tests: the gathered casters the next FillSunCasters reads, whether the last one took the delta path, and whether
    // two frames hold the same list and motion table field for field.
    std::vector<VisibleRenderable>& CastingForTest() { return m_Casting; }
    bool LastSunListWasDelta() const { return m_SunLastDelta; }
    bool SameSunListAs(const ShadowFrame& other) const;
    // Sun caster lists since startup: how many took the delta path (--perf-bench prints a phase's share).
    struct SunListTotals { std::uint64_t Delta = 0, Full = 0; };
    static SunListTotals SunListTotalsSoFar();

    // The sun's gather kept across frames (GatherSunCasters): the static cells are gathered against the cascades padded
    // by kSunGatherPadTexels, and while this frame's cascades still sit inside that padded set (and nothing the gather
    // reads changed: no static candidate moved or LOD level flipped inside it, no candidate edit, the same sun
    // direction, cascade count, render settings and world) the kept static part is reused; only the Always cells
    // (animated, unbounded) are gathered again. With the sun direction unchanged every cascade is a box of the same
    // light space, so a padded cascade's cell test takes every cell the cascade's own takes: the kept list holds all a
    // fresh gather would, and its extras lie outside every cascade (their draws are culled as before). Every
    // kSunGatherFullEvery frames it gathers anew anyway. TARTARUS_NO_SUN_GATHER_REUSE=1: gather every frame.
    // Verify mode (CacheVerify) also runs a fresh gather on every reused frame and counts any caster it holds that could
    // draw and the kept list lacks or holds differently, and any extra that would draw (Log::Error).
    static constexpr float kSunGatherPadTexels = 16.0f;
    static constexpr std::uint64_t kSunGatherFullEvery = 64;
    // Whether every corner of `now`'s n cascades lies inside `base`'s, each grown by `margin` in clip space.
    static bool CascadesInside(const glm::mat4* base, const glm::mat4* now, int n, float margin);
    // Verify: reused frames checked; casters that can draw missing from (or held stale in) the kept list; extras in it
    // that would draw; and casters only a fresh gather holds that draw nothing (their ground cell touches a cascade).
    struct SunGatherTotals {
        std::uint64_t Reused = 0, Gathered = 0, VerifyChecked = 0, VerifyMissing = 0, VerifyExtraDraws = 0, VerifyAbsentNoDraw = 0;
    };
    static SunGatherTotals SunGatherTotalsSoFar();

    // What the last RenderLocal redrew (GI Phase 1 perf: slot re-renders vs per-caster cost; --gi-tour writes it per
    // frame). A "static redraw" rebuilds a cache from scratch - what a slot changing hands costs; a "dynamic" pass only
    // draws the moving casters over the cache.
    struct LocalStats {
        int SpotLive = 0, SpotStaticRedraws = 0, SpotDynamicPasses = 0, SpotWholeRedraws = 0;
        int PointLive = 0, PointStaticFaces = 0, PointDynamicFaces = 0, PointKeptFaces = 0;
        int CasterDraws = 0;       // casters submitted across every local map this frame
        int StaticCasterDraws = 0; // of them, into a static cache (the re-render cost)
        int SpotQuiet = 0, PointQuiet = 0; // live lights kept without a look (G4-c)
    };
    const LocalStats& LastLocalStats() const { return m_Stats; }

    // G4-c is on: TARTARUS_NO_SHADOW_SLOT_DIRTY unset and the GPU scene on (its change log).
    static bool SlotDirtyEnabled();
    // Verify mode (TARTARUS_SHADOW_CACHE_VERIFY=1, or --smoke-test): quiet lights are looked at anyway, and one that
    // then redraws anything is a miss (Log::Error). Totals since start.
    static void SetCacheVerify(bool on);
    static bool CacheVerify();
    struct VerifyTotals { std::uint64_t Checked = 0, Missed = 0; };
    static const VerifyTotals& CacheVerifyTotals();
    // Live local lights looked at and of them kept quiet (G4-c), totals since start: --perf-bench prints each phase's share.
    struct QuietTotals { std::uint64_t Live = 0, Quiet = 0; };
    static const QuietTotals& LocalQuietTotals();

private:
    LocalStats m_Stats;
    bool m_CountingStatic = false; // DrawLocalCasters is filling a static cache (LocalStats::StaticCasterDraws)
    struct SunCaster {
        Model* model;
        const std::vector<std::shared_ptr<MaterialAsset>>* materials;
        const VisibleIndexBuffer* visible; // an outfit piece's uncovered triangles (OutfitHideTag)
        glm::mat4 xform;
        AABB bounds;
        bool bounded, twoSided;
        std::uint32_t slot; // VisibleRenderable::Slot (the GPU scene)
    };
    struct LocalCaster {
        Model* model;
        const std::vector<std::shared_ptr<MaterialAsset>>* materials;
        const VisibleIndexBuffer* visible; // an outfit piece's uncovered triangles (OutfitHideTag)
        glm::mat4 xform;
        AABB bounds;
        std::uint32_t id;
        std::uint32_t slot;        // VisibleRenderable::Slot (the GPU scene)
        bool bounded, twoSided, animated;
        bool moving;               // moved within the last kMovingFrames (a rolling ball, a door)
        bool settling;             // stopped moving / animating within the last kSettleFrames (see DynamicFor)
        std::uint64_t settleStart; // the frame it stopped counting as moving / animated
        bool player;               // the player's own body and gun (world copies): no shadow in their own flashlight
        std::uint64_t hash;        // id, model, materials, transform and twoSided folded once (see HashCaster)
    };
    // Per caster, its transform and when it last changed; drives LocalCaster::moving. A skinned caster counts as
    // animated only while its pose changes (PoseVersion): a corpse at rest is a static caster like any prop.
    struct CasterMotion { glm::mat4 xform; std::uint64_t moved; std::uint64_t pose = 0, posed = 0, seen = 0; };

    void GatherLocalCasters(const ShadowFrameInputs& in);
    void RenderSpots(const ShadowFrameInputs& in);
    struct LocalPassLocs;
    bool RenderSpot(const ShadowFrameInputs& in, const LocalPassLocs& locs, int s, bool& lightMoved, bool& busy);
    void RenderPoints(const ShadowFrameInputs& in);
    bool DynamicFor(const LocalCaster& c, int turnSlot, std::uint64_t frame) const;
    // The collect + signature step RenderSpots and RenderPoints share: fills m_Visible / m_VisibleDynamic with the local
    // casters inside `frustum` (a spot) or `range` (a point; the other is null), skipping the player's own copies when
    // `skipPlayer`, and folds each static one's hash into `sig`. Returns whether any visible caster is dynamic.
    // `busy`: some visible caster is moving, animated or settling (the light can't turn quiet).
    bool CollectLocalCasters(const Frustum* frustum, const AABB* range, bool skipPlayer, int turnSlot, std::uint64_t frame,
                             std::uint64_t& sig, bool& busy);
    // A quiet light's verify check: it redrew something (`drawn`) though G4-c would have kept it.
    // Verify mode also names what changed: the static casters of this look against the light's last look.
    struct VerifyCaster;
    void VerifyQuiet(bool wasQuiet, bool drawn, const char* kind, int light, std::uint64_t frame, std::vector<VerifyCaster>& last);
    // The spot and point passes' shared steps. Begin: depth on, back faces culled (the light-facing surface), the local
    // depth program bound; returns its light uniforms. Draw: m_Visible's animated (or static) casters, those outside
    // `clip` (a point light's face) skipped. End: the state both leave behind.
    struct LocalPassLocs { int ViewProj = -1, LightPos = -1, Far = -1; };
    LocalPassLocs BeginLocalPass(Shader& shader);
    void DrawLocalCasters(Shader& shader, bool animatedOnes, const Frustum* clip);
    static void EndLocalPass();

    // Sun: RenderSun's steps, in order, over one SunPass (ShadowFrame.cpp).
    struct SunPass;
    void GatherSunCasters(SunPass& p, const glm::mat4& fitView, const glm::mat4& fitProj);
    void DrawSunCasters(SunPass& p);
    void DrawSunImpostors(SunPass& p);
    void DrawSunGrass(SunPass& p, const glm::mat4& fitView);
    struct SunFit { std::uint64_t frame = ~0ull; glm::mat4 view{0.0f}, proj{0.0f}; };
    SunFit m_LastSunFit;
    std::vector<SunCaster> m_SunCasters;
    std::vector<VisibleRenderable> m_Casting;      // the sun's gather: the kept static part, then this frame's Always part
    std::vector<VisibleRenderable> m_LocalCasting; // the local lights' gather (GatherLocalCasters)
    // The kept static part (see kSunGatherPadTexels): what it was gathered for and against.
    struct SunGatherBase {
        bool valid = false;
        const World* world = nullptr;
        int count = 0;
        glm::mat4 vp[CascadedShadowMap::kMaxCascades];      // the cascades as fitted then
        Frustum padded[CascadedShadowMap::kMaxCascades];    // ...grown by the pad: the static cells were gathered against these
        float pad = 0.0f;                                   // the pad in clip space (2 * texels / resolution)
        glm::vec3 sunDir{0.0f};
        std::uint64_t edits = 0, frame = 0;
        std::uint32_t epoch = 0;
        std::size_t staticCount = 0;                        // m_Casting's first staticCount entries
    };
    SunGatherBase m_SunGather;
    std::vector<VisibleRenderable> m_SunAlways, m_SunVerify; // scratch: this frame's Always part, verify's fresh gather
    std::vector<std::int32_t> m_SunVerifyIndex;              // verify: slot -> index in m_Casting, -1 none
    std::vector<AABB> m_SunMoves;                            // scratch: static candidates moved since the last frame
    std::vector<entt::entity> m_SunLodToggles;               // scratch: LOD levels flipped since the last frame
    bool SunGatherHolds(World& world, const FrameLights& fl, const CascadedShadowMap& map);
    void VerifySunGather(SunPass& p);
    std::vector<DepthCaster> m_Draws;
    // The layered sun pass's grouping + commands, kept while its casters hold: one per set of cascades drawn (the
    // render mask), as the far cascades take their turns on different frames.
    DepthCasterCache m_SunCasterCache[1u << CascadedShadowMap::kMaxCascades];
    World* m_SceneWorld = nullptr; // the world this frame's casters came from: DrawDepthCasters reads the GPU scene

    // Spot / point
    // G4-c: this frame's per-light reach and inputs key (RenderLocal), the quiet lights and the change log.
    Frustum m_SpotReach[SpotShadowMap::kMaxSpots] = {};
    std::uint64_t m_SpotKey[SpotShadowMap::kMaxSpots] = {};
    AABB m_PointRange[PointShadowMap::kMaxPoints] = {};
    std::uint64_t m_PointKey[PointShadowMap::kMaxPoints] = {};
    bool m_SlotDirty = false; // G4-c on this frame
    LocalShadowQuiet m_Quiet;
    LodToggleWake m_LodWake;
    GpuScene::ChangeLog m_ChangeLog;
    // Verify mode: each look's static casters (CollectLocalCasters), and each light's from its last look.
    struct VerifyCaster {
        std::uint32_t Id = 0;
        const Model* Mesh = nullptr;
        std::uint64_t Materials = 0;
        glm::mat4 Xform{1.0f};
        unsigned Visible = 0;
        bool TwoSided = false;
    };
    std::vector<VerifyCaster> m_VerifyNow;
    std::vector<VerifyCaster> m_VerifySpot[SpotShadowMap::kMaxSpots], m_VerifyPoint[PointShadowMap::kMaxPoints];
    int m_LookWhy = 0; // the last look's reasons to draw: 1 static changed, 2 animated now, 4 animated last frame
    std::vector<LocalCaster> m_LocalCasters;
    std::unordered_map<std::uint32_t, CasterMotion> m_CasterMotion;
    // G4-d: each sun caster as last gathered, by GPU-scene slot (stable while the world is): a caster that moved or
    // re-posed since redraws the held far cascades its shadow reaches, where it was and where it is.
    struct SunMotion { std::uint32_t id = ~0u; std::uint64_t pose = 0; glm::mat4 xform{1.0f}; glm::vec3 lo{0.0f}, hi{0.0f}; bool bounded = false; };
    std::vector<SunMotion> m_SunMotion;
    // The sun caster list's delta path (FillSunCasters): the set the list was built from, field by field, and what it
    // re-checks each frame.
    struct SunSetEntry {
        std::uint32_t slot = 0;
        std::uint32_t entity = 0;
        const void* renderable = nullptr; // component storage can move: a moved renderable is a new caster
        bool operator==(const SunSetEntry& o) const { return slot == o.slot && entity == o.entity && renderable == o.renderable; }
    };
    std::vector<SunSetEntry> m_SunSet;
    std::uint32_t m_SunSetEpoch = 0;             // RenderSettingsEpoch when the full loop last ran
    std::vector<std::uint32_t> m_SunHiddenSet;   // outfit-hidden entities (sorted) when the full loop last ran
    std::vector<std::int32_t> m_SunSlotIndex;    // GPU scene slot -> index in m_SunCasters, -1 none
    std::vector<std::uint32_t> m_SunAlwaysCheck; // indices re-checked every delta frame: skinned and outfit-hidden casters
    std::uint64_t m_SunListFrame = ~0ull;        // the frame the list was last brought up to date
    std::uint64_t m_SunFullFrame = ~0ull;        // ...and last fully refilled
    bool m_SunListValid = false;
    bool m_SunLastDelta = false;
    std::uint64_t m_ChangeLogFrame = ~0ull, m_ChangeLogPrevFrame = ~0ull; // when RenderLocal took m_ChangeLog, and the take before
    void FillSunCaster(World& world, const VisibleRenderable& v, SunCaster& sc);
    void TrackSunMotion(const SunListIn& in, const VisibleRenderable& v, const SunCaster& sc, unsigned& force);
    std::vector<const LocalCaster*> m_Visible;
    std::vector<std::uint8_t> m_VisibleDynamic; // per m_Visible entry: DynamicFor for the light being rendered
    std::uint64_t m_SpotShadowSig[SpotShadowMap::kMaxSpots] = {}; // the static casters'
    std::uint64_t m_SpotLiveSig[SpotShadowMap::kMaxSpots] = {};   // the static signature a moving light's live layer was drawn with
    bool m_SpotHadAnimated[SpotShadowMap::kMaxSpots] = {};
    glm::ivec4 m_SpotDynamicRect[SpotShadowMap::kMaxSpots] = {};  // the texels its animated casters covered when last drawn
    glm::mat4 m_SpotLastVP[SpotShadowMap::kMaxSpots] = {};
    std::uint64_t m_PointShadowSig[PointShadowMap::kMaxPoints] = {}; // the static casters'
    bool m_PointFaceHadDynamic[PointShadowMap::kMaxPoints][6] = {};  // moving casters were drawn into the live face last frame
};
