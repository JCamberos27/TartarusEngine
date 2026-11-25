#include "SceneRenderer.h"
#include "SceneViewPass.h"
#include "Animation.h" // NormalMatrix
#include <cstdio>
#include <cstdlib>
#include <array>
#include <optional>
#include "RenderFrameContext.h"

#include "World.h"
#include "Components.h"
#include "Sky.h"
#include "SkyAtmosphere.h"
#include "VolumetricFog.h"
#include "PuddleSsr.h"
#include "Shader.h"
#include "Cubemap.h"
#include "CascadedShadowMap.h"
#include "SpotShadowMap.h"
#include "PointShadowMap.h"
#include "IblProbe.h"
#include "ReflectionProbeArray.h"
#include "GIVolumes.h"
#include "LightBuffer.h"
#include "ClusterGrid.h"
#include "DecalSystem.h"
#include "HdrTarget.h"
#include "OpaqueColorCopy.h"
#include "Ssao.h"
#include "SkinHideBuffer.h"
#include "Model.h"
#include "InstanceBatch.h"
#include "MaterialAsset.h"
#include "ShaderVariant.h"
#include "DefaultTextures.h"
#include "Frustum.h"
#include "ProjectSettings.h"
#include "Camera.h"    // MakePerspective — the view-model sub-pass's one projection switch
#include "ParticleRenderer.h"
#include "BloodRenderer.h"
#include "FurRenderer.h"
#include "GrassRenderer.h"
#include "ImpostorRenderer.h"
#include "FxSpriteRenderer.h"
#include "WorldText.h"
#include "GLStateCache.h"
#include "HiZCuller.h"
#include "gl.h"
#include "Core/Profiler.h"
#include "Core/Env.h"
#include "TimeService.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <execution>
#include <numeric>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <unordered_map>

// Extracted from main.cpp's `drawScene` lambda (audit #359, pass 2) and then split into
// GatherFrameState + ApplyFrameState (audit #354) so the common per-frame uniform/texture set
// can be pushed onto any program a material's ShaderAsset variant selects — not just the one
// fixed modelShader. No behaviour change: same GL calls, same uniform order, and with a single
// program the sequence is byte-identical to pass 2 apart from the cluster-cull dispatch now
// running just before the uniform set instead of in the middle of it.

const float* ShadowWeightsOrFull(const float* weights) {
    static const auto kFull = [] {
        std::array<float, std::max(SpotShadowMap::kMaxSpots, PointShadowMap::kMaxPoints)> a{};
        a.fill(1.0f);
        return a;
    }();
    return weights ? weights : kFull.data();
}

FrameState SceneRenderer::GatherFrameState(World& world, const RenderFrameContext& ctx,
                                           const SceneRenderInputs& in) const {
    FrameState fs;
    fs.ctx = &ctx;
    fs.in = &in;
    fs.world = &world;

    fs.shadowsOn    = in.shadowsEnabled && !ctx.Unlit;
    fs.sunShadowsOn = fs.shadowsOn && in.sunShadowsReady;
    fs.iblOn        = in.iblProbe->IsValid() && !ctx.Unlit;
    fs.ssaoOn       = in.ssaoEnabled && !in.postFxOff && ctx.SsaoSrc && ctx.SsaoSrc->IsValid() && !ctx.Unlit;
    fs.ssaoIntensity = in.ssaoIntensity;
    fs.spotCountForView  = fs.shadowsOn ? in.spotShadowCount  : 0;
    fs.pointCountForView = fs.shadowsOn ? in.pointShadowCount : 0;

    // The caller set the viewport before this call and nothing here changes it (audit PERF-102).
    glGetIntegerv(GL_VIEWPORT, fs.vp);

    // Perspective views only — the froxel AABB build assumes rays from the eye, so an
    // orthographic editor view (not perf-critical) falls back to the full light loop.
    const bool perspective = std::abs(ctx.Proj[3][3]) < 0.5f; // proj[3][3] == 1 for ortho
    fs.clusterOn = perspective && !ctx.Unlit && fs.vp[2] > 0 && fs.vp[3] > 0;
    if (fs.clusterOn) {
        fs.clusterNearZ = std::abs(ctx.Proj[3][2] / (ctx.Proj[2][2] - 1.0f));
        fs.clusterFarZ  = std::abs(ctx.Proj[3][2] / (ctx.Proj[2][2] + 1.0f));
    }
    fs.fogOn = !in.postFxOff;
    fs.volOn = fs.fogOn && !ctx.ProbeCapture && VolumetricFog::WantedFor(world, ctx, fs.vp);
    return fs;
}

SceneRenderer::SceneRenderer()
    : m_Volumetrics(std::make_unique<VolumetricFog>()), m_Ssr(std::make_unique<PuddleSsr>()),
      m_Scratch(std::make_unique<SceneViewScratch>()) {}
SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::WarmUpVolumetrics() { m_Volumetrics->WarmUp(); m_Ssr->WarmUp(); }

