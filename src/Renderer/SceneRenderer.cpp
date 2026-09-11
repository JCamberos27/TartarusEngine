#include "SceneRenderer.h"
#include "RenderFrameContext.h"

#include "World.h"
#include "Components.h"
#include "Sky.h"
#include "Shader.h"
#include "Cubemap.h"
#include "CascadedShadowMap.h"
#include "SpotShadowMap.h"
#include "PointShadowMap.h"
#include "IblProbe.h"
#include "ReflectionProbeArray.h"
#include "LightBuffer.h"
#include "ClusterGrid.h"
#include "HdrTarget.h"
#include "OpaqueColorCopy.h"
#include "Ssao.h"
#include "Model.h"
#include "MaterialAsset.h"
#include "ShaderVariant.h"
#include "DefaultTextures.h"
#include "Frustum.h"
#include "gl.h"
#include "Core/Profiler.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>

// Per-run tally of how many meshes drew through each ShaderAsset variant key (audit #354). Read
// by --smoke-test to log which lobes actually rendered; harmless in normal runs.
namespace {
std::unordered_map<std::uint32_t, std::uint64_t> g_variantDrawCounts;
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
    fs.ssaoOn       = in.ssaoEnabled && ctx.SsaoSrc && ctx.SsaoSrc->IsValid() && !ctx.Unlit;
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
    return fs;
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
    // a tight ~2 texel edge; crank the light's Angular Size for softer.
    program.SetFloat("uShadowSoftness",
        std::clamp(in.sunAngularDeg * 3.0f, 1.0f, 14.0f) * in.sunShadowSoftness);
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

    program.SetInt("uUnlit", ctx.Unlit ? 1 : 0);
    // Real scene path: emit linear HDR; the shared Tonemapper pass maps it after MSAA resolve.
    // (Offscreen model thumbnails set this to 1 to self-tonemap.)
    program.SetInt("uApplyTonemap", 0);
}

