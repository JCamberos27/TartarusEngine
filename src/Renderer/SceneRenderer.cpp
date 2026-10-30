#include "SceneRenderer.h"
#include <cstdio>
#include <cstdlib>
#include <array>
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
#include "BloodPalette.h"
#include "FxSpriteRenderer.h"
#include "WorldText.h"
#include "GLStateCache.h"
#include "gl.h"
#include "Core/Profiler.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <unordered_map>

// Per-run tally of how many meshes drew through each ShaderAsset variant key (audit #354). Read
// by --smoke-test to log which lobes actually rendered; harmless in normal runs.
namespace {
std::unordered_map<std::uint32_t, std::uint64_t> g_variantDrawCounts;

// How far from the eye the view model (arms, weapon, the player body's sleeves) can reach, metres: decals
// farther than this can't touch it, so its pass skips the decal cull (DecalSystem::NearestDynamicReceiverDecal).
constexpr float kViewModelDecalReach = 3.0f;

// #177 - deliberately leaked: a function-local static would run its GL deletes at exit, after the
// context is gone. The OS reclaims the handful of GL objects with the process.
ParticleRenderer* Particles() {
    static ParticleRenderer* particles = new ParticleRenderer();
    return particles;
}
}
namespace SceneRendererDebug {
const std::unordered_map<std::uint32_t, std::uint64_t>& VariantDrawCounts() { return g_variantDrawCounts; }
void ResetVariantDrawCounts() { g_variantDrawCounts.clear(); }
}

// Extracted from main.cpp's `drawScene` lambda (audit #359, pass 2) and then split into
// GatherFrameState + ApplyFrameState (audit #354) so the common per-frame uniform/texture set
// can be pushed onto any program a material's ShaderAsset variant selects — not just the one
// fixed modelShader. No behaviour change: same GL calls, same uniform order, and with a single
// program the sequence is byte-identical to pass 2 apart from the cluster-cull dispatch now
// running just before the uniform set instead of in the middle of it.

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

SceneRenderer::SceneRenderer() : m_Volumetrics(std::make_unique<VolumetricFog>()), m_Ssr(std::make_unique<PuddleSsr>()) {}
SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::WarmUpVolumetrics() { m_Volumetrics->WarmUp(); m_Ssr->WarmUp(); }