namespace {
// What GatherVisible needs of each renderable, read from the registry once per frame instead of once per view and
// per shadow light (each read is a sparse-set lookup, and with a few thousand props they were a fifth of the CPU
// frame). Good while the world's transform cache stamp and its renderables stay the same: tags, transforms and
// static bounds only change between frames. LodCulledTag (set per view) and posed bounds (an animated model) are
// still read live.
struct Candidate {
    entt::entity Entity;
    RenderableComponent* Renderable;
    const Model* ModelAtBuild; // a renderable whose model changes since gets its bounds read live
    std::uint16_t Flags;
    int Layer;
    std::uint32_t Cell;        // CandidateCache::Cells index
    VisibleRenderable V;       // transform, bounds and world bounds as of the build
};
// Candidates bucketed by where they stand (a square of the ground plan), so a camera's frustum rejects a whole bucket
// at once: a 150k-object forest is mostly behind or beside the camera. Anything animated or without bounds sits in
// the one Always cell (index 0 when present) and is tested on its own.
struct CandidateCell {
    glm::vec3 Min, Max;        // grows when a member moves (patched), never shrinks until the next full build
    std::uint32_t First, Count;
    bool Always;
};
struct CandidateCache {
    const World* Owner = nullptr;
    std::uint64_t Stamp = 0;
    size_t Renderables = 0;
    entt::entity First = entt::null, Last = entt::null;
    std::uint64_t Flags = 0;          // FlagSignature at the build
    std::vector<Candidate> List;      // grouped by cell
    // List's entity and flags, packed: most of a forest's candidates are rejected by these alone (an LOD level not in
    // use), and reading them from the ~200-byte Candidate cost more than the rest of the gather.
    struct HotCandidate { entt::entity Entity; std::uint16_t Flags; };
    std::vector<HotCandidate> Hot;
    std::vector<CandidateCell> Cells;
    std::vector<std::uint32_t> Index; // entity slot -> List index (kNoCandidate when none)
    std::vector<const Model*> PoolModels; // each renderable's model, in pool order: a swapped model rebuilds
    // Patched since the build (RefreshCandidates): the renderables that were no candidate (no model, or no transform),
    // the trailing Always cell new renderables join, and how many candidates left (kept, flagged inactive, Renderable
    // null) - too many of either and the next change rebuilds in full.
    std::vector<entt::entity> Uncounted;
    std::uint32_t PatchCell = ~0u;
    size_t Dead = 0;
    std::uint64_t FullBuilds = 0;
    bool Forget = false; // tests: the next call rebuilds in full
    // G-1 (GpuScene): the List indices patched since the last TakeSceneSlotChanges, each once (DirtyMark), and whether
    // the list was rebuilt in full since (every index then means another renderable). Kept only once someone asks.
    bool TrackSlots = false;
    bool SlotsRebuilt = true;
    std::vector<std::uint32_t> DirtySlots;
    std::vector<std::uint8_t> DirtyMark;
    void MarkSlot(std::uint32_t i) {
        if (!TrackSlots || SlotsRebuilt) return;
        if (DirtyMark.size() <= i) DirtyMark.resize((size_t)i + 1, 0);
        if (DirtyMark[i]) return;
        DirtyMark[i] = 1;
        DirtySlots.push_back(i);
    }
};
constexpr std::uint32_t kNoCandidate = ~0u;

// A fingerprint of everything a Candidate's Flags and Layer were read from: while it holds, a new frame only has to
// patch the candidates whose transforms moved (World::ChangedWorldTransforms) instead of re-reading every renderable.
template <class Tag>
std::uint64_t TagFingerprint(World& world, std::uint64_t salt) {
    const entt::sparse_set& st = world.Registry.storage<Tag>();
    std::uint64_t h = (std::uint64_t)st.size() * salt;
    const entt::entity* data = st.data();
    for (size_t i = 0, n = st.size(); i < n; ++i) {
        std::uint64_t v = ((std::uint64_t)entt::to_integral(data[i]) + salt) * 0x9E3779B97F4A7C15ull;
        v ^= v >> 29;
        h += v * 0xBF58476D1CE4E5B9ull;
    }
    return h;
}
std::uint64_t FlagSignature(World& world) {
    std::uint64_t h = TagFingerprint<InactiveTag>(world, 1) ^ TagFingerprint<PoseSourceTag>(world, 3) ^
                      TagFingerprint<OwnerViewOnlyTag>(world, 5) ^ TagFingerprint<HiddenFromOwnerTag>(world, 7) ^
                      TagFingerprint<ViewModelTag>(world, 11) ^ TagFingerprint<PlayerBodyTag>(world, 13) ^
                      TagFingerprint<HiddenInSceneTag>(world, 17) ^ TagFingerprint<LayerComponent>(world, 19);
    for (auto [e, lc] : world.Registry.view<LayerComponent>().each()) // a layer is edited in place
        h += ((std::uint64_t)entt::to_integral(e) * 31 + (std::uint64_t)(std::uint32_t)lc.Layer) * 0xD6E8FEB86659FD93ull;
    return h;
}

void FillBounds(VisibleRenderable& v, const RenderableComponent& r) {
    r.ModelRef->PosedBounds(v.LocalMin, v.LocalMax);
    v.Bounded = v.LocalMin.x <= v.LocalMax.x && v.LocalMin.y <= v.LocalMax.y && v.LocalMin.z <= v.LocalMax.z;
    if (v.Bounded) {
        const AABB wb = AABB{v.LocalMin, v.LocalMax}.Transformed(v.Xform);
        v.WorldMin = wb.Min;
        v.WorldMax = wb.Max;
    } else {
        v.WorldMin = v.WorldMax = glm::vec3(0.0f);
    }
}

CandidateCache& CandidateStore() {
    static CandidateCache cache;
    return cache;
}

// What a candidate's Flags are read from (read only: safe across the cores).
std::uint16_t CandidateFlagsOf(const entt::registry& reg, entt::entity entity, const RenderableComponent& r) {
    std::uint16_t f = 0;
    if (reg.all_of<InactiveTag>(entity)) f |= kCandInactive;
    if (reg.all_of<PoseSourceTag>(entity)) f |= kCandPoseSource;
    if (reg.all_of<OwnerViewOnlyTag>(entity)) f |= kCandOwnerViewOnly;
    if (reg.all_of<HiddenFromOwnerTag>(entity)) f |= kCandHiddenFromOwner;
    if (reg.all_of<ViewModelTag>(entity)) f |= kCandViewModel;
    if (reg.all_of<PlayerBodyTag>(entity)) f |= kCandPlayerBody;
    if (reg.all_of<HiddenInSceneTag>(entity)) f |= kCandHiddenInScene;
    if (r.ModelRef->HasBones()) f |= kCandPosed;
    return f;
}
int CandidateLayerOf(const entt::registry& reg, entt::entity entity) {
    const auto* lc = reg.try_get<LayerComponent>(entity);
    return lc ? lc->Layer : 0;
}
// A renderable with a model, as a candidate (its Cell is set by the caller).
Candidate MakeCandidate(World& world, entt::entity entity, RenderableComponent& r) {
    Candidate c{entity, &r, r.ModelRef.get(), CandidateFlagsOf(world.Registry, entity, r), 0, 0, {}};
    c.Layer = CandidateLayerOf(world.Registry, entity);
    c.V.Entity = entity;
    c.V.CandFlags = c.Flags;
    c.V.Layer = c.Layer;
    c.V.Renderable = &r;
    c.V.Xform = world.GetCachedWorldTransform(entity);
    c.V.Normal = NormalMatrix(c.V.Xform);
    FillBounds(c.V, r);
    return c;
}

// One rebuild on from the last build, the moved candidates: new transform and bounds, their cell grown to hold them.
void PatchMovedCandidates(World& world, CandidateCache& cache) {
    for (entt::entity e : world.ChangedWorldTransforms()) {
        const size_t slot = (size_t)entt::to_entity(e);
        if (slot >= cache.Index.size() || cache.Index[slot] == kNoCandidate) continue;
        Candidate& c = cache.List[cache.Index[slot]];
        if (c.Entity != e || !c.Renderable) continue;
        c.V.Xform = world.GetCachedWorldTransform(e);
        c.V.Normal = NormalMatrix(c.V.Xform);
        FillBounds(c.V, *c.Renderable);
        cache.MarkSlot(cache.Index[slot]);
        CandidateCell& cell = cache.Cells[c.Cell];
        if (!cell.Always) {
            if (!c.V.Bounded) {
                cell.Always = true; // lost its bounds: the cell is tested member by member now
            } else {
                cell.Min = glm::min(cell.Min, c.V.WorldMin);
                cell.Max = glm::max(cell.Max, c.V.WorldMax);
            }
        }
    }
}

// One rebuild on from the last build, with tags, models or the set of renderables changed (an outfit landing: pieces
// created and destroyed, hiding tags moved): every candidate re-reads its flags, layer and component (a destroy moves
// another renderer's component in the pool) across the cores; the ones that left are flagged out where they stand,
// the transform cache's new entities join the trailing Always cell, and the moved ones are patched as usual - instead
// of re-reading, re-bounding and re-sorting every renderable (~10 ms on the forest, on each such frame). Returns false
// when only the full build can tell: the renderable count isn't what that explains (a renderer added in place, or one
// that had no transform or model given one).
bool RefreshCandidates(World& world, CandidateCache& cache) {
    auto& reg = world.Registry;
    auto& renderPool = reg.storage<RenderableComponent>();
    const auto& transformPool = reg.storage<TransformComponent>();
    size_t counted = 0;
    {
        size_t keep = 0;
        for (entt::entity e : cache.Uncounted) {
            if (!reg.valid(e) || !renderPool.contains(e)) continue;
            if (transformPool.contains(e) && renderPool.get(e).ModelRef) return false; // a candidate now
            cache.Uncounted[keep++] = e;
        }
        cache.Uncounted.resize(keep);
        counted += keep;
    }
    const size_t n = cache.List.size();
    constexpr size_t kChunk = 8192;
    const size_t chunks = (n + kChunk - 1) / kChunk;
    static std::vector<size_t> chunkIds, chunkLive;
    static std::vector<std::vector<std::uint32_t>> chunkGone, chunkFix, chunkChanged;
    chunkIds.resize(chunks);
    std::iota(chunkIds.begin(), chunkIds.end(), size_t(0));
    chunkLive.assign(chunks, 0);
    if (chunkGone.size() < chunks) {
        chunkGone.resize(chunks);
        chunkFix.resize(chunks);
        chunkChanged.resize(chunks);
    }
    constexpr std::uint16_t kOwnCell = kCandPosed | kCandViewModel | kCandPlayerBody; // never in a static cell
    const auto scan = [&](size_t ch) {
        std::vector<std::uint32_t>& gone = chunkGone[ch];
        std::vector<std::uint32_t>& fix = chunkFix[ch];
        std::vector<std::uint32_t>& changed = chunkChanged[ch]; // flags, layer or model: GpuScene re-reads it
        gone.clear();
        fix.clear();
        changed.clear();
        size_t live = 0;
        for (size_t i = ch * kChunk, end = std::min(n, i + kChunk); i < end; ++i) {
            Candidate& c = cache.List[i];
            if (!c.Renderable) continue; // left at an earlier patch
            const entt::entity e = c.Entity;
            RenderableComponent* r = reg.valid(e) && renderPool.contains(e) && transformPool.contains(e) ? &renderPool.get(e) : nullptr;
            if (!r || !r->ModelRef) {
                gone.push_back((std::uint32_t)i);
                continue;
            }
            const std::uint16_t flags = CandidateFlagsOf(reg, e, *r);
            const bool modelChanged = r->ModelRef.get() != c.ModelAtBuild;
            const bool ownCell = (flags & kOwnCell) && !(c.Flags & kOwnCell);
            const int layer = CandidateLayerOf(reg, e);
            if (modelChanged || flags != c.Flags || layer != c.Layer) changed.push_back((std::uint32_t)i);
            c.Renderable = c.V.Renderable = r;
            c.Flags = c.V.CandFlags = cache.Hot[i].Flags = flags;
            c.Layer = c.V.Layer = layer;
            if (modelChanged) {
                c.ModelAtBuild = r->ModelRef.get();
                FillBounds(c.V, *r);
            }
            if (modelChanged || ownCell) fix.push_back((std::uint32_t)i);
            ++live;
        }
        chunkLive[ch] = live;
    };
    if (chunks > 1) std::for_each(std::execution::par, chunkIds.begin(), chunkIds.end(), scan);
    else if (chunks == 1) scan(0);
    for (size_t ch = 0; ch < chunks; ++ch) {
        counted += chunkLive[ch];
        for (std::uint32_t i : chunkGone[ch]) {
            Candidate& c = cache.List[i];
            const size_t slot = (size_t)entt::to_entity(c.Entity);
            if (slot < cache.Index.size() && cache.Index[slot] == i) cache.Index[slot] = kNoCandidate;
            if (reg.valid(c.Entity) && renderPool.contains(c.Entity)) { // kept its renderer, lost its model or transform
                cache.Uncounted.push_back(c.Entity);
                ++counted;
            }
            c.Renderable = c.V.Renderable = nullptr;
            c.Flags = c.V.CandFlags = cache.Hot[i].Flags = kCandInactive;
            ++cache.Dead;
            cache.MarkSlot(i);
        }
        for (std::uint32_t i : chunkChanged[ch]) cache.MarkSlot(i);
        for (std::uint32_t i : chunkFix[ch]) {
            const Candidate& c = cache.List[i];
            CandidateCell& cell = cache.Cells[c.Cell];
            if (cell.Always) continue;
            if (!c.V.Bounded || (c.Flags & kOwnCell)) {
                cell.Always = true;
            } else {
                cell.Min = glm::min(cell.Min, c.V.WorldMin);
                cell.Max = glm::max(cell.Max, c.V.WorldMax);
            }
        }
    }
    for (entt::entity e : world.TransformStructureChanges().Added) {
        if (!renderPool.contains(e) || !transformPool.contains(e)) continue;
        const size_t slot = (size_t)entt::to_entity(e);
        if (slot < cache.Index.size() && cache.Index[slot] != kNoCandidate && cache.List[cache.Index[slot]].Entity == e)
            continue; // a candidate already
        ++counted;
        RenderableComponent& r = renderPool.get(e);
        if (!r.ModelRef) {
            cache.Uncounted.push_back(e);
            continue;
        }
        Candidate c = MakeCandidate(world, e, r);
        if (cache.PatchCell == ~0u) {
            cache.Cells.push_back({c.V.WorldMin, c.V.WorldMax, (std::uint32_t)cache.List.size(), 0, true});
            cache.PatchCell = (std::uint32_t)cache.Cells.size() - 1;
        }
        c.Cell = cache.PatchCell;
        ++cache.Cells[cache.PatchCell].Count;
        if (slot >= cache.Index.size()) cache.Index.resize(slot + 1, kNoCandidate);
        cache.Index[slot] = (std::uint32_t)cache.List.size();
        cache.Hot.push_back({c.Entity, c.Flags});
        cache.List.push_back(c);
        cache.MarkSlot((std::uint32_t)cache.List.size() - 1);
    }
    if (counted != renderPool.size()) return false;
    PatchMovedCandidates(world, cache);
    return true;
}

const CandidateCache& Candidates(World& world) {
    CandidateCache& cache = CandidateStore();
    if (cache.Forget) {
        cache.Forget = false;
        cache.Owner = nullptr;
    }
    // Its entities (the storage's own iterators yield components): the first and last as a cheap check that the set
    // didn't change under an unchanged size.
    const entt::sparse_set& storage = world.Registry.storage<RenderableComponent>();
    const entt::entity first = storage.empty() ? entt::null : storage.data()[0];
    const entt::entity last = storage.empty() ? entt::null : storage.data()[storage.size() - 1];
    const bool sameSet = cache.Owner == &world && world.TransformCacheStamp() != 0 && cache.Renderables == storage.size() &&
                         cache.First == first && cache.Last == last;
    if (sameSet && cache.Stamp == world.TransformCacheStamp()) return cache;
    auto& reg = world.Registry;
    const std::uint64_t flags = FlagSignature(world);
    // A renderable given another model (Inspector, outfits) brings other bounds, which its cell may not hold. Read
    // straight from the component pool, across the cores: one pass over a forest's 150k renderables touches ~10 MB
    // and took half a millisecond a frame on one thread.
    const auto& pool = reg.storage<RenderableComponent>();
    auto sameModels = [&]() {
        const size_t n = pool.size();
        if (n != cache.PoolModels.size()) return false;
        constexpr size_t kChunk = 8192;
        const size_t chunks = (n + kChunk - 1) / kChunk;
        static std::vector<size_t> chunkIds;
        static std::vector<std::uint8_t> chunkSame;
        chunkIds.resize(chunks);
        chunkSame.assign(chunks, 1);
        for (size_t c = 0; c < chunks; ++c) chunkIds[c] = c;
        const auto first = pool.cbegin();
        const auto scan = [&](size_t c) {
            for (size_t k = c * kChunk, end = std::min(n, k + kChunk); k < end; ++k)
                if (cache.PoolModels[k] != first[(std::ptrdiff_t)k].ModelRef.get()) { chunkSame[c] = 0; return; }
        };
        if (chunks > 1) std::for_each(std::execution::par, chunkIds.begin(), chunkIds.end(), scan);
        else if (chunks == 1) scan(0);
        return std::all_of(chunkSame.begin(), chunkSame.end(), [](std::uint8_t v) { return v != 0; });
    };
    // One rebuild on from the last build, nothing entered or left the transform cache and no flag or model changed:
    // patch the moved ones (and everything posed reads its bounds live anyway).
    const bool afterOneRebuild = cache.Owner == &world && cache.Stamp != 0 && cache.Stamp == world.PreviousTransformCacheStamp();
    if (sameSet && afterOneRebuild && cache.Flags == flags && !world.TransformCacheMembershipChanged() && sameModels()) {
        PatchMovedCandidates(world, cache);
        cache.Stamp = world.TransformCacheStamp();
        return cache;
    }
    // Tags, models or the renderables changed (and the transform cache can say what came and went): patch those too,
    // until what was patched in stops paying for itself.
    const size_t patchedIn = cache.Dead + (cache.PatchCell != ~0u ? cache.Cells[cache.PatchCell].Count : 0);
    if (afterOneRebuild && (!world.TransformCacheMembershipChanged() || world.TransformStructureChanges().Valid) &&
        patchedIn <= std::max<size_t>(1024, cache.List.size() / 8) && RefreshCandidates(world, cache)) {
        cache.Stamp = world.TransformCacheStamp();
        cache.Renderables = storage.size();
        cache.First = first;
        cache.Last = last;
        cache.Flags = flags;
        cache.PoolModels.clear();
        for (auto it = pool.cbegin(), end = pool.cend(); it != end; ++it) cache.PoolModels.push_back(it->ModelRef.get());
        return cache;
    }
    ++cache.FullBuilds;
    cache.SlotsRebuilt = true; // every slot is another renderable now: GpuScene re-reads them all
    cache.DirtySlots.clear();
    cache.DirtyMark.clear();
    cache.Owner = &world;
    cache.Stamp = world.TransformCacheStamp();
    cache.Renderables = storage.size();
    cache.First = first;
    cache.Last = last;
    cache.Flags = flags;
    cache.List.clear();
    cache.Hot.clear();
    cache.Cells.clear();
    cache.Uncounted.clear();
    cache.PatchCell = ~0u;
    cache.Dead = 0;
    std::fill(cache.Index.begin(), cache.Index.end(), kNoCandidate);
    cache.PoolModels.clear();
    for (auto it = pool.cbegin(), end = pool.cend(); it != end; ++it) cache.PoolModels.push_back(it->ModelRef.get());
    static std::vector<Candidate> built;
    built.clear();
    for (entt::entity entity : storage) {
        auto& r = reg.get<RenderableComponent>(entity);
        // Its model failed to load (or isn't assigned yet), or it has no transform: nothing to draw.
        if (!r.ModelRef || !reg.all_of<TransformComponent>(entity)) {
            cache.Uncounted.push_back(entity);
            continue;
        }
        built.push_back(MakeCandidate(world, entity, r));
    }
    // Bucket by ground-plan square: key = cell (0 = Always) above the candidate's index, sorted.
    constexpr float kCellSize = 32.0f;
    static std::vector<std::uint64_t> keys;
    keys.resize(built.size());
    for (size_t i = 0; i < built.size(); ++i) {
        const Candidate& c = built[i];
        std::uint64_t cell = 0;
        if (c.V.Bounded && !(c.Flags & (kCandPosed | kCandViewModel | kCandPlayerBody))) {
            const glm::vec3 centre = (c.V.WorldMin + c.V.WorldMax) * 0.5f;
            auto axis = [&](float v) {
                const float k = std::floor(v / kCellSize) + 524288.0f; // 2^19: the world centred in 20 bits
                return (std::uint64_t)std::clamp(k, 1.0f, 1048575.0f);
            };
            cell = (axis(centre.x) << 20) | axis(centre.z);
        }
        keys[i] = (cell << 24) | (std::uint64_t)i; // 2^24 candidates at most
    }
    std::sort(keys.begin(), keys.end());
    cache.List.reserve(built.size());
    std::uint64_t cellNow = ~0ull;
    for (const std::uint64_t key : keys) {
        Candidate c = built[(size_t)(key & 0xFFFFFFull)];
        const std::uint64_t cell = key >> 24;
        if (cell != cellNow) {
            cache.Cells.push_back({c.V.WorldMin, c.V.WorldMax, (std::uint32_t)cache.List.size(), 0, cell == 0});
            cellNow = cell;
        }
        CandidateCell& at = cache.Cells.back();
        at.Min = glm::min(at.Min, c.V.WorldMin);
        at.Max = glm::max(at.Max, c.V.WorldMax);
        ++at.Count;
        c.Cell = (std::uint32_t)cache.Cells.size() - 1;
        const size_t slot = (size_t)entt::to_entity(c.Entity);
        if (slot >= cache.Index.size()) cache.Index.resize(slot + 1, kNoCandidate);
        cache.Index[slot] = (std::uint32_t)cache.List.size();
        cache.Hot.push_back({c.Entity, c.Flags});
        cache.List.push_back(c);
    }
    return cache;
}
} // namespace

