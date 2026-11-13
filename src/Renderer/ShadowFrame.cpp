#include "ShadowFrame.h"
#include <cstdio>
#include <unordered_map>
#include "Core/Env.h"
#include "TimeService.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "Components.h"
#include "Core/Env.h"
#include "DefaultTextures.h"
#include "Frustum.h"
#include "GLStateCache.h"
#include "GrassRenderer.h"
#include "ImpostorRenderer.h"
#include "SkinHideBuffer.h"
#include "TimeService.h"
#include "gl.h"
#include "LightGather.h"
#include "Model.h"
#include "Profiler.h"
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
} // namespace

// #109 - the sun's cascades are fitted to the view being RENDERED, per drawScene(): they used to be fitted once per
// frame to one camera while both the Scene and Game views sampled them, so the other view had missing or blurry
// shadows. In the default layout Scene and Game are tabs and only one renders per frame, so this costs nothing extra;
// with both on screen the cascades render twice. A second view with the same matrices in the same frame reuses them.
bool ShadowFrame::RenderSun(const ShadowFrameInputs& in, const glm::mat4& fitView, const glm::mat4& fitProj) {
    World& world = *in.world;
    const FrameLights& fl = *in.lights;
    if (!(world.ShadowsEnabled && fl.HaveDirectional && fl.SunCastShadows)) return false;
    if (m_LastSunFit.frame == in.frame && m_LastSunFit.view == fitView && m_LastSunFit.proj == fitProj)
        return true; // already fitted to exactly this view this frame
    PROFILE_SCOPE("Sun Shadow Pass");
    PROFILE_GPU_SCOPE("Sun Shadow Pass");
    CascadedShadowMap& shadowMap = *in.sunMap;
    Shader& shadowShader = *in.sunShader;
    // The caller has its own target bound; the pass renders into the cascade array.
    GLint prevDrawFbo = 0, prevReadFbo = 0, prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    shadowMap.Configure(world.ShadowResolution, world.ShadowCascades);
    // Sub-scopes for the pass's CPU stages (fit + gather, sort + cascade masks, caster draws, grass).
    std::optional<Profiler::ScopeTimer> stage;
    stage.emplace("Sun: Fit + Gather");
    // Gather the casters once - registry lookups, world transform, world bounds - and reuse the list for every cascade.
    m_SunCasters.clear();
    // #160 - the cascades' near planes reach every occluder: fitted to a box around everything that can draw
    // (RenderableBounds, a cheap superset of the casters), so the casters can then be gathered against the fitted
    // cascades - a forest's casters outside every cascade are never read at all.
    glm::vec3 casterMin(std::numeric_limits<float>::max()), casterMax(-std::numeric_limits<float>::max());
    RenderableBounds(world, casterMin, casterMax);
    shadowMap.SetSplitLambda(world.ShadowCascadeSplit);
    shadowMap.Update(fitView, fitProj, fl.SunDir, world.ShadowDistance, &casterMin, &casterMax);
    const std::uint64_t frame = Time::FrameCount();
    const unsigned renderMask = shadowMap.PlanRender(frame); // the far cascades on alternate frames
    Frustum casterCells[CascadedShadowMap::kMaxCascades];
    for (int c = 0; c < shadowMap.Count(); ++c) casterCells[c] = Frustum::FromViewProj(shadowMap.LightViewProj(c));
    VisibilityQuery casts; // #163 - Cast Shadows: Off skips
    casts.ShadowCasters = true;
    casts.CellCull = casterCells; // castsInto below draws nothing outside every cascade's frustum
    casts.CellCullCount = shadowMap.Count();
    GatherVisible(world, casts, m_Casting);
    for (const VisibleRenderable& v : m_Casting) {
        const auto& r = *v.Renderable;
        SunCaster& sc = m_SunCasters.emplace_back();
        sc.model = r.ModelRef.get();
        sc.materials = &r.Materials;
        const auto* hide = world.Registry.try_get<OutfitHideTag>(v.Entity);
        sc.visible = hide ? hide->Visible.get() : nullptr;
        sc.xform = v.Xform;
        // #163 - Cast Shadows: Two Sided draws without back-face culling.
        sc.twoSided = r.CastShadows == RenderableComponent::ShadowCasting::TwoSided;
        // As posed (Model::PosedBounds), the bounds the main pass culls with.
        sc.bounded = v.Bounded;
        sc.bounds = AABB{v.WorldMin, v.WorldMax};
    }
    // Consecutive draws of the same model share its VAO and (usually) its material, which the state cache then skips.
    stage.reset();
    stage.emplace("Sun: Sort + Masks");
    std::sort(m_SunCasters.begin(), m_SunCasters.end(), [](const SunCaster& a, const SunCaster& b) { return a.model < b.model; });

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
    const int cascadeCount = shadowMap.Count();
    // A caster goes into a cascade only if its shadow can land where that cascade is read
    // (CascadedShadowMap::CasterReachesSlice), with the PCF kernel SunShadow uses.
    const float sunKernelTexels = CascadedShadowMap::SunKernelTexels(fl.SunAngularDeg, fl.SunShadowSoftness);
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
                m_Draws.push_back({sc.model, sc.materials, sc.visible, &sc.xform, sc.twoSided, c, end - c + 1});
                c = end + 1;
            }
        }
        // TARTARUS_TRIS_REPORT=<frame>: the sun casters' triangles on that frame, per cascade and by model.
        if (static const int reportAt = Env::Int(Env::Var::TrisReport, -1);
            reportAt >= 0 && Time::FrameCount() == (std::uint64_t)reportAt) {
            long long perCascade[CascadedShadowMap::kMaxCascades] = {};
            std::unordered_map<std::string, std::pair<long long, int>> byModel; // tris over all cascades, draws
            for (const auto& d : m_Draws) {
                const long long t = d.Visible ? (long long)d.Visible->Triangles() : (long long)d.Ref->TriangleCount();
                for (int k = 0; k < d.Layers; ++k) perCascade[d.LayerFirst + k] += t;
                auto& r = byModel[d.Ref->Path()];
                r.first += t * d.Layers;
                ++r.second;
            }
            std::printf("[TrisReport] frame %llu sun casters: %zu draws, tris per cascade", (unsigned long long)Time::FrameCount(),
                        m_Draws.size());
            for (int c = 0; c < cascadeCount; ++c) std::printf(" %lld", perCascade[c]);
            std::printf("\n");
            std::vector<std::pair<std::string, std::pair<long long, int>>> v(byModel.begin(), byModel.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
            for (size_t i = 0; i < v.size() && i < 30; ++i)
                std::printf("[TrisReport]   shadow %8lld tris  %5d draws  %s\n", v[i].second.first, v[i].second.second, v[i].first.c_str());
            std::fflush(stdout);
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
        DrawDepthCasters(shadowShader, m_Draws, firstLoc);
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
                if (castsInto(sc, c, cascadeFrustum)) m_Draws.push_back({sc.model, sc.materials, sc.visible, &sc.xform, sc.twoSided});
            DrawDepthCasters(shadowShader, m_Draws);
        }
    }

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

    shadowMap.MarkRendered(renderMask, frame);

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

    glDisable(GL_POLYGON_OFFSET_FILL);
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    GLStateCache::Invalidate();
    m_LastSunFit = {in.frame, fitView, fitProj};
    return true;
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