void SceneRenderer::RenderScene(World& world, const RenderFrameContext& ctx,
                                const SceneRenderInputs& in, RenderStats* outStats) {
    Sky&        sky         = *in.sky;
    Shader&     modelShader = *in.modelShader;
    LightBuffer& lightBuffer = *in.lightBuffer;
    ClusterGrid& clusterGrid = *in.clusterGrid;

    // PR13: sky draw — HDRI cubemap or procedural gradient.
    if (world.SkySourceMode == World::SkySource::Hdri && in.hdriCube) {
        float rotRad = glm::radians(world.SkyRotationDegrees);
        sky.DrawHdri(in.hdriCube->Texture(), rotRad, ctx.View, ctx.Proj);
    } else {
        sky.Draw(ctx.View, ctx.Proj, world.SkyHorizonColor, world.SkyZenithColor);
    }

    const FrameState fs = GatherFrameState(world, ctx, in);

    // Clustered-forward light culling (#120) — one compute dispatch per frame, before any
    // program that reads the froxel lists is bound.
    if (fs.clusterOn) {
        PROFILE_GPU_SCOPE("Cluster Cull");
        clusterGrid.Cull(*in.clusterBuildShader, *in.clusterCullShader, ctx.View, ctx.Proj,
                         fs.clusterNearZ, fs.clusterFarZ, fs.vp[2], fs.vp[3]);
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
    auto selectProgram = [&](const MaterialAsset* ma) -> Shader* {
        Shader* prog = &modelShader;
        std::uint32_t key = 0;
        if (ma && ma->Shader) {
            key = ShaderVariantKeyFor(ma->Mat, *ma->Shader);
            if (Shader* v = ma->Shader->Variant(key)) prog = v;
        }
        if (std::find(appliedPrograms.begin(), appliedPrograms.end(), prog) == appliedPrograms.end()) {
            ApplyFrameState(*prog, fs);
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
        int Meshes, Tris, Verts;
        // PR9 transparent queue
        MaterialAsset::Queue Queue;
        int  QueueIndex;
        float ViewDepth; // view-space -Z (more positive = farther) for back-to-front sort
    };
    static std::vector<DrawItem> drawList;
    static std::vector<DrawItem> transparentList;
    drawList.clear();
    transparentList.clear();

    for (auto entity : world.Registry.view<TransformComponent, RenderableComponent>()) {
        if (world.Registry.all_of<InactiveTag>(entity)) continue; // Hierarchy eye toggle / GameObject active

        // Editor Scene viewport only: per-entity SceneVis hide (#236 B) + per-layer visibility
        // mask (#236 A1). The scene, saves and Game view are unaffected.
        if (ctx.EditorView) {
            if (world.Registry.all_of<HiddenInSceneTag>(entity)) continue;
            const auto* lc = world.Registry.try_get<LayerComponent>(entity);
            const int layer = lc ? lc->Layer : 0;
            if (layer >= 0 && layer < 32 && !((in.layerVisibleMask >> layer) & 1u)) continue;
        }
        auto& renderable = world.Registry.get<RenderableComponent>(entity);
        glm::mat4 model = world.GetCachedWorldTransform(entity);

        // Frustum culling: skip the draw call entirely for anything outside the camera's view.
        // Bounds come from the same Model::BoundsMin/Max already used for gizmo framing and Snap
        // to Ground - an invalid (never-populated) bounds pair draws unconditionally.
        glm::vec3 boundsMin = renderable.ModelRef->BoundsMin();
        glm::vec3 boundsMax = renderable.ModelRef->BoundsMax();
        bool validBounds = boundsMin.x <= boundsMax.x && boundsMin.y <= boundsMax.y && boundsMin.z <= boundsMax.z;
        if (validBounds) {
            // Bounds are bind-pose only. A skinned model's limbs can swing well past them, so
            // inflate around the centre before the frustum test for animated models (#113).
            if (renderable.ModelRef->HasAnimations()) {
                glm::vec3 c = (boundsMin + boundsMax) * 0.5f;
                glm::vec3 h = (boundsMax - boundsMin) * 0.5f * 1.75f;
                boundsMin = c - h;
                boundsMax = c + h;
            }
            AABB worldBounds = AABB{boundsMin, boundsMax}.Transformed(model);
            if (!camFrustum.Intersects(worldBounds)) {
                localStats.Culled++;
                continue;
            }
        }

        Model* m = renderable.ModelRef.get();
        const auto& slots = renderable.Materials;
        std::uint64_t matKey = (!slots.empty() && slots[0])
            ? slots[0]->Mat.Hash() : m->MaterialSortKey();
        // PR9: classify into opaque or transparent based on slot 0's queue.
        MaterialAsset::Queue q = MaterialAsset::Queue::Opaque;
        int qi = 2000;
        float viewDepth = 0.0f;
        if (!slots.empty() && slots[0]) {
            q  = slots[0]->RenderQueue;
            qi = slots[0]->QueueIndex;
        }
        if (q == MaterialAsset::Queue::Transparent) {
            glm::vec3 centre = glm::vec3(model[3]);
            viewDepth = -(ctx.View * glm::vec4(centre, 1.0f)).z;
            transparentList.push_back({ model, m, &slots, matKey,
                m->MeshCount(), (int)m->TriangleCount(), (int)m->VertexCount(),
                q, qi, viewDepth });
        } else {
            drawList.push_back({ model, m, &slots, matKey,
                m->MeshCount(), (int)m->TriangleCount(), (int)m->VertexCount(),
                q, qi, 0.0f });
        }
    }

    // --- Opaque pass: sort by material key then Model* (unchanged from PR8) --------
    std::sort(drawList.begin(), drawList.end(), [](const DrawItem& a, const DrawItem& b) {
        if (a.MatKey != b.MatKey) return a.MatKey < b.MatKey;
        return reinterpret_cast<std::uintptr_t>(a.Ref) < reinterpret_cast<std::uintptr_t>(b.Ref);
    });

    passAlphaBlend = 0;
    modelShader.SetInt("uAlphaBlend", 0); // explicit: ensure opaque pass outputs alpha=1
    for (const DrawItem& it : drawList) {
        it.Ref->DrawSelected(modelShader, it.Xform, *it.Slots, selectProgram);

        localStats.DrawCalls += it.Meshes;
        localStats.Triangles += it.Tris;
        localStats.Vertices += it.Verts;
    }

    // --- Transparent pass: back-to-front sorted, blended, no depth write -----------
    if (!transparentList.empty()) {
        passAlphaBlend = 1;
        // Every program already brought up for the opaque pass flips to blended output; new
        // programs picked up below get passAlphaBlend via the selector.
        for (Shader* p : appliedPrograms) { p->Bind(); p->SetInt("uAlphaBlend", 1); }

        // PR12: resolve MSAA opaque color and copy into mipped texture for refraction. Bind the
        // capture on unit 14 for every program that may run the _TRANSMISSION branch.
        if (ctx.TxHdr && ctx.TxCapture) {
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
        std::sort(transparentList.begin(), transparentList.end(),
            [](const DrawItem& a, const DrawItem& b) {
                if (a.QueueIndex != b.QueueIndex) return a.QueueIndex < b.QueueIndex;
                if (a.ViewDepth  != b.ViewDepth)  return a.ViewDepth  > b.ViewDepth;
                return a.MatKey < b.MatKey;
            });

        glEnable(GL_BLEND);
        glDepthMask(GL_FALSE);
        glBlendFuncSeparate(GL_SRC_ALPHA,  GL_ONE_MINUS_SRC_ALPHA,
                            0x0001/*GL_ONE*/, GL_ONE_MINUS_SRC_ALPHA);

        for (const DrawItem& it : transparentList) {
            float opacity = (!it.Slots->empty() && (*it.Slots)[0])
                ? (*it.Slots)[0]->Opacity : 1.0f;
            it.Ref->DrawSelected(modelShader, it.Xform, *it.Slots, selectProgram, opacity);

            localStats.DrawCalls += it.Meshes;
            localStats.Triangles += it.Tris;
            localStats.Vertices += it.Verts;
        }

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        for (Shader* p : appliedPrograms) { p->Bind(); p->SetInt("uAlphaBlend", 0); } // restore for next frame's opaque pass
    }
    } // end "Scene Draw" profile scope
    if (outStats) *outStats = localStats;
}