void ForgetGatherCandidates() { CandidateStore().Forget = true; }

void TakeSceneSlotChanges(World& world, SceneSlotChanges& out) {
    CandidateCache& cache = CandidateStore();
    const bool first = !cache.TrackSlots;
    cache.TrackSlots = true;
    const CandidateCache& now = Candidates(world);
    out.Count = (std::uint32_t)now.List.size();
    out.Slots.clear();
    // Rebuilt, first asked, or patched past half the list (cheaper to send it all): every slot.
    out.Full = first || cache.SlotsRebuilt || cache.DirtySlots.size() * 2 > now.List.size();
    if (!out.Full) {
        out.Slots = cache.DirtySlots;
        // Posed models read their bounds live (GatherVisible): sent every call. They're the Always cells' few.
        for (const CandidateCell& cell : now.Cells) {
            if (!cell.Always) continue;
            for (std::uint32_t i = cell.First, end = cell.First + cell.Count; i < end; ++i)
                if ((now.List[i].Flags & kCandPosed) && (i >= cache.DirtyMark.size() || !cache.DirtyMark[i])) out.Slots.push_back(i);
        }
    }
    for (std::uint32_t i : cache.DirtySlots) cache.DirtyMark[i] = 0;
    cache.DirtySlots.clear();
    cache.SlotsRebuilt = false;
}

