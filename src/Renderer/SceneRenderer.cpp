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

// Extracted verbatim from main.cpp's `drawScene` lambda (audit #359, pass 2). No behavior
// change: identical GL calls, identical uniform order. The lambda's ~25 `[&]`-captures are now
// members of `in` (SceneRenderInputs); everything else (EditorSettings toggles, layer mask)
// arrives as plain values on `in` too, so this translation unit has no editor dependency.
void SceneRenderer::RenderScene(World& world, const RenderFrameContext& ctx,
                                const SceneRenderInputs& in, RenderStats* outStats) {
    Sky& sky = *in.sky;
    Shader& modelShader = *in.modelShader;
    CascadedShadowMap& shadowMap = *in.sunShadow;
    SpotShadowMap& spotShadowMap = *in.spotShadow;
    PointShadowMap& pointShadowMap = *in.pointShadow;
    IblProbe& iblProbe = *in.iblProbe;
    ReflectionProbeArray& probeArray = *in.probeArray;
    LightBuffer& lightBuffer = *in.lightBuffer;
    ClusterGrid& clusterGrid = *in.clusterGrid;

    // Shadows for this view. Spot and point shadows only need shadows-enabled +
    // a lit pass; the cascaded SUN shadow additionally needs its once-per-frame
    // pass to have actually run (which requires a directional light). These used to
    // share one flag, so deleting the sun silently killed spot/point shadows too.
    bool shadowsOn = in.shadowsEnabled && !ctx.Unlit;
    bool sunShadowsOn = shadowsOn && in.sunShadowsReady;

    // PR13: sky draw — HDRI cubemap or procedural gradient
    if (world.SkySourceMode == World::SkySource::Hdri && in.hdriCube) {
        float rotRad = glm::radians(world.SkyRotationDegrees);
        sky.DrawHdri(in.hdriCube->Texture(), rotRad, ctx.View, ctx.Proj);
    } else {
        sky.Draw(ctx.View, ctx.Proj, world.SkyHorizonColor, world.SkyZenithColor);
    }

    modelShader.Bind();
    modelShader.SetMat4("uView", ctx.View);
    modelShader.SetMat4("uProj", ctx.Proj);
    modelShader.SetVec3("uViewPos", ctx.ViewPos);
    modelShader.SetInt("uDebugView", ctx.DebugView); // #236 R2 scene-view debug modes

    // The caller set the viewport before this call and nothing here changes it, so read it once
    // (audit PERF-102: was three separate glGetIntegerv(GL_VIEWPORT) round-trips in this pass).
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);

    // Cascaded-shadow uniforms + the depth array on unit 8 (material maps use 1..7).
    modelShader.SetInt("uShadowEnabled", sunShadowsOn ? 1 : 0);
    modelShader.SetInt("uShadowCascadeCount", shadowMap.Count());
    {
        glm::mat4 mats[CascadedShadowMap::kMaxCascades];
        for (int i = 0; i < shadowMap.Count(); ++i) mats[i] = shadowMap.LightViewProj(i);
        modelShader.SetMat4Array("uShadowMatrices[0]", shadowMap.Count(), mats);
    }
    modelShader.SetVec4("uCascadeSplits", shadowMap.SplitDepthsVec4());
    // World units per shadow texel, per cascade — the shader scales its normal
    // offset + depth bias by the selected cascade's value so one number isn't
    // simultaneously too much for cascade 0 and too little for cascade 3 (#117).
    modelShader.SetVec4("uShadowTexelWorld", shadowMap.TexelWorldSizesVec4());
    // Penumbra width in shadow-map texels, from the sun's apparent size. 0.53 deg
    // (real sun) -> a tight ~2 texel edge; crank the light's Angular Size for softer.
    modelShader.SetFloat("uShadowSoftness",
        std::clamp(in.sunAngularDeg * 3.0f, 1.0f, 14.0f) * in.sunShadowSoftness);
    // Per-light sun shadow multipliers (#140 phase 2); 1.0 == pre-phase-2 output.
    modelShader.SetFloat("uSunShadowBias", in.sunShadowBias);
    modelShader.SetFloat("uSunShadowNormalBias", in.sunShadowNormalBias);
    glActiveTexture(GL_TEXTURE0 + 8);
    {
        unsigned int sunTex = shadowMap.DepthArray();
        glBindTexture(GL_TEXTURE_2D_ARRAY, sunTex ? sunTex : DefaultTextures::DepthArray());
    }
    modelShader.SetInt("uShadowMap", 8);
    modelShader.SetFloat("uShadowMapResolution", (float)shadowMap.Resolution()); // #190

    // Spot-light shadow maps on unit 9 (#119). Count is zeroed when shadows are off
    // or in Unlit so the shader's SpotShadow() early-outs.
    int spotCountForView = shadowsOn ? in.spotShadowCount : 0;
    modelShader.SetInt("uSpotShadowCount", spotCountForView);
    if (spotCountForView > 0) {
        // Whole-array uploads, one glUniform*fv each — was a per-index std::to_string-built
        // uniform-name loop every frame (audit PERF-102). The arrays alias main.cpp's
        // kMaxSpots-sized stack storage; only the first spotCountForView entries are sent.
        modelShader.SetMat4Array ("uSpotShadowVP[0]",         spotCountForView, in.spotShadowVP);
        modelShader.SetVec3Array ("uSpotShadowPos[0]",        spotCountForView, in.spotShadowPos);
        modelShader.SetFloatArray("uSpotShadowFar[0]",        spotCountForView, in.spotShadowFar);
        modelShader.SetFloatArray("uSpotShadowHalfTan[0]",    spotCountForView, in.spotShadowHalfTan);
        modelShader.SetFloatArray("uSpotShadowBias[0]",       spotCountForView, in.spotShadowBias);
        modelShader.SetFloatArray("uSpotShadowNormalBias[0]", spotCountForView, in.spotShadowNormalBias);
        modelShader.SetFloatArray("uSpotShadowSoftness[0]",   spotCountForView, in.spotShadowSoftness);
    }
    glActiveTexture(GL_TEXTURE0 + 9);
    // A valid depth-compare texture even when no spot shadow map is allocated, so
    // the sampler2DArrayShadow on unit 9 is never backed by texture 0 (audit #357:
    // was a per-frame KHR 131222 "shadow sampler ... undefined behavior"). The
    // shader still early-outs on uSpotShadowCount == 0.
    {
        unsigned int spotTex = spotShadowMap.DepthArray();
        glBindTexture(GL_TEXTURE_2D_ARRAY, spotTex ? spotTex : DefaultTextures::DepthArray());
    }
    modelShader.SetInt("uSpotShadowMap", 9);
    modelShader.SetFloat("uSpotShadowMapResolution", (float)spotShadowMap.Resolution()); // #190

    // Point-light cube shadow maps on unit 10 (#119).
    int pointCountForView = shadowsOn ? in.pointShadowCount : 0;
    modelShader.SetInt("uPointShadowCount", pointCountForView);
    if (pointCountForView > 0) {
        modelShader.SetFloatArray("uPointShadowFar[0]",        pointCountForView, in.pointShadowFar);
        modelShader.SetFloatArray("uPointShadowBias[0]",       pointCountForView, in.pointShadowBias);
        modelShader.SetFloatArray("uPointShadowNormalBias[0]", pointCountForView, in.pointShadowNormalBias);
    }
    glActiveTexture(GL_TEXTURE0 + 10);
    { // valid samplerCubeArrayShadow backing even with no point shadow map (audit #357)
        unsigned int pointTex = pointShadowMap.DepthCubeArray();
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, pointTex ? pointTex : DefaultTextures::DepthCubeArray());
    }
    modelShader.SetInt("uPointShadowMap", 10);
    modelShader.SetFloat("uPointShadowMapResolution", (float)pointShadowMap.Resolution()); // #190

    // IBL probes on units 11/12/13 (#196). The shader declares those units with
    // layout(binding=) qualifiers, so there's no SetInt here — just the bind. Unlit
    // mode skips lighting entirely, so it doesn't need them either.
    bool iblOn = iblProbe.IsValid() && !ctx.Unlit;
    if (iblOn) {
        glActiveTexture(GL_TEXTURE0 + 11);
        glBindTexture(GL_TEXTURE_CUBE_MAP, iblProbe.IrradianceMap());
        glActiveTexture(GL_TEXTURE0 + 12);
        glBindTexture(GL_TEXTURE_CUBE_MAP, iblProbe.SpecularMap());
        glActiveTexture(GL_TEXTURE0 + 13);
        glBindTexture(GL_TEXTURE_2D, iblProbe.BrdfLut());
        modelShader.SetFloat("uIBLIntensity", std::max(world.SkyAmbientIntensity, 0.0f));
        modelShader.SetFloat("uIBLSpecularMaxLod", (float)(IblProbe::kSpecularMips - 1));
    }
    modelShader.SetInt("uIBLEnabled", iblOn ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);

    // PR14: bind nearest 2 probes to modelShader for parallax box projection.
    // Uses ctx.ViewPos as the draw centroid; no-op (uProbeCount=0) when scene has none.
    probeArray.Bind(modelShader, ctx.ViewPos);

    // PR15: SSAO occlusion map (unit 15). Pre-computed before this drawScene call.
    // ctx.SsaoSrc is the caller's own Ssao instance (Scene and Game view each have their
    // own, since both can be rendering in the same frame at different resolutions).
    // IsValid() is false until the first SSAO-enabled frame fills that instance's FBOs.
    bool ssaoOn = in.ssaoEnabled && ctx.SsaoSrc && ctx.SsaoSrc->IsValid() && !ctx.Unlit;
    if (ssaoOn) {
        glActiveTexture(GL_TEXTURE0 + 15);
        glBindTexture(GL_TEXTURE_2D, ctx.SsaoSrc->OcclusionTexture());
        modelShader.SetInt("uSSAOMap", 15);
        modelShader.SetVec2("uScreenSize", glm::vec2((float)vp[2], (float)vp[3]));
        glActiveTexture(GL_TEXTURE0);
    }
    modelShader.SetInt("uSSAOEnabled", ssaoOn ? 1 : 0);

    // The light SSBO (binding 0) is built once per frame above — just bind it.
    lightBuffer.Bind(0);

    // Clustered-forward light culling (#120). Dice this view's frustum and assign
    // every point/spot light to the froxels it reaches, so the fragment shader loops
    // a short per-froxel list instead of all uLightCount lights. Perspective views
    // only — the froxel AABB build assumes rays from the eye, so an orthographic
    // editor view (not perf-critical) falls back to the full loop.
    bool perspective = std::abs(ctx.Proj[3][3]) < 0.5f; // proj[3][3] == 1 for ortho
    bool clusterOn = perspective && !ctx.Unlit && vp[2] > 0 && vp[3] > 0;
    if (clusterOn) {
        PROFILE_GPU_SCOPE("Cluster Cull");
        float nearZ = std::abs(ctx.Proj[3][2] / (ctx.Proj[2][2] - 1.0f));
        float farZ  = std::abs(ctx.Proj[3][2] / (ctx.Proj[2][2] + 1.0f));
        clusterGrid.Cull(*in.clusterBuildShader, *in.clusterCullShader, ctx.View, ctx.Proj,
                         nearZ, farZ, vp[2], vp[3]);
        modelShader.Bind(); // Cull() left a compute program bound
        clusterGrid.BindForShading();
        modelShader.SetVec2("uClusterScreenSize", glm::vec2((float)vp[2], (float)vp[3]));
        modelShader.SetVec4("uClusterZParams", ClusterGrid::ZParams(nearZ, farZ));
    }
    modelShader.SetInt("uClusterEnabled", clusterOn ? 1 : 0);

    modelShader.SetInt("uUnlit", ctx.Unlit ? 1 : 0);
    // Real scene path: emit linear HDR; the shared Tonemapper pass maps it after
    // MSAA resolve. (Offscreen model thumbnails set this to 1 to self-tonemap.)
    modelShader.SetInt("uApplyTonemap", 0);

    // One draw loop for everything placed in the world — former level-geometry
    // boxes render through the exact same PBR path as imported/primitive models now,
    // since both are just entities with a Transform + Renderable.
    RenderStats localStats;
    localStats.PointLights = std::max(0, in.frameLightCount - 1); // minus the directional sun
    localStats.LightBufferOverflowed = lightBuffer.Overflowed(); // #204
    localStats.ClusterSaturated = clusterOn && clusterGrid.Saturated(); // #204
    Frustum camFrustum = Frustum::FromViewProj(ctx.Proj * ctx.View);
    { // scope limits PROFILE_SCOPE to just this loop, not the rest of the frame
    PROFILE_SCOPE("Scene Draw");
    PROFILE_GPU_SCOPE("Scene Draw"); // shared by both Scene-tab and Game-tab draws
    // #194: resolve once, outside the per-entity loop below.
    int modelModelLoc = modelShader.Loc("uModel");
    int modelNormalMatrixLoc = modelShader.Loc("uNormalMatrix");

    // #192: gather the frustum-culled visible set, then sort it by material before
    // drawing, so entities sharing a material land adjacent — which is what makes
    // the BindMaterial dedup (GLStateCache::MaterialAlreadyBound) actually hit. All
    // scene geometry is opaque and depth-tested, so reordering is invisible. The
    // buffer is static so it is reused across the Scene- and Game-tab passes and
    // across frames rather than reallocated each call.
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

        // Editor Scene viewport only: per-entity SceneVis hide (#236 B) + per-layer
        // visibility mask (#236 A1). The scene, saves and Game view are unaffected.
        if (ctx.EditorView) {
            if (world.Registry.all_of<HiddenInSceneTag>(entity)) continue;
            const auto* lc = world.Registry.try_get<LayerComponent>(entity);
            const int layer = lc ? lc->Layer : 0;
            if (layer >= 0 && layer < 32 && !((in.layerVisibleMask >> layer) & 1u)) continue;
        }
        auto& renderable = world.Registry.get<RenderableComponent>(entity);
        glm::mat4 model = world.GetCachedWorldTransform(entity);

        // Frustum culling: skip the draw call entirely for anything outside the
        // camera's view. Bounds come from the same Model::BoundsMin/Max already used
        // for gizmo framing and Snap to Ground - an invalid (never-populated, e.g. a
        // failed import) bounds pair draws unconditionally rather than risk hiding it.
        glm::vec3 boundsMin = renderable.ModelRef->BoundsMin();
        glm::vec3 boundsMax = renderable.ModelRef->BoundsMax();
        bool validBounds = boundsMin.x <= boundsMax.x && boundsMin.y <= boundsMax.y && boundsMin.z <= boundsMax.z;
        if (validBounds) {
            // Bounds are bind-pose only. A skinned model's limbs can swing well past
            // them (reach, jump, weapon), so inflate around the centre before the
            // frustum test for animated models — still culls one that's genuinely
            // far off-screen, without popping the shadow/mesh of one at the edge (#113).
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
            // Compute view-space depth of entity centre for back-to-front sort.
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

    modelShader.SetInt("uAlphaBlend", 0); // explicit: ensure opaque pass outputs alpha=1
    for (const DrawItem& it : drawList) {
        modelShader.SetMat4(modelModelLoc, it.Xform);
        // Normal matrix (inverse-transpose) computed here, not per-vertex (#104).
        modelShader.SetMat4(modelNormalMatrixLoc,
            glm::mat4(glm::transpose(glm::inverse(glm::mat3(it.Xform)))));
        it.Ref->Draw(modelShader, *it.Slots);

        localStats.DrawCalls += it.Meshes;
        localStats.Triangles += it.Tris;
        localStats.Vertices += it.Verts;
    }

    // --- Transparent pass: back-to-front sorted, blended, no depth write -----------
    if (!transparentList.empty()) {
        // PR12: resolve MSAA opaque color and copy into mipped texture for refraction.
        if (ctx.TxHdr && ctx.TxCapture) {
            ctx.TxHdr->ResolveTo();
            ctx.TxCapture->CopyFrom(ctx.TxHdr->ResolvedColorTexture(), vp[2], vp[3]);
            ctx.TxHdr->BindForRender(); // rebind MSAA FBO for the transparent draw pass
            glActiveTexture(GL_TEXTURE0 + 14);
            glBindTexture(GL_TEXTURE_2D, ctx.TxCapture->Texture());
            modelShader.SetInt("uOpaqueColor", 14);
            modelShader.SetVec2("uScreenSize", glm::vec2((float)vp[2], (float)vp[3]));
        }

        // Sort: QueueIndex ascending, then ViewDepth descending (farthest first),
        // then MatKey for dedup within the same depth bucket.
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
        modelShader.SetInt("uAlphaBlend", 1);

        for (const DrawItem& it : transparentList) {
            float opacity = (!it.Slots->empty() && (*it.Slots)[0])
                ? (*it.Slots)[0]->Opacity : 1.0f;
            modelShader.SetFloat("uOpacity", opacity);
            modelShader.SetMat4(modelModelLoc, it.Xform);
            modelShader.SetMat4(modelNormalMatrixLoc,
                glm::mat4(glm::transpose(glm::inverse(glm::mat3(it.Xform)))));
            it.Ref->Draw(modelShader, *it.Slots);

            localStats.DrawCalls += it.Meshes;
            localStats.Triangles += it.Tris;
            localStats.Vertices += it.Verts;
        }

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        modelShader.SetInt("uAlphaBlend", 0); // restore for next frame's opaque pass
    }
    } // end "Scene Draw" profile scope
    if (outStats) *outStats = localStats;
}
