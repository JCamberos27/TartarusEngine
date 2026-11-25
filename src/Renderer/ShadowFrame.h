#pragma once
#include <cstdint>
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

// A local (spot / point) caster's change signature: its id, model, every material slot, transform, hidden-triangle set
// (VisibleIndexBuffer id, 0 = none) and Two Sided. A still light keeps its static depth while the signatures hold.
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
    std::vector<VisibleRenderable> m_Casting;
    std::vector<DepthCaster> m_Draws;
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