bool ReadSceneSlot(std::uint32_t slot, SceneSlot& out) {
    const CandidateCache& cache = CandidateStore();
    if (slot >= cache.List.size()) return false;
    const Candidate& c = cache.List[slot];
    out.Alive = c.Renderable && c.Renderable->ModelRef && !(c.Flags & kCandInactive);
    out.Flags = c.Flags;
    out.Layer = c.Layer;
    if (c.Renderable) { // read live, as GatherVisible's consider() does
        const RenderableComponent& r = *c.Renderable;
        if (r.CastShadows == RenderableComponent::ShadowCasting::Off) out.Flags |= kSlotFlagCastOff;
        if (r.CastShadows == RenderableComponent::ShadowCasting::ShadowsOnly) out.Flags |= kSlotFlagShadowsOnly;
        if (!r.Materials.empty() && r.Materials[0] && r.Materials[0]->RenderQueue == MaterialAsset::Queue::Transparent)
            out.Flags |= kSlotFlagTransparent;
    }
    VisibleRenderable v = c.V;
    if (c.Renderable && c.Renderable->ModelRef && ((c.Flags & kCandPosed) || c.Renderable->ModelRef.get() != c.ModelAtBuild))
        FillBounds(v, *c.Renderable); // as GatherVisible reads it
    out.Xform = v.Xform;
    out.Normal = v.Normal;
    out.Bounded = v.Bounded;
    out.WorldMin = v.WorldMin;
    out.WorldMax = v.WorldMax;
    return true;
}
std::uint64_t GatherCandidateFullBuilds() { return CandidateStore().FullBuilds; }