namespace {
// What GatherVisible needs of each renderable, read from the registry once per frame instead of once per view and
// per shadow light (each read is a sparse-set lookup, and with a few thousand props they were a fifth of the CPU
// frame). Good while the world's transform cache stamp and its renderables stay the same: tags, transforms and
// static bounds only change between frames. LodCulledTag (set per view) and posed bounds (an animated model) are
// still read live.
enum CandidateFlag : std::uint16_t {
    kCandInactive = 1 << 0, kCandPoseSource = 1 << 1, kCandOwnerViewOnly = 1 << 2, kCandHiddenFromOwner = 1 << 3,
    kCandViewModel = 1 << 4, kCandPlayerBody = 1 << 5, kCandHiddenInScene = 1 << 6, kCandPosed = 1 << 7,
};
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

const CandidateCache& Candidates(World& world) {
    static CandidateCache cache;
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
    // A renderable given another model (Inspector, outfits) brings other bounds, which its cell may not hold.
    auto sameModels = [&]() {
        size_t k = 0;
        for (auto [e, r] : reg.view<RenderableComponent>().each()) {
            if (k >= cache.PoolModels.size() || cache.PoolModels[k] != r.ModelRef.get()) return false;
            ++k;
        }
        return k == cache.PoolModels.size();
    };
    // One rebuild on from the last build, nothing entered or left the transform cache and no flag or model changed:
    // patch the moved ones (and everything posed reads its bounds live anyway).
    if (sameSet && cache.Flags == flags && cache.Stamp != 0 && cache.Stamp == world.PreviousTransformCacheStamp() &&
        !world.TransformCacheMembershipChanged() && sameModels()) {
        for (entt::entity e : world.ChangedWorldTransforms()) {
            const size_t slot = (size_t)entt::to_entity(e);
            if (slot >= cache.Index.size() || cache.Index[slot] == kNoCandidate) continue;
            Candidate& c = cache.List[cache.Index[slot]];
            if (c.Entity != e) continue;
            c.V.Xform = world.GetCachedWorldTransform(e);
            FillBounds(c.V, *c.Renderable);
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
        cache.Stamp = world.TransformCacheStamp();
        return cache;
    }
    cache.Owner = &world;
    cache.Stamp = world.TransformCacheStamp();
    cache.Renderables = storage.size();
    cache.First = first;
    cache.Last = last;
    cache.Flags = flags;
    cache.List.clear();
    cache.Hot.clear();
    cache.Cells.clear();
    std::fill(cache.Index.begin(), cache.Index.end(), kNoCandidate);
    cache.PoolModels.clear();
    for (auto [e, r] : reg.view<RenderableComponent>().each()) cache.PoolModels.push_back(r.ModelRef.get());
    static std::vector<Candidate> built;
    built.clear();
    for (auto entity : reg.view<TransformComponent, RenderableComponent>()) {
        auto& r = reg.get<RenderableComponent>(entity);
        if (!r.ModelRef) continue; // its model failed to load (or isn't assigned yet): nothing to draw
        Candidate c{entity, &r, r.ModelRef.get(), 0, 0, 0, {}};
        if (reg.all_of<InactiveTag>(entity)) c.Flags |= kCandInactive;
        if (reg.all_of<PoseSourceTag>(entity)) c.Flags |= kCandPoseSource;
        if (reg.all_of<OwnerViewOnlyTag>(entity)) c.Flags |= kCandOwnerViewOnly;
        if (reg.all_of<HiddenFromOwnerTag>(entity)) c.Flags |= kCandHiddenFromOwner;
        if (reg.all_of<ViewModelTag>(entity)) c.Flags |= kCandViewModel;
        if (reg.all_of<PlayerBodyTag>(entity)) c.Flags |= kCandPlayerBody;
        if (reg.all_of<HiddenInSceneTag>(entity)) c.Flags |= kCandHiddenInScene;
        if (r.ModelRef->HasBones()) c.Flags |= kCandPosed;
        const auto* lc = reg.try_get<LayerComponent>(entity);
        c.Layer = lc ? lc->Layer : 0;
        c.V.Entity = entity;
        c.V.Renderable = &r;
        c.V.Xform = world.GetCachedWorldTransform(entity);
        FillBounds(c.V, r);
        built.push_back(c);
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
            if (!cache.List[i].Renderable->ModelRef) continue;
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
    out.clear();
    const bool anyLod = !world.Registry.storage<LodCulledTag>().empty();
    const CandidateCache& cache = Candidates(world);
    for (const CandidateCell& cell : cache.Cells) {
    if (!cell.Always) { // the whole square is out of view
        const AABB box{cell.Min, cell.Max};
        if (q.Cull && !q.Cull->Intersects(box)) continue;
        if (q.CellCullCount > 0) {
            bool any = false;
            for (int f = 0; f < q.CellCullCount && !any; ++f) any = q.CellCull[f].Intersects(box);
            if (!any) continue;
        }
    }
    for (std::uint32_t i = cell.First, end = cell.First + cell.Count; i < end; ++i) {
        // GameObject active / LOD level (#163); PoseSourceTag is animated only (the first-person arms rig).
        const CandidateCache::HotCandidate& hot = cache.Hot[i];
        if (hot.Flags & (kCandInactive | kCandPoseSource)) continue;
        if (anyLod && world.IsLodCulled(hot.Entity)) continue;
        const Candidate& c = cache.List[i];
        // Split poses: the first-person copy only in the player's own camera, the world copy everywhere else (and
        // the world copy is the one that casts).
        if (c.Flags & ((q.ShadowCasters || !q.OwnerView) ? kCandOwnerViewOnly : kCandHiddenFromOwner)) continue;
        if (q.SkipViewModels && (c.Flags & kCandViewModel)) continue;
        if (q.SkipPlayerBody && (c.Flags & kCandPlayerBody)) continue;
        // The editor Scene view: hidden by the entity's own toggle (HiddenInSceneTag, #236 B) or by its layer (#236 A1).
        if (q.EditorView && ((c.Flags & kCandHiddenInScene) ||
                             (c.Layer >= 0 && c.Layer < 32 && !((q.LayerVisibleMask >> c.Layer) & 1u)))) continue;
        const RenderableComponent& r = *c.Renderable;
        if (!r.ModelRef) continue;
        // #163 - Cast Shadows: Off never casts; Shadows Only is drawn by the shadow passes, never in a camera view.
        if (q.ShadowCasters ? r.CastShadows == RenderableComponent::ShadowCasting::Off
                            : r.CastShadows == RenderableComponent::ShadowCasting::ShadowsOnly) continue;
        if (q.SkipTransparent && !r.Materials.empty() && r.Materials[0] &&
            r.Materials[0]->RenderQueue == MaterialAsset::Queue::Transparent) continue;
        // An animated model's bounds move with its pose, and a swapped model brings its own: read those live.
        const bool live = (c.Flags & kCandPosed) || r.ModelRef.get() != c.ModelAtBuild;
        VisibleRenderable liveV;
        if (live) { liveV = c.V; FillBounds(liveV, r); }
        const VisibleRenderable& v = live ? liveV : c.V; // tested in place; copied out only when it passes
        if (v.Bounded) {
            if (q.Cull && !q.Cull->Intersects(AABB{v.WorldMin, v.WorldMax})) continue;
            if (q.SizeCullEye && TooSmallOnScreen(v.WorldMin, v.WorldMax, *q.SizeCullEye, q.SizeCullProjScale,
                                                  q.MinScreenHeight)) continue;
        }
        out.push_back(v);
    }
    }
}

void SceneRenderer::ApplyFrameState(Shader& program, const FrameState& fs) const {
    const RenderFrameContext& ctx = *fs.ctx;
    const SceneRenderInputs&  in  = *fs.in;
    CascadedShadowMap&    shadowMap     = *in.sunShadow;
    SpotShadowMap&        spotShadowMap = *in.spotShadow;
    PointShadowMap&       pointShadowMap = *in.pointShadow;
    IblProbe&             iblProbe      = *in.iblProbe;
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
    program.SetFloat("uWetness", fs.world ? fs.world->Wetness : 0.0f);
    program.SetFloat("uPuddleAmount", fs.world ? fs.world->PuddleAmount : 0.0f);
    program.SetFloat("uPuddleScale", fs.world ? fs.world->PuddleScale : 4.0f);

    // Cascaded-shadow uniforms + the depth array on unit 8 (material maps use 1..7).
    program.SetInt("uShadowEnabled", fs.sunShadowsOn ? 1 : 0);
    program.SetInt("uShadowCascadeCount", shadowMap.Count());
    {
        glm::mat4 mats[CascadedShadowMap::kMaxCascades];
        for (int i = 0; i < shadowMap.Count(); ++i) mats[i] = shadowMap.LightViewProj(i);
        program.SetMat4Array("uShadowMatrices[0]", shadowMap.Count(), mats);
    }
    program.SetVec4("uCascadeSplits", shadowMap.SplitDepthsVec4());
    // World units per shadow texel, per cascade — the shader scales its normal offset + depth
    // bias by the selected cascade's value so one number isn't simultaneously too much for
    // cascade 0 and too little for cascade 3 (#117).
    program.SetVec4("uShadowTexelWorld", shadowMap.TexelWorldSizesVec4());
    // Penumbra width in shadow-map texels, from the sun's apparent size. 0.53 deg (real sun) ->
    // a tight ~2 texel edge; crank the light's Angular Size for softer. Softness 0 sends 0 = a
    // hard single-tap edge.
    program.SetFloat("uShadowSoftness", CascadedShadowMap::SunKernelTexels(in.sunAngularDeg, in.sunShadowSoftness));
    // Per-light sun shadow multipliers (#140 phase 2); 1.0 == pre-phase-2 output.
    program.SetFloat("uSunShadowBias", in.sunShadowBias);
    program.SetFloat("uSunShadowNormalBias", in.sunShadowNormalBias);
    glActiveTexture(GL_TEXTURE0 + 8);
    {
        unsigned int sunTex = shadowMap.DepthArray();
        glBindTexture(GL_TEXTURE_2D_ARRAY, sunTex ? sunTex : DefaultTextures::DepthArray());
    }
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
        program.SetFloatArray("uSpotShadowSoftness[0]",   fs.spotCountForView, in.spotShadowSoftness);
    }
    glActiveTexture(GL_TEXTURE0 + 9);
    // A valid depth-compare texture even when no spot shadow map is allocated, so the
    // sampler2DArrayShadow on unit 9 is never backed by texture 0 (audit #357). The shader still
    // early-outs on uSpotShadowCount == 0.
    {
        unsigned int spotTex = spotShadowMap.DepthArray();
        glBindTexture(GL_TEXTURE_2D_ARRAY, spotTex ? spotTex : DefaultTextures::DepthArray());
    }
    program.SetInt("uSpotShadowMap", 9);
    program.SetFloat("uSpotShadowMapResolution", (float)spotShadowMap.Resolution()); // #190

    // Point-light cube shadow maps on unit 10 (#119).
    program.SetInt("uPointShadowCount", fs.pointCountForView);
    if (fs.pointCountForView > 0) {
        program.SetFloatArray("uPointShadowFar[0]",        fs.pointCountForView, in.pointShadowFar);
        program.SetFloatArray("uPointShadowBias[0]",       fs.pointCountForView, in.pointShadowBias);
        program.SetFloatArray("uPointShadowNormalBias[0]", fs.pointCountForView, in.pointShadowNormalBias);
    }
    glActiveTexture(GL_TEXTURE0 + 10);
    { // valid samplerCubeArrayShadow backing even with no point shadow map (audit #357)
        unsigned int pointTex = pointShadowMap.DepthCubeArray();
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, pointTex ? pointTex : DefaultTextures::DepthCubeArray());
    }
    program.SetInt("uPointShadowMap", 10);
    program.SetFloat("uPointShadowMapResolution", (float)pointShadowMap.Resolution()); // #190

    // IBL probes on units 11/12/13 (#196). The shader declares those units with layout(binding=)
    // qualifiers, so there's no SetInt here — just the bind. Unlit mode skips lighting entirely.
    if (fs.iblOn) {
        glActiveTexture(GL_TEXTURE0 + 11);
        glBindTexture(GL_TEXTURE_CUBE_MAP, iblProbe.IrradianceMap());
        glActiveTexture(GL_TEXTURE0 + 12);
        glBindTexture(GL_TEXTURE_CUBE_MAP, iblProbe.SpecularMap());
        glActiveTexture(GL_TEXTURE0 + 13);
        glBindTexture(GL_TEXTURE_2D, iblProbe.BrdfLut());
        program.SetFloat("uIBLIntensity", std::max(fs.world->SkyAmbientIntensity, 0.0f));
        program.SetFloat("uIBLSpecularMaxLod", (float)(IblProbe::kSpecularMips - 1));
    }
    program.SetInt("uIBLEnabled", fs.iblOn ? 1 : 0);

    // Physical sky: aerial perspective (unit 30) and cloud shadows (unit 31). Neutral textures
    // and both switched off for the other sky modes.
    if (in.skyAtmosphere && !ctx.Unlit)
        in.skyAtmosphere->ApplyToProgram(program, ctx.TxHdr, fs.vp);
    else
        SkyAtmosphere::ApplyDisabled(program);
    // Froxel volumetric fog (unit 27): this view's volume, built in RenderScene, or off.
    m_Volumetrics->ApplyToProgram(program, ctx.TxHdr, fs.volOn);

    // #162 - fog.
    program.SetInt("uFogMode", fs.fogOn && fs.world && fs.world->FogEnabled ? std::clamp(fs.world->FogMode, 1, 3) : 0);
    if (fs.fogOn && fs.world && fs.world->FogEnabled) {
        const World& w = *fs.world;
        {
            program.SetVec3("uFogColor", glm::max(w.FogColor, glm::vec3(0.0f)));
            program.SetFloat("uFogDensity", std::max(w.FogDensity, 0.0f));
            program.SetFloat("uFogStart", w.FogStart);
            program.SetFloat("uFogEnd", w.FogEnd);
            program.SetFloat("uFogHeightFalloff", std::max(w.FogHeightFalloff, 0.0f));
            program.SetFloat("uFogBaseHeight", w.FogBaseHeight);
        }
    }
    // Captured room probes (ReflectionProbeCapture) on unit 26, the puddle SSR (PuddleSsr) on unit 29: units 1-7 and
    // 16-20 are material maps (ShaderAsset::BuildBindings) - a 2D map and the cube array sharing a unit made draws
    // fail, and the SSR read a material's white default. Never texture 0 either.
    glBindTextureUnit(26, probeArray.CubeArray() ? probeArray.CubeArray() : DefaultTextures::BlackCubeArray());
    program.SetInt("uSsrEnabled", fs.ssrTex ? 1 : 0);
    glBindTextureUnit(29, fs.ssrTex ? fs.ssrTex : DefaultTextures::Black());
    glActiveTexture(GL_TEXTURE0);

    // PR14: nearest 2 reflection probes for parallax box projection — no-op (uProbeCount=0 or the
    // uniforms absent) when the shader lacks the _REFLECTION_PROBES keyword / the scene has none.
    probeArray.Bind(program, ctx.ViewPos);

    // PR15: SSAO occlusion map (unit 15), pre-computed before this pass. ctx.SsaoSrc is the
    // caller's own per-viewport Ssao instance; IsValid() is false until its first filled frame.
    if (fs.ssaoOn) {
        glActiveTexture(GL_TEXTURE0 + 15);
        glBindTexture(GL_TEXTURE_2D, ctx.SsaoSrc->OcclusionTexture());
        program.SetInt("uSSAOMap", 15);
        program.SetVec2("uScreenSize", glm::vec2((float)fs.vp[2], (float)fs.vp[3]));
        glActiveTexture(GL_TEXTURE0);
    }
    program.SetInt("uSSAOEnabled", fs.ssaoOn ? 1 : 0);
    program.SetFloat("uSSAOIntensity", fs.ssaoIntensity);

    // The light SSBO (binding 0) is built once per frame by main.cpp — just (re)bind it.
    in.lightBuffer->Bind(0);

    // Clustered-forward light lists (#120). The cull dispatch itself runs once in RenderScene;
    // here every program just rebinds the range/index SSBOs and takes the froxel params.
    if (fs.clusterOn) {
        in.clusterGrid->BindForShading();
        program.SetVec2("uClusterScreenSize", glm::vec2((float)fs.vp[2], (float)fs.vp[3]));
        program.SetVec4("uClusterZParams", ClusterGrid::ZParams(fs.clusterNearZ, fs.clusterFarZ));
    }
    program.SetInt("uClusterEnabled", fs.clusterOn ? 1 : 0);

    // Clustered decals: the list (SSBO 11) and the texture arrays on units 22-25 (the blood stains and
    // bullet holes among them, and the blood splats' maps). The FX sprites bind their own arrays over
    // 22-24 after the opaque and transparent passes; every program that draws after them comes through here.
    program.SetInt("uDecalCount", fs.decalCount);
    // Substrates (the table on SSBO 16, maps in the same arrays): the Grunge reveal and peeling-paint
    // decals read them, in the views that prepared decals this frame.
    program.SetInt("uSubstrateOn", in.decals && in.decals->SubstrateCount() > 0 && fs.clusterOn ? 1 : 0);
    if (in.decals) in.decals->BindForShading();

    program.SetInt("uUnlit", ctx.Unlit ? 1 : 0);
    // Real scene path: emit linear HDR; the shared Tonemapper pass maps it after MSAA resolve.
    // (Offscreen model thumbnails set this to 1 to self-tonemap.)
    program.SetInt("uApplyTonemap", 0);
}

void SceneRenderer::WarmEffects() {
    Particles()->WarmUp();
    FxSpriteRenderer::Get().WarmUp();
    BloodRenderer::Get().WarmUp();
    WorldTextRenderer::Get().WarmUp();
}

namespace {
using WarmedVariants = std::unordered_map<ShaderAsset*, std::vector<ShaderVariantKey>>;
int WarmMaterials(const RenderableComponent& rc, WarmedVariants& done) {
    int built = 0;
    for (const auto& ma : rc.Materials) {
        if (!ma || !ma->Shader) continue;
        const ShaderVariantKey key = ShaderVariantKeyFor(ma->Mat, *ma->Shader);
        std::vector<ShaderVariantKey>& keys = done[ma->Shader.get()];
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
        keys.push_back(key);
        ma->Shader->Variant(key);
        ++built;
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

void SceneRenderer::RenderScene(World& world, const RenderFrameContext& ctx,
                                const SceneRenderInputs& in, RenderStats* outStats) {
    Sky&        sky         = *in.sky;
    Shader&     modelShader = *in.modelShader;
    LightBuffer& lightBuffer = *in.lightBuffer;
    ClusterGrid& clusterGrid = *in.clusterGrid;

    // The physical sky renders this view's aerial perspective and clouds up front (compute
    // passes, no FBO change): the model shader samples the aerial volume. The sky itself draws
    // after the opaque pass (below), only where no geometry landed.
    // With a depth pre-pass, the clouds wait for it (RenderClouds below): what the scene hides isn't marched.
    const bool cloudsAfterDepth = in.skyAtmosphere && in.depthShader && ProjectSettings::Quality().DepthPrepass;
    if (in.skyAtmosphere) in.skyAtmosphere->RenderView(ctx.TxHdr, ctx.View, ctx.Proj, ctx.ViewPos, cloudsAfterDepth);

    FrameState gathered = GatherFrameState(world, ctx, in);
    // Clustered decals (docs/DECALS.md): this view's visible set, uploaded before the cluster cull
    // bins it. They need the froxel lists, so nothing without clustering (the previews) gets any.
    if (in.decals && in.decalCullShader && gathered.clusterOn) {
        PROFILE_SCOPE("Decal Prepare");
        DecalGrading grading;
        for (auto [e, g] : world.Registry.view<const DecalGradingComponent>().each()) {
            if (world.Registry.all_of<InactiveTag>(e)) continue;
            grading.Age = std::clamp(g.Age, 0.0f, 1.0f);
            grading.Wear = std::clamp(g.Wear, 0.0f, 1.0f);
            break; // the first one in the scene wins
        }
        gathered.decalCount = in.decals->Prepare(world, ctx.Proj * ctx.View, ctx.ViewPos,
                                                 ProjectSettings::Quality().DecalMaxDistance, grading, ctx.EditorView);
        in.decals->BindForCull();
    }
    // Puddle SSR: wanted with standing water, IBL and a depth pre-pass to trace against; the trace itself runs after
    // that pre-pass, below. Not for probe captures, debug views or orthographic views.
    const bool ssrWanted = world.PuddleSsr && world.PuddleAmount > 0.0f && gathered.iblOn && !ctx.ProbeCapture &&
                           ctx.DebugView == 0 && ctx.TxHdr && ctx.TxHdr->IsValid() && in.depthShader &&
                           ProjectSettings::Quality().DepthPrepass && std::abs(ctx.Proj[3][3]) < 0.5f &&
                           gathered.vp[0] == 0 && gathered.vp[1] == 0 && gathered.vp[2] == ctx.TxHdr->Width() &&
                           gathered.vp[3] == ctx.TxHdr->Height();
    gathered.ssrTex = ssrWanted ? m_Ssr->Prepare(ctx.TxHdr) : 0u;
    const FrameState fs = gathered;
    // Which frame state the program selector pushes. The view-model sub-pass swaps this for its
    // own (see below) — that swap is the only reason it isn't a plain const FrameState.
    const FrameState* activeFs = &fs;

    // Clustered-forward light culling (#120) — one compute dispatch per frame, before any
    // program that reads the froxel lists is bound.
    if (fs.clusterOn) {
        PROFILE_GPU_SCOPE("Cluster Cull");
        // The cull reads the lights at binding 0: this frame's, not whatever an editor preview (the outfit or material
        // preview, lit by its own fixed lights) left bound there - binned from those, every lamp went dark.
        lightBuffer.Bind(0);
        clusterGrid.Cull(*in.clusterBuildShader, *in.clusterCullShader, ctx.View, ctx.Proj,
                         fs.clusterNearZ, fs.clusterFarZ, fs.vp[2], fs.vp[3], in.decalCullShader, fs.decalCount);
    }

    // Froxel volumetric fog: inject + light this view's froxels (reading the cluster lists just
    // culled and this frame's shadow maps), then integrate them, before any program samples it.
    if (fs.volOn) {
        PROFILE_GPU_SCOPE("Volumetric Fog");
        m_Volumetrics->Render(fs);
        GLStateCache::Invalidate(); // it rebinds material units 6 and 7
    }

    // Bind the default program and push the common per-frame state onto it. Meshes with a linked
    // ShaderAsset will select a variant program in the draw loop (audit #354, later PR) and get
    // this same state via ApplyFrameState.
    ApplyFrameState(modelShader, fs);

    RenderStats localStats;
    localStats.PointLights = std::max(0, in.frameLightCount - 1); // minus the directional sun
    localStats.LightBufferOverflowed = lightBuffer.Overflowed();  // #204
    localStats.ClusterSaturated = fs.clusterOn && clusterGrid.Saturated(); // #204
    Frustum camFrustum = Frustum::FromViewProj(ctx.Proj * ctx.View);

    // --- View-model sub-pass setup -----------------------------------------------------------
    // Entities tagged ViewModelTag (the first-person arms + weapon) are routed into their own
    // lists above and drawn last, under a projection that differs from the world's only in FOV:
    // near/far are read back out of ctx.Proj, so depth stays in the same terms the cluster build
    // and everything else this frame already assume. Disabled (-1 default) for callers that
    // preview through their own camera, and for orthographic views, where a perspective
    // ViewModelFov would be meaningless — both fall back to drawing them as ordinary geometry.
    bool viewModelPass = ctx.ViewModelFov > 0.0f && std::abs(ctx.Proj[3][3]) < 0.5f &&
                         fs.vp[2] > 0 && fs.vp[3] > 0;
    glm::mat4 vmProj = ctx.Proj;
    Frustum vmFrustum = camFrustum;
    if (viewModelPass) {
        const float nearZ = std::abs(ctx.Proj[3][2] / (ctx.Proj[2][2] - 1.0f));
        const float farZ  = std::abs(ctx.Proj[3][2] / (ctx.Proj[2][2] + 1.0f));
        vmProj = MakePerspective(ctx.ViewModelFov, (float)fs.vp[2] / (float)fs.vp[3], nearZ, farZ);
        vmFrustum = Frustum::FromViewProj(vmProj * ctx.View);
    }
    { // scope limits PROFILE_SCOPE to just this loop, not the rest of the frame
    PROFILE_SCOPE("Scene Draw");
    PROFILE_GPU_SCOPE("Scene Draw"); // shared by both Scene-tab and Game-tab draws

    // Program selection (audit #354). A mesh whose slot-0 material links a ShaderAsset draws
    // through that asset's variant program (keyword mask from the material's authored lobes);
    // everything else draws through `modelShader`, which already got ApplyFrameState above. Each
    // distinct program is brought up to this frame's state exactly once. `passAlphaBlend` is the
    // opaque/transparent flag for whichever pass is currently running.
    static std::vector<Shader*> appliedPrograms;
    appliedPrograms.clear();
    appliedPrograms.push_back(&modelShader);
    int passAlphaBlend = 0;
    // The program a material draws through, without touching GL state — also the opaque sort key.
    auto programFor = [&](const MaterialAsset* ma, std::uint32_t& key) -> Shader* {
        key = 0;
        if (ma && ma->Shader) {
            key = ShaderVariantKeyForCached(ma->Mat, *ma->Shader);
            if (Shader* v = ma->Shader->Variant(key)) return v;
        }
        return &modelShader;
    };
    auto selectProgram = [&](const MaterialAsset* ma) -> Shader* {
        std::uint32_t key = 0;
        Shader* prog = programFor(ma, key);
        if (std::find(appliedPrograms.begin(), appliedPrograms.end(), prog) == appliedPrograms.end()) {
            ApplyFrameState(*prog, *activeFs);
            prog->SetInt("uAlphaBlend", passAlphaBlend);
            appliedPrograms.push_back(prog);
        }
        g_variantDrawCounts[prog == &modelShader ? 0u : key]++;
        return prog;
    };

    // #192: gather the frustum-culled visible set, then sort it by material before drawing, so
    // entities sharing a material land adjacent — which is what makes the BindMaterial dedup
    // (GLStateCache::MaterialAlreadyBound) actually hit. All scene geometry is opaque and
    // depth-tested, so reordering is invisible. The buffer is static so it is reused across the
    // Scene- and Game-tab passes and across frames rather than reallocated each call.
    struct DrawItem {
        glm::mat4 Xform;
        Model* Ref;
        const std::vector<std::shared_ptr<MaterialAsset>>* Slots;
        std::uint64_t MatKey;
        const Shader* Prog;   // #112 - slot 0's program; the opaque pass groups by it first
        int Meshes, Tris, Verts;
        // PR9 transparent queue
        MaterialAsset::Queue Queue;
        int  QueueIndex;
        float ViewDepth; // view-space -Z (more positive = farther) for back-to-front sort
        glm::vec3 Centre; // world-space bounds centre — picks this object's reflection probes (#108)
        Model::MeshPass Pass; // #112 — which of the model's submeshes this item draws
        bool ReceiveShadows;  // #163
        int  LayerBit;        // #203 - 1 << Layer, tested against each light's excluded layers
        float NearHide = 0.0f; // PlayerBodyTag: metres around the eye not drawn (the player's own body)
        const int* HideBones = nullptr; // PlayerBodyTag::CameraHideBones in the camera's view, else none
        bool NoSsao = false;  // left out of the SSAO depth pre-pass (main.cpp), so it must not sample the map
        unsigned HideVerts = 0; // OutfitHideTag: the piece's covered vertices (SkinHideBuffer), 0 = none
        float NearHideWidth = 0.0f; // PlayerBodyTag: clothing's Near Hide to either side (0 = a sphere)
        unsigned CollarVerts = 0;   // PlayerBodyTag::CollarVerts in the camera's world pass, 0 = none
        // PlayerBodyTag bone masks: 1 = drop the vertices weighted mostly to BoneMask's bones (the camera's
        // world pass: the head's surroundings, and the sleeves while they're in the view-model pass),
        // 2 = draw only those (the view-model pass's copy of the sleeves). 0 = the whole piece.
        int BoneMaskMode = 0;
        std::array<std::uint32_t, 16> BoneMask{};
        // Clothing (an outfit piece, not a body part): drawn double-sided, so a collar's or cuff's inside
        // isn't culled away to show straight through it.
        bool DoubleSided = false;
        // OutfitHideTag::Visible: the piece's triangles not covered entirely, drawn instead of its own.
        const VisibleIndexBuffer* Visible = nullptr;
        float LayerPull = 0.0f; // OutfitLayerTag: drawn this many metres nearer the camera (depth only)
        // Skinned or a rigidbody: drawn after the static world. Blood stains take only to the static world
        // (Affect Dynamic off), so a character walking through a pool or a crate on a stain covers it.
        bool Dynamic = false;
        unsigned Entity = 0xFFFFFFFFu; // for the blood splats on it (BloodRenderer::SplatRange)
        // Which decals take to it (Affect Static / Dynamic): StaticTag, or nothing skinned or simulated.
        bool StaticReceiver = true;
    };
    static std::vector<DrawItem> drawList;
    static std::vector<DrawItem> transparentList;
    // Held out of the world pass while a view-model sub-pass will run (see viewModelPass above).
    static std::vector<DrawItem> viewModelList;
    static std::vector<DrawItem> viewModelTransparentList;
    drawList.clear();
    transparentList.clear();
    viewModelList.clear();
    viewModelTransparentList.clear();
    bool anyTransmission = false; // #112 — refraction capture only when something samples it

    // What this camera sees (GatherVisible): the editor Scene view's own hide filters, no Shadows Only, its copy of a
    // split pose. The frustum test is below - it has two frusta and an exemption.
    VisibilityQuery inView;
    inView.OwnerView = ctx.OwnerView;
    inView.EditorView = ctx.EditorView;
    inView.LayerVisibleMask = in.layerVisibleMask;
    if (ctx.ProbeCapture) { // a probe's room capture: the room, not the player or what's in the air
        inView.SkipViewModels = true;
        inView.SkipPlayerBody = true;
        inView.SkipTransparent = true;
    }
    // Too small to see from here (Project Settings > Quality); perspective views only. The SSAO depth pre-pass culls
    // with the same test, so the two agree on what stands in front.
    if (std::abs(ctx.Proj[3][3]) < 0.5f) {
        inView.SizeCullEye = &ctx.ViewPos;
        inView.SizeCullProjScale = ctx.Proj[1][1];
        inView.MinScreenHeight = ProjectSettings::Quality().SmallObjectCullHeight;
    }
    // Whole cells outside both of the frusta the test below uses are skipped up front.
    const Frustum viewCells[2] = {camFrustum, vmFrustum};
    inView.CellCull = viewCells;
    inView.CellCullCount = viewModelPass ? 2 : 1;
    static std::vector<VisibleRenderable> visible;
    GatherVisible(world, inView, visible);
    for (const VisibleRenderable& vis : visible) {
        const entt::entity entity = vis.Entity;
        auto& renderable = *vis.Renderable;
        // Tagged view-model geometry belongs to the sub-pass below when there is one; with no
        // sub-pass this view draws it here like anything else. `isViewModel` is therefore just
        // "route to the other list", not "hide".
        const bool isViewModel = viewModelPass && world.Registry.all_of<ViewModelTag>(entity);
        // The player's own body, around the camera: not in the view-model pass (its arms must be whole there).
        const auto* bodyTag = world.Registry.try_get<PlayerBodyTag>(entity);
        const bool cameraBody = bodyTag && !isViewModel && !ctx.EditorView && ctx.OwnerView;
        if (cameraBody && bodyTag->CameraHidden) continue; // the head: only the player's own camera skips it
        const float nearHide = cameraBody ? bodyTag->NearHide : 0.0f;
        const int* hideBones = cameraBody && bodyTag->CameraHideBones[0] >= 0 ? bodyTag->CameraHideBones : nullptr;
        // The SSAO pre-pass skips view-model geometry always and the camera's own body (its hidden parts
        // would occlude): read at their pixels, the map holds whatever stands behind them - see-through AO.
        const bool noSsao = cameraBody || world.Registry.all_of<ViewModelTag>(entity);
        // Clothing on the player's arms while they're in the view-model pass: the sleeves are drawn there
        // too (a second copy, only them), the rest here without them - see PlayerBodyTag.
        const bool splitSleeves = viewModelPass && cameraBody && bodyTag->SleevesInViewModel;
        // What the camera's world pass leaves out of this piece: around the head, and the sleeves when split (or
        // hidden with the arms).
        std::array<std::uint32_t, 16> cameraMask{};
        bool anyCameraMask = false;
        if (cameraBody && bodyTag->HasHeadBones) {
            for (int w = 0; w < 16; ++w) cameraMask[w] |= bodyTag->HeadBones[w];
            anyCameraMask = true;
        }
        if (splitSleeves || (cameraBody && bodyTag->SleevesHidden)) {
            for (int w = 0; w < 16; ++w) cameraMask[w] |= bodyTag->SleeveBones[w];
            anyCameraMask = true;
        }
        const float nearHideWidth = cameraBody ? bodyTag->NearHideWidth : 0.0f;
        const unsigned collarVerts = cameraBody && bodyTag->CollarVerts ? bodyTag->CollarVerts->Id() : 0u;
        const auto* outfitHide = world.Registry.try_get<OutfitHideTag>(entity);
        const unsigned hideVerts = outfitHide && outfitHide->Buffer ? outfitHide->Buffer->Id() : 0u;
        const auto* outfitLayer = world.Registry.try_get<OutfitLayerTag>(entity);
        const float layerPull = outfitLayer ? outfitLayer->Pull : 0.0f;
        const auto* outfitPiece = world.Registry.try_get<OutfitPieceComponent>(entity);
        const bool clothing = outfitPiece && !(outfitPiece->Flags & OutfitPieceBodyPart);
        glm::mat4 model = vis.Xform;
        const bool dynamicItem = renderable.ModelRef->HasBones() || world.Registry.all_of<RigidbodyComponent>(entity);
        const bool staticReceiver = world.Registry.all_of<StaticTag>(entity) || !dynamicItem;

        // Frustum culling: skip the draw call entirely for anything outside the camera's view.
        // Bounds are the model's as posed (Model::PosedBounds: the bind bounds for anything unskinned or
        // unposed) - an invalid (never-populated) bounds pair draws unconditionally. A skinned model's bind box,
        // even padded, isn't enough: a ragdoll's root stays where the NPC stood, and its head, a body length
        // away on the floor, was culled whenever that spot left the view - pieces blinking out up close.
        // The player's own body in their camera (world or view-model pass) is never culled: the camera sits
        // inside it, and a small piece's bind-pose bounds (a watch, a bracelet at the A-pose wrist, beside
        // and below the eye) missed the frustum while the posed hand was right in front of it.
        const bool playerBody = bodyTag && !ctx.EditorView && ctx.OwnerView;
        const glm::vec3& boundsMin = vis.LocalMin; // (their centre also sorts the transparent draws below)
        const glm::vec3& boundsMax = vis.LocalMax;
        const bool validBounds = vis.Bounded;
        if (validBounds && !playerBody) {
            const AABB worldBounds{vis.WorldMin, vis.WorldMax};
            const bool seen = (isViewModel ? vmFrustum : camFrustum).Intersects(worldBounds) ||
                              (splitSleeves && vmFrustum.Intersects(worldBounds));
            if (!seen) {
                localStats.Culled++;
                continue;
            }
        }

        Model* m = renderable.ModelRef.get();
        const auto& slots = renderable.Materials;
        std::uint64_t matKey = (!slots.empty() && slots[0])
            ? slots[0]->Mat.Hash() : m->MaterialSortKey();
        std::uint32_t progKeyUnused = 0;
        const Shader* prog = programFor(!slots.empty() ? slots[0].get() : nullptr, progKeyUnused);
        // PR9 / #112: classify per submesh — a model whose slot 1 is transparent used to draw
        // entirely opaque because only slot 0's queue was checked (and vice versa). A model
        // with both kinds is queued in both passes, each drawing only its own submeshes.
        const auto* layerComp = world.Registry.try_get<LayerComponent>(entity); // #203 lighting layers
        const int layerBit = (int)(1u << (layerComp ? std::clamp(layerComp->Layer, 0, 31) : 0));
        bool anyOpaque = false, anyTransparent = false;
        int qi = 2000; // QueueIndex of the first transparent slot
        for (int i = 0; i < m->MeshCount(); ++i) {
            const bool hasSlot = i < (int)slots.size() && slots[i];
            if (hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::Transparent) {
                if (!anyTransparent) qi = slots[i]->QueueIndex;
                anyTransparent = true;
            } else {
                anyOpaque = true;
            }
        }
        const Model::MeshPass opaquePass = anyTransparent ? Model::MeshPass::Opaque : Model::MeshPass::All;
        float viewDepth = 0.0f;
        // #112 — depth from the world-space bounds centre, not the entity origin (a large mesh
        // whose pivot sits at one end sorted as if it were all at that end).
        const glm::vec3 centre = validBounds ? glm::vec3(model * glm::vec4((boundsMin + boundsMax) * 0.5f, 1.0f))
                                             : glm::vec3(model[3]);
        viewDepth = -(ctx.View * glm::vec4(centre, 1.0f)).z;
        if (anyTransparent) {
            if (!anyTransmission) {
                for (int i = 0; i < m->MeshCount() && !anyTransmission; ++i) {
                    const bool hasSlot = i < (int)slots.size() && slots[i];
                    if (!hasSlot || slots[i]->RenderQueue != MaterialAsset::Queue::Transparent) continue;
                    anyTransmission = slots[i]->Mat.TransmissionStrength > 0.0f;
                }
            }
            DrawItem item{ model, m, &slots, matKey, prog,
                m->MeshCount(), (int)m->TriangleCount(), (int)m->VertexCount(),
                MaterialAsset::Queue::Transparent, qi, viewDepth, centre, Model::MeshPass::Transparent,
                renderable.ReceiveShadows, layerBit, nearHide, hideBones, noSsao, hideVerts, nearHideWidth, collarVerts };
            item.StaticReceiver = staticReceiver;
            item.DoubleSided = clothing;
            item.LayerPull = layerPull;
            if (outfitHide && outfitHide->Visible) {
                item.Visible = outfitHide->Visible.get();
                item.Tris = (int)item.Visible->Triangles();
            }
            if (anyCameraMask) {
                item.BoneMaskMode = 1;
                item.BoneMask = cameraMask;
            }
            if (splitSleeves) {
                DrawItem sleeves = item;
                sleeves.BoneMaskMode = 2;
                std::copy(std::begin(bodyTag->SleeveBones), std::end(bodyTag->SleeveBones), sleeves.BoneMask.begin());
                // Keeps the piece's Near Hide: the sleeve's top reaches the shoulder, at the eye in this
                // projection, and its open end showed as black slivers there.
                sleeves.HideBones = nullptr;
                // The sleeves are the arms' own: not the collar, and their Near Hide a plain sphere.
                sleeves.CollarVerts = 0;
                sleeves.NearHideWidth = 0.0f;
                viewModelTransparentList.push_back(sleeves);
            }
            (isViewModel ? viewModelTransparentList : transparentList).push_back(item);
        }
        if (anyOpaque) {
            DrawItem item{ model, m, &slots, matKey, prog,
                m->MeshCount(), (int)m->TriangleCount(), (int)m->VertexCount(),
                MaterialAsset::Queue::Opaque, 2000, viewDepth, centre, opaquePass,
                renderable.ReceiveShadows, layerBit, nearHide, hideBones, noSsao, hideVerts, nearHideWidth, collarVerts };
            item.DoubleSided = clothing;
            item.LayerPull = layerPull;
            item.Dynamic = dynamicItem;
            item.StaticReceiver = staticReceiver;
            item.Entity = (unsigned)entt::to_integral(entity);
            if (outfitHide && outfitHide->Visible) {
                item.Visible = outfitHide->Visible.get();
                item.Tris = (int)item.Visible->Triangles();
            }
            if (anyCameraMask) {
                item.BoneMaskMode = 1;
                item.BoneMask = cameraMask;
            }
            if (splitSleeves) {
                DrawItem sleeves = item;
                sleeves.BoneMaskMode = 2;
                std::copy(std::begin(bodyTag->SleeveBones), std::end(bodyTag->SleeveBones), sleeves.BoneMask.begin());
                // Keeps the piece's Near Hide: the sleeve's top reaches the shoulder, at the eye in this
                // projection, and its open end showed as black slivers there.
                sleeves.HideBones = nullptr;
                // The sleeves are the arms' own: not the collar, and their Near Hide a plain sphere.
                sleeves.CollarVerts = 0;
                sleeves.NearHideWidth = 0.0f;
                viewModelList.push_back(sleeves);
            }
            (isViewModel ? viewModelList : drawList).push_back(item);
        }
    }

    // --- Opaque pass: sort by shader program (the costliest switch), then material key, then
    // front-to-back within a material (#112 — cheaper overdraw: nearer surfaces fill depth
    // first and hide what is behind them). Shared with the view-model sub-pass, which draws the
    // same way under a different projection.
    // A DrawItem is ~250 bytes: sorting the lists in place moved each one O(log n) times. The sort runs on a
    // small key per item (its sort fields and its index), then every item moves once into its place. Equal
    // keys keep their gather order (the index breaks the tie), so the order is the same every frame.
    struct SortKey {
        std::uint64_t A, B; // A: Dynamic + program (opaque) or the queue index (transparent); B: material key
        float Depth;
        std::uint32_t Index;
    };
    static std::vector<SortKey> sortKeys;
    static std::vector<DrawItem> sortScratch;
    const auto sortItems = [](std::vector<DrawItem>& list, auto makeKey, auto less) {
        sortKeys.clear();
        for (std::uint32_t i = 0; i < (std::uint32_t)list.size(); ++i) {
            SortKey k = makeKey(list[i]);
            k.Index = i;
            sortKeys.push_back(k);
        }
        std::sort(sortKeys.begin(), sortKeys.end(), [&less](const SortKey& a, const SortKey& b) {
            if (less(a, b)) return true;
            if (less(b, a)) return false;
            return a.Index < b.Index;
        });
        sortScratch.clear();
        for (const SortKey& k : sortKeys) sortScratch.push_back(std::move(list[k.Index]));
        list.swap(sortScratch);
    };
    // The static world first, then by program (the costliest switch), material,
    // and front to back. A user-space pointer leaves the top bit free for Dynamic.
    const auto sortOpaque = [&](std::vector<DrawItem>& list) {
        sortItems(list,
            [](const DrawItem& d) {
                return SortKey{(d.Dynamic ? 1ull << 63 : 0ull) | (std::uint64_t)(std::uintptr_t)d.Prog, d.MatKey, d.ViewDepth, 0u};
            },
            [](const SortKey& a, const SortKey& b) {
                if (a.A != b.A) return a.A < b.A;
                if (a.B != b.B) return a.B < b.B;
                return a.Depth < b.Depth;
            });
    };
    // Transparent: QueueIndex ascending, then ViewDepth descending (farthest first), then MatKey.
    const auto sortTransparent = [&](std::vector<DrawItem>& list) {
        sortItems(list,
            [](const DrawItem& d) {
                return SortKey{(std::uint64_t)((std::int64_t)d.QueueIndex) ^ (1ull << 63), d.MatKey, d.ViewDepth, 0u};
            },
            [](const SortKey& a, const SortKey& b) {
                if (a.A != b.A) return a.A < b.A;
                if (a.Depth != b.Depth) return a.Depth > b.Depth;
                return a.B < b.B;
            });
    };
    sortOpaque(drawList);
    // TARTARUS_TRIS_REPORT=<frame>: the opaque pass's triangles on that frame of the Game view, by size (for budgeting).
    if (static const int reportAt = [] { char* e = nullptr; size_t n = 0; int at = -1; if (_dupenv_s(&e, &n, "TARTARUS_TRIS_REPORT") == 0 && e) at = std::atoi(e); std::free(e); return at; }();
        reportAt >= 0 && !ctx.EditorView) {
        static int frame = 0;
        if (frame++ == reportAt) {
            std::vector<const DrawItem*> v;
            long long total = 0, big = 0;
            for (const DrawItem& it : drawList) { v.push_back(&it); total += it.Tris; if (it.Tris >= 5000) big += it.Tris; }
            std::sort(v.begin(), v.end(), [](const DrawItem* a, const DrawItem* b) { return a->Tris > b->Tris; });
            std::printf("[TrisReport] opaque items %zu, %lld tris, %lld in items >= 5k\n", v.size(), total, big);
            std::stable_partition(v.begin(), v.end(), [](const DrawItem* d) { return !d->Dynamic; }); // the static ones first
            for (size_t i = 0; i < v.size() && i < 25; ++i)
                std::printf("[TrisReport]   %7d tris  %s  at (%.1f, %.1f, %.1f)%s\n", v[i]->Tris, v[i]->Ref->Path().c_str(), v[i]->Centre.x,
                            v[i]->Centre.y, v[i]->Centre.z, v[i]->Dynamic ? " dynamic" : "");
            std::fflush(stdout);
        }
    }

    // #108 — reflection probes are chosen per object (from its bounds centre), not once per
    // frame from the camera position, which gave every object in view the same two probes and
    // made reflections pop as the camera moved. Only when the scene has probes at all.
    // #163 - and Receive Shadows is per object too; both go through this per-draw hook.
    const DrawItem* probeItem = nullptr;
    const bool anyProbes = in.probeArray->Count() > 0;
    // The view's right, flattened: clothing's Near Hide stretches along it (to the shoulders), and it stays
    // level looking down, so the chest below the eye isn't swept into it. The view matrix's first row.
    glm::vec3 nearHideRight(ctx.View[0][0], 0.0f, ctx.View[2][0]);
    nearHideRight = glm::dot(nearHideRight, nearHideRight) > 1e-8f ? glm::normalize(nearHideRight) : glm::vec3(1.0f, 0.0f, 0.0f);
    int instanceBase = -1; // the opaque pass's current group in the instance buffer, -1 = drawn with uModel
    const std::function<void(Shader&)> perDraw = [&](Shader& prog) {
        prog.SetInt("uInstanced", instanceBase >= 0 ? 1 : 0);
        if (instanceBase >= 0) prog.SetInt("uInstanceBase", instanceBase);
        if (anyProbes) in.probeArray->Bind(prog, probeItem->Centre);
        prog.SetInt("uNoReceiveShadows", probeItem->ReceiveShadows ? 0 : 1);
        prog.SetInt("uObjectLayerBit", probeItem->LayerBit); // #203
        prog.SetInt("uDecalStatic", probeItem->StaticReceiver ? 1 : 0);
        prog.SetFloat("uNearHide", probeItem->NearHide);
        prog.SetFloat("uNearHideWidth", probeItem->NearHideWidth);
        prog.SetVec3("uNearHideRight", nearHideRight);
        static const char* const kHideBone[8] = {"uHideBones[0]", "uHideBones[1]", "uHideBones[2]", "uHideBones[3]",
                                                 "uHideBones[4]", "uHideBones[5]", "uHideBones[6]", "uHideBones[7]"};
        int hideCount = 0;
        if (probeItem->HideBones)
            for (; hideCount < 8 && probeItem->HideBones[hideCount] >= 0; ++hideCount) prog.SetInt(kHideBone[hideCount], probeItem->HideBones[hideCount]);
        prog.SetInt("uHideBoneCount", hideCount);
        prog.SetInt("uBoneMaskMode", probeItem->BoneMaskMode);
        if (probeItem->BoneMaskMode) {
            int mask[16];
            for (int w = 0; w < 16; ++w) mask[w] = (int)probeItem->BoneMask[w]; // the bits, as GLSL ints
            prog.SetIntArray("uBoneMask[0]", 16, mask); // one upload, not 16
        }
        prog.SetInt("uSSAOEnabled", activeFs->ssaoOn && !probeItem->NoSsao ? 1 : 0);
        SkinHideBuffer::Bind(probeItem->HideVerts);
        prog.SetInt("uHideVerts", probeItem->HideVerts ? 1 : 0);
        SkinHideBuffer::Bind(probeItem->CollarVerts, SkinHideBuffer::kCollarBinding);
        prog.SetInt("uCollarVerts", probeItem->CollarVerts ? 1 : 0);
        prog.SetFloat("uLayerPull", probeItem->LayerPull);
        int bloodFirst = 0, bloodCount = 0; // blood on this mesh
        BloodRenderer::Get().SplatRange(probeItem->Entity, bloodFirst, bloodCount);
        prog.SetInt("uBloodSplatCount", bloodCount);
        if (bloodCount > 0) {
            prog.SetInt("uBloodSplatFirst", bloodFirst);
            glm::vec4 fresh, dried; // the body's own palette (a spider's ichor), a person's by default
            BloodRenderer::Get().SplatLook(probeItem->Entity, fresh, dried);
            prog.SetVec4("uBloodFresh", fresh);
            prog.SetVec4("uBloodDried", dried);
        }
    };
    BloodRenderer::Get().BindSplatResources();

    // Depth pre-pass (Project Settings > Quality): the static world's depth first, colour off, so the opaque pass
    // below shades each pixel once - the drawList's order is by program and material, front to back only within
    // one, and a room's floor, walls and props otherwise each ran the full lighting where the next one covered
    // them. The depth program's positions round a hair differently from the model program's, so this pass is
    // pushed back by a polygon offset: under the opaque pass's GL_LEQUAL the surface itself still passes, anything
    // behind it doesn't. Only what draws whole is in it: nothing skinned or moving (Dynamic), and none of the
    // player body's or outfits' hidden parts, which the model program discards.
    const bool depthPrepass = in.depthShader && ProjectSettings::Quality().DepthPrepass;
    constexpr float kPrepassSlopeOffset = 0.02f;
    const auto prepassed = [&](const DrawItem& it) {
        return depthPrepass && !(it.Dynamic || it.NearHide > 0.0f || it.HideBones || it.HideVerts || it.CollarVerts ||
                                 it.BoneMaskMode || it.LayerPull != 0.0f || it.Ref->HasBones());
    };
    if (depthPrepass) {
        PROFILE_GPU_SCOPE("Depth Pre-pass");
        Shader& depth = *in.depthShader;
        depth.Bind();
        depth.SetMat4("uLightViewProj", ctx.Proj * ctx.View);
        depth.SetInt("uSplitViewProj", 1); // the model VS's own uProj * (uView * world): see ShadowDepth.vert.glsl
        depth.SetMat4("uView", ctx.View);
        depth.SetMat4("uProj", ctx.Proj);
        depth.SetInt("uUseSkinning", 0);
        depth.SetInt("uCascadeLayered", 0);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        // A small slope factor: the two programs' positions differ by rounding, far under a pixel. A whole pixel's
        // slope (factor 1) pushed a grazing floor back ~15 cm at 10 m, so a floor mat 1.3 cm above it lost the
        // LEQUAL test to the floor whenever the floor's program happened to sort after the mat's (Office hallway).
        glPolygonOffset(kPrepassSlopeOffset, 4.0f);
        static std::vector<DepthCaster> prepass;
        prepass.clear();
        for (const DrawItem& it : drawList)
            if (prepassed(it)) prepass.push_back({it.Ref, it.Slots, it.Visible, &it.Xform});
        DrawDepthCasters(depth, prepass);
        glPolygonOffset(0.0f, 0.0f);
        glDisable(GL_POLYGON_OFFSET_FILL);

        // SSAO from this pre-pass (RenderFrameContext::SsaoFromPrepass): copy the depth into SSAO's own target and
        // add only what the pre-pass left out, rather than SSAO drawing the whole visible scene a second time. The
        // view model and the player's own trimmed body stay out, as they did from SSAO's own pass (DrawItem::NoSsao).
        bool viewDepthCopied = false; // SSAO's depth target holds this view's depth now
        if (ctx.SsaoFromPrepass && fs.ssaoOn && ctx.TxHdr && ctx.TxHdr->IsValid() &&
            ctx.SsaoSrc->Width() == ctx.TxHdr->Width() && ctx.SsaoSrc->Height() == ctx.TxHdr->Height()) {
            PROFILE_GPU_SCOPE("SSAO Depth From Pre-pass");
            const int w = ctx.TxHdr->Width(), h = ctx.TxHdr->Height();
            { PROFILE_GPU_SCOPE("SSAO Depth Blit");
            glBlitNamedFramebuffer(ctx.TxHdr->MultisampleFbo(), ctx.SsaoSrc->DepthFbo(), 0, 0, w, h, 0, 0, w, h,
                                   GL_DEPTH_BUFFER_BIT, GL_NEAREST); }
            glBindFramebuffer(GL_FRAMEBUFFER, ctx.SsaoSrc->DepthFbo());
            glViewport(0, 0, w, h);
            prepass.clear();
            for (const DrawItem& it : drawList)
                if (!prepassed(it) && !it.NoSsao) prepass.push_back({it.Ref, it.Slots, it.Visible, &it.Xform});
            DrawDepthCasters(depth, prepass);
            depth.SetInt("uSplitViewProj", 0);
            viewDepthCopied = true;
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); // the occlusion passes write colour
            ctx.SsaoFromPrepass();
            GLStateCache::Invalidate();
            ctx.TxHdr->BindForRender();
            glViewport(fs.vp[0], fs.vp[1], fs.vp[2], fs.vp[3]);
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glDisable(GL_BLEND);
            ApplyFrameState(modelShader, fs); // the occlusion passes used texture units 0 and 1
        }
        depth.SetInt("uSplitViewProj", 0); // the shadow passes share this program
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        GLStateCache::Invalidate();
        if (ssrWanted) {
            // Against this view's depth: SSAO's copy (everything) when it was made, else the pre-pass's own (static).
            unsigned int ssrDepth = 0;
            if (viewDepthCopied) {
                ssrDepth = ctx.SsaoSrc->DepthTexture();
            } else {
                ctx.TxHdr->ResolveDepthOnly();
                ssrDepth = ctx.TxHdr->ResolvedDepthTexture();
            }
            m_Ssr->Trace(ctx.TxHdr, ssrDepth, ctx.View, ctx.Proj, world);
        }
        modelShader.Bind();
        if (cloudsAfterDepth) {
            // The clouds, skipping the sky the scene hides all around (a street, a room) - with SSAO off there is no
            // single-sample copy of the depth to read, and every texel is marched as before.
            in.skyAtmosphere->RenderClouds(ctx.TxHdr, ctx.View, ctx.Proj, ctx.ViewPos,
                                           viewDepthCopied ? ctx.SsaoSrc->DepthTexture() : 0u);
            ApplyFrameState(modelShader, fs); // the cloud passes used texture units up to 12
        }
    }

    passAlphaBlend = 0;
    modelShader.SetInt("uAlphaBlend", 0); // explicit: ensure opaque pass outputs alpha=1
    // GPU instancing (Project Settings > Quality): copies of a static model that draw exactly alike - the same
    // materials and programs, sidedness, shadow receiving, lighting layer and SSAO - go out as one instanced draw at the
    // first copy's place in the sorted list. With reflection probes, only copies that pick the same probes with the same
    // blend (ReflectionProbeArray::SelectionKey) - every city scene has probes, and the whole pass used to fall back to
    // one draw per copy for them. Blood splats, and anything skinned, moving or partly hidden, still draw one by one.
    // The groups keep the list's order: the static world (all the batches) first, then the moving things.
    const bool instancing = ProjectSettings::Quality().GpuInstancing;
    static std::vector<std::uint32_t> probeKeys;
    probeKeys.assign(anyProbes && instancing ? drawList.size() : 0, 0u);
    for (std::size_t i = 0; i < probeKeys.size(); ++i) probeKeys[i] = in.probeArray->SelectionKey(drawList[i].Centre);
    const auto probeKey = [&](std::uint32_t i) { return i < probeKeys.size() ? probeKeys[i] : 0u; };
    const auto batchable = [&](const DrawItem& it) {
        if (!instancing || it.Dynamic || it.Ref->HasBones() || it.Visible || it.NearHide > 0.0f || it.HideBones ||
            it.HideVerts || it.CollarVerts || it.BoneMaskMode || it.LayerPull != 0.0f)
            return false;
        int bloodFirst = 0, bloodCount = 0;
        BloodRenderer::Get().SplatRange(it.Entity, bloodFirst, bloodCount);
        if (bloodCount > 0) return false;
        // Every program its meshes draw through must read the per-instance transforms.
        std::uint32_t key = 0;
        for (int i = 0; i < it.Ref->MeshCount(); ++i) {
            const MaterialAsset* ma = i < (int)it.Slots->size() ? (*it.Slots)[i].get() : nullptr;
            if (!programFor(ma, key)->Instancing()) return false;
        }
        return true;
    };
    static InstanceGroups opaqueGroups;
    GroupInstances(
        drawList.size(),
        [&](std::uint32_t i) -> std::uint64_t {
            const DrawItem& it = drawList[i];
            if (!batchable(it)) return kNoBatch;
            std::uint64_t seed = (std::uint64_t)(std::uintptr_t)it.Ref->ImportId() * 0x9E3779B97F4A7C15ull;
            seed ^= (std::uint64_t)it.Pass | ((std::uint64_t)it.DoubleSided << 4) | ((std::uint64_t)it.ReceiveShadows << 5) |
                    ((std::uint64_t)it.NoSsao << 6) | ((std::uint64_t)(std::uint32_t)it.LayerBit << 8) |
                    ((std::uint64_t)probeKey(i) << 32);
            return HashSlots(*it.Slots, seed);
        },
        [&](std::uint32_t a, std::uint32_t b) {
            const DrawItem& x = drawList[a];
            const DrawItem& y = drawList[b];
            return x.Ref->ImportId() == y.Ref->ImportId() && x.Pass == y.Pass && x.DoubleSided == y.DoubleSided && x.ReceiveShadows == y.ReceiveShadows &&
                   x.NoSsao == y.NoSsao && x.LayerBit == y.LayerBit && probeKey(a) == probeKey(b) && SameSlots(*x.Slots, *y.Slots);
        },
        opaqueGroups);
    static std::vector<int> groupBase;
    groupBase.assign(opaqueGroups.Groups.size(), -1);
    {
        std::vector<glm::mat4>& staging = InstanceBuffer::Staging();
        staging.clear();
        for (std::size_t g = 0; g < opaqueGroups.Groups.size(); ++g) {
            const auto& grp = opaqueGroups.Groups[g];
            if (grp.Count < 2) continue;
            groupBase[g] = (int)(staging.size() / 2);
            for (std::uint32_t k = 0; k < grp.Count; ++k) {
                const glm::mat4& x = drawList[opaqueGroups.Order[grp.First + k]].Xform;
                staging.push_back(x);
                staging.push_back(glm::mat4(glm::transpose(glm::inverse(glm::mat3(x)))));
            }
        }
        InstanceBuffer::Upload();
    }
    // TAA: the opaque pass (ModelFragment.glsl) is what writes motion vectors - location 1 joins the draw
    // buffers for it alone, and leaves again before fur, the sky and the transparents.
    const bool velocityOut = ctx.TxHdr && ctx.TxHdr->VelocityEnabled();
    if (velocityOut) ctx.TxHdr->SetVelocityWrites(true);
    {
    PROFILE_GPU_SCOPE("Opaque");
    // What the pre-pass laid down draws without writing depth again: the model program can discard (alpha clip, hidden
    // parts), and a program that discards while writing depth loses early depth testing - every hidden layer of a
    // street or a forest was shaded in full. With writes off the test runs first, and the pre-pass depth (the same
    // clip) already holds the answer.
    GLboolean depthWrites = GL_TRUE;
    for (std::size_t g = 0; g < opaqueGroups.Groups.size(); ++g) {
        const auto& grp = opaqueGroups.Groups[g];
        const DrawItem& it = drawList[opaqueGroups.Order[grp.First]];
        const GLboolean wantWrites = prepassed(it) ? GL_FALSE : GL_TRUE;
        if (wantWrites != depthWrites) glDepthMask(depthWrites = wantWrites);
        probeItem = &it;
        instanceBase = groupBase[g];
        it.Ref->DrawSelected(modelShader, it.Xform, *it.Slots, selectProgram, 1.0f, perDraw, it.Pass, it.DoubleSided, it.Visible,
                             instanceBase >= 0 ? (int)grp.Count : 1);

        localStats.DrawCalls += it.Meshes;
        for (std::uint32_t k = 0; k < grp.Count; ++k) {
            const DrawItem& member = drawList[opaqueGroups.Order[grp.First + k]];
            localStats.Triangles += member.Tris;
            localStats.Vertices += member.Verts;
        }
    }
    if (depthWrites != GL_TRUE) glDepthMask(GL_TRUE);
    }
    instanceBase = -1;
    if (velocityOut) ctx.TxHdr->SetVelocityWrites(false);
    // Nothing after this pass draws instanced: leave every program it used reading uModel again (the previews share
    // the material shaders' programs).
    for (Shader* p : appliedPrograms) {
        p->Bind();
        p->SetInt("uInstanced", 0);
    }
    modelShader.Bind();

    // Shell fur (FurRenderer) over the opaque surfaces just drawn - the visible set only, so nothing off-screen pays
    // for it. Depth-writing and alpha-tested, so it needs no order of its own.
    if (!FurRenderer::Disabled()) {
        static std::vector<FurRenderer::Item> furItems;
        furItems.clear();
        for (const DrawItem& it : drawList) {
            if (!FurRenderer::HasFur(*it.Ref, *it.Slots)) continue;
            FurRenderer::Item f;
            f.Ref = it.Ref;
            f.Slots = it.Slots;
            f.Xform = it.Xform;
            f.Centre = it.Centre;
            const float scale = std::max({glm::length(glm::vec3(it.Xform[0])), glm::length(glm::vec3(it.Xform[1])),
                                          glm::length(glm::vec3(it.Xform[2]))});
            f.Radius = 0.5f * glm::length(it.Ref->BoundsMax() - it.Ref->BoundsMin()) * scale;
            f.LayerBit = it.LayerBit;
            f.ReceiveShadows = it.ReceiveShadows;
            furItems.push_back(f);
        }
        if (!furItems.empty()) {
            long long furTris = 0;
            localStats.DrawCalls += FurRenderer::Get().Draw(furItems, ctx.View, ctx.Proj, ctx.ViewPos, (float)fs.vp[3],
                                                            ctx.TxHdr && ctx.TxHdr->Samples() > 1,
                                                            [&](Shader& p) { ApplyFrameState(p, fs); }, furTris);
            localStats.Triangles += (int)furTris;
            modelShader.Bind();
        }
    }

    // Volumetric blood in the air: opaque fluid, so before the sky and with the
    // opaque geometry it lands on already in depth.
    localStats.DrawCalls += BloodRenderer::Get().DrawSprays(ctx.View, ctx.Proj,
                                                            [&](Shader& p) { ApplyFrameState(p, fs); });
    modelShader.Bind();

    // PR13: sky - the physical sky, HDRI cubemap or procedural gradient - at the far plane under a
    // GL_LEQUAL test, so the opaque pass above has already rejected every covered pixel. Before
    // the transparent pass (it blends over the sky, and transmission captures it).
    if (in.skyAtmosphere) {
        in.skyAtmosphere->DrawSky(ctx.TxHdr);
    } else if (world.SkySourceMode == World::SkySource::Hdri && in.hdriCube) {
        float rotRad = glm::radians(world.SkyRotationDegrees);
        sky.DrawHdri(in.hdriCube->Texture(), rotRad, ctx.View, ctx.Proj);
    } else {
        sky.Draw(ctx.View, ctx.Proj, world.SkyHorizonColor, world.SkyZenithColor);
    }
    // The volumetric fog over the sky's pixels (every surface applied it in its own shading).
    if (fs.volOn) {
        m_Volumetrics->DrawSky(ctx.TxHdr, fs.vp);
        GLStateCache::Invalidate();
        modelShader.Bind();
    }

    // --- Transparent pass: back-to-front sorted, blended, no depth write -----------
    if (!transparentList.empty()) {
        PROFILE_GPU_SCOPE("Transparent");
        passAlphaBlend = 1;
        // Every program already brought up for the opaque pass flips to blended output; new
        // programs picked up below get passAlphaBlend via the selector.
        for (Shader* p : appliedPrograms) { p->Bind(); p->SetInt("uAlphaBlend", 1); }

        // PR12: resolve MSAA opaque color and copy into mipped texture for refraction. Bind the
        // capture on unit 14 for every program that may run the _TRANSMISSION branch.
        // #112 — only when a visible transparent material uses transmission; this resolve + mip
        // copy used to run for any transparent object at all, per viewport per frame.
        if (anyTransmission && ctx.TxHdr && ctx.TxCapture) {
            ctx.TxHdr->ResolveTo();
            ctx.TxCapture->CopyFrom(ctx.TxHdr->ResolvedColorTexture(), fs.vp[2], fs.vp[3]);
            ctx.TxHdr->BindForRender(); // rebind MSAA FBO for the transparent draw pass
            glActiveTexture(GL_TEXTURE0 + 14);
            glBindTexture(GL_TEXTURE_2D, ctx.TxCapture->Texture());
            for (Shader* p : appliedPrograms) {
                p->Bind();
                p->SetInt("uOpaqueColor", 14);
                p->SetVec2("uScreenSize", glm::vec2((float)fs.vp[2], (float)fs.vp[3]));
            }
        }

        // Sort: QueueIndex ascending, then ViewDepth descending (farthest first), then MatKey.
        sortTransparent(transparentList);

        glEnable(GL_BLEND);
        glDepthMask(GL_FALSE);
        glBlendFuncSeparate(GL_SRC_ALPHA,  GL_ONE_MINUS_SRC_ALPHA,
                            0x0001/*GL_ONE*/, GL_ONE_MINUS_SRC_ALPHA);

        for (const DrawItem& it : transparentList) {
            probeItem = &it;
            // Opacity comes from each transparent submesh's own slot (#112).
            it.Ref->DrawSelected(modelShader, it.Xform, *it.Slots, selectProgram, 1.0f, perDraw, it.Pass, it.DoubleSided, it.Visible);

            localStats.DrawCalls += it.Meshes;
            localStats.Triangles += it.Tris;
            localStats.Vertices += it.Verts;
        }

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        for (Shader* p : appliedPrograms) { p->Bind(); p->SetInt("uAlphaBlend", 0); } // restore for next frame's opaque pass
    }

    // #177 - particles last: depth-tested against everything above, blended, no depth write.
    // The player's own muzzle flame waits for the view-model sub-pass.
    if (!ctx.ProbeCapture) { // a probe's room capture keeps only the room
    if (Particles()->Draw(world, ctx, viewModelPass) > 0) ++localStats.DrawCalls;
    // The Knife flipbook particles: blood mist, impact dust and debris, muzzle smoke.
    localStats.DrawCalls += FxSpriteRenderer::Get().Draw(ctx.View, ctx.Proj, fs.vp, ctx.TxHdr, false,
                                                         [&](Shader& p) { ApplyFrameState(p, fs); });
    // Labels in the world (World Text): unlit, depth-tested against everything above.
    localStats.DrawCalls += WorldTextRenderer::Get().Draw(world, ctx.View, ctx.Proj, ctx.ViewPos, ctx.EditorView);
    }
    if (ctx.WorldOverlay) {
        ctx.WorldOverlay();
        GLStateCache::Invalidate();
    }

    // --- View-model sub-pass: tagged geometry, its own projection, after a depth clear --------
    // Everything above has been written; only the tagged arms/weapon remain, and they now draw
    // on top of all of it rather than fighting it for depth.
    if (viewModelPass && !viewModelList.empty()) {
        PROFILE_SCOPE("View Model Draw");
        PROFILE_GPU_SCOPE("View Model Draw");

        // Depth writes must be unmasked first — the transparent and particle passes both turn
        // them off, and glClear(GL_DEPTH_BUFFER_BIT) is a silent no-op while they are masked.
        // Colour is untouched, so the world stays visible around the hands while nothing in it
        // can punch through them: the arms sit ~0.3 m from the eye over floor metres away, and
        // sharing one depth range with the scene is exactly what would clip them into it.
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        // Nothing here is clipped at the near plane: a view model reaches back past it (the player
        // body's arms piece has its shoulders at the eye), and clipped it showed as an open cut through
        // the shoulder. Depth clamp pins those fragments to the near plane instead, keeping the
        // projection - and so the depth that depth of field and motion blur read - unchanged.
        glEnable(0x864F /*GL_DEPTH_CLAMP, core since 3.2; not in this loader*/);

        // Re-fit the froxel lists to this projection. They were culled for the world's FOV, so
        // reading them as-is would light the arms from slices built for a different one. Safe
        // here: the world's draws have already consumed this frame's lists.
        RenderFrameContext vmCtx = ctx;
        vmCtx.Proj = vmProj;
        vmCtx.TaaVelocity = false; // unjittered, and it moves with the eye: zero motion (written below)
        FrameState vmFs = GatherFrameState(world, vmCtx, in);
        vmFs.ssrTex = 0; // traced for the world's projection, not the arms'
        // The SSAO map was built from the world's depth pre-pass, which skips the view model, so
        // sampling it here stamps whatever stands behind the gun (a character, a wall) onto it as
        // occlusion - the weapon read as see-through. The view model gets no screen-space AO.
        vmFs.ssaoOn = false;
        // The world view's decals, re-binned into these froxels too (their bounds are still uploaded) - when
        // any could land on the view model: one that takes dynamic receivers within the arms' reach of the eye,
        // and a view-model material that receives decals. Otherwise the second decal cull is skipped (its lists
        // would come out empty for every view-model pixel anyway).
        bool vmDecals = vmFs.clusterOn && fs.decalCount > 0 && in.decals &&
                        in.decals->NearestDynamicReceiverDecal() < kViewModelDecalReach;
        if (vmDecals) {
            bool receives = false;
            for (const DrawItem& it : viewModelList) {
                if (!it.Slots || it.Slots->empty()) { receives = true; break; } // the model's own materials
                for (const auto& m : *it.Slots)
                    if (!m || m->Mat.ReceiveDecals) { receives = true; break; }
                if (receives) break;
            }
            vmDecals = receives;
        }
        vmFs.decalCount = vmDecals ? fs.decalCount : 0;
        if (vmFs.clusterOn) {
            PROFILE_GPU_SCOPE("View Model Cluster Cull");
            if (vmFs.decalCount > 0) in.decals->BindForCull();
            lightBuffer.Bind(0); // this frame's lights, as for the main view's cull
            clusterGrid.Cull(*in.clusterBuildShader, *in.clusterCullShader, vmCtx.View, vmCtx.Proj,
                             vmFs.clusterNearZ, vmFs.clusterFarZ, vmFs.vp[2], vmFs.vp[3], in.decalCullShader,
                             vmFs.decalCount);
        }

        // Everything below runs against the view model's frame state — uProj above all, which is
        // the entire point of the pass.
        activeFs = &vmFs;
        appliedPrograms.clear();
        appliedPrograms.push_back(&modelShader);
        passAlphaBlend = 0;
        ApplyFrameState(modelShader, vmFs);
        modelShader.SetInt("uAlphaBlend", 0);

        sortOpaque(viewModelList);
        // The arms and gun get a depth pre-pass of their own, for the world pass's reason: the model program can
        // discard, so drawn with depth writes on they lost early depth testing and every hidden layer of the gun (its
        // inner parts, the hands behind it) was shaded in full - about 1 ms of a frame for two models. Skinned pieces
        // can go in here, unlike the world's: both programs read this frame's pre-skinned vertices (Model::PreSkin).
        // Left out, as there: what the model program hides (near hide, hidden bones/vertices) and two-sided draws.
        const auto vmPrepassed = [&](const DrawItem& it) {
            return depthPrepass && !(it.NearHide > 0.0f || it.HideBones || it.HideVerts || it.CollarVerts ||
                                     it.BoneMaskMode || it.LayerPull != 0.0f || it.DoubleSided);
        };
        if (depthPrepass) {
            PROFILE_GPU_SCOPE("View Model Depth Pre-pass");
            static std::vector<DepthCaster> vmPrepass;
            vmPrepass.clear();
            for (const DrawItem& it : viewModelList)
                if (vmPrepassed(it)) vmPrepass.push_back({it.Ref, it.Slots, it.Visible, &it.Xform});
            if (!vmPrepass.empty()) {
                Shader& depth = *in.depthShader;
                depth.Bind();
                depth.SetMat4("uLightViewProj", vmProj * ctx.View);
                depth.SetInt("uSplitViewProj", 1);
                depth.SetMat4("uView", ctx.View);
                depth.SetMat4("uProj", vmProj);
                depth.SetInt("uUseSkinning", 0);
                depth.SetInt("uCascadeLayered", 0);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(1.0f, 4.0f);
                DrawDepthCasters(depth, vmPrepass);
                glPolygonOffset(0.0f, 0.0f);
                glDisable(GL_POLYGON_OFFSET_FILL);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                depth.SetInt("uSplitViewProj", 0);
                GLStateCache::Invalidate();
                modelShader.Bind();
            }
        }
        PROFILE_GPU_SCOPE("View Model Opaque");
        if (velocityOut) ctx.TxHdr->SetVelocityWrites(true); // TAA: the arms and gun write zero motion
        for (const DrawItem& it : viewModelList) {
            probeItem = &it;
            const bool prepassedVm = vmPrepassed(it);
            if (prepassedVm) glDepthMask(GL_FALSE);
            it.Ref->DrawSelected(modelShader, it.Xform, *it.Slots, selectProgram, 1.0f, perDraw, it.Pass, it.DoubleSided, it.Visible);
            if (prepassedVm) glDepthMask(GL_TRUE);

            localStats.DrawCalls += it.Meshes;
            localStats.Triangles += it.Tris;
            localStats.Vertices += it.Verts;
        }
        if (velocityOut) ctx.TxHdr->SetVelocityWrites(false);

        // Transparent view-model slots (a lens, glass) follow the opaque arms rather than
        // preceding them as they would in the world pass — here the opaque geometry of the same
        // object is already down, so they composite over it instead of under it.
        if (!viewModelTransparentList.empty()) {
            passAlphaBlend = 1;
            for (Shader* p : appliedPrograms) { p->Bind(); p->SetInt("uAlphaBlend", 1); }

            if (anyTransmission && ctx.TxHdr && ctx.TxCapture) {
                ctx.TxHdr->ResolveTo();
                ctx.TxCapture->CopyFrom(ctx.TxHdr->ResolvedColorTexture(), vmFs.vp[2], vmFs.vp[3]);
                ctx.TxHdr->BindForRender();
                glActiveTexture(GL_TEXTURE0 + 14);
                glBindTexture(GL_TEXTURE_2D, ctx.TxCapture->Texture());
                for (Shader* p : appliedPrograms) {
                    p->Bind();
                    p->SetInt("uOpaqueColor", 14);
                    p->SetVec2("uScreenSize", glm::vec2((float)vmFs.vp[2], (float)vmFs.vp[3]));
                }
                glActiveTexture(GL_TEXTURE0);
            }

            sortTransparent(viewModelTransparentList);

            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
            glBlendFuncSeparate(GL_SRC_ALPHA,  GL_ONE_MINUS_SRC_ALPHA,
                                0x0001/*GL_ONE*/, GL_ONE_MINUS_SRC_ALPHA);
            for (const DrawItem& it : viewModelTransparentList) {
                probeItem = &it;
                it.Ref->DrawSelected(modelShader, it.Xform, *it.Slots, selectProgram, 1.0f, perDraw, it.Pass, it.DoubleSided, it.Visible);

                localStats.DrawCalls += it.Meshes;
                localStats.Triangles += it.Tris;
                localStats.Vertices += it.Verts;
            }
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            for (Shader* p : appliedPrograms) { p->Bind(); p->SetInt("uAlphaBlend", 0); }
        }

        // The player's own muzzle flame, at the gun's projection, behind or in front of the arms and gun
        // as it really is.
        if (Particles()->Draw(world, vmCtx, true, true) > 0) ++localStats.DrawCalls;
        localStats.DrawCalls += FxSpriteRenderer::Get().Draw(vmCtx.View, vmCtx.Proj, vmFs.vp, nullptr, true,
                                                             [&](Shader& p) { ApplyFrameState(p, vmFs); });

        glDisable(0x864F /*GL_DEPTH_CLAMP*/);
        activeFs = &fs; // nothing below reads it; leave the selector on the caller's state
    }
    } // end "Scene Draw" profile scope
    if (outStats) *outStats = localStats;
}
