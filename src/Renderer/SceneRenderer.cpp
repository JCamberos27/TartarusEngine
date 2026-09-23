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
#include "Camera.h"    // MakePerspective — the view-model sub-pass's one projection switch
#include "ParticleRenderer.h"
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

    // #162 - fog.
    program.SetInt("uFogMode", fs.world && fs.world->FogEnabled ? std::clamp(fs.world->FogMode, 1, 3) : 0);
    if (fs.world && fs.world->FogEnabled) {
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
    // Which frame state the program selector pushes. The view-model sub-pass swaps this for its
    // own (see below) — that swap is the only reason it isn't a plain const FrameState.
    const FrameState* activeFs = &fs;

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
            key = ShaderVariantKeyFor(ma->Mat, *ma->Shader);
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

    for (auto entity : world.Registry.view<TransformComponent, RenderableComponent>()) {
        if (world.Registry.any_of<InactiveTag, LodCulledTag>(entity)) continue; // GameObject active / LOD level (#163)

        // Editor Scene viewport only: per-entity SceneVis hide (#236 B) + per-layer visibility
        // mask (#236 A1). The scene, saves and Game view are unaffected.
        if (ctx.EditorView) {
            if (world.Registry.all_of<HiddenInSceneTag>(entity)) continue;
            const auto* lc = world.Registry.try_get<LayerComponent>(entity);
            const int layer = lc ? lc->Layer : 0;
            if (layer >= 0 && layer < 32 && !((in.layerVisibleMask >> layer) & 1u)) continue;
        }
        auto& renderable = world.Registry.get<RenderableComponent>(entity);
        // #163 - Shadows Only: drawn by the shadow passes, never in the camera view.
        if (renderable.CastShadows == RenderableComponent::ShadowCasting::ShadowsOnly) continue;
        // Tagged view-model geometry belongs to the sub-pass below when there is one; with no
        // sub-pass this view draws it here like anything else. `isViewModel` is therefore just
        // "route to the other list", not "hide".
        const bool isViewModel = viewModelPass && world.Registry.all_of<ViewModelTag>(entity);
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
            if (!(isViewModel ? vmFrustum : camFrustum).Intersects(worldBounds)) {
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
            const DrawItem item{ model, m, &slots, matKey, prog,
                m->MeshCount(), (int)m->TriangleCount(), (int)m->VertexCount(),
                MaterialAsset::Queue::Transparent, qi, viewDepth, centre, Model::MeshPass::Transparent,
                renderable.ReceiveShadows, layerBit };
            (isViewModel ? viewModelTransparentList : transparentList).push_back(item);
        }
        if (anyOpaque) {
            const DrawItem item{ model, m, &slots, matKey, prog,
                m->MeshCount(), (int)m->TriangleCount(), (int)m->VertexCount(),
                MaterialAsset::Queue::Opaque, 2000, viewDepth, centre, opaquePass,
                renderable.ReceiveShadows, layerBit };
            (isViewModel ? viewModelList : drawList).push_back(item);
        }
    }

    // --- Opaque pass: sort by shader program (the costliest switch), then material key, then
    // front-to-back within a material (#112 — cheaper overdraw: nearer surfaces fill depth
    // first and hide what is behind them). Shared with the view-model sub-pass, which draws the
    // same way under a different projection.
    const auto opaqueOrder = [](const DrawItem& a, const DrawItem& b) {
        if (a.Prog != b.Prog) return std::less<const Shader*>()(a.Prog, b.Prog);
        if (a.MatKey != b.MatKey) return a.MatKey < b.MatKey;
        return a.ViewDepth < b.ViewDepth;
    };
    std::sort(drawList.begin(), drawList.end(), opaqueOrder);

    // #108 — reflection probes are chosen per object (from its bounds centre), not once per
    // frame from the camera position, which gave every object in view the same two probes and
    // made reflections pop as the camera moved. Only when the scene has probes at all.
    // #163 - and Receive Shadows is per object too; both go through this per-draw hook.
    const DrawItem* probeItem = nullptr;
    const bool anyProbes = in.probeArray->Count() > 0;
    const std::function<void(Shader&)> perDraw = [&](Shader& prog) {
        if (anyProbes) in.probeArray->Bind(prog, probeItem->Centre);
        prog.SetInt("uNoReceiveShadows", probeItem->ReceiveShadows ? 0 : 1);
        prog.SetInt("uObjectLayerBit", probeItem->LayerBit); // #203
    };

    passAlphaBlend = 0;
    modelShader.SetInt("uAlphaBlend", 0); // explicit: ensure opaque pass outputs alpha=1
    for (const DrawItem& it : drawList) {
        probeItem = &it;
        it.Ref->DrawSelected(modelShader, it.Xform, *it.Slots, selectProgram, 1.0f, perDraw, it.Pass);

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
            probeItem = &it;
            // Opacity comes from each transparent submesh's own slot (#112).
            it.Ref->DrawSelected(modelShader, it.Xform, *it.Slots, selectProgram, 1.0f, perDraw, it.Pass);

            localStats.DrawCalls += it.Meshes;
            localStats.Triangles += it.Tris;
            localStats.Vertices += it.Verts;
        }

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        for (Shader* p : appliedPrograms) { p->Bind(); p->SetInt("uAlphaBlend", 0); } // restore for next frame's opaque pass
    }

    // #177 - particles last: depth-tested against everything above, blended, no depth write.
    {
        // Deliberately leaked: a function-local static would run its GL deletes at exit, after
        // the context is gone. The OS reclaims the handful of GL objects with the process.
        static ParticleRenderer* particles = new ParticleRenderer();
        if (particles->Draw(world, ctx) > 0) ++localStats.DrawCalls;
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

        // Re-fit the froxel lists to this projection. They were culled for the world's FOV, so
        // reading them as-is would light the arms from slices built for a different one. Safe
        // here: the world's draws have already consumed this frame's lists.
        RenderFrameContext vmCtx = ctx;
        vmCtx.Proj = vmProj;
        const FrameState vmFs = GatherFrameState(world, vmCtx, in);
        if (vmFs.clusterOn) {
            clusterGrid.Cull(*in.clusterBuildShader, *in.clusterCullShader, vmCtx.View, vmCtx.Proj,
                             vmFs.clusterNearZ, vmFs.clusterFarZ, vmFs.vp[2], vmFs.vp[3]);
        }

        // Everything below runs against the view model's frame state — uProj above all, which is
        // the entire point of the pass.
        activeFs = &vmFs;
        appliedPrograms.clear();
        appliedPrograms.push_back(&modelShader);
        passAlphaBlend = 0;
        ApplyFrameState(modelShader, vmFs);
        modelShader.SetInt("uAlphaBlend", 0);

        std::sort(viewModelList.begin(), viewModelList.end(), opaqueOrder);
        for (const DrawItem& it : viewModelList) {
            probeItem = &it;
            it.Ref->DrawSelected(modelShader, it.Xform, *it.Slots, selectProgram, 1.0f, perDraw, it.Pass);

            localStats.DrawCalls += it.Meshes;
            localStats.Triangles += it.Tris;
            localStats.Vertices += it.Verts;
        }

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

            std::sort(viewModelTransparentList.begin(), viewModelTransparentList.end(),
                [](const DrawItem& a, const DrawItem& b) {
                    if (a.QueueIndex != b.QueueIndex) return a.QueueIndex < b.QueueIndex;
                    if (a.ViewDepth  != b.ViewDepth)  return a.ViewDepth  > b.ViewDepth;
                    return a.MatKey < b.MatKey;
                });

            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
            glBlendFuncSeparate(GL_SRC_ALPHA,  GL_ONE_MINUS_SRC_ALPHA,
                                0x0001/*GL_ONE*/, GL_ONE_MINUS_SRC_ALPHA);
            for (const DrawItem& it : viewModelTransparentList) {
                probeItem = &it;
                it.Ref->DrawSelected(modelShader, it.Xform, *it.Slots, selectProgram, 1.0f, perDraw, it.Pass);

                localStats.DrawCalls += it.Meshes;
                localStats.Triangles += it.Tris;
                localStats.Vertices += it.Verts;
            }
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            for (Shader* p : appliedPrograms) { p->Bind(); p->SetInt("uAlphaBlend", 0); }
        }

        activeFs = &fs; // nothing below reads it; leave the selector on the caller's state
    }
    } // end "Scene Draw" profile scope
    if (outStats) *outStats = localStats;
}