bool RenderableBounds(World& world, glm::vec3& outMin, glm::vec3& outMax) {
    const CandidateCache& cache = Candidates(world);
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (const CandidateCell& cell : cache.Cells) {
        if (!cell.Always) {
            lo = glm::min(lo, cell.Min);
            hi = glm::max(hi, cell.Max);
            continue;
        }
        for (std::uint32_t i = cell.First, end = cell.First + cell.Count; i < end; ++i) { // as they stand now
            VisibleRenderable v = cache.List[i].V;
            if (!cache.List[i].Renderable || !cache.List[i].Renderable->ModelRef) continue; // left since the build
            FillBounds(v, *cache.List[i].Renderable);
            if (!v.Bounded) continue;
            lo = glm::min(lo, v.WorldMin);
            hi = glm::max(hi, v.WorldMax);
        }
    }
    outMin = lo;
    outMax = hi;
    return lo.x <= hi.x;
}

bool TooSmallOnScreen(const glm::vec3& worldMin, const glm::vec3& worldMax, const glm::vec3& eye, float projScaleY,
                      float minScreenHeight) {
    if (minScreenHeight <= 0.0f || projScaleY <= 0.0f) return false;
    const glm::vec3 centre = (worldMin + worldMax) * 0.5f;
    const float radius = glm::length(worldMax - worldMin) * 0.5f;
    const float dist = glm::length(centre - eye);
    if (dist <= radius) return false; // the eye is inside or touching it
    // The bounding sphere's height as a share of the viewport height (projScaleY = proj[1][1] = 1 / tan(fovY / 2)).
    return radius * projScaleY / dist < minScreenHeight;
}

