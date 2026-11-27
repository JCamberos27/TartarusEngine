#include "ShadowFrame.h"
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "Components.h"
#include "CubeFaces.h"
#include "Core/Env.h"
#include "Core/Log.h"
#include "DefaultTextures.h"
#include "Frustum.h"
#include "GLStateCache.h"
#include "GLStateScope.h"
#include "GpuCull.h"
#include "GrassRenderer.h"
#include "ImpostorRenderer.h"
#include "SkinHideBuffer.h"
#include "TimeService.h"
#include "gl.h"
#include "LightGather.h"
#include "MaterialAsset.h"
#include "Model.h"
#include "Profiler.h"
#include "RenderSettingsEpoch.h"
#include "Shader.h"
#include "World.h"

namespace {
constexpr std::uint64_t kMovingFrames = 30;
// Smoothness: when casters stop together (everything settling 30 frames after Play), every light that sees them used
// to rebuild its whole static cache in the same frame. A caster that just stopped stays "settling" this many frames,
// and each light takes it into its static depth on its own turn of them (DynamicFor); until then that light draws it
// with the moving casters, over a cache without it - never a missing shadow.
constexpr std::uint64_t kSettleFrames = 4;
constexpr std::uint64_t kStaleFrames = 600; // a caster unseen this long is gone (scene reload, deleted entity)
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;

std::uint64_t HashBytes(std::uint64_t h, const void* data, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

std::uint64_t HashWord(std::uint64_t h, std::uint64_t w) {
    h = (h ^ w) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

// TARTARUS_TRIS_REPORT: one frame's sun casters - triangles per cascade, and the heaviest models.
void ReportSunTris(const std::vector<DepthCaster>& draws, int cascadeCount) {
    long long perCascade[CascadedShadowMap::kMaxCascades] = {};
    std::unordered_map<std::string, std::pair<long long, int>> byModel; // tris over all cascades, draws
    for (const auto& d : draws) {
        const long long t = d.Visible ? (long long)d.Visible->Triangles() : (long long)d.Ref->TriangleCount();
        for (int k = 0; k < d.Layers; ++k) perCascade[d.LayerFirst + k] += t;
        auto& r = byModel[d.Ref->Path()];
        r.first += t * d.Layers;
        ++r.second;
    }
    std::printf("[TrisReport] frame %llu sun casters: %zu draws, tris per cascade", (unsigned long long)Time::FrameCount(),
                draws.size());
    for (int c = 0; c < cascadeCount; ++c) std::printf(" %lld", perCascade[c]);
    std::printf("\n");
    std::vector<std::pair<std::string, std::pair<long long, int>>> v(byModel.begin(), byModel.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    for (size_t i = 0; i < v.size() && i < 30; ++i)
        std::printf("[TrisReport]   shadow %8lld tris  %5d draws  %s\n", v[i].second.first, v[i].second.second, v[i].first.c_str());
    std::fflush(stdout);
}

} // namespace

// A change signature, not an identity: 64-bit words through a multiply-xorshift mix. Every material slot counts: a
// colourway swaps one slot in place (same count), and its cutout or cull state changes the depth.
std::uint64_t LocalCasterHash(std::uint32_t id, const Model* model, const std::vector<std::shared_ptr<MaterialAsset>>& materials,
                              const glm::mat4& xform, unsigned visibleId, bool twoSided) {
    std::uint64_t h = kFnvOffset;
    h = HashWord(h, id);
    h = HashWord(h, (std::uint64_t)(std::uintptr_t)model);
    h = HashWord(h, (std::uint64_t)materials.size());
    for (const auto& m : materials) h = HashWord(h, (std::uint64_t)(std::uintptr_t)m.get());
    // ...and what each mesh's depth draw reads from its material (casts at all + cull: DepthMeshState; cutout, sway:
    // Material::DepthKey), so an edit written into the same MaterialAsset (a cutoff slider, a texture swap, a reload, a
    // cutout map that finished streaming) redraws the light, while a colour edit doesn't.
    if (model)
        for (int i = 0; i < model->MeshCount(); ++i) {
            int cull = 0;
            std::uint64_t stateKey = 0;
            if (!model->DepthMeshState(i, materials, cull, stateKey)) { h = HashWord(h, ~0ull); continue; }
            const bool hasSlot = i < (int)materials.size() && materials[i];
            const Material& mat = hasSlot ? materials[i]->Mat : model->MeshMaterial(i);
            h = HashWord(h, mat.DepthKey() ^ ((std::uint64_t)(std::uint32_t)cull << 40));
        }
    std::uint64_t xw[8];
    std::memcpy(xw, &xform, sizeof xw);
    for (const std::uint64_t w : xw) h = HashWord(h, w);
    h = HashWord(h, visibleId); // the outfit's hidden-triangle set
    return HashWord(h, twoSided ? 1u : 0u);
}

// #109 - the sun's cascades are fitted to the view being RENDERED, per drawScene(): they used to be fitted once per
// frame to one camera while both the Scene and Game views sampled them, so the other view had missing or blurry
// shadows. In the default layout Scene and Game are tabs and only one renders per frame, so this costs nothing extra;
// with both on screen the cascades render twice. A second view with the same matrices in the same frame reuses them.
// RenderSun's steps share one pass: the world and lights, the cascade map and depth program, the profiler stage,
// and what the fit decides (the frame, the cascades drawn this frame, each cascade's frustum and the PCF reach).
namespace {
ShadowFrame::SunGatherTotals g_SunGatherTotals;
} // namespace

struct ShadowFrame::SunPass {
    World& world;
    const FrameLights& fl;
    CascadedShadowMap& map;
    Shader& shader;
    std::optional<Profiler::ScopeTimer>& stage;
    std::uint64_t frame = 0;
    std::uint64_t inFrame = 0; // ShadowFrameInputs::frame: the clock RenderLocal stamps its change log with
    unsigned renderMask = 0;
    Frustum cells[CascadedShadowMap::kMaxCascades];
    int cascadeCount = 0;
    float kernelTexels = 0.0f;
};

bool ShadowFrame::RenderSun(const ShadowFrameInputs& in, const glm::mat4& fitView, const glm::mat4& fitProj) {
    World& world = *in.world;
    const FrameLights& fl = *in.lights;
    if (!(world.Render.ShadowsEnabled && fl.HaveDirectional && fl.SunCastShadows)) return false;
    if (m_LastSunFit.frame == in.frame && m_LastSunFit.view == fitView && m_LastSunFit.proj == fitProj)
        return true; // already fitted to exactly this view this frame
    PROFILE_SCOPE("Sun Shadow Pass");
    PROFILE_GPU_SCOPE("Sun Shadow Pass");
    CascadedShadowMap& shadowMap = *in.sunMap;
    Shader& shadowShader = *in.sunShader;
    // The caller has its own target bound; the pass renders into the cascade array.
    GLTargetScope callerTarget; // its framebuffers + viewport come back on return

    shadowMap.Configure(world.Render.ShadowResolution, world.Render.ShadowCascades);
    // Sub-scopes for the pass's CPU stages (fit + gather, sort + cascade masks, caster draws, grass).
    std::optional<Profiler::ScopeTimer> stage;
    SunPass p{world, fl, shadowMap, shadowShader, stage};
    p.cascadeCount = shadowMap.Count();
    p.kernelTexels = CascadedShadowMap::SunKernelTexels(fl.SunAngularDeg, fl.SunShadowSoftness);
    p.inFrame = in.frame;
    GatherSunCasters(p, fitView, fitProj);
    DrawSunCasters(p);
    DrawSunImpostors(p);
    shadowMap.MarkRendered(p.renderMask, p.frame);
    DrawSunGrass(p, fitView);

    glDisable(GL_POLYGON_OFFSET_FILL);
    glCullFace(GL_BACK);
    GLStateCache::Invalidate();
    m_LastSunFit = {in.frame, fitView, fitProj};
    return true;
}

// The cascades fitted to the view (near planes reaching every occluder), the casters gathered once against them,
// and sorted by model for the per-draw path.
void ShadowFrame::GatherSunCasters(SunPass& p, const glm::mat4& fitView, const glm::mat4& fitProj) {
    World& world = p.world;
    const FrameLights& fl = p.fl;
    CascadedShadowMap& shadowMap = p.map;
    std::optional<Profiler::ScopeTimer>& stage = p.stage;
    stage.emplace("Sun: Fit + Gather");
    // Gather the casters once - registry lookups, world transform, world bounds - and reuse the list for every cascade.
    // #160 - the cascades' near planes reach every occluder: fitted to a box around everything that can draw
    // (RenderableBounds, a cheap superset of the casters), so the casters can then be gathered against the fitted
    // cascades - a forest's casters outside every cascade are never read at all.
    glm::vec3 casterMin(std::numeric_limits<float>::max()), casterMax(-std::numeric_limits<float>::max());
    RenderableBounds(world, casterMin, casterMax);
    shadowMap.SetSplitLambda(world.Render.ShadowCascadeSplit);
    shadowMap.Update(fitView, fitProj, fl.SunDir, world.Render.ShadowDistance, &casterMin, &casterMax);
    p.frame = Time::FrameCount();
    p.renderMask = shadowMap.PlanRender(p.frame); // the far cascades every FarRefresh() frames, staggered
    Frustum (&casterCells)[CascadedShadowMap::kMaxCascades] = p.cells;
    glm::mat4 vp[CascadedShadowMap::kMaxCascades];
    for (int c = 0; c < shadowMap.Count(); ++c) {
        vp[c] = shadowMap.LightViewProj(c);
        casterCells[c] = Frustum::FromViewProj(vp[c]);
    }
    const bool reused = GatherSunCasting(world, vp, shadowMap.Count(), shadowMap.Resolution(), fl.SunDir, casterCells, p.frame);
    m_SceneWorld = &world;
    if (reused && CacheVerify()) VerifySunGather(p);
    // The caster list, and G4-d's held far cascades a moved or re-posed caster forces (FillSunCasters).
    SunListIn li;
    li.world = &world;
    li.frame = p.inFrame;
    li.held = p.renderMask ^ ((1u << shadowMap.Count()) - 1u);
    li.cascadeCount = shadowMap.Count();
    const float kernel = p.kernelTexels, bias = fl.SunShadowNormalBias;
    li.reaches = [&](int c, const glm::vec3& lo, const glm::vec3& hi) {
        return shadowMap.CasterReachesSlice(c, lo, hi, kernel, bias, false);
    };
    li.log = m_SlotDirty && m_ChangeLogFrame == p.inFrame ? &m_ChangeLog : nullptr;
    li.logPrevFrame = m_ChangeLogPrevFrame;
    static const bool noDelta = Env::Flag(Env::Var::NoSunCasterDelta);
    li.allowDelta = !noDelta && MultiDrawEnabled(); // the per-draw path sorts the list below: its order isn't m_Casting's
    const unsigned force = FillSunCasters(li);
    p.renderMask = shadowMap.ForceRender(p.renderMask, force);
    // Consecutive draws of the same model share its VAO and (usually) its material, which the state cache then skips.
    // The multi-draw path doesn't need it: GroupCasters groups by hash in any order, its batches bind by page and state,
    // and a depth-only pass lays down the same depth whatever the order (sorting the casters cost ~0.3 ms in the Forest).
    stage.reset();
    stage.emplace("Sun: Sort + Masks");
    if (!MultiDrawEnabled())
        std::sort(m_SunCasters.begin(), m_SunCasters.end(), [](const SunCaster& a, const SunCaster& b) { return a.model < b.model; });
}

bool ShadowFrame::CascadesInside(const glm::mat4* base, const glm::mat4* now, int n, float margin) {
    const float lim = 1.0f + margin;
    for (int c = 0; c < n; ++c) {
        const glm::mat4 toBase = base[c] * glm::inverse(now[c]); // this frame's clip space -> the base's
        for (int i = 0; i < 8; ++i) {
            glm::vec4 q = toBase * glm::vec4((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f, 1.0f);
            q /= q.w;
            if (!(std::abs(q.x) <= lim && std::abs(q.y) <= lim && std::abs(q.z) <= lim)) return false;
        }
    }
    return true;
}

ShadowFrame::SunGatherTotals ShadowFrame::SunGatherTotalsSoFar() { return g_SunGatherTotals; }

namespace {
// TARTARUS_NO_SUN_GATHER_REUSE=1: the A/B's control, the single gather every frame as before the reuse.
bool SunGatherReuseOff() {
    static const bool off = Env::Flag(Env::Var::NoSunGatherReuse);
    return off;
}
} // namespace

// The static cells: kept from an earlier frame while its padded cascades still hold this frame's (SunGatherHolds),
// else gathered against this frame's cascades padded by kSunGatherPadTexels. Then the Always cells, every frame.
bool ShadowFrame::GatherSunCasting(World& world, const glm::mat4* vp, int count, int resolution, const glm::vec3& sunDir,
                                   const Frustum* cells, std::uint64_t frame) {
    VisibilityQuery casts; // #163 - Cast Shadows: Off skips
    casts.ShadowCasters = true;
    casts.CellCullCount = count;
    SunGatherBase& g = m_SunGather;
    if (SunGatherReuseOff()) { // one gather of every cell against this frame's cascades, nothing kept or drained
        ForgetSunStaticAt();
        g.valid = false;
        casts.CellCull = cells;
        GatherVisible(world, casts, m_Casting);
        ++g_SunGatherTotals.Gathered;
        return false;
    }
    const bool reused = SunGatherHolds(world, vp, count, sunDir, frame);
    if (reused) {
        m_Casting.resize(g.staticCount);
        ++g_SunGatherTotals.Reused;
        if (!m_SunLodToggles.empty()) {
            casts.CellCull = g.padded;
            casts.Part = VisibilityQuery::Cells::Static;
            PatchSunLodToggles(world, casts);
        }
    } else {
        ForgetSunStaticAt();
        g.valid = true;
        g.world = &world;
        g.count = count;
        g.pad = 2.0f * kSunGatherPadTexels / (float)std::max(resolution, 1);
        const glm::mat4 grow = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f / (1.0f + g.pad))); // clip [-1-pad, 1+pad] -> [-1, 1]
        for (int c = 0; c < count; ++c) {
            g.vp[c] = vp[c];
            g.padded[c] = Frustum::FromViewProj(grow * vp[c]);
        }
        g.sunDir = sunDir;
        g.edits = GatherCandidateEdits();
        g.epoch = RenderSettingsEpoch();
        g.frame = frame;
        casts.CellCull = g.padded; // DrawSunCasters' castsInto draws nothing outside every cascade's frustum
        casts.Part = VisibilityQuery::Cells::Static;
        GatherVisible(world, casts, m_Casting);
        g.staticCount = m_Casting.size();
        ++g_SunGatherTotals.Gathered;
    }
    casts.CellCull = cells;
    casts.Part = VisibilityQuery::Cells::Always;
    GatherVisible(world, casts, m_SunAlways);
    m_Casting.insert(m_Casting.end(), m_SunAlways.begin(), m_SunAlways.end());
    return reused;
}

// Whether the kept static part still holds everything a fresh gather would (see kSunGatherPadTexels). The moves and
// LOD flips are drained on every call, so a new base only ever sees what came after it.
bool ShadowFrame::SunGatherHolds(World& world, const glm::mat4* now, int n, const glm::vec3& sunDir, std::uint64_t frame) {
    PROFILE_SCOPE("Sun: Gather Reuse Check");
    SyncGatherCandidates(world); // this frame's moves and edits in before they're read
    bool movesAll = false, lodAll = false;
    TakeStaticCandidateMoves(m_SunMoves, movesAll);
    world.TakeLodToggles(m_SunLodToggles, lodAll, 1);
    const SunGatherBase& g = m_SunGather;
    if (!g.valid || g.world != &world || g.count != n || movesAll || lodAll || g.edits != GatherCandidateEdits() ||
        g.epoch != RenderSettingsEpoch() || g.sunDir != sunDir || frame - g.frame >= kSunGatherFullEvery)
        return false;
    // A texel of the pad kept back: float slack for the plane tests (sub-millimetre against a texel's centimetres).
    if (!CascadesInside(g.vp, now, n, g.pad * (kSunGatherPadTexels - 1.0f) / kSunGatherPadTexels)) return false;
    auto inside = [&](const AABB& b) {
        for (int c = 0; c < n; ++c)
            if (g.padded[c].Intersects(b)) return true;
        return false;
    };
    for (const AABB& b : m_SunMoves)
        if (inside(b)) return false; // a kept caster went stale, or a new one came in
    return true; // LOD level flips (m_SunLodToggles) are patched in by PatchSunLodToggles
}

// The renderers whose LOD level flipped since the last call, patched into the kept static part (m_Casting, still only
// that part): each one a fresh gather against the kept padded cells would hold (`casts`: the same query and cell test)
// is in it with its fields now, each one it wouldn't is out. A flip in the Always cells needs nothing: they're gathered
// every frame. The order isn't kept (a removal moves the last entry in): the list's users don't depend on it.
void ShadowFrame::PatchSunLodToggles(World& world, const VisibilityQuery& casts) {
    PROFILE_SCOPE("Sun: Gather LOD Patch");
    if (!m_SunStaticAtBuilt) { // slot -> index, built on the first patch since the gather
        for (std::size_t i = 0; i < m_Casting.size(); ++i) {
            const std::uint32_t s = m_Casting[i].Slot;
            if (s >= m_SunStaticAt.size()) m_SunStaticAt.resize((std::size_t)s + 1, -1);
            m_SunStaticAt[s] = (std::int32_t)i;
        }
        m_SunStaticAtBuilt = true;
    }
    bool patched = false;
    VisibleRenderable v;
    for (const entt::entity e : m_SunLodToggles) {
        std::uint32_t slot = 0;
        const int take = GatherStaticCandidate(world, casts, e, slot, v);
        if (take < 0) continue; // no static-cell renderable
        if (slot >= m_SunStaticAt.size()) m_SunStaticAt.resize((std::size_t)slot + 1, -1);
        const std::int32_t at = m_SunStaticAt[slot];
        if (take == 1) {
            if (at >= 0) {
                m_Casting[(std::size_t)at] = v;
            } else {
                m_SunStaticAt[slot] = (std::int32_t)m_Casting.size();
                m_Casting.push_back(v);
            }
            patched = true;
        } else if (at >= 0) {
            const std::size_t last = m_Casting.size() - 1;
            if ((std::size_t)at != last) {
                m_Casting[(std::size_t)at] = m_Casting[last];
                m_SunStaticAt[m_Casting[(std::size_t)at].Slot] = at;
            }
            m_Casting.pop_back();
            m_SunStaticAt[slot] = -1;
            patched = true;
        }
    }
    m_SunGather.staticCount = m_Casting.size();
    if (patched) ++g_SunGatherTotals.Patched;
}

// The kept static part's slot index (PatchSunLodToggles) forgotten before m_Casting is gathered anew: only the entries
// it set are cleared, not the whole table.
void ShadowFrame::ForgetSunStaticAt() {
    if (!m_SunStaticAtBuilt) return;
    for (std::size_t i = 0; i < m_SunGather.staticCount && i < m_Casting.size(); ++i)
        if (m_Casting[i].Slot < m_SunStaticAt.size()) m_SunStaticAt[m_Casting[i].Slot] = -1;
    m_SunStaticAtBuilt = false;
}

// Verify mode: this frame's fresh gather against the kept list. Every caster it holds that can draw (DrawSunCasters'
// test: its bounds in some cascade's frustum and reaching that cascade's slice) must be in the list with the same fields,
// and every extra the list holds must fail that test for each cascade. A caster the fresh gather holds only because its
// ground cell's box touches a cascade - its own bounds miss them all - is counted, not an error: it draws nothing. So is
// one held with the fields it had before a small move outside the padded cascades: it draws from neither place.
void ShadowFrame::VerifySunGather(SunPass& p) {
    VisibilityQuery casts;
    casts.ShadowCasters = true;
    casts.CellCull = p.cells;
    casts.CellCullCount = p.cascadeCount;
    GatherVisible(p.world, casts, m_SunVerify);
    m_SunVerifyIndex.assign(m_SunVerifyIndex.size(), -1);
    for (std::size_t i = 0; i < m_Casting.size(); ++i) {
        const std::uint32_t s = m_Casting[i].Slot;
        if (s >= m_SunVerifyIndex.size()) m_SunVerifyIndex.resize((std::size_t)s + 1, -1);
        m_SunVerifyIndex[s] = (std::int32_t)i;
    }
    const float kernel = p.kernelTexels, bias = p.fl.SunShadowNormalBias;
    auto draws = [&](const VisibleRenderable& v) {
        if (!v.Bounded) return true;
        for (int c = 0; c < p.cascadeCount; ++c)
            if (p.cells[c].Intersects(AABB{v.WorldMin, v.WorldMax}) && p.map.CasterReachesSlice(c, v.WorldMin, v.WorldMax, kernel, bias))
                return true;
        return false;
    };
    std::uint64_t missing = 0, extraDraws = 0;
    for (const VisibleRenderable& f : m_SunVerify) {
        const std::int32_t i = f.Slot < m_SunVerifyIndex.size() ? m_SunVerifyIndex[f.Slot] : -1;
        const VisibleRenderable* k = i >= 0 ? &m_Casting[(std::size_t)i] : nullptr;
        if (!k) {
            if (draws(f)) ++missing;
            else ++g_SunGatherTotals.VerifyAbsentNoDraw;
        } else if (k->Entity != f.Entity || k->Renderable != f.Renderable) {
            ++missing; // another caster in its slot
        } else if (k->Xform != f.Xform || k->Bounded != f.Bounded || k->WorldMin != f.WorldMin || k->WorldMax != f.WorldMax) {
            // Held as it was: an error if it draws from either place. One that moved a little outside the padded
            // cascades (SunGatherHolds lets that pass) keeps its old fields, and draws nothing from either.
            if (draws(f) || draws(*k)) ++missing;
            else ++g_SunGatherTotals.VerifyAbsentNoDraw;
        }
        if (i >= 0) m_SunVerifyIndex[f.Slot] = -2; // in both
    }
    for (const VisibleRenderable& k : m_Casting)
        if (m_SunVerifyIndex[k.Slot] != -2 && draws(k)) ++extraDraws;
    ++g_SunGatherTotals.VerifyChecked;
    g_SunGatherTotals.VerifyMissing += missing;
    g_SunGatherTotals.VerifyExtraDraws += extraDraws;
    if (missing || extraDraws) {
        char line[256];
        std::snprintf(line, sizeof line, "Sun gather reuse verify: frame %llu, %llu caster(s) missing or stale, %llu extra(s) that draw",
                      (unsigned long long)Time::FrameCount(), (unsigned long long)missing, (unsigned long long)extraDraws);
        Log::Error(line);
    }
}

namespace {
ShadowFrame::SunListTotals g_SunListTotals;
bool RealSkinned(const Model* m) { return m->HasBones(); }
std::uint64_t RealPose(const Model* m) { return m->HasBones() ? m->PoseVersion() : 0; }
} // namespace

ShadowFrame::SunListTotals ShadowFrame::SunListTotalsSoFar() { return g_SunListTotals; }

// One caster's fields: the full loop and the delta path both fill a caster only through here.
void ShadowFrame::FillSunCaster(World& world, const VisibleRenderable& v, SunCaster& sc) {
    const auto& r = *v.Renderable;
    sc.model = r.ModelRef.get();
    sc.materials = &r.Materials;
    const auto* hide = world.Registry.try_get<OutfitHideTag>(v.Entity);
    sc.visible = hide ? hide->Visible.get() : nullptr;
    sc.xform = v.Xform;
    sc.slot = v.Slot;
    // #163 - Cast Shadows: Two Sided draws without back-face culling.
    sc.twoSided = r.CastShadows == RenderableComponent::ShadowCasting::TwoSided;
    // As posed (Model::PosedBounds), the bounds the main pass culls with.
    sc.bounded = v.Bounded;
    sc.bounds = AABB{v.WorldMin, v.WorldMax};
}

// G4-d: the held far layers a caster that moved or re-posed since its last gather reaches (CasterReachesSlice, what
// receivers read now - not the hold margin), where it was or is now: its old shadow must go as its new one lands. One
// new to its slot (first seen, a world switch) counts as moved. Unbounded movers reach every cascade.
void ShadowFrame::TrackSunMotion(const SunListIn& in, const VisibleRenderable& v, const SunCaster& sc, unsigned& force) {
    auto reach = [&](bool bounded, const glm::vec3& lo, const glm::vec3& hi) {
        for (int c = 2; c < in.cascadeCount; ++c)
            if ((in.held & ~force & (1u << c)) && (!bounded || in.reaches(c, lo, hi))) force |= 1u << c;
    };
    if (v.Slot >= m_SunMotion.size()) m_SunMotion.resize((size_t)v.Slot + 1);
    SunMotion& m = m_SunMotion[v.Slot];
    const std::uint32_t id = static_cast<std::uint32_t>(v.Entity);
    const std::uint64_t pose = (in.probe.Pose ? in.probe.Pose : RealPose)(sc.model);
    if (m.id != id || m.pose != pose || m.xform != sc.xform) {
        if (in.held & ~force) {
            reach(sc.bounded, v.WorldMin, v.WorldMax);
            if (m.id == id) reach(m.bounded, m.lo, m.hi);
        }
        m = SunMotion{id, pose, sc.xform, v.WorldMin, v.WorldMax, sc.bounded};
    }
}

unsigned ShadowFrame::FillSunCasters(const SunListIn& in) {
    PROFILE_SCOPE("Sun: Caster List");
    World& world = *in.world;
    bool (*skinned)(const Model*) = in.probe.Skinned ? in.probe.Skinned : RealSkinned;
    unsigned force = 0;

    // The outfit-hidden entities now (a handful): a tag added or removed is a full refill.
    static std::vector<std::uint32_t> hidden;
    hidden.clear();
    for (const entt::entity e : world.Registry.view<OutfitHideTag>()) hidden.push_back(static_cast<std::uint32_t>(e));
    std::sort(hidden.begin(), hidden.end());

    // The delta path: only when every check holds; anything else is the keyed path or the full loop below. `sameRun`:
    // every check but the gathered set itself, which is all the keyed path needs.
    const bool sameRun = in.allowDelta && m_SunListValid && in.log && !in.log->All && m_SunListFrame == in.logPrevFrame &&
                         in.frame > m_SunFullFrame && in.frame - m_SunFullFrame < kSunFullEvery &&
                         m_SunSetEpoch == RenderSettingsEpoch() && hidden == m_SunHiddenSet && m_SunCasters.size() == m_SunSet.size();
    m_SunLastKeyed = false;
    bool delta = sameRun && m_SunSet.size() == m_Casting.size();
    for (std::size_t i = 0; delta && i < m_Casting.size(); ++i) {
        const VisibleRenderable& v = m_Casting[i];
        delta = m_SunSet[i] == SunSetEntry{v.Slot, static_cast<std::uint32_t>(v.Entity), v.Renderable};
    }
    if (delta) {
        // Refill and re-check what can differ: the casters the change log names (moved, re-bounded, posed), and every
        // skinned and outfit-hidden one. The rest are as the list holds them, and the motion check would find them
        // unchanged (same entity, pose 0, same transform).
        auto redo = [&](std::uint32_t i) {
            FillSunCaster(world, m_Casting[i], m_SunCasters[i]);
            TrackSunMotion(in, m_Casting[i], m_SunCasters[i], force);
        };
        for (const GpuScene::SlotChange& c : in.log->Changes)
            if (c.Slot < m_SunSlotIndex.size() && m_SunSlotIndex[c.Slot] >= 0) redo((std::uint32_t)m_SunSlotIndex[c.Slot]);
        for (const std::uint32_t i : m_SunAlwaysCheck) redo(i);
        m_SunListFrame = in.frame;
        m_SunLastDelta = true;
        ++g_SunListTotals.Delta;
        return force;
    }

    if (sameRun) {
        // The keyed path: the set changed, nothing else did. By slot: a caster the list held there - the same set entry
        // (slot, entity, renderable), not in this frame's change log, not skinned or outfit-hidden - holds exactly what
        // the full loop would refill it to (nothing unlogged changes a caster's fields), and its motion check would
        // find it unchanged, so it is copied. The list holds no per-cascade data: a moved camera or cascade only
        // matters through TrackSunMotion, which acts on a changed caster alone. The rest: filled and checked as below.
        if (m_SunSlotLogged.size() < m_SunSlotIndex.size()) m_SunSlotLogged.resize(m_SunSlotIndex.size(), 0);
        for (const GpuScene::SlotChange& c : in.log->Changes)
            if (c.Slot < m_SunSlotLogged.size()) m_SunSlotLogged[c.Slot] = 1;
        m_SunOldAlways.assign(m_SunSet.size(), 0);
        for (const std::uint32_t i : m_SunAlwaysCheck) m_SunOldAlways[i] = 1;
        m_SunCastersNext.clear();
        m_SunCastersNext.reserve(m_Casting.size());
        std::uint64_t copied = 0;
        for (const VisibleRenderable& v : m_Casting) {
            const std::int32_t j = v.Slot < m_SunSlotIndex.size() ? m_SunSlotIndex[v.Slot] : -1;
            if (j >= 0 && m_SunSet[(std::size_t)j] == SunSetEntry{v.Slot, static_cast<std::uint32_t>(v.Entity), v.Renderable} &&
                !m_SunSlotLogged[v.Slot] && !m_SunOldAlways[(std::size_t)j]) {
                m_SunCastersNext.push_back(m_SunCasters[(std::size_t)j]);
                ++copied;
                continue;
            }
            SunCaster& sc = m_SunCastersNext.emplace_back();
            FillSunCaster(world, v, sc);
            TrackSunMotion(in, v, sc, force);
        }
        for (const GpuScene::SlotChange& c : in.log->Changes)
            if (c.Slot < m_SunSlotLogged.size()) m_SunSlotLogged[c.Slot] = 0;
        m_SunCasters.swap(m_SunCastersNext);
        RebuildSunSet(hidden, skinned, false);
        m_SunListFrame = in.frame;
        m_SunLastDelta = false;
        m_SunLastKeyed = true;
        ++g_SunListTotals.Keyed;
        g_SunListTotals.KeyedCopied += copied;
        g_SunListTotals.KeyedFilled += m_Casting.size() - copied;
        return force;
    }

    // The full loop (the reference): every caster filled and checked.
    m_SunCasters.clear();
    for (const VisibleRenderable& v : m_Casting) {
        SunCaster& sc = m_SunCasters.emplace_back();
        FillSunCaster(world, v, sc);
        TrackSunMotion(in, v, sc, force);
    }
    RebuildSunSet(hidden, skinned, true);
    m_SunHiddenSet = hidden;
    m_SunSetEpoch = RenderSettingsEpoch();
    m_SunListValid = true;
    m_SunListFrame = m_SunFullFrame = in.frame;
    m_SunLastDelta = false;
    ++g_SunListTotals.Full;
    return force;
}

// What the next frames' delta and keyed paths compare against and re-check.
void ShadowFrame::RebuildSunSet(const std::vector<std::uint32_t>& hidden, bool (*skinned)(const Model*), bool resetAll) {
    if (resetAll) {
        for (std::int32_t& s : m_SunSlotIndex) s = -1;
    } else {
        for (const SunSetEntry& e : m_SunSet)
            if (e.slot < m_SunSlotIndex.size()) m_SunSlotIndex[e.slot] = -1;
    }
    m_SunSet.resize(m_Casting.size());
    m_SunAlwaysCheck.clear();
    for (std::size_t i = 0; i < m_Casting.size(); ++i) {
        const VisibleRenderable& v = m_Casting[i];
        m_SunSet[i] = SunSetEntry{v.Slot, static_cast<std::uint32_t>(v.Entity), v.Renderable};
        if (v.Slot >= m_SunSlotIndex.size()) m_SunSlotIndex.resize((std::size_t)v.Slot + 1, -1);
        m_SunSlotIndex[v.Slot] = (std::int32_t)i;
        if (skinned(m_SunCasters[i].model) || m_SunCasters[i].visible ||
            std::binary_search(hidden.begin(), hidden.end(), static_cast<std::uint32_t>(v.Entity)))
            m_SunAlwaysCheck.push_back((std::uint32_t)i);
    }
}

bool ShadowFrame::SameSunListAs(const ShadowFrame& o) const {
    if (m_SunCasters.size() != o.m_SunCasters.size()) return false;
    for (std::size_t i = 0; i < m_SunCasters.size(); ++i) {
        const SunCaster& a = m_SunCasters[i];
        const SunCaster& b = o.m_SunCasters[i];
        if (a.model != b.model || a.materials != b.materials || a.visible != b.visible || a.xform != b.xform ||
            a.bounds.Min != b.bounds.Min || a.bounds.Max != b.bounds.Max || a.bounded != b.bounded || a.twoSided != b.twoSided ||
            a.slot != b.slot)
            return false;
    }
    const std::size_t n = std::max(m_SunMotion.size(), o.m_SunMotion.size());
    for (std::size_t s = 0; s < n; ++s) {
        const SunMotion none{};
        const SunMotion& a = s < m_SunMotion.size() ? m_SunMotion[s] : none;
        const SunMotion& b = s < o.m_SunMotion.size() ? o.m_SunMotion[s] : none;
        if (a.id != b.id || a.pose != b.pose || a.xform != b.xform || a.lo != b.lo || a.hi != b.hi || a.bounded != b.bounded)
            return false;
    }
    return true;
}

// The casters into the cascades: one layered pass (each caster once, instanced over its cascades), or one pass per
// cascade where layered rendering isn't supported.
void ShadowFrame::DrawSunCasters(SunPass& p) {
    const FrameLights& fl = p.fl;
    CascadedShadowMap& shadowMap = p.map;
    Shader& shadowShader = p.shader;
    std::optional<Profiler::ScopeTimer>& stage = p.stage;
    const unsigned renderMask = p.renderMask;
    const Frustum (&casterCells)[CascadedShadowMap::kMaxCascades] = p.cells;
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    // Store the LIGHT-FACING surface (back-face cull), matching the spot/point passes. Front-face culling stored a
    // caster's far side, which under any convex/curved occluder (sphere worst) sits far enough behind the contact that
    // the shadow detached from the object's base at grazing sun angles - the #134 sun-bay gap. SunShadow()'s bias is
    // texel-proportional and now carries the acne protection; this polygon offset is just a hair of extra slack.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 2.0f);

    shadowShader.Bind();
    // #194: resolve this hot uniform location once, outside the cascade loop below.
    const int shadowLightViewProjLoc = shadowShader.Loc("uLightViewProj");
    const int cascadeCount = p.cascadeCount;
    // A caster goes into a cascade only if its shadow can land where that cascade is read
    // (CascadedShadowMap::CasterReachesSlice), with the PCF kernel SunShadow uses.
    const float sunKernelTexels = p.kernelTexels;
    auto castsInto = [&](const SunCaster& sc, int c, const Frustum& cascadeFrustum) {
        if (!(renderMask & (1u << c))) return false; // held from last frame
        if (!sc.bounded) return true;
        return cascadeFrustum.Intersects(sc.bounds) &&
               shadowMap.CasterReachesSlice(c, sc.bounds.Min, sc.bounds.Max, sunKernelTexels, fl.SunShadowNormalBias);
    };
    if (CascadedShadowMap::LayeredSupported()) {
        // One pass for every cascade: each caster is drawn once, instanced over the consecutive cascades its bounds
        // touch, and the vertex shader routes each instance to its layer (gl_Layer) - a quarter of the draw calls of
        // the per-cascade loop below, which remains the fallback.
        shadowMap.BeginLayered(renderMask);
        glm::mat4 cascadeVP[CascadedShadowMap::kMaxCascades];
        for (int c = 0; c < cascadeCount; ++c) cascadeVP[c] = shadowMap.LightViewProj(c);
        shadowShader.SetMat4Array("uCascadeViewProj[0]", cascadeCount, cascadeVP);
        const int cascadeLayeredLoc = shadowShader.Loc("uCascadeLayered");
        shadowShader.SetInt(cascadeLayeredLoc, 1);
        const int firstLoc = shadowShader.Loc("uCascadeFirst");
        m_Draws.clear();
        for (const SunCaster& sc : m_SunCasters) {
            unsigned mask = 0;
            for (int c = 0; c < cascadeCount; ++c)
                if (castsInto(sc, c, casterCells[c])) mask |= 1u << c;
            if (!mask) continue;
            // One draw per run of consecutive cascades (almost always one run), instanced over them; the copies of a
            // model with the same run draw together (GPU instancing).
            for (int c = 0; c < cascadeCount;) {
                if (!(mask & (1u << c))) { ++c; continue; }
                int end = c;
                while (end + 1 < cascadeCount && (mask & (1u << (end + 1)))) ++end;
                m_Draws.push_back({sc.model, sc.materials, sc.visible, &sc.xform, sc.twoSided, c, end - c + 1, sc.slot});
                c = end + 1;
            }
        }
        // TARTARUS_TRIS_REPORT=<frame>: the sun casters' triangles on that frame, per cascade and by model.
        if (static const int reportAt = Env::Int(Env::Var::TrisReport, -1);
            reportAt >= 0 && Time::FrameCount() == (std::uint64_t)reportAt)
            ReportSunTris(m_Draws, cascadeCount);
        // G4-a verify (TARTARUS_GPU_CULL_VERIFY, or the smoke check's one-shot): the GPU's per-cascade caster sets
        // against these draws' runs. Waits on the GPU; nothing draws from it yet.
        if (GpuCull::SunVerifyDue()) {
            static std::vector<GpuCull::SunPair> pairs;
            pairs.clear();
            for (const DepthCaster& d : m_Draws)
                for (int c = d.LayerFirst; c < d.LayerFirst + d.Layers; ++c) pairs.push_back({d.Slot, (std::uint32_t)c});
            GpuCull::VerifySunCasters(*m_SceneWorld,
                                      GpuCull::MakeSunCasterBlock(shadowMap, casterCells, renderMask, sunKernelTexels,
                                                                  fl.SunShadowNormalBias),
                                      pairs);
            shadowShader.Bind(); // the verify ran its own program
        }
        // Each cascade rasterises only the texels its slice of the view can read (CascadedShadowMap::
        // BeginLayeredScissor): the rest of a layer is never sampled, and the forest's terrain and canopy filled all
        // of it - most of the pass's fill cost.
        static const bool sliceScissor = CascadedShadowMap::ViewportIndexSupported();
        const int cascadeViewportLoc = sliceScissor ? shadowShader.Loc("uCascadeViewport") : -1;
        if (sliceScissor) {
            shadowMap.BeginLayeredScissor(sunKernelTexels, fl.SunShadowNormalBias);
            shadowShader.SetInt(cascadeViewportLoc, 1);
        }
        stage.reset();
        stage.emplace("Sun: Draw Casters");
        // TARTARUS_NO_SUN_CASTER_CACHE=1: group and build them every frame (A/B).
        static const bool noCache = Env::Flag(Env::Var::NoSunCasterCache);
        DepthCasterCache* cache = noCache ? nullptr : &m_SunCasterCache[renderMask & ((1u << CascadedShadowMap::kMaxCascades) - 1u)];
        DrawDepthCasters(shadowShader, m_Draws, firstLoc, m_SceneWorld, cache);
        if (sliceScissor) {
            shadowShader.SetInt(cascadeViewportLoc, 0);
            CascadedShadowMap::EndLayeredScissor();
        }
        // The SSAO depth pre-pass reuses this program with uLightViewProj.
        shadowShader.SetInt(cascadeLayeredLoc, 0);
    } else {
        for (int c = 0; c < cascadeCount; ++c) {
            if (!(renderMask & (1u << c))) continue; // held from last frame
            shadowMap.Begin(c);
            shadowShader.SetMat4(shadowLightViewProjLoc, shadowMap.LightViewProj(c));
            // Cull each cascade's caster list against that cascade's own ortho frustum - the near slice covers a few
            // metres yet used to redraw the whole level x4. Animated casters cull on their padded bounds too.
            const Frustum& cascadeFrustum = casterCells[c];
            m_Draws.clear();
            for (const SunCaster& sc : m_SunCasters)
                if (castsInto(sc, c, cascadeFrustum))
                    m_Draws.push_back({sc.model, sc.materials, sc.visible, &sc.xform, sc.twoSided, 0, 1, sc.slot});
            DrawDepthCasters(shadowShader, m_Draws, -1, m_SceneWorld);
        }
    }
}

void ShadowFrame::DrawSunImpostors(SunPass& p) {
    World& world = p.world;
    const FrameLights& fl = p.fl;
    CascadedShadowMap& shadowMap = p.map;
    Shader& shadowShader = p.shader;
    const unsigned renderMask = p.renderMask;
    const Frustum (&casterCells)[CascadedShadowMap::kMaxCascades] = p.cells;
    const int cascadeCount = p.cascadeCount;
    const float sunKernelTexels = p.kernelTexels;
    // Octahedral impostors (ImpostorRenderer): the trees ApplyLod put on theirs cast as the sun sees their frames.
    {
        glm::mat4 cascadeVP[CascadedShadowMap::kMaxCascades];
        for (int c = 0; c < cascadeCount; ++c) cascadeVP[c] = shadowMap.LightViewProj(c);
        ImpostorRenderer::Get().DrawSun(
            world, cascadeVP, casterCells, cascadeCount, renderMask, -fl.SunDir,
            [&](int c, const glm::vec3& lo, const glm::vec3& hi) {
                return shadowMap.CasterReachesSlice(c, lo, hi, sunKernelTexels, fl.SunShadowNormalBias);
            },
            [&](int c) {
                shadowMap.Resume(c);
                int r[4];
                shadowMap.SliceScissor(c, sunKernelTexels, fl.SunShadowNormalBias, r);
                glScissor(r[0], r[1], r[2], r[3]);
                glEnable(GL_SCISSOR_TEST);
            });
        glDisable(GL_SCISSOR_TEST);
        shadowShader.Bind();
    }
}

void ShadowFrame::DrawSunGrass(SunPass& p, const glm::mat4& fitView) {
    World& world = p.world;
    CascadedShadowMap& shadowMap = p.map;
    Shader& shadowShader = p.shader;
    std::optional<Profiler::ScopeTimer>& stage = p.stage;
    const unsigned renderMask = p.renderMask;
    const int cascadeCount = p.cascadeCount;
    // Grass (GrassRenderer): its blades into the nearest cascades, scattered and culled against each cascade's own
    // box (thinned by distance from the view's eye, as the view sees them), so the field shades itself and the
    // ground under it. Farther out a blade is under a texel and its shadow is only noise.
    stage.reset();
    {
        PROFILE_SCOPE("Sun: Grass");
        GrassRenderer& grass = GrassRenderer::Get();
        grass.Gather(world);
        if (grass.HasAreas()) {
            PROFILE_GPU_SCOPE("Grass Shadow");
            const glm::vec3 eye = glm::vec3(glm::inverse(fitView)[3]);
            const int grassCascades = std::min(cascadeCount, GrassRenderer::kShadowCascades);
            for (int c = 0; c < grassCascades; ++c) {
                if (!(renderMask & (1u << c))) continue;
                const glm::mat4 vp = shadowMap.LightViewProj(c);
                if (!grass.Cull(glm::mat4(1.0f), vp, eye, GrassRenderer::kShadowDistance)) continue;
                shadowMap.Resume(c);
                // The nearest cascade's texels are a few millimetres: full blades. Past it, two-segment ones.
                grass.DrawShadow(vp, c == 0 ? GrassRenderer::kNearSegments : 2);
            }
            shadowShader.Bind();
        }
    }
}

// Drawn with the moving casters for this light, this frame: animated, moving, or settling before the light's turn
// (kSettleFrames). `turnSlot` spreads the lights over the turns (point lights offset from spots). A settling caster
// stays with the moving ones until the light's first turn since it settled, and is static from then on (so the
// light's static signature changes once, on its turn).
bool ShadowFrame::DynamicFor(const LocalCaster& c, int turnSlot, std::uint64_t frame) const {
    if (c.animated || c.moving) return true;
    if (!c.settling) return false;
    const std::uint64_t k = kSettleFrames;
    const std::uint64_t turn = c.settleStart + ((std::uint64_t)turnSlot % k + k - c.settleStart % k) % k;
    return frame < turn;
}

namespace {
bool g_CacheVerify = Env::Flag(Env::Var::ShadowCacheVerify);
ShadowFrame::VerifyTotals g_VerifyTotals;
ShadowFrame::QuietTotals g_QuietTotals;
} // namespace

bool ShadowFrame::SlotDirtyEnabled() {
    static const bool off = Env::Flag(Env::Var::NoShadowSlotDirty);
    return !off && GpuScene::Enabled();
}
void ShadowFrame::SetCacheVerify(bool on) { g_CacheVerify = on; }
bool ShadowFrame::CacheVerify() { return g_CacheVerify; }
const ShadowFrame::VerifyTotals& ShadowFrame::CacheVerifyTotals() { return g_VerifyTotals; }
const ShadowFrame::QuietTotals& ShadowFrame::LocalQuietTotals() { return g_QuietTotals; }

void ShadowFrame::VerifyQuiet(bool wasQuiet, bool drawn, const char* kind, int light, std::uint64_t frame,
                              std::vector<VerifyCaster>& last) {
    if (wasQuiet) ++g_VerifyTotals.Checked;
    if (wasQuiet && drawn && ++g_VerifyTotals.Missed <= 20) { // the first few: a stuck light would fill the log every frame
        // What differs from the light's last look: a caster added, removed, or one of its signature's fields.
        std::string why;
        int listed = 0;
        auto note = [&](const std::string& s) { if (listed++ < 4) why += " " + s + ";"; };
        auto name = [](const VerifyCaster& c) {
            return "entity " + std::to_string(c.Id) + " (" + (c.Mesh ? c.Mesh->Path() : std::string("?")) + ")";
        };
        for (const VerifyCaster& n : m_VerifyNow) {
            const auto it = std::find_if(last.begin(), last.end(), [&](const VerifyCaster& o) { return o.Id == n.Id; });
            if (it == last.end()) { note("added " + name(n)); continue; }
            std::string f;
            if (it->Mesh != n.Mesh) f += " model";
            if (it->Materials != n.Materials) f += " materials";
            if (it->Xform != n.Xform) f += " transform";
            if (it->Visible != n.Visible) f += " hidden-triangles";
            if (it->TwoSided != n.TwoSided) f += " two-sided";
            if (!f.empty()) note(name(n) + ":" + f);
        }
        for (const VerifyCaster& o : last)
            if (std::none_of(m_VerifyNow.begin(), m_VerifyNow.end(), [&](const VerifyCaster& n) { return n.Id == o.Id; }))
                note("removed " + name(o));
        if (listed > 4) why += " +" + std::to_string(listed - 4) + " more;";
        Log::Error(std::string("[ShadowCache] ") + kind + " light " + std::to_string(light) + " was quiet but its map changed on frame " +
                   std::to_string(frame) + " (why bits " + std::to_string(m_LookWhy) + ", " + std::to_string(last.size()) + " -> " +
                   std::to_string(m_VerifyNow.size()) + " static casters):" + (why.empty() ? std::string(" no static caster changed") : why));
    }
    last = m_VerifyNow;
}

void LocalShadowQuiet::Reset() {
    for (Light& l : Spots) l.Quiet = false;
    for (Light& l : Points) l.Quiet = false;
}

void LocalShadowQuiet::Wake(const GpuScene::ChangeLog& log, std::uint32_t epoch, const Lights& in) {
    const bool all = log.All || !EpochSeen || epoch != Epoch;
    Epoch = epoch;
    EpochSeen = true;
    int quiet = 0;
    auto take = [&](Light& l, bool live, std::uint64_t key) {
        if (!live || all || l.Key != key) l.Quiet = false;
        if (live) l.Key = key;
        quiet += l.Quiet ? 1 : 0;
    };
    for (int s = 0; s < SpotShadowMap::kMaxSpots; ++s) {
        const bool live = s < in.SpotCount && in.SpotLive[s];
        take(Spots[s], live, live ? in.SpotKey[s] : 0);
    }
    for (int p = 0; p < PointShadowMap::kMaxPoints; ++p) {
        const bool live = p < in.PointCount && in.PointLive[p];
        take(Points[p], live, live ? in.PointKey[p] : 0);
    }
    // Each change against each still-quiet light: where the renderable was or is now inside its reach wakes it.
    for (const GpuScene::SlotChange& c : log.Changes) {
        if (quiet == 0) return;
        const AABB box{c.Min, c.Max};
        for (int s = 0; s < in.SpotCount; ++s)
            if (Spots[s].Quiet && (!c.Bounded || in.SpotReach[s].Intersects(box))) { Spots[s].Quiet = false; --quiet; }
        for (int p = 0; p < in.PointCount; ++p)
            if (Points[p].Quiet && (!c.Bounded || in.PointRange[p].Intersects(box))) { Points[p].Quiet = false; --quiet; }
    }
}

void LodToggleWake::Append(World& world, GpuScene::ChangeLog& log) {
    bool all = false;
    world.TakeLodToggles(Toggled, all);
    if (For != &world) { all = true; For = &world; } // another world's entities, or the first look
    if (all) { // re-read the whole set; every light wakes anyway
        std::fill(Seen.begin(), Seen.end(), entt::null);
        for (entt::entity e : world.Registry.view<LodCulledTag>()) {
            const size_t s = (size_t)entt::to_entity(e);
            if (s >= Seen.size()) Seen.resize(s + 1, entt::null);
            Seen[s] = e;
        }
        log.All = true;
        log.Changes.clear();
        return;
    }
    for (entt::entity e : Toggled) {
        const size_t s = (size_t)entt::to_entity(e);
        if (s >= Seen.size()) Seen.resize(s + 1, entt::null);
        const bool now = world.IsLodCulled(e), was = Seen[s] == e;
        if (now == was) continue; // flipped back since the last look
        Seen[s] = now ? e : entt::null;
        if (log.All) continue;
        const auto* r = world.Registry.valid(e) ? world.Registry.try_get<RenderableComponent>(e) : nullptr;
        if (!r || !r->ModelRef || r->CastShadows == RenderableComponent::ShadowCasting::Off) continue; // casts nothing
        GpuScene::SlotChange c;
        c.Slot = ~0u; // no slot moved: the renderer itself
        glm::vec3 lo, hi;
        r->ModelRef->PosedBounds(lo, hi);
        c.Bounded = lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z;
        if (c.Bounded) {
            const AABB wb = AABB{lo, hi}.Transformed(world.GetCachedWorldTransform(e));
            c.Min = wb.Min;
            c.Max = wb.Max;
        }
        if (log.Changes.size() >= GpuScene::kMaxLoggedChanges) { log.All = true; log.Changes.clear(); continue; }
        log.Changes.push_back(c);
    }
}

void ShadowFrame::RenderLocal(const ShadowFrameInputs& in) {
    World& world = *in.world;
    const FrameLights& fl = *in.lights;
    // Layers are allocated for the budget, not the hard ceiling (#110); a zero budget still keeps one layer so
    // sampler unit 9 is never unbound.
    if (world.Render.ShadowsEnabled) {
        in.spotMap->Configure(std::min(world.Render.ShadowResolution, 4096), std::max(fl.SpotCount, 1)); // sized to the slots in use (grow-only)
        in.pointMap->Configure(std::min(world.Render.ShadowResolution, 1024), std::max(fl.PointBudget, 1));
    }
    m_LocalCasters.clear();
    m_Stats = {};
    if (!world.Render.ShadowsEnabled || (fl.SpotCount <= 0 && fl.PointCount <= 0)) {
        m_Quiet.Reset();
        return;
    }
    // Each light's reach (a spot's map frustum, the box of a point's range) and the key of its own inputs: the map,
    // its size, the light's matrices, position and range (and for a spot, whether it leaves the player out).
    const unsigned spotArray = in.spotMap->DepthArray(), pointArray = in.pointMap->DepthCubeArray();
    const int spotRes = in.spotMap->Resolution() * 64 + in.spotMap->Layers();
    const int pointRes = in.pointMap->Resolution() * 64 + in.pointMap->Cubes();
    for (int s = 0; s < fl.SpotCount; ++s) {
        m_SpotReach[s] = Frustum::FromViewProj(fl.SpotVP[s]);
        const bool torch = s == fl.TorchShadowSlot;
        std::uint64_t k = HashBytes(kFnvOffset, &spotArray, sizeof spotArray);
        k = HashBytes(k, &spotRes, sizeof spotRes);
        k = HashBytes(k, &fl.SpotVP[s], sizeof fl.SpotVP[s]);
        k = HashBytes(k, &fl.SpotPos[s], sizeof fl.SpotPos[s]);
        k = HashBytes(k, &fl.SpotFar[s], sizeof fl.SpotFar[s]);
        m_SpotKey[s] = HashBytes(k, &torch, sizeof torch);
    }
    for (int p = 0; p < fl.PointCount; ++p) {
        m_PointRange[p] = AABB{fl.PointPos[p] - glm::vec3(fl.PointFar[p]), fl.PointPos[p] + glm::vec3(fl.PointFar[p])};
        std::uint64_t k = HashBytes(kFnvOffset, &pointArray, sizeof pointArray);
        k = HashBytes(k, &pointRes, sizeof pointRes);
        k = HashBytes(k, &fl.PointPos[p], sizeof fl.PointPos[p]);
        k = HashBytes(k, &fl.PointNear[p], sizeof fl.PointNear[p]);
        m_PointKey[p] = HashBytes(k, &fl.PointFar[p], sizeof fl.PointFar[p]);
    }
    // G4-c: the lights nothing touched since their last look keep their maps without a gather or a signature.
    m_SlotDirty = SlotDirtyEnabled();
    if (m_SlotDirty) {
        PROFILE_SCOPE("Local Shadow Wake");
        GpuScene& scene = GpuScene::Get();
        {
            // Its own scope: most of Wake's cost is this Sync, which moved here from the draw's SyncOnce (that one now
            // finds the scene up to date), so it isn't new work.
            PROFILE_SCOPE("Local Shadow Wake Sync");
            scene.Sync(world); // this frame's changes, posed slots included (SyncOnce skips a frame whose transforms held)
            scene.TakeChangeLog(world, m_ChangeLog);
            m_ChangeLogPrevFrame = m_ChangeLogFrame; // the sun's caster list reads it too (FillSunCasters)
            m_ChangeLogFrame = in.frame;
        }
        m_LodWake.Append(world, m_ChangeLog); // ApplyLod ran for this frame's local shadows (Application)
        LocalShadowQuiet::Lights lights;
        lights.SpotCount = fl.SpotCount;
        lights.SpotLive = fl.SpotLive;
        lights.SpotKey = m_SpotKey;
        lights.SpotReach = m_SpotReach;
        lights.PointCount = fl.PointCount;
        lights.PointLive = fl.PointLive;
        lights.PointKey = m_PointKey;
        lights.PointRange = m_PointRange;
        m_Quiet.Wake(m_ChangeLog, RenderSettingsEpoch(), lights);
    } else {
        m_Quiet.Reset();
    }
    GatherLocalCasters(in);
    GLTargetScope callerTarget; // the caller's framebuffers + viewport come back after the local maps
    if (fl.SpotCount > 0) RenderSpots(in);
    if (fl.PointCount > 0) RenderPoints(in);
}

// Spot and point casters, gathered once for every light and cube face. Animated models are culled on bounds padded
// around the centre, like the sun and main passes, instead of being drawn into every map.
void ShadowFrame::GatherLocalCasters(const ShadowFrameInputs& in) {
    PROFILE_SCOPE("Local Shadow Gather");
    World& world = *in.world;
    VisibilityQuery casts; // #163: not Cast Shadows Off
    casts.ShadowCasters = true;
    // Only casters some shadowed spot or point light reaches (its map's frustum, or the box of its range): the cells
    // first, then each caster's bounds. CollectLocalCasters still culls them per light; this keeps a forest's thousands
    // of casters nowhere near a lamp out of the bookkeeping below (~1.9 ms a frame in the Forest).
    const FrameLights& fl = *in.lights;
    // G4-c: a quiet light is kept as it is, so its casters aren't gathered (verify mode looks at it anyway).
    static std::vector<Frustum> reach;
    reach.clear();
    const bool keepQuiet = m_SlotDirty && !CacheVerify();
    for (int s = 0; s < fl.SpotCount; ++s)
        if (fl.SpotLive[s] && !(keepQuiet && m_Quiet.Spots[s].Quiet)) reach.push_back(m_SpotReach[s]);
    for (int p = 0; p < fl.PointCount; ++p) {
        if (!fl.PointLive[p] || (keepQuiet && m_Quiet.Points[p].Quiet)) continue;
        const glm::vec3 lo = m_PointRange[p].Min, hi = m_PointRange[p].Max;
        Frustum box;
        box.Planes[0] = {glm::vec3(1, 0, 0), -lo.x};
        box.Planes[1] = {glm::vec3(-1, 0, 0), hi.x};
        box.Planes[2] = {glm::vec3(0, 1, 0), -lo.y};
        box.Planes[3] = {glm::vec3(0, -1, 0), hi.y};
        box.Planes[4] = {glm::vec3(0, 0, 1), -lo.z};
        box.Planes[5] = {glm::vec3(0, 0, -1), hi.z};
        reach.push_back(box);
    }
    if (reach.empty()) return; // no live shadowed light: nothing to gather
    casts.CellCull = reach.data();
    casts.CellCullCount = (int)reach.size();
    GatherVisible(world, casts, m_LocalCasting);
    m_SceneWorld = &world;
    const std::uint64_t now = in.frame;
    for (const VisibleRenderable& v : m_LocalCasting) {
        // A cell holds far more than a lamp reaches: the caster itself must touch some light's reach.
        if (v.Bounded) {
            const AABB b{v.WorldMin, v.WorldMax};
            bool any = false;
            for (const Frustum& f : reach)
                if (f.Intersects(b)) { any = true; break; }
            if (!any) continue;
        }
        const entt::entity entity = v.Entity;
        const auto& r = *v.Renderable;
        LocalCaster& lc = m_LocalCasters.emplace_back();
        lc.model = r.ModelRef.get();
        lc.materials = &r.Materials;
        const auto* hide = world.Registry.try_get<OutfitHideTag>(entity);
        lc.visible = hide ? hide->Visible.get() : nullptr;
        lc.xform = v.Xform;
        lc.slot = v.Slot;
        lc.id = static_cast<std::uint32_t>(entity);
        lc.twoSided = r.CastShadows == RenderableComponent::ShadowCasting::TwoSided;
        lc.player = world.Registry.any_of<HiddenFromOwnerTag, PlayerBodyTag>(entity);
        // Skinned, not "has its own clips": a character whose clips live in other files (every NPC and outfit piece)
        // animates too, and its shadow must follow. First seen counts as settled (unsigned wrap keeps that true on
        // early frames).
        auto [motion, first] = m_CasterMotion.try_emplace(lc.id, CasterMotion{lc.xform, now - kMovingFrames - kSettleFrames, lc.model->PoseVersion(), now - kMovingFrames - kSettleFrames, now});
        // A record not touched for a while belongs to a gone entity: a recycled id starts fresh, not with its history.
        if (!first && now - motion->second.seen > kStaleFrames)
            motion->second = CasterMotion{lc.xform, now - kMovingFrames - kSettleFrames, lc.model->PoseVersion(), now - kMovingFrames - kSettleFrames, now};
        motion->second.seen = now;
        if (motion->second.xform != lc.xform) { motion->second.xform = lc.xform; motion->second.moved = now; }
        lc.moving = now - motion->second.moved < kMovingFrames;
        if (lc.model->HasBones() && motion->second.pose != lc.model->PoseVersion()) {
            motion->second.pose = lc.model->PoseVersion();
            motion->second.posed = now;
        }
        lc.animated = lc.model->HasBones() && now - motion->second.posed < kMovingFrames;
        lc.settleStart = std::max(motion->second.moved, lc.model->HasBones() ? motion->second.posed : 0) + kMovingFrames;
        lc.settling = !lc.moving && !lc.animated && now - lc.settleStart < kSettleFrames;
        lc.bounded = v.Bounded; // as posed, like the main pass
        lc.bounds = AABB{v.WorldMin, v.WorldMax};
        lc.hash = LocalCasterHash(lc.id, lc.model, *lc.materials, lc.xform, lc.visible ? lc.visible->Id() : 0u, lc.twoSided);
    }
    // Drop the records of casters gone for a while so the map doesn't grow across scene loads.
    if ((now & 255) == 0)
        for (auto it = m_CasterMotion.begin(); it != m_CasterMotion.end();)
            it = now - it->second.seen > kStaleFrames ? m_CasterMotion.erase(it) : std::next(it);
}

// A local shadow map is only redrawn when something it sees changed: its light, the map itself, or any caster inside it
// (moved, swapped model or materials, or animated). Everything static - most lamps, most of the time - keeps last
// frame's depth. Each caster's own hash is computed once when it is gathered; a light folds it into its signature with
// one multiply instead of re-hashing ~100 bytes per caster per light.
static std::uint64_t HashCaster(std::uint64_t h, std::uint64_t casterHash) { return (h ^ casterHash) * 1099511628211ull; }

bool ShadowFrame::CollectLocalCasters(const Frustum* frustum, const AABB* range, bool skipPlayer, int turnSlot,
                                      std::uint64_t frame, std::uint64_t& sig, bool& busy) {
    m_Visible.clear();
    m_VisibleDynamic.clear();
    bool anyDynamic = false;
    busy = false;
    m_VerifyNow.clear();
    for (const LocalCaster& c : m_LocalCasters) {
        if (c.bounded && (frustum ? !frustum->Intersects(c.bounds) : !range->Intersects(c.bounds))) continue;
        if (c.player && skipPlayer) continue;
        m_Visible.push_back(&c);
        const bool dynamic = DynamicFor(c, turnSlot, frame);
        m_VisibleDynamic.push_back(dynamic);
        anyDynamic = anyDynamic || dynamic;
        busy = busy || c.moving || c.animated || c.settling;
        if (!dynamic) sig = HashCaster(sig, c.hash);
        if (!dynamic && g_CacheVerify) {
            std::uint64_t mats = kFnvOffset;
            for (const auto& m : *c.materials) mats = HashWord(mats, (std::uint64_t)(std::uintptr_t)m.get());
            m_VerifyNow.push_back({c.id, c.model, mats, c.xform, c.visible ? c.visible->Id() : 0u, c.twoSided});
        }
    }
    return anyDynamic;
}

// The spot / point passes' prologue: store the LIGHT-FACING surface (back-face cull), NOT the far side. The sun CSM
// front-face-culls fine because its rays are parallel, but a perspective light's rays diverge and a curved caster's back
// face sits far enough past the contact that culling front faces detached the contact shadow - the sphere gap and
// peter-panning in #134. gl_FragDepth is written linearly here, so glPolygonOffset does nothing; the bias lives
// entirely in SpotShadow() / PointShadow()'s `ref` term.
ShadowFrame::LocalPassLocs ShadowFrame::BeginLocalPass(Shader& shader) {
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    shader.Bind(); // linear distance-to-light depth
    // The program's sampler2D uAlbedo reads unit 0 at the pass's first clear/blit/draw, before DrawDepthOnly binds
    // anything there (KHR 131204 on frame 1).
    GLStateCache::BindTexture2D(0, DefaultTextures::White());
    // #194: resolved once, outside the per-light x per-caster loops.
    return {shader.Loc("uLightViewProj"), shader.Loc("uShadowLightPos"), shader.Loc("uShadowFar")};
}

void ShadowFrame::DrawLocalCasters(Shader& shader, bool animatedOnes, const Frustum* clip) {
    m_Draws.clear();
    for (size_t vi = 0; vi < m_Visible.size(); ++vi) {
        const LocalCaster* c = m_Visible[vi];
        if ((m_VisibleDynamic[vi] != 0) != animatedOnes) continue;
        if (clip && c->bounded && !clip->Intersects(c->bounds)) continue;
        m_Draws.push_back({c->model, c->materials, c->visible, &c->xform, c->twoSided, 0, 1, c->slot});
    }
    m_Stats.CasterDraws += (int)m_Draws.size();
    if (m_CountingStatic) m_Stats.StaticCasterDraws += (int)m_Draws.size();
    DrawDepthCasters(shader, m_Draws, -1, m_SceneWorld);
}

void ShadowFrame::EndLocalPass() {
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    GLStateCache::Invalidate();
}

// One spot light's look (RenderSpots): its signature, then the layer kept (false) or redrawn (true).
bool ShadowFrame::RenderSpot(const ShadowFrameInputs& in, const LocalPassLocs& locs, int s, bool& lightMoved, bool& busy) {
    const FrameLights& fl = *in.lights;
    SpotShadowMap& spotShadowMap = *in.spotMap;
    Shader& localShadowShader = *in.localShader;
    const std::uint64_t frame = in.frame;
    // A light that moves (the flashlight) draws straight into its live layer - caching static depth it would throw
    // away next frame was a whole extra draw and layer copy.
    lightMoved = fl.SpotVP[s] != m_SpotLastVP[s];
    m_SpotLastVP[s] = fl.SpotVP[s];
    const Frustum& lf = m_SpotReach[s];
    // The static casters' signature: animated ones redraw every frame anyway.
    std::uint64_t sig = kFnvOffset;
    const unsigned int map = spotShadowMap.DepthArray();
    const int res = spotShadowMap.Resolution() * 64 + spotShadowMap.Layers(); // a reallocation
    sig = HashBytes(sig, &map, sizeof map);
    sig = HashBytes(sig, &res, sizeof res);
    sig = HashBytes(sig, &fl.SpotVP[s], sizeof fl.SpotVP[s]);
    sig = HashBytes(sig, &fl.SpotPos[s], sizeof fl.SpotPos[s]);
    sig = HashBytes(sig, &fl.SpotFar[s], sizeof fl.SpotFar[s]);
    // Animated or recently moved casters are drawn over the cached static depth every frame; the rest are the
    // static signature. The torch is in the player's hand: no player shadow in its own light.
    const bool animated = CollectLocalCasters(&lf, nullptr, s == fl.TorchShadowSlot, s, frame, sig, busy);
    const bool staticChanged = sig != m_SpotShadowSig[s];
    // Drawn whole when the light last moved, nothing has changed or moved in it since, and it isn't this light's
    // turn: that layer is still right. Its cache is built on its turn, not all at once the frame after a batch of
    // lights is reassigned.
    const bool spotTurn = frame % kSettleFrames == (std::uint64_t)s % kSettleFrames;
    m_LookWhy = (staticChanged ? 1 : 0) | (animated ? 2 : 0) | (m_SpotHadAnimated[s] ? 4 : 0);
    if (!lightMoved && m_SpotShadowSig[s] == 0 && sig == m_SpotLiveSig[s] && !animated && !m_SpotHadAnimated[s] && !spotTurn)
        return false;
    // Unchanged, and no animated caster in it now or last frame: keep the layer.
    if (!staticChanged && !animated && !m_SpotHadAnimated[s]) return false;
    m_SpotHadAnimated[s] = animated;
    localShadowShader.SetMat4(locs.ViewProj, fl.SpotVP[s]);
    localShadowShader.SetVec3(locs.LightPos, fl.SpotPos[s]);
    localShadowShader.SetFloat(locs.Far, fl.SpotFar[s]);
    // Where this frame's animated casters land in the map: their bounds' corners through the light, a texel of
    // slack each side. A caster without bounds or reaching behind the light covers it all.
    const int mapRes = spotShadowMap.Resolution();
    glm::ivec4 dynRect(mapRes, mapRes, 0, 0); // empty
    for (size_t vi = 0; vi < m_Visible.size(); ++vi) {
        if (!m_VisibleDynamic[vi]) continue;
        const LocalCaster* c = m_Visible[vi];
        bool whole = !c->bounded;
        for (int k = 0; k < 8 && !whole; ++k) {
            const glm::vec3 corner((k & 1) ? c->bounds.Max.x : c->bounds.Min.x, (k & 2) ? c->bounds.Max.y : c->bounds.Min.y,
                                   (k & 4) ? c->bounds.Max.z : c->bounds.Min.z);
            const glm::vec4 clip = fl.SpotVP[s] * glm::vec4(corner, 1.0f);
            if (clip.w <= 1e-4f) { whole = true; break; }
            const glm::vec2 texel = (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * (float)mapRes;
            dynRect = glm::ivec4(std::min(dynRect.x, (int)std::floor(texel.x) - 1), std::min(dynRect.y, (int)std::floor(texel.y) - 1),
                                 std::max(dynRect.z, (int)std::ceil(texel.x) + 1), std::max(dynRect.w, (int)std::ceil(texel.y) + 1));
        }
        if (whole) { dynRect = glm::ivec4(0, 0, mapRes, mapRes); break; }
    }
    auto drawCasters = [&](bool animatedOnes) { DrawLocalCasters(localShadowShader, animatedOnes, nullptr); };
    if (lightMoved) {
        ++m_Stats.SpotWholeRedraws;
        spotShadowMap.Begin(s);
        m_CountingStatic = true;
        drawCasters(false);
        m_CountingStatic = false;
        drawCasters(true);
        m_SpotShadowSig[s] = 0; // no cache: built on a later still frame (see spotTurn)
        m_SpotLiveSig[s] = sig;
        m_SpotDynamicRect[s] = glm::ivec4(0, 0, mapRes, mapRes);
        return true; // drawn whole
    }
    // Static casters into the cache only when they changed; then the live layer is that depth plus this frame's
    // animated casters (an NPC standing in the spot used to redraw the whole arena into it every frame).
    if (staticChanged) {
        ++m_Stats.SpotStaticRedraws;
        spotShadowMap.BeginStatic(s);
        m_CountingStatic = true;
        drawCasters(false);
        m_CountingStatic = false;
        m_SpotShadowSig[s] = sig;
    }
    ++m_Stats.SpotDynamicPasses;
    // The live layer is the cache plus last frame's animated casters, which stayed within their rect: restoring
    // that rect and this frame's from the cache is the whole layer's worth (a new cache is copied whole).
    const glm::ivec4 prev = m_SpotDynamicRect[s];
    const glm::ivec4 copy = staticChanged ? glm::ivec4(0, 0, mapRes, mapRes)
                                          : glm::ivec4(glm::min(glm::ivec2(dynRect), glm::ivec2(prev)), glm::max(glm::ivec2(dynRect.z, dynRect.w), glm::ivec2(prev.z, prev.w)));
    spotShadowMap.BeginFromStatic(s, copy.x, copy.y, copy.z, copy.w);
    drawCasters(true);
    m_SpotDynamicRect[s] = dynRect;
    return true;
}

void ShadowFrame::RenderSpots(const ShadowFrameInputs& in) {
    PROFILE_SCOPE("Spot Shadow Pass");
    PROFILE_GPU_SCOPE("Spot Shadow Pass");
    const FrameLights& fl = *in.lights;
    const LocalPassLocs locs = BeginLocalPass(*in.localShader);
    for (int s = 0; s < fl.SpotCount; ++s) {
        if (!fl.SpotLive[s]) continue;
        ++m_Stats.SpotLive; ++g_QuietTotals.Live;
        const bool wasQuiet = m_SlotDirty && m_Quiet.Spots[s].Quiet;
        if (wasQuiet && !CacheVerify()) { // G4-c: nothing it sees changed since its last look
            ++m_Stats.SpotQuiet; ++g_QuietTotals.Quiet;
            continue;
        }
        bool lightMoved = false, busy = false;
        const bool drawn = RenderSpot(in, locs, s, lightMoved, busy);
        // Quiet from here: a still light with its cache built, its live layer that cache, and nothing in it in motion.
        m_Quiet.Spots[s].Quiet = m_SlotDirty && !lightMoved && !busy && m_SpotShadowSig[s] != 0 && !m_SpotHadAnimated[s];
        if (CacheVerify()) VerifyQuiet(wasQuiet, drawn, "spot", s, in.frame, m_VerifySpot[s]);
    }
    EndLocalPass();
}

// Point-light cube shadow maps (#119) - six faces per casting point light.
void ShadowFrame::RenderPoints(const ShadowFrameInputs& in) {
    PROFILE_SCOPE("Point Shadow Pass");
    PROFILE_GPU_SCOPE("Point Shadow Pass");
    const FrameLights& fl = *in.lights;
    PointShadowMap& pointShadowMap = *in.pointMap;
    Shader& localShadowShader = *in.localShader;
    const std::uint64_t frame = in.frame;
    const LocalPassLocs locs = BeginLocalPass(localShadowShader);
    for (int s = 0; s < fl.PointCount; ++s) {
        if (!fl.PointLive[s]) continue;
        ++m_Stats.PointLive; ++g_QuietTotals.Live;
        const bool wasQuiet = m_SlotDirty && m_Quiet.Points[s].Quiet;
        if (wasQuiet && !CacheVerify()) { // G4-c: nothing it sees changed since its last look
            ++m_Stats.PointQuiet; ++g_QuietTotals.Quiet;
            continue;
        }
        // The light's whole reach: casters outside it can't show on any face.
        const glm::vec3 lp = fl.PointPos[s];
        const float reach = fl.PointFar[s];
        const AABB& range = m_PointRange[s];
        std::uint64_t sig = kFnvOffset;
        const unsigned int map = pointShadowMap.DepthCubeArray();
        const int res = pointShadowMap.Resolution() * 64 + pointShadowMap.Cubes(); // a reallocation
        sig = HashBytes(sig, &map, sizeof map);
        sig = HashBytes(sig, &res, sizeof res);
        sig = HashBytes(sig, &lp, sizeof lp);
        sig = HashBytes(sig, &fl.PointNear[s], sizeof fl.PointNear[s]);
        sig = HashBytes(sig, &reach, sizeof reach);
        // Animated or recently moved casters are drawn over each face's cached static depth; the rest make the static
        // signature (as the spot maps do). A face redraws only when the static casters changed or a moving one is in it
        // (or was, last frame): an NPC walking past a lamp used to redraw all six faces of every cube in reach.
        bool busy = false, drawn = false, anyFaceDynamic = false;
        CollectLocalCasters(nullptr, &range, false, s + 2, frame, sig, busy);
        const bool staticChanged = sig != m_PointShadowSig[s];
        m_PointShadowSig[s] = sig;
        const glm::mat4 proj = MakePerspective(90.0f, 1.0f, fl.PointNear[s], reach); // #202
        localShadowShader.SetVec3(locs.LightPos, lp);
        localShadowShader.SetFloat(locs.Far, reach);
        for (int f = 0; f < 6; ++f) {
            const CubeFaces::FaceBasis& fb = CubeFaces::kFaces[f]; // GL face order, the conventional ups
            const glm::vec3 dir(fb.forward[0], fb.forward[1], fb.forward[2]), up(fb.up[0], fb.up[1], fb.up[2]);
            const glm::mat4 vp = proj * glm::lookAt(lp, lp + dir, up);
            const Frustum lf = Frustum::FromViewProj(vp);
            bool dynamicHere = false;
            for (size_t vi = 0; vi < m_Visible.size(); ++vi) {
                const LocalCaster* c = m_Visible[vi];
                if (m_VisibleDynamic[vi] && (!c->bounded || lf.Intersects(c->bounds))) { dynamicHere = true; break; }
            }
            if (!staticChanged && !dynamicHere && !m_PointFaceHadDynamic[s][f]) { ++m_Stats.PointKeptFaces; continue; } // keep the face
            drawn = true;
            m_PointFaceHadDynamic[s][f] = dynamicHere;
            anyFaceDynamic = anyFaceDynamic || dynamicHere;
            localShadowShader.SetMat4(locs.ViewProj, vp);
            const auto drawCasters = [&](bool dynamicOnes) { DrawLocalCasters(localShadowShader, dynamicOnes, &lf); };
            if (staticChanged) {
                ++m_Stats.PointStaticFaces;
                pointShadowMap.BeginStaticFace(s, f);
                m_CountingStatic = true;
                drawCasters(false);
                m_CountingStatic = false;
            } else {
                ++m_Stats.PointDynamicFaces;
            }
            pointShadowMap.BeginFaceFromStatic(s, f);
            if (dynamicHere) drawCasters(true);
        }
        // Quiet from here: no face holds a moving caster (a kept face had none last frame either) and none is in reach.
        m_Quiet.Points[s].Quiet = m_SlotDirty && !busy && !anyFaceDynamic;
        if (CacheVerify()) {
            m_LookWhy = staticChanged ? 1 : 0;
            VerifyQuiet(wasQuiet, drawn, "point", s, frame, m_VerifyPoint[s]);
        }
    }
    EndLocalPass();
}