void ShadowFrame::RenderLocal(const ShadowFrameInputs& in) {
    const World& world = *in.world;
    const FrameLights& fl = *in.lights;
    // Layers are allocated for the budget, not the hard ceiling (#110); a zero budget still keeps one layer so
    // sampler unit 9 is never unbound.
    if (world.ShadowsEnabled) {
        in.spotMap->Configure(std::min(world.ShadowResolution, 4096), std::max(fl.SpotCount, 1)); // sized to the slots in use (grow-only)
        in.pointMap->Configure(std::min(world.ShadowResolution, 1024), std::max(fl.PointBudget, 1));
    }
    m_LocalCasters.clear();
    if (!world.ShadowsEnabled || (fl.SpotCount <= 0 && fl.PointCount <= 0)) return;
    GatherLocalCasters(in);
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
    static std::vector<Frustum> reach;
    reach.clear();
    for (int s = 0; s < fl.SpotCount; ++s)
        if (fl.SpotLive[s]) reach.push_back(Frustum::FromViewProj(fl.SpotVP[s]));
    for (int p = 0; p < fl.PointCount; ++p) {
        if (!fl.PointLive[p]) continue;
        const glm::vec3 lo = fl.PointPos[p] - glm::vec3(fl.PointFar[p]), hi = fl.PointPos[p] + glm::vec3(fl.PointFar[p]);
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
    GatherVisible(world, casts, m_Casting);
    const std::uint64_t now = in.frame;
    for (const VisibleRenderable& v : m_Casting) {
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
        // A change signature, not an identity: 64-bit words through a multiply-xorshift mix.
        std::uint64_t h = kFnvOffset;
        h = HashWord(h, lc.id);
        h = HashWord(h, (std::uint64_t)(std::uintptr_t)lc.model);
        h = HashWord(h, (std::uint64_t)(std::uintptr_t)(lc.materials->empty() ? nullptr : lc.materials->front().get()));
        h = HashWord(h, (std::uint64_t)lc.materials->size());
        std::uint64_t xw[8];
        std::memcpy(xw, &lc.xform, sizeof xw);
        for (const std::uint64_t w : xw) h = HashWord(h, w);
        lc.hash = HashWord(h, lc.twoSided ? 1u : 0u);
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
                                      std::uint64_t frame, std::uint64_t& sig) {
    m_Visible.clear();
    m_VisibleDynamic.clear();
    bool anyDynamic = false;
    for (const LocalCaster& c : m_LocalCasters) {
        if (c.bounded && (frustum ? !frustum->Intersects(c.bounds) : !range->Intersects(c.bounds))) continue;
        if (c.player && skipPlayer) continue;
        m_Visible.push_back(&c);
        const bool dynamic = DynamicFor(c, turnSlot, frame);
        m_VisibleDynamic.push_back(dynamic);
        anyDynamic = anyDynamic || dynamic;
        if (!dynamic) sig = HashCaster(sig, c.hash);
    }
    return anyDynamic;
}

void ShadowFrame::RenderSpots(const ShadowFrameInputs& in) {
    PROFILE_SCOPE("Spot Shadow Pass");
    PROFILE_GPU_SCOPE("Spot Shadow Pass");
    const FrameLights& fl = *in.lights;
    SpotShadowMap& spotShadowMap = *in.spotMap;
    Shader& localShadowShader = *in.localShader;
    const std::uint64_t frame = in.frame;
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    // Store the LIGHT-FACING surface (back-face cull), NOT the far side. The sun CSM front-face-culls fine because its
    // rays are parallel, but a perspective spot's rays diverge and a curved caster's back face sits far enough past
    // the contact that culling front faces detached the contact shadow - the sphere gap in #134. gl_FragDepth is
    // written linearly here, so glPolygonOffset does nothing; the bias lives entirely in SpotShadow()'s `ref` term.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    localShadowShader.Bind(); // linear distance-to-light depth
    // The program's sampler2D uAlbedo reads unit 0 at the pass's first clear/blit/draw, before DrawDepthOnly binds
    // anything there (KHR 131204 on frame 1).
    GLStateCache::BindTexture2D(0, DefaultTextures::White());
    // #194: resolve once, outside the per-spot x per-caster loop.
    const int localLightViewProjLoc = localShadowShader.Loc("uLightViewProj");
    const int localLightPosLoc = localShadowShader.Loc("uShadowLightPos");
    const int localFarLoc = localShadowShader.Loc("uShadowFar");
    for (int s = 0; s < fl.SpotCount; ++s) {
        if (!fl.SpotLive[s]) continue;
        // A light that moves (the flashlight) draws straight into its live layer - caching static depth it would throw
        // away next frame was a whole extra draw and layer copy.
        const bool lightMoved = fl.SpotVP[s] != m_SpotLastVP[s];
        m_SpotLastVP[s] = fl.SpotVP[s];
        const Frustum lf = Frustum::FromViewProj(fl.SpotVP[s]);
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
        const bool animated = CollectLocalCasters(&lf, nullptr, s == fl.TorchShadowSlot, s, frame, sig);
        const bool staticChanged = sig != m_SpotShadowSig[s];
        // Drawn whole when the light last moved, nothing has changed or moved in it since, and it isn't this light's
        // turn: that layer is still right. Its cache is built on its turn, not all at once the frame after a batch of
        // lights is reassigned.
        const bool spotTurn = frame % kSettleFrames == (std::uint64_t)s % kSettleFrames;
        if (!lightMoved && m_SpotShadowSig[s] == 0 && sig == m_SpotLiveSig[s] && !animated && !m_SpotHadAnimated[s] && !spotTurn)
            continue;
        // Unchanged, and no animated caster in it now or last frame: keep the layer.
        if (!staticChanged && !animated && !m_SpotHadAnimated[s]) continue;
        m_SpotHadAnimated[s] = animated;
        localShadowShader.SetMat4(localLightViewProjLoc, fl.SpotVP[s]);
        localShadowShader.SetVec3(localLightPosLoc, fl.SpotPos[s]);
        localShadowShader.SetFloat(localFarLoc, fl.SpotFar[s]);
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
        auto drawCasters = [&](bool animatedOnes) {
            m_Draws.clear();
            for (size_t vi = 0; vi < m_Visible.size(); ++vi) {
                const LocalCaster* c = m_Visible[vi];
                if ((m_VisibleDynamic[vi] != 0) == animatedOnes) m_Draws.push_back({c->model, c->materials, c->visible, &c->xform, c->twoSided});
            }
            DrawDepthCasters(localShadowShader, m_Draws);
        };
        if (lightMoved) {
            spotShadowMap.Begin(s);
            drawCasters(false);
            drawCasters(true);
            m_SpotShadowSig[s] = 0; // no cache: built on a later still frame (see spotTurn)
            m_SpotLiveSig[s] = sig;
            m_SpotDynamicRect[s] = glm::ivec4(0, 0, mapRes, mapRes);
            continue;
        }
        // Static casters into the cache only when they changed; then the live layer is that depth plus this frame's
        // animated casters (an NPC standing in the spot used to redraw the whole arena into it every frame).
        if (staticChanged) {
            spotShadowMap.BeginStatic(s);
            drawCasters(false);
            m_SpotShadowSig[s] = sig;
        }
        // The live layer is the cache plus last frame's animated casters, which stayed within their rect: restoring
        // that rect and this frame's from the cache is the whole layer's worth (a new cache is copied whole).
        const glm::ivec4 prev = m_SpotDynamicRect[s];
        const glm::ivec4 copy = staticChanged ? glm::ivec4(0, 0, mapRes, mapRes)
                                              : glm::ivec4(glm::min(glm::ivec2(dynRect), glm::ivec2(prev)), glm::max(glm::ivec2(dynRect.z, dynRect.w), glm::ivec2(prev.z, prev.w)));
        spotShadowMap.BeginFromStatic(s, copy.x, copy.y, copy.z, copy.w);
        drawCasters(true);
        m_SpotDynamicRect[s] = dynRect;
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, in.windowWidth, in.windowHeight);
    GLStateCache::Invalidate();
}

// Point-light cube shadow maps (#119) - six faces per casting point light.
void ShadowFrame::RenderPoints(const ShadowFrameInputs& in) {
    PROFILE_SCOPE("Point Shadow Pass");
    PROFILE_GPU_SCOPE("Point Shadow Pass");
    const FrameLights& fl = *in.lights;
    PointShadowMap& pointShadowMap = *in.pointMap;
    Shader& localShadowShader = *in.localShader;
    const std::uint64_t frame = in.frame;
    // Standard GL cube-map face order: +X -X +Y -Y +Z -Z, with the conventional ups.
    static const glm::vec3 kFaceDir[6] = {
        { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0}, {0, -1, 0}, {0, 0,  1}, {0, 0, -1} };
    static const glm::vec3 kFaceUp[6] = {
        {0, -1, 0}, {0, -1, 0}, {0, 0,  1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0} };

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    // Store the LIGHT-FACING surface (back-face cull), matching the spot pass. Front-face culling here stored a
    // sphere's FAR hemisphere, which under this 90° perspective sits well past the true contact - that was the sphere
    // peter-panning in #134. gl_FragDepth is linear, so glPolygonOffset is inert; PointShadow()'s `ref` term carries
    // all the bias.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    localShadowShader.Bind(); // linear distance-to-light depth
    // The program's sampler2D uAlbedo reads unit 0 at the pass's first clear/blit/draw, before DrawDepthOnly binds
    // anything there (KHR 131204 on frame 1).
    GLStateCache::BindTexture2D(0, DefaultTextures::White());
    // #194: resolve once, outside the per-point x 6-face x per-caster loop.
    const int cubeLightPosLoc = localShadowShader.Loc("uShadowLightPos");
    const int cubeFarLoc = localShadowShader.Loc("uShadowFar");
    const int cubeLightViewProjLoc = localShadowShader.Loc("uLightViewProj");
    for (int s = 0; s < fl.PointCount; ++s) {
        if (!fl.PointLive[s]) continue;
        // The light's whole reach: casters outside it can't show on any face.
        const glm::vec3 lp = fl.PointPos[s];
        const float reach = fl.PointFar[s];
        const AABB range{lp - glm::vec3(reach), lp + glm::vec3(reach)};
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
        CollectLocalCasters(nullptr, &range, false, s + 2, frame, sig);
        const bool staticChanged = sig != m_PointShadowSig[s];
        m_PointShadowSig[s] = sig;
        const glm::mat4 proj = MakePerspective(90.0f, 1.0f, fl.PointNear[s], reach); // #202
        localShadowShader.SetVec3(cubeLightPosLoc, lp);
        localShadowShader.SetFloat(cubeFarLoc, reach);
        for (int f = 0; f < 6; ++f) {
            const glm::mat4 vp = proj * glm::lookAt(lp, lp + kFaceDir[f], kFaceUp[f]);
            const Frustum lf = Frustum::FromViewProj(vp);
            bool dynamicHere = false;
            for (size_t vi = 0; vi < m_Visible.size(); ++vi) {
                const LocalCaster* c = m_Visible[vi];
                if (m_VisibleDynamic[vi] && (!c->bounded || lf.Intersects(c->bounds))) { dynamicHere = true; break; }
            }
            if (!staticChanged && !dynamicHere && !m_PointFaceHadDynamic[s][f]) continue; // keep the face
            m_PointFaceHadDynamic[s][f] = dynamicHere;
            localShadowShader.SetMat4(cubeLightViewProjLoc, vp);
            const auto drawCasters = [&](bool dynamicOnes) {
                m_Draws.clear();
                for (size_t vi = 0; vi < m_Visible.size(); ++vi) {
                    const LocalCaster* c = m_Visible[vi];
                    if ((m_VisibleDynamic[vi] != 0) != dynamicOnes) continue;
                    if (c->bounded && !lf.Intersects(c->bounds)) continue;
                    m_Draws.push_back({c->model, c->materials, c->visible, &c->xform, c->twoSided});
                }
                DrawDepthCasters(localShadowShader, m_Draws);
            };
            if (staticChanged) {
                pointShadowMap.BeginStaticFace(s, f);
                drawCasters(false);
            }
            pointShadowMap.BeginFaceFromStatic(s, f);
            if (dynamicHere) drawCasters(true);
        }
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, in.windowWidth, in.windowHeight);
    GLStateCache::Invalidate();
}