void GatherVisible(World& world, const VisibilityQuery& q, std::vector<VisibleRenderable>& out) {
    PROFILE_SCOPE("Gather Visible");
    out.clear();
    const bool anyLod = !world.Registry.storage<LodCulledTag>().empty();
    const CandidateCache& cache = Candidates(world);
    // One candidate: appended to `to` when the query takes it. Read only (the cache, the registry's components, the LOD
    // table), so the static cells below run across the cores.
    const auto consider = [&](std::uint32_t i, std::vector<VisibleRenderable>& to) {
        // GameObject active / LOD level (#163); PoseSourceTag is animated only (the first-person arms rig).
        const CandidateCache::HotCandidate& hot = cache.Hot[i];
        if (hot.Flags & (kCandInactive | kCandPoseSource)) return;
        if (anyLod && world.IsLodCulled(hot.Entity)) return;
        const Candidate& c = cache.List[i];
        // Split poses: the first-person copy only in the player's own camera, the world copy everywhere else (and
        // the world copy is the one that casts).
        if (c.Flags & ((q.ShadowCasters || !q.OwnerView) ? kCandOwnerViewOnly : kCandHiddenFromOwner)) return;
        if (q.SkipViewModels && (c.Flags & kCandViewModel)) return;
        if (q.SkipPlayerBody && (c.Flags & kCandPlayerBody)) return;
        // The editor Scene view: hidden by the entity's own toggle (HiddenInSceneTag, #236 B) or by its layer (#236 A1).
        if (q.EditorView && ((c.Flags & kCandHiddenInScene) ||
                             (c.Layer >= 0 && c.Layer < 32 && !((q.LayerVisibleMask >> c.Layer) & 1u)))) return;
        const RenderableComponent& r = *c.Renderable;
        if (!r.ModelRef) return;
        // #163 - Cast Shadows: Off never casts; Shadows Only is drawn by the shadow passes, never in a camera view.
        if (q.ShadowCasters ? r.CastShadows == RenderableComponent::ShadowCasting::Off
                            : r.CastShadows == RenderableComponent::ShadowCasting::ShadowsOnly) return;
        if (q.SkipTransparent && !r.Materials.empty() && r.Materials[0] &&
            r.Materials[0]->RenderQueue == MaterialAsset::Queue::Transparent) return;
        // An animated model's bounds move with its pose, and a swapped model brings its own: read those live.
        const bool live = (c.Flags & kCandPosed) || r.ModelRef.get() != c.ModelAtBuild;
        VisibleRenderable liveV;
        if (live) { liveV = c.V; FillBounds(liveV, r); }
        const VisibleRenderable& v = live ? liveV : c.V; // tested in place; copied out only when it passes
        if (v.Bounded) {
            if (q.Cull && !q.Cull->Intersects(AABB{v.WorldMin, v.WorldMax})) return;
            if (q.Within && !q.Within->Intersects(AABB{v.WorldMin, v.WorldMax})) return;
            if (q.SizeCullEye && TooSmallOnScreen(v.WorldMin, v.WorldMax, *q.SizeCullEye, q.SizeCullProjScale,
                                                  q.MinScreenHeight)) return;
        }
        to.push_back(v);
    };
    // The cells the query can see. The Always cell (animated, unbounded: posed bounds are resolved on the model as they
    // are read) stays on this thread; the static cells of a big scene go out in chunks of about kChunk candidates, each
    // into its own list, joined in cell order (what one pass over the cells takes).
    static std::vector<std::uint32_t> cells;
    cells.clear();
    std::size_t staticCandidates = 0;
    for (std::uint32_t ci = 0; ci < (std::uint32_t)cache.Cells.size(); ++ci) {
        const CandidateCell& cell = cache.Cells[ci];
        if (cell.Always) {
            for (std::uint32_t i = cell.First, end = cell.First + cell.Count; i < end; ++i) consider(i, out);
            continue;
        }
        const AABB box{cell.Min, cell.Max}; // the whole square is out of view
        if (q.Cull && !q.Cull->Intersects(box)) continue;
        if (q.CellCullCount > 0) {
            bool any = false;
            for (int f = 0; f < q.CellCullCount && !any; ++f) any = q.CellCull[f].Intersects(box);
            if (!any) continue;
        }
        cells.push_back(ci);
        staticCandidates += cell.Count;
    }
    constexpr std::size_t kChunk = 4096;
    if (staticCandidates < 4 * kChunk) { // a few thousand: not worth the hand-off
        for (std::uint32_t ci : cells) {
            const CandidateCell& cell = cache.Cells[ci];
            for (std::uint32_t i = cell.First, end = cell.First + cell.Count; i < end; ++i) consider(i, out);
        }
        return;
    }
    struct Chunk { std::size_t FirstCell, EndCell; };
    static std::vector<Chunk> chunks;
    static std::vector<std::vector<VisibleRenderable>> parts;
    chunks.clear();
    for (std::size_t k = 0, n = 0, first = 0; k < cells.size(); ++k) {
        n += cache.Cells[cells[k]].Count;
        if (n >= kChunk || k + 1 == cells.size()) {
            chunks.push_back({first, k + 1});
            first = k + 1;
            n = 0;
        }
    }
    if (parts.size() < chunks.size()) parts.resize(chunks.size());
    std::for_each(std::execution::par, chunks.begin(), chunks.end(), [&](const Chunk& ch) {
        std::vector<VisibleRenderable>& to = parts[(std::size_t)(&ch - chunks.data())];
        to.clear();
        for (std::size_t k = ch.FirstCell; k < ch.EndCell; ++k) {
            const CandidateCell& cell = cache.Cells[cells[k]];
            for (std::uint32_t i = cell.First, end = cell.First + cell.Count; i < end; ++i) consider(i, to);
        }
    });
    std::size_t total = out.size();
    for (std::size_t k = 0; k < chunks.size(); ++k) total += parts[k].size();
    out.reserve(total);
    for (std::size_t k = 0; k < chunks.size(); ++k) out.insert(out.end(), parts[k].begin(), parts[k].end());
}

void SceneRenderer::ApplyFrameState(Shader& program, const FrameState& fs) const {
    const RenderFrameContext& ctx = *fs.ctx;
    const SceneRenderInputs&  in  = *fs.in;
    CascadedShadowMap&    shadowMap     = *in.sunShadow;
    SpotShadowMap&        spotShadowMap = *in.spotShadow;
    PointShadowMap&       pointShadowMap = *in.pointShadow;
    ReflectionProbeArray& probeArray    = *in.probeArray;

    program.Bind();
    program.SetMat4("uView", ctx.View);
    program.SetMat4("uProj", ctx.Proj);
    program.SetVec3("uViewPos", ctx.ViewPos);
    program.SetInt("uDebugView", ctx.DebugView); // #236 R2 scene-view debug modes
    // TAA motion vectors (TemporalAA.h) and rain wetness (ModelFragment.glsl ApplyWetness).
    program.SetInt("uTaaVelocity", ctx.TaaVelocity ? 1 : 0);
    if (ctx.TaaVelocity) {
        program.SetMat4("uPrevViewProj", ctx.TaaPrevViewProj);
        program.SetVec2("uTaaJitter", ctx.TaaJitterNdc);
    }
    program.SetFloat("uWetness", fs.world ? fs.world->Render.Wetness : 0.0f);
    program.SetFloat("uPuddleAmount", fs.world ? fs.world->Render.PuddleAmount : 0.0f);
    program.SetFloat("uPuddleScale", fs.world ? fs.world->Render.PuddleScale : 4.0f);

    // Cascaded-shadow uniforms + the depth array on unit 8 (material maps use 1..7).
    program.SetInt("uShadowEnabled", fs.sunShadowsOn ? 1 : 0);
    program.SetInt("uShadowCascadeCount", shadowMap.Count());
    {
        glm::mat4 mats[CascadedShadowMap::kMaxCascades];
        for (int i = 0; i < shadowMap.Count(); ++i) mats[i] = shadowMap.SampleViewProj(i);
        program.SetMat4Array("uShadowMatrices[0]", shadowMap.Count(), mats);
    }
    program.SetVec4("uCascadeSplits", shadowMap.SplitDepthsVec4());
    // World units per shadow texel, per cascade — the shader scales its normal offset + depth
    // bias by the selected cascade's value so one number isn't simultaneously too much for
    // cascade 0 and too little for cascade 3 (#117).
    program.SetVec4("uShadowTexelWorld", shadowMap.SampleTexelWorldSizesVec4());
    // Penumbra width in shadow-map texels, from the sun's apparent size. 0.53 deg (real sun) ->
    // a tight ~2 texel edge; crank the light's Angular Size for softer. Softness 0 sends 0 = a
    // hard single-tap edge.
    program.SetFloat("uShadowSoftness", CascadedShadowMap::SunKernelTexels(in.sunAngularDeg, in.sunShadowSoftness));
    // Per-light sun shadow multipliers (#140 phase 2); 1.0 == pre-phase-2 output.
    program.SetFloat("uSunShadowBias", in.sunShadowBias);
    program.SetFloat("uSunShadowNormalBias", in.sunShadowNormalBias);
    program.SetInt("uShadowMap", 8);
    program.SetFloat("uShadowMapResolution", (float)shadowMap.Resolution()); // #190

    // Spot-light shadow maps on unit 9 (#119). Count is zeroed when shadows are off or in Unlit
    // so the shader's SpotShadow() early-outs.
    program.SetInt("uSpotShadowCount", fs.spotCountForView);
    if (fs.spotCountForView > 0) {
        // Whole-array uploads, one glUniform*fv each (audit PERF-102). The arrays alias main.cpp's
        // kMaxSpots-sized stack storage; only the first spotCountForView entries are sent.
        program.SetMat4Array ("uSpotShadowVP[0]",         fs.spotCountForView, in.spotShadowVP);
        program.SetVec3Array ("uSpotShadowPos[0]",        fs.spotCountForView, in.spotShadowPos);
        program.SetFloatArray("uSpotShadowFar[0]",        fs.spotCountForView, in.spotShadowFar);
        program.SetFloatArray("uSpotShadowHalfTan[0]",    fs.spotCountForView, in.spotShadowHalfTan);
        program.SetFloatArray("uSpotShadowBias[0]",       fs.spotCountForView, in.spotShadowBias);
        program.SetFloatArray("uSpotShadowNormalBias[0]", fs.spotCountForView, in.spotShadowNormalBias);
        // fix-3 spot PCF: the radius in spot-map texels per slot (SpotShadowMap::KernelTexels), 0 = one hard compare.
        std::array<float, SpotShadowMap::kMaxSpots> spotKernel{};
        for (int i = 0; i < fs.spotCountForView && i < SpotShadowMap::kMaxSpots; ++i)
            spotKernel[i] = in.spotShadowSoftness ? SpotShadowMap::KernelTexels(in.spotShadowSoftness[i]) : 0.0f;
        program.SetFloatArray("uSpotShadowSoftness[0]",   fs.spotCountForView, spotKernel.data());
        program.SetFloatArray("uSpotShadowWeight[0]",     fs.spotCountForView, ShadowWeightsOrFull(in.spotShadowWeight));
    }
    program.SetInt("uSpotShadowMap", 9);
    program.SetFloat("uSpotShadowMapResolution", (float)spotShadowMap.Resolution()); // #190

    // Point-light cube shadow maps on unit 10 (#119).
    program.SetInt("uPointShadowCount", fs.pointCountForView);
    if (fs.pointCountForView > 0) {
        program.SetFloatArray("uPointShadowFar[0]",        fs.pointCountForView, in.pointShadowFar);
        program.SetFloatArray("uPointShadowBias[0]",       fs.pointCountForView, in.pointShadowBias);
        program.SetFloatArray("uPointShadowNormalBias[0]", fs.pointCountForView, in.pointShadowNormalBias);
        // S2: the PCF radius in cube texels per slot (PointShadowMap::KernelTexels), 0 = the hard single compare.
        // TARTARUS_EXP_POINT_PCF_OFF: every slot hard (the tent PCF's cost, A/B only).
        static const bool pcfOff = Env::Flag(Env::Var::ExpPointPcfOff);
        std::array<float, PointShadowMap::kMaxPoints> kernel{};
        for (int i = 0; i < fs.pointCountForView && i < PointShadowMap::kMaxPoints; ++i)
            kernel[i] = in.pointShadowSoftness && !pcfOff ? PointShadowMap::KernelTexels(in.pointShadowSoftness[i]) : 0.0f;
        program.SetFloatArray("uPointShadowSoftness[0]",   fs.pointCountForView, kernel.data());
        program.SetFloatArray("uPointShadowWeight[0]",     fs.pointCountForView, ShadowWeightsOrFull(in.pointShadowWeight));
    }
    program.SetInt("uPointShadowMap", 10);
    program.SetFloat("uPointShadowMapResolution", (float)pointShadowMap.Resolution()); // #190

    // IBL probes on units 11/12/13 (#196): the shader declares those units with layout(binding=)
    // qualifiers, so there's no SetInt. Unlit mode skips lighting entirely.
    if (fs.iblOn) {
        program.SetFloat("uIBLIntensity", ctx.GIBake ? 0.0f : std::max(fs.world->Render.SkyAmbientIntensity, 0.0f));
        program.SetFloat("uIBLSpecularMaxLod", (float)(IblProbe::kSpecularMips - 1));
    }
    program.SetInt("uIBLEnabled", fs.iblOn ? 1 : 0);
    program.SetInt("uGIBake", ctx.GIBake ? 1 : 0);
    program.SetFloat("uGIBakeEmissive", ctx.GIBakeEmissive);

    // Physical sky: aerial perspective (unit 30) and cloud shadows (unit 31). Neutral textures
    // and both switched off for the other sky modes.
    if (in.skyAtmosphere && !ctx.Unlit)
        in.skyAtmosphere->SetProgramUniforms(program, ctx.TxHdr, fs.vp);
    else
        SkyAtmosphere::SetDisabledUniforms(program);
    // Froxel volumetric fog (unit 27): this view's volume, built in RenderScene, or off.
    m_Volumetrics->SetProgramUniforms(program, ctx.TxHdr, fs.volOn);

    // #162 - fog.
    program.SetInt("uFogMode", fs.fogOn && fs.world && fs.world->Render.FogEnabled ? std::clamp(fs.world->Render.FogMode, 1, 3) : 0);
    if (fs.fogOn && fs.world && fs.world->Render.FogEnabled) {
        const World& w = *fs.world;
        {
            program.SetVec3("uFogColor", glm::max(w.Render.FogColor, glm::vec3(0.0f)));
            program.SetFloat("uFogDensity", std::max(w.Render.FogDensity, 0.0f));
            program.SetFloat("uFogStart", w.Render.FogStart);
            program.SetFloat("uFogEnd", w.Render.FogEnd);
            program.SetFloat("uFogHeightFalloff", std::max(w.Render.FogHeightFalloff, 0.0f));
            program.SetFloat("uFogBaseHeight", w.Render.FogBaseHeight);
        }
    }
    program.SetInt("uSsrEnabled", fs.ssrTex ? 1 : 0);

    // PR14: nearest 2 reflection probes for parallax box projection — no-op (uProbeCount=0 or the
    // uniforms absent) when the shader lacks the _REFLECTION_PROBES keyword / the scene has none.
    probeArray.Bind(program, ctx.ViewPos);
    // Baked GI (GIVolumes): atlas on unit 28, volumes on UBO binding 4. Unlit draws no lighting.
    if (in.giVolumes && !ctx.Unlit) in.giVolumes->SetProgramUniforms(program);
    else program.SetInt("uGIEnabled", 0);

    // PR15: SSAO occlusion map (unit 15), pre-computed before this pass. ctx.SsaoSrc is the
    // caller's own per-viewport Ssao instance; IsValid() is false until its first filled frame.
    if (fs.ssaoOn) {
        program.SetInt("uSSAOMap", 15);
        program.SetVec2("uScreenSize", glm::vec2((float)fs.vp[2], (float)fs.vp[3]));
    }
    program.SetInt("uSSAOEnabled", fs.ssaoOn ? 1 : 0);
    program.SetFloat("uSSAOIntensity", fs.ssaoIntensity);

    // Clustered-forward light lists (#120). The cull dispatch itself runs once in RenderScene;
    // here every program takes the froxel params (BindFrameResources binds the lists).
    if (fs.clusterOn) {
        program.SetVec2("uClusterScreenSize", glm::vec2((float)fs.vp[2], (float)fs.vp[3]));
        program.SetVec4("uClusterZParams", ClusterGrid::ZParams(fs.clusterNearZ, fs.clusterFarZ));
    }
    program.SetInt("uClusterEnabled", fs.clusterOn ? 1 : 0);

    // Clustered decals (their list and maps: BindFrameResources).
    program.SetInt("uDecalCount", fs.decalCount);
    // Substrates (the table on SSBO 16, maps in the same arrays): the Grunge reveal and peeling-paint
    // decals read them, in the views that prepared decals this frame.
    program.SetInt("uSubstrateOn", in.decals && in.decals->SubstrateCount() > 0 && fs.clusterOn ? 1 : 0);

    program.SetInt("uUnlit", ctx.Unlit ? 1 : 0);
    // Real scene path: emit linear HDR; the shared Tonemapper pass maps it after MSAA resolve.
    // (Offscreen model thumbnails set this to 1 to self-tonemap.)
    program.SetInt("uApplyTonemap", 0);

    BindFrameResources(fs);
}

void SceneRenderer::BindFrameResources(const FrameState& fs) const {
    const RenderFrameContext& ctx = *fs.ctx;
    const SceneRenderInputs&  in  = *fs.in;

    // The shadow maps on units 8 (sun cascades), 9 (spots) and 10 (points); material maps use 1..7. A valid
    // depth-compare texture even when a map isn't allocated, so a shadow sampler is never backed by texture 0
    // (audit #357); the shader early-outs on the zero counts.
    const unsigned int sunTex = in.sunShadow->DepthArray();
    glBindTextureUnit(8, sunTex ? sunTex : DefaultTextures::DepthArray());
    const unsigned int spotTex = in.spotShadow->DepthArray();
    glBindTextureUnit(9, spotTex ? spotTex : DefaultTextures::DepthArray());
    const unsigned int pointTex = in.pointShadow->DepthCubeArray();
    glBindTextureUnit(10, pointTex ? pointTex : DefaultTextures::DepthCubeArray());
    if (fs.iblOn) {
        glBindTextureUnit(11, in.iblProbe->IrradianceMap());
        glBindTextureUnit(12, in.iblProbe->SpecularMap());
        glBindTextureUnit(13, in.iblProbe->BrdfLut());
    }
    // PR15: SSAO occlusion map (unit 15), pre-computed before this pass. With SSAO off, white (no occlusion), so the
    // sampler is never backed by texture 0 either.
    glBindTextureUnit(15, fs.ssaoOn ? ctx.SsaoSrc->OcclusionTexture() : DefaultTextures::White());

    // Captured room probes (ReflectionProbeCapture) on unit 26, the puddle SSR (PuddleSsr) on unit 29: units 1-7 and
    // 16-20 are material maps (ShaderAsset::BuildBindings) - a 2D map and the cube array sharing a unit made draws
    // fail, and the SSR read a material's white default. Never texture 0 either.
    const unsigned int probeTex = in.probeArray->CubeArray();
    glBindTextureUnit(26, probeTex ? probeTex : DefaultTextures::BlackCubeArray());
    glBindTextureUnit(29, fs.ssrTex ? fs.ssrTex : DefaultTextures::Black());
    m_Volumetrics->BindTexture(ctx.TxHdr, fs.volOn); // unit 27
    if (in.giVolumes && !ctx.Unlit) in.giVolumes->BindResources(); // unit 28 + UBO binding 4
    else glBindTextureUnit(28, DefaultTextures::Volume());
    if (in.skyAtmosphere && !ctx.Unlit) in.skyAtmosphere->BindTextures(ctx.TxHdr); // units 30, 31
    else SkyAtmosphere::BindDisabledTextures();

    // The light SSBO (binding 0) is built once per frame by main.cpp: just (re)bind it. Then the cluster lists, and
    // the clustered decals: their list (SSBO 11) and texture arrays on units 22-25 (the blood stains and bullet holes
    // among them, and the blood splats' maps). The FX sprites bind their own arrays over 22-24 after the opaque and
    // transparent passes; every program that draws after them comes through here.
    in.lightBuffer->Bind(0);
    if (fs.clusterOn) in.clusterGrid->BindForShading();
    if (in.decals) in.decals->BindForShading();
    glActiveTexture(GL_TEXTURE0);
}

void SceneRenderer::WarmEffects(const World& world) {
    SceneParticles()->WarmUp(world);
    FxSpriteRenderer::Get().WarmUp();
    BloodRenderer::Get().WarmUp();
    WorldTextRenderer::Get().WarmUp();
    GrassRenderer::Get().WarmUp();
    ImpostorRenderer::Get().WarmUp();
}

namespace {
using WarmedVariants = std::unordered_map<ShaderAsset*, std::vector<ShaderVariantKey>>;
int WarmMaterials(const RenderableComponent& rc, WarmedVariants& done) {
    int built = 0;
    for (const auto& ma : rc.Materials) {
        if (!ma || !ma->Shader) continue;
        // The scene draw adds the engine bits (ShaderVariant.h): a game view (no debug mode) with and without point or
        // spot lights and decals - a flashlight or the first bullet hole must not compile a program mid-fight.
        const bool engine = EngineVariantsEnabled();
        const ShaderVariantKey own = ShaderVariantKeyFor(ma->Mat, *ma->Shader) |
                                     (engine ? ShaderVariantEngineBits(ma->Mat) | kVariantNoDebugView : 0u);
        std::vector<ShaderVariantKey>& keys = done[ma->Shader.get()];
        if (std::find(keys.begin(), keys.end(), own) != keys.end()) continue;
        keys.push_back(own);
        if (!engine) {
            ma->Shader->Variant(own);
            ++built;
            continue;
        }
        for (const ShaderVariantKey frame : {0u, kVariantNoLocalLights, kVariantNoDecals, kVariantNoLocalLights | kVariantNoDecals}) {
            ma->Shader->Variant(own | frame);
            ++built;
        }
    }
    return built;
}
} // namespace

int SceneRenderer::WarmShaderVariants(World& world) {
    WarmedVariants done;
    int built = 0;
    for (auto entity : world.Registry.view<RenderableComponent>())
        built += WarmMaterials(world.Registry.get<RenderableComponent>(entity), done);
    return built;
}

int SceneRenderer::WarmShaderVariants(World& world, entt::entity root) {
    WarmedVariants done;
    int built = 0;
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        if (!world.Registry.valid(e)) continue;
        if (const auto* rc = world.Registry.try_get<RenderableComponent>(e)) built += WarmMaterials(*rc, done);
        if (const auto* h = world.Registry.try_get<HierarchyComponent>(e))
            stack.insert(stack.end(), h->Children.begin(), h->Children.end());
    }
    return built;
}

