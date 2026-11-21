// SceneRenderer::RenderScene, stage by stage (SceneViewPass.h): one ViewPass per call holds the view's state, and
// Run() calls the stages in drawing order.
#include "SceneViewPass.h"
#include "AssetCost.h"
#include "RenderFrameContext.h"

#include "World.h"
#include "Components.h"
#include "Sky.h"
#include "SkyAtmosphere.h"
#include "VolumetricFog.h"
#include "PuddleSsr.h"
#include "Shader.h"
#include "Cubemap.h"
#include "ReflectionProbeArray.h"
#include "GIVolumes.h"
#include "LightBuffer.h"
#include "ClusterGrid.h"
#include "DecalSystem.h"
#include "HdrTarget.h"
#include "OpaqueColorCopy.h"
#include "Ssao.h"
#include "SkinHideBuffer.h"
#include "MaterialAsset.h"
#include "ShaderVariant.h"
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
#include "gl.h"
#include "Core/Profiler.h"
#include "Core/Env.h"
#include "TimeService.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

// Per-run tally of how many meshes drew through each ShaderAsset variant key (audit #354). Read
// by --smoke-test to log which lobes actually rendered; harmless in normal runs.
namespace {
std::unordered_map<std::uint32_t, std::uint64_t> g_variantDrawCounts;
bool g_countVariants = false; // set by ResetVariantDrawCounts: only a run that reads the tally pays for it

// How far from the eye the view model (arms, weapon, the player body's sleeves) can reach, metres: decals
// farther than this can't touch it, so its pass skips the decal cull (DecalSystem::NearestDynamicReceiverDecal).
constexpr float kViewModelDecalReach = 3.0f;

bool Perspective(const glm::mat4& proj) { return std::abs(proj[3][3]) < 0.5f; }
} // namespace

namespace SceneRendererDebug {
const std::unordered_map<std::uint32_t, std::uint64_t>& VariantDrawCounts() { return g_variantDrawCounts; }
void ResetVariantDrawCounts() {
    g_variantDrawCounts.clear();
    g_countVariants = true;
}
}

ParticleRenderer* SceneParticles() {
    static ParticleRenderer* particles = new ParticleRenderer();
    return particles;
}

void SceneRenderer::RenderScene(World& world, const RenderFrameContext& ctx,
                                const SceneRenderInputs& in, RenderStats* outStats) {
    ViewPass pass(*this, world, ctx, in);
    pass.Run();
    if (outStats) *outStats = pass.Stats();
}

SceneRenderer::ViewPass::ViewPass(SceneRenderer& renderer, World& world, const RenderFrameContext& ctx,
                                  const SceneRenderInputs& in)
    : m_R(renderer), m_World(world), m_Ctx(ctx), m_In(in), m_S(*renderer.m_Scratch), m_ModelShader(*in.modelShader) {
    m_Select = [this](const MaterialAsset* ma) { return SelectProgram(ma); };
    m_PerDraw = [this](Shader& prog) { PerDraw(prog); };
}

void SceneRenderer::ViewPass::Run() {
    PrepareView();
    SetUpViewModel();
    { // scope limits PROFILE_SCOPE to the draws, not the view's setup above
    PROFILE_SCOPE("Scene Draw");
    PROFILE_GPU_SCOPE("Scene Draw"); // shared by both Scene-tab and Game-tab draws
    SetUpPrograms();
    BuildDrawLists();
    SortItems(m_S.DrawList, false);
    ReportTris();
    if (AssetCost::Enabled() && !m_Ctx.ProbeCapture) // TARTARUS_ASSET_COST (--perf-bench): what this view drew
        AssetCost::NoteView(m_World, m_Ctx.EditorView,
                            {&m_S.DrawList, &m_S.TransparentList, &m_S.ViewModelList, &m_S.ViewModelTransparentList});
    DepthPrepass();
    BuildBatches();
    DrawOpaque();
    DrawFurAndBlood();
    DrawSky();
    // --- Transparent pass: back-to-front sorted, blended, no depth write -----------
    if (!m_S.TransparentList.empty()) {
        PROFILE_GPU_SCOPE("Transparent");
        DrawTransparentList(m_S.TransparentList);
    }
    DrawEffects();
    DrawViewModel();
    } // end "Scene Draw" profile scope
}

void SceneRenderer::ViewPass::PrepareView() {
    const SceneRenderInputs& in = m_In;
    const RenderFrameContext& ctx = m_Ctx;
    // The physical sky renders this view's aerial perspective and clouds up front (compute
    // passes, no FBO change): the model shader samples the aerial volume. The sky itself draws
    // after the opaque pass (DrawSky), only where no geometry landed.
    // With a depth pre-pass, the clouds wait for it (DepthPrepass): what the scene hides isn't marched.
    m_CloudsAfterDepth = in.skyAtmosphere && in.depthShader && ProjectSettings::Quality().DepthPrepass;
    if (in.skyAtmosphere) in.skyAtmosphere->RenderView(ctx.TxHdr, ctx.View, ctx.Proj, ctx.ViewPos, m_CloudsAfterDepth);

    FrameState gathered = m_R.GatherFrameState(m_World, ctx, in);
    // Grass Areas (GrassRenderer): gathered once a frame; scattered and drawn per view, never in a probe's room
    // capture, a debug view or an orthographic one.
    GrassRenderer& grass = GrassRenderer::Get();
    grass.Gather(m_World);
    m_GrassOn = grass.HasAreas() && !ctx.ProbeCapture && ctx.DebugView == 0 && Perspective(ctx.Proj);
    // Clustered decals (docs/DECALS.md): this view's visible set, uploaded before the cluster cull
    // bins it. They need the froxel lists, so nothing without clustering (the previews) gets any.
    if (in.decals && in.decalCullShader && gathered.clusterOn) {
        PROFILE_SCOPE("Decal Prepare");
        DecalGrading grading;
        for (auto [e, g] : m_World.Registry.view<const DecalGradingComponent>().each()) {
            if (m_World.Registry.all_of<InactiveTag>(e)) continue;
            grading.Age = std::clamp(g.Age, 0.0f, 1.0f);
            grading.Wear = std::clamp(g.Wear, 0.0f, 1.0f);
            break; // the first one in the scene wins
        }
        gathered.decalCount = in.decals->Prepare(m_World, ctx.Proj * ctx.View, ctx.ViewPos,
                                                 ProjectSettings::Quality().DecalMaxDistance, grading, ctx.EditorView);
        in.decals->BindForCull();
    }
    // Puddle SSR: wanted with standing water, IBL and a depth pre-pass to trace against; the trace itself runs after
    // that pre-pass. Not for probe captures, debug views or orthographic views.
    m_SsrWanted = m_World.Render.PuddleSsr && m_World.Render.PuddleAmount > 0.0f && gathered.iblOn && !ctx.ProbeCapture &&
                  ctx.DebugView == 0 && ctx.TxHdr && ctx.TxHdr->IsValid() && in.depthShader &&
                  ProjectSettings::Quality().DepthPrepass && Perspective(ctx.Proj) &&
                  gathered.vp[0] == 0 && gathered.vp[1] == 0 && gathered.vp[2] == ctx.TxHdr->Width() &&
                  gathered.vp[3] == ctx.TxHdr->Height();
    gathered.ssrTex = m_SsrWanted ? m_R.m_Ssr->Prepare(ctx.TxHdr) : 0u;
    m_Fs = gathered;
    m_ActiveFs = &m_Fs;
    const FrameState& fs = m_Fs;

    // Clustered-forward light culling (#120) — one compute dispatch per frame, before any
    // program that reads the froxel lists is bound.
    if (fs.clusterOn) {
        PROFILE_GPU_SCOPE("Cluster Cull");
        // The cull reads the lights at binding 0: this frame's, not whatever an editor preview (the outfit or material
        // preview, lit by its own fixed lights) left bound there - binned from those, every lamp went dark.
        in.lightBuffer->Bind(0);
        in.clusterGrid->Cull(*in.clusterBuildShader, *in.clusterCullShader, ctx.View, ctx.Proj,
                             fs.clusterNearZ, fs.clusterFarZ, fs.vp[2], fs.vp[3], in.decalCullShader, fs.decalCount);
    }

    // Froxel volumetric fog: inject + light this view's froxels (reading the cluster lists just
    // culled and this frame's shadow maps), then integrate them, before any program samples it.
    if (fs.volOn) {
        PROFILE_GPU_SCOPE("Volumetric Fog");
        m_R.m_Volumetrics->Render(fs);
        GLStateCache::Invalidate(); // it rebinds material units 6 and 7
    }

    // Bind the default program and push the common per-frame state onto it. Meshes with a linked
    // ShaderAsset select a variant program in the draw loop and get this same state via ApplyFrameState.
    m_R.ApplyFrameState(m_ModelShader, fs);

    m_Stats.PointLights = std::max(0, in.frameLightCount - 1); // minus the directional sun
    m_Stats.LightBufferOverflowed = in.lightBuffer->Overflowed();  // #204
    m_Stats.ClusterSaturated = fs.clusterOn && in.clusterGrid->Saturated(); // #204
    m_CamFrustum = Frustum::FromViewProj(ctx.Proj * ctx.View);
}

void SceneRenderer::ViewPass::SetUpViewModel() {
    // --- View-model sub-pass setup -----------------------------------------------------------
    // Entities tagged ViewModelTag (the first-person arms + weapon) are routed into their own
    // lists (BuildDrawLists) and drawn last, under a projection that differs from the world's only in FOV:
    // near/far are read back out of ctx.Proj, so depth stays in the same terms the cluster build
    // and everything else this frame already assume. Disabled (-1 default) for callers that
    // preview through their own camera, and for orthographic views, where a perspective
    // ViewModelFov would be meaningless — both fall back to drawing them as ordinary geometry.
    const RenderFrameContext& ctx = m_Ctx;
    m_ViewModelPass = ctx.ViewModelFov > 0.0f && Perspective(ctx.Proj) && m_Fs.vp[2] > 0 && m_Fs.vp[3] > 0;
    m_VmProj = ctx.Proj;
    m_VmFrustum = m_CamFrustum;
    if (m_ViewModelPass) {
        const float nearZ = std::abs(ctx.Proj[3][2] / (ctx.Proj[2][2] - 1.0f));
        const float farZ  = std::abs(ctx.Proj[3][2] / (ctx.Proj[2][2] + 1.0f));
        m_VmProj = MakePerspective(ctx.ViewModelFov, (float)m_Fs.vp[2] / (float)m_Fs.vp[3], nearZ, farZ);
        m_VmFrustum = Frustum::FromViewProj(m_VmProj * ctx.View);
    }
}

void SceneRenderer::ViewPass::SetUpPrograms() {
    // Program selection (audit #354). A mesh whose slot-0 material links a ShaderAsset draws
    // through that asset's variant program (keyword mask from the material's authored lobes);
    // everything else draws through the model shader, which already got ApplyFrameState. Each
    // distinct program is brought up to this frame's state exactly once.
    m_S.AppliedPrograms.clear();
    m_S.AppliedPrograms.push_back(&m_ModelShader);
    m_PassAlphaBlend = 0;
    m_NoLocalLights = m_In.lightBuffer->Count() == m_In.lightBuffer->DirectionalCount();
}

// The code this view's programs can leave out (ShaderVariant.h engine bits): no point or spot light this frame,
// no clustered decal in the view, no debug mode. m_ActiveFs is the pass's own state (the view-model pass has its
// own decal count).
std::uint32_t SceneRenderer::ViewPass::FrameVariantBits() const {
    if (!EngineVariantsEnabled()) return 0u;
    std::uint32_t bits = 0u;
    if (m_NoLocalLights) bits |= kVariantNoLocalLights;
    if (m_ActiveFs->decalCount == 0) bits |= kVariantNoDecals;
    if (m_Ctx.DebugView == 0) bits |= kVariantNoDebugView;
    return bits;
}

Shader* SceneRenderer::ViewPass::ProgramFor(const MaterialAsset* ma, std::uint32_t& key) const {
    key = 0;
    if (ma && ma->Shader) {
        key = ShaderVariantKeyForCached(ma->Mat, *ma->Shader) | ShaderVariantEngineBits(ma->Mat) | FrameVariantBits();
        if (Shader* v = ma->Shader->Variant(key)) return v;
    }
    return &m_ModelShader;
}

Shader* SceneRenderer::ViewPass::SelectProgram(const MaterialAsset* ma) {
    std::uint32_t key = 0;
    Shader* prog = ProgramFor(ma, key);
    std::vector<Shader*>& applied = m_S.AppliedPrograms;
    if (std::find(applied.begin(), applied.end(), prog) == applied.end()) {
        m_R.ApplyFrameState(*prog, *m_ActiveFs);
        prog->SetInt("uAlphaBlend", m_PassAlphaBlend);
        applied.push_back(prog);
    }
    if (g_countVariants) g_variantDrawCounts[prog == &m_ModelShader ? 0u : key]++;
    return prog;
}

void SceneRenderer::ViewPass::BuildDrawLists() {
    const RenderFrameContext& ctx = m_Ctx;
    World& world = m_World;
    m_S.DrawList.clear();
    m_S.TransparentList.clear();
    m_S.ViewModelList.clear();
    m_S.ViewModelTransparentList.clear();
    m_AnyTransmission = false;

    // What this camera sees (GatherVisible): the editor Scene view's own hide filters, no Shadows Only, its copy of a
    // split pose. The frustum test is below - it has two frusta and an exemption.
    VisibilityQuery inView;
    inView.OwnerView = ctx.OwnerView;
    inView.EditorView = ctx.EditorView;
    inView.LayerVisibleMask = m_In.layerVisibleMask;
    if (ctx.ProbeCapture) { // a probe's room capture: the room, not the player or what's in the air
        inView.SkipViewModels = true;
        inView.SkipPlayerBody = true;
        inView.SkipTransparent = true;
    }
    // A GI bake capture draws only what's near the volumes being baked (the house, its yard through the windows): the
    // forest beyond was most of every tiny capture's cost and lights nothing inside.
    const AABB giClip{ctx.GIBakeClipMin, ctx.GIBakeClipMax};
    if (ctx.GIBake && ctx.GIBakeClipMin.x <= ctx.GIBakeClipMax.x) inView.Within = &giClip;
    // Too small to see from here (Project Settings > Quality); perspective views only. The SSAO depth pre-pass culls
    // with the same test, so the two agree on what stands in front.
    if (Perspective(ctx.Proj)) {
        inView.SizeCullEye = &ctx.ViewPos;
        inView.SizeCullProjScale = ctx.Proj[1][1];
        inView.MinScreenHeight = ProjectSettings::Quality().SmallObjectCullHeight;
    }
    // Whole cells outside both of the frusta the test below uses are skipped up front.
    const Frustum viewCells[2] = {m_CamFrustum, m_VmFrustum};
    inView.CellCull = viewCells;
    inView.CellCullCount = m_ViewModelPass ? 2 : 1;
    GatherVisible(world, inView, m_S.Visible);
    for (const VisibleRenderable& vis : m_S.Visible) {
        const entt::entity entity = vis.Entity;
        auto& renderable = *vis.Renderable;
        // Tagged view-model geometry belongs to the sub-pass when there is one; with no
        // sub-pass this view draws it here like anything else. `isViewModel` is therefore just
        // "route to the other list", not "hide".
        const bool hasViewModelTag = (vis.CandFlags & kCandViewModel) != 0;
        const bool isViewModel = m_ViewModelPass && hasViewModelTag;
        // The player's own body, around the camera: not in the view-model pass (its arms must be whole there).
        const auto* bodyTag = (vis.CandFlags & kCandPlayerBody) ? world.Registry.try_get<PlayerBodyTag>(entity) : nullptr;
        const bool cameraBody = bodyTag && !isViewModel && !ctx.EditorView && ctx.OwnerView;
        if (cameraBody && bodyTag->CameraHidden) continue; // the head: only the player's own camera skips it
        const float nearHide = cameraBody ? bodyTag->NearHide : 0.0f;
        const int* hideBones = cameraBody && bodyTag->CameraHideBones[0] >= 0 ? bodyTag->CameraHideBones : nullptr;
        // The SSAO pre-pass skips view-model geometry always and the camera's own body (its hidden parts
        // would occlude): read at their pixels, the map holds whatever stands behind them - see-through AO.
        const bool noSsao = cameraBody || hasViewModelTag;
        // Clothing on the player's arms while they're in the view-model pass: the sleeves are drawn there
        // too (a second copy, only them), the rest here without them - see PlayerBodyTag.
        const bool splitSleeves = m_ViewModelPass && cameraBody && bodyTag->SleevesInViewModel;
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
        const glm::mat4& model = vis.Xform;
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
        const glm::vec3& boundsMin = vis.LocalMin; // (their centre also sorts the transparent draws)
        const glm::vec3& boundsMax = vis.LocalMax;
        const bool validBounds = vis.Bounded;
        if (validBounds && !playerBody) {
            const AABB worldBounds{vis.WorldMin, vis.WorldMax};
            const bool seen = (isViewModel ? m_VmFrustum : m_CamFrustum).Intersects(worldBounds) ||
                              (splitSleeves && m_VmFrustum.Intersects(worldBounds));
            if (!seen) {
                m_Stats.Culled++;
                continue;
            }
        }

        Model* m = renderable.ModelRef.get();
        const auto& slots = renderable.Materials;
        const std::uint64_t matKey = (!slots.empty() && slots[0]) ? slots[0]->Mat.Hash() : m->MaterialSortKey();
        std::uint32_t progKeyUnused = 0;
        const Shader* prog = ProgramFor(!slots.empty() ? slots[0].get() : nullptr, progKeyUnused);
        // PR9 / #112: classify per submesh — a model whose slot 1 is transparent used to draw
        // entirely opaque because only slot 0's queue was checked (and vice versa). A model
        // with both kinds is queued in both passes, each drawing only its own submeshes.
        const int layerBit = (int)(1u << std::clamp(vis.Layer, 0, 31)); // #203 lighting layers (0 without a LayerComponent)
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
        // #112 — depth from the world-space bounds centre, not the entity origin (a large mesh
        // whose pivot sits at one end sorted as if it were all at that end).
        const glm::vec3 centre = validBounds ? glm::vec3(model * glm::vec4((boundsMin + boundsMax) * 0.5f, 1.0f))
                                             : glm::vec3(model[3]);
        const float viewDepth = -(ctx.View * glm::vec4(centre, 1.0f)).z;
        const std::uint64_t giMask =
            m_In.giVolumes ? (validBounds ? m_In.giVolumes->MaskFor(vis.WorldMin, vis.WorldMax) : m_In.giVolumes->AllMask()) : 0ull;
        // What both passes' items share; each pass then fills its own fields.
        const auto makeItem = [&](int queueIndex, Model::MeshPass pass) {
            DrawItem item{ model, m, &slots, matKey, prog,
                m->MeshCount(), (int)m->TriangleCount(), (int)m->VertexCount(),
                queueIndex, viewDepth, centre, pass,
                renderable.ReceiveShadows, layerBit, nearHide, hideBones, noSsao, hideVerts, nearHideWidth, collarVerts };
            item.StaticReceiver = staticReceiver;
            item.GIMask = giMask;
            item.SortId = (std::uint32_t)entt::to_integral(entity);
            item.DoubleSided = clothing;
            item.LayerPull = layerPull;
            item.LodFade = world.LodFadeOf(entity);
            if (outfitHide && outfitHide->Visible) {
                item.Visible = outfitHide->Visible.get();
                item.Tris = (int)item.Visible->Triangles();
            }
            if (anyCameraMask) {
                item.BoneMaskMode = 1;
                item.BoneMask = cameraMask;
            }
            return item;
        };
        // The view-model pass's copy of a split piece: only its sleeves.
        const auto pushSleeves = [&](const DrawItem& item, std::vector<DrawItem>& to) {
            DrawItem sleeves = item;
            sleeves.BoneMaskMode = 2;
            std::copy(std::begin(bodyTag->SleeveBones), std::end(bodyTag->SleeveBones), sleeves.BoneMask.begin());
            // Keeps the piece's Near Hide: the sleeve's top reaches the shoulder, at the eye in this
            // projection, and its open end showed as black slivers there.
            sleeves.HideBones = nullptr;
            // The sleeves are the arms' own: not the collar, and their Near Hide a plain sphere.
            sleeves.CollarVerts = 0;
            sleeves.NearHideWidth = 0.0f;
            to.push_back(sleeves);
        };
        if (anyTransparent) {
            if (!m_AnyTransmission) {
                for (int i = 0; i < m->MeshCount() && !m_AnyTransmission; ++i) {
                    const bool hasSlot = i < (int)slots.size() && slots[i];
                    if (!hasSlot || slots[i]->RenderQueue != MaterialAsset::Queue::Transparent) continue;
                    m_AnyTransmission = slots[i]->Mat.TransmissionStrength > 0.0f;
                }
            }
            const DrawItem item = makeItem(qi, Model::MeshPass::Transparent);
            if (splitSleeves) pushSleeves(item, m_S.ViewModelTransparentList);
            (isViewModel ? m_S.ViewModelTransparentList : m_S.TransparentList).push_back(item);
        }
        if (anyOpaque) {
            DrawItem item = makeItem(2000, opaquePass);
            item.Dynamic = dynamicItem;
            item.Entity = (unsigned)entt::to_integral(entity);
            item.WorldMin = vis.WorldMin;
            item.WorldMax = vis.WorldMax;
            item.Bounded = validBounds;
            item.Normal = vis.Normal;
            if (splitSleeves) pushSleeves(item, m_S.ViewModelList);
            (isViewModel ? m_S.ViewModelList : m_S.DrawList).push_back(item);
        }
    }
}

// Opaque: the static world first, then by program (the costliest switch), material, and front to back within a
// material (#112 — cheaper overdraw: nearer surfaces fill depth first). A user-space pointer leaves the top bit free
// for Dynamic. Transparent: QueueIndex ascending, then ViewDepth descending (farthest first), then MatKey. Equal keys
// go by the entity drawn (SortId), then by list order (one entity's items), not by the order GatherVisible handed them
// out in, which a patched candidate list changes: the order is the same every frame, patched or not.
void SceneRenderer::ViewPass::SortItems(std::vector<DrawItem>& list, bool transparent) {
    std::vector<SceneSortKey>& keys = m_S.SortKeys;
    keys.clear();
    for (std::uint32_t i = 0; i < (std::uint32_t)list.size(); ++i) {
        const DrawItem& d = list[i];
        if (transparent)
            keys.push_back({(std::uint64_t)((std::int64_t)d.QueueIndex) ^ (1ull << 63), d.MatKey, d.ViewDepth, d.SortId, i});
        else
            keys.push_back({(d.Dynamic ? 1ull << 63 : 0ull) | (std::uint64_t)(std::uintptr_t)d.Prog, d.MatKey, d.ViewDepth, d.SortId, i});
    }
    const auto less = [transparent](const SceneSortKey& a, const SceneSortKey& b) {
        if (a.A != b.A) return a.A < b.A;
        if (transparent) {
            if (a.Depth != b.Depth) return a.Depth > b.Depth;
            return a.B < b.B;
        }
        if (a.B != b.B) return a.B < b.B;
        return a.Depth < b.Depth;
    };
    std::sort(keys.begin(), keys.end(), [&less](const SceneSortKey& a, const SceneSortKey& b) {
        if (less(a, b)) return true;
        if (less(b, a)) return false;
        if (a.Id != b.Id) return a.Id < b.Id;
        return a.Index < b.Index;
    });
    m_S.SortScratch.clear();
    for (const SceneSortKey& k : keys) m_S.SortScratch.push_back(std::move(list[k.Index]));
    list.swap(m_S.SortScratch);
}

// TARTARUS_TRIS_REPORT=<frame>: the opaque pass's triangles on that frame (Time::FrameCount) of the Game view, by
// model (for budgeting). ShadowFrame prints the sun casters' on the same frame.
void SceneRenderer::ViewPass::ReportTris() const {
    static const int reportAt = Env::Int(Env::Var::TrisReport, -1);
    if (reportAt < 0 || m_Ctx.EditorView || Time::FrameCount() != (std::uint64_t)reportAt) return;
    struct Row { long long Tris = 0; int Items = 0; bool Dynamic = false; };
    std::unordered_map<std::string, Row> byModel;
    long long total = 0;
    for (const DrawItem& it : m_S.DrawList) {
        Row& r = byModel[it.Ref->Path()];
        r.Tris += it.Tris;
        ++r.Items;
        r.Dynamic |= it.Dynamic;
        total += it.Tris;
    }
    std::vector<std::pair<std::string, Row>> v(byModel.begin(), byModel.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.Tris > b.second.Tris; });
    std::printf("[TrisReport] frame %llu opaque: %zu items, %zu models, %lld tris\n", (unsigned long long)Time::FrameCount(),
                m_S.DrawList.size(), v.size(), total);
    for (size_t i = 0; i < v.size() && i < 30; ++i)
        std::printf("[TrisReport]   opaque %8lld tris  %5d items  %s%s\n", v[i].second.Tris, v[i].second.Items, v[i].first.c_str(),
                    v[i].second.Dynamic ? " (dynamic)" : "");
    std::fflush(stdout);
}

// #108 — reflection probes are chosen per object (from its bounds centre), not once per frame from the camera
// position, which gave every object in view the same two probes and made reflections pop as the camera moved. Only
// when the scene has probes at all. #163 - and Receive Shadows is per object too; both go through this per-draw hook.
void SceneRenderer::ViewPass::PerDraw(Shader& prog) {
    if (m_PerDrawKeyed) {
        if (&prog == m_LastPerDrawProg && m_PerDrawKey == m_LastPerDrawKey) return;
        m_LastPerDrawProg = &prog;
        m_LastPerDrawKey = m_PerDrawKey;
    } else {
        m_LastPerDrawProg = nullptr;
    }
    const DrawItem& it = *m_ProbeItem;
    prog.SetInt("uInstanced", m_InstanceBase >= 0 ? m_InstanceMode : 0);
    if (m_InstanceBase >= 0) prog.SetInt("uInstanceBase", m_InstanceBase);
    if (m_AnyProbes) m_In.probeArray->Bind(prog, it.Centre);
    if (m_In.giVolumes) GIVolumes::SetMask(prog, it.GIMask);
    prog.SetInt("uNoReceiveShadows", it.ReceiveShadows ? 0 : 1);
    prog.SetInt("uObjectLayerBit", it.LayerBit); // #203
    prog.SetInt("uDecalStatic", it.StaticReceiver ? 1 : 0);
    prog.SetFloat("uNearHide", it.NearHide);
    prog.SetFloat("uLodFade", it.LodFade);
    prog.SetFloat("uNearHideWidth", it.NearHideWidth);
    prog.SetVec3("uNearHideRight", m_NearHideRight);
    static const char* const kHideBone[8] = {"uHideBones[0]", "uHideBones[1]", "uHideBones[2]", "uHideBones[3]",
                                             "uHideBones[4]", "uHideBones[5]", "uHideBones[6]", "uHideBones[7]"};
    int hideCount = 0;
    if (it.HideBones)
        for (; hideCount < 8 && it.HideBones[hideCount] >= 0; ++hideCount) prog.SetInt(kHideBone[hideCount], it.HideBones[hideCount]);
    prog.SetInt("uHideBoneCount", hideCount);
    prog.SetInt("uBoneMaskMode", it.BoneMaskMode);
    if (it.BoneMaskMode) {
        int mask[16];
        for (int w = 0; w < 16; ++w) mask[w] = (int)it.BoneMask[w]; // the bits, as GLSL ints
        prog.SetIntArray("uBoneMask[0]", 16, mask); // one upload, not 16
    }
    prog.SetInt("uSSAOEnabled", m_ActiveFs->ssaoOn && !it.NoSsao ? 1 : 0);
    SkinHideBuffer::Bind(it.HideVerts);
    prog.SetInt("uHideVerts", it.HideVerts ? 1 : 0);
    SkinHideBuffer::Bind(it.CollarVerts, SkinHideBuffer::kCollarBinding);
    prog.SetInt("uCollarVerts", it.CollarVerts ? 1 : 0);
    prog.SetFloat("uLayerPull", it.LayerPull);
    int bloodFirst = 0, bloodCount = 0; // blood on this mesh
    BloodRenderer::Get().SplatRange(it.Entity, bloodFirst, bloodCount);
    prog.SetInt("uBloodSplatCount", bloodCount);
    if (bloodCount > 0) {
        prog.SetInt("uBloodSplatFirst", bloodFirst);
        glm::vec4 fresh, dried; // the body's own palette (a creature's ichor), a person's by default
        BloodRenderer::Get().SplatLook(it.Entity, fresh, dried);
        prog.SetVec4("uBloodFresh", fresh);
        prog.SetVec4("uBloodDried", dried);
    }
}

// Depth pre-pass (Project Settings > Quality): only what draws whole is in it: nothing skinned or moving (Dynamic),
// and none of the player body's or outfits' hidden parts, which the model program discards.
bool SceneRenderer::ViewPass::Prepassed(const DrawItem& it) const {
    return m_DepthPrepass && !(it.Dynamic || it.NearHide > 0.0f || it.LodFade != 0.0f || it.HideBones || it.HideVerts ||
                               it.CollarVerts || it.BoneMaskMode || it.LayerPull != 0.0f || it.Ref->HasBones());
}

// The depth program set up for this view's camera: the model VS's own uProj * (uView * world) (ShadowDepth.vert.glsl),
// unskinned, not layered.
void SceneRenderer::ViewPass::BeginDepthDraw(Shader& depth, const glm::mat4& proj) const {
    depth.Bind();
    depth.SetMat4("uLightViewProj", proj * m_Ctx.View);
    depth.SetInt("uSplitViewProj", 1);
    depth.SetMat4("uView", m_Ctx.View);
    depth.SetMat4("uProj", proj);
    depth.SetInt("uUseSkinning", 0);
    depth.SetInt("uCascadeLayered", 0);
}

void SceneRenderer::ViewPass::DepthPrepass() {
    const RenderFrameContext& ctx = m_Ctx;
    const SceneRenderInputs& in = m_In;
    const FrameState& fs = m_Fs;
    // The per-draw hook's per-view inputs. The view's right, flattened: clothing's Near Hide stretches along it (to
    // the shoulders), and it stays level looking down, so the chest below the eye isn't swept into it. The view
    // matrix's first row.
    m_AnyProbes = in.probeArray->Count() > 0;
    glm::vec3 right(ctx.View[0][0], 0.0f, ctx.View[2][0]);
    m_NearHideRight = glm::dot(right, right) > 1e-8f ? glm::normalize(right) : glm::vec3(1.0f, 0.0f, 0.0f);
    BloodRenderer::Get().BindSplatResources();

    // Depth pre-pass (Project Settings > Quality): the static world's depth first, colour off, so the opaque pass
    // shades each pixel once - the drawList's order is by program and material, front to back only within one, and a
    // room's floor, walls and props otherwise each ran the full lighting where the next one covered them. Both
    // programs compute an invariant gl_Position by the same expressions, so the opaque pass tests what this pass laid
    // down GL_EQUAL (it used GL_LEQUAL over a polygon offset): a cut-out leaf's clear texels then fail the depth test
    // before the model program runs - ~0.8 ms of the Forest's opaque pass.
    m_DepthPrepass = in.depthShader && ProjectSettings::Quality().DepthPrepass;
    // Octahedral impostors (ImpostorRenderer): the trees this view's ApplyLod put on theirs, in its frustum. Into the
    // pre-pass with the static world, then lit over it with the opaque pass.
    m_ImpostorsOn = !ctx.GIBake && ImpostorRenderer::Get().Gather(m_World, m_CamFrustum, ctx.ViewPos);
    if (!m_DepthPrepass) return;

    constexpr float kPrepassSlopeOffset = 0.02f;
    PROFILE_SCOPE("Scene: Depth Pre-pass");
    PROFILE_GPU_SCOPE("Depth Pre-pass");
    Shader& depth = *in.depthShader;
    BeginDepthDraw(depth, ctx.Proj);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    // No polygon offset: both programs compute an invariant gl_Position by the same expressions (ShadowDepth.vert /
    // ModelVertex.glsl), so the opaque pass tests the pre-passed draws GL_EQUAL - a cut-out leaf's clear
    // texels then fail the depth test before the model program runs, instead of shading up to its discard.
    std::vector<DepthCaster>& prepass = m_S.Prepass;
    prepass.clear();
    for (const DrawItem& it : m_S.DrawList)
        if (Prepassed(it)) prepass.push_back({it.Ref, it.Slots, it.Visible, &it.Xform});
    DrawDepthCasters(depth, prepass);
    if (m_ImpostorsOn) {
        ImpostorRenderer::Get().DrawDepth(ctx.View, ctx.Proj, ctx.ViewPos);
        m_Stats.DrawCalls += ImpostorRenderer::Get().DrawCalls();
        depth.Bind();
    }

    // SSAO from this pre-pass (RenderFrameContext::SsaoFromPrepass): copy the depth into SSAO's own target and
    // add only what the pre-pass left out, rather than SSAO drawing the whole visible scene a second time. The
    // view model and the player's own trimmed body stay out, as they did from SSAO's own pass (DrawItem::NoSsao).
    // So does a LOD level mid cross-fade (DrawItem::LodFade): this depth would hold it whole while the screen shows
    // it dithered, and the occlusion culler below tests against this copy - every tree behind a fading one went.
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
        for (const DrawItem& it : m_S.DrawList)
            if (!Prepassed(it) && !it.NoSsao && it.LodFade == 0.0f) prepass.push_back({it.Ref, it.Slots, it.Visible, &it.Xform});
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
        // The occlusion passes used texture units 0 and 1 and other programs: the model shader's uniforms are still
        // this frame's, only the bindings come back.
        m_ModelShader.Bind();
        m_R.BindFrameResources(fs);
    }
    // The grass into the view's depth after SSAO has taken its copy: the lit pass then shades each grass pixel
    // once, but SSAO and Hi-Z never see the blades - thin, swaying, half-transparent to the eye, they only left
    // dark noisy halos in the occlusion and hid nothing worth culling.
    if (m_GrassOn) {
        GrassRenderer& grass = GrassRenderer::Get();
        m_GrassCulled = grass.Cull(ctx.View, ctx.Proj, ctx.ViewPos);
        if (m_GrassCulled) {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(kPrepassSlopeOffset, 4.0f);
            grass.DrawDepth(ctx.View, ctx.Proj);
            glPolygonOffset(0.0f, 0.0f);
            glDisable(GL_POLYGON_OFFSET_FILL);
            m_GrassInPrepass = true;
            depth.Bind();
        }
    }
    depth.SetInt("uSplitViewProj", 0); // the shadow passes share this program
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GLStateCache::Invalidate();
    // GPU occlusion culling (HiZCuller.h) tests against SSAO's copy (the pre-pass plus what it left out, all of
    // which the opaque pass draws in front of the culled copies too), else the pre-pass's own depth.
    if (ctx.TxHdr && ctx.TxHdr->IsValid() && !ctx.ProbeCapture && ctx.DebugView == 0 && Perspective(ctx.Proj) &&
        ProjectSettings::Quality().GpuInstancing && HiZCuller::Enabled()) {
        if (viewDepthCopied) {
            m_HizDepth = ctx.SsaoSrc->DepthTexture();
        } else {
            ctx.TxHdr->ResolveDepthOnly();
            m_HizDepth = ctx.TxHdr->ResolvedDepthTexture();
        }
    }
    if (m_SsrWanted) {
        // Against this view's depth: SSAO's copy (everything) when it was made, else the pre-pass's own (static).
        unsigned int ssrDepth = 0;
        if (viewDepthCopied) {
            ssrDepth = ctx.SsaoSrc->DepthTexture();
        } else if (m_HizDepth) {
            ssrDepth = m_HizDepth; // resolved just above
        } else {
            ctx.TxHdr->ResolveDepthOnly();
            ssrDepth = ctx.TxHdr->ResolvedDepthTexture();
        }
        m_R.m_Ssr->Trace(ctx.TxHdr, ssrDepth, ctx.View, ctx.Proj, m_World);
    }
    m_ModelShader.Bind();
    if (m_CloudsAfterDepth) {
        // The clouds, skipping the sky the scene hides all around (a street, a room) - with SSAO off there is no
        // single-sample copy of the depth to read, and every texel is marched as before.
        in.skyAtmosphere->RenderClouds(ctx.TxHdr, ctx.View, ctx.Proj, ctx.ViewPos,
                                       viewDepthCopied ? ctx.SsaoSrc->DepthTexture() : 0u);
        // The cloud passes bound their own programs and used texture units up to 12: the model shader is current
        // again (its uniforms are still this frame's), only the bindings come back.
        m_ModelShader.Bind();
        m_R.BindFrameResources(fs);
    }
}

std::uint32_t SceneRenderer::ViewPass::ProbeKey(std::uint32_t i) const {
    return i < m_S.ProbeKeys.size() ? m_S.ProbeKeys[i] : 0u;
}

// GPU instancing: copies of a static model that draw exactly alike. Blood splats, and anything skinned, moving or
// partly hidden, draw one by one; every program its meshes draw through must read the per-instance transforms.
bool SceneRenderer::ViewPass::Batchable(const DrawItem& it) const {
    if (!m_Instancing || it.Dynamic || it.Ref->HasBones() || it.Visible || it.NearHide > 0.0f || it.LodFade != 0.0f ||
        it.HideBones || it.HideVerts || it.CollarVerts || it.BoneMaskMode || it.LayerPull != 0.0f)
        return false;
    int bloodFirst = 0, bloodCount = 0;
    BloodRenderer::Get().SplatRange(it.Entity, bloodFirst, bloodCount);
    if (bloodCount > 0) return false;
    std::uint32_t key = 0;
    for (int i = 0; i < it.Ref->MeshCount(); ++i) {
        const MaterialAsset* ma = i < (int)it.Slots->size() ? (*it.Slots)[i].get() : nullptr;
        if (!ProgramFor(ma, key)->Instancing()) return false;
    }
    return true;
}

void SceneRenderer::ViewPass::BuildBatches() {
    const RenderFrameContext& ctx = m_Ctx;
    const FrameState& fs = m_Fs;
    const std::vector<DrawItem>& drawList = m_S.DrawList;
    InstanceGroups& groups = m_S.OpaqueGroups;
    m_PassAlphaBlend = 0;
    m_ModelShader.SetInt("uAlphaBlend", 0); // explicit: ensure opaque pass outputs alpha=1
    // GPU instancing (Project Settings > Quality): copies of a static model that draw exactly alike - the same
    // materials and programs, sidedness, shadow receiving, lighting layer and SSAO - go out as one instanced draw at the
    // first copy's place in the sorted list. With reflection probes, only copies that pick the same probes with the same
    // blend (ReflectionProbeArray::SelectionKey) - every city scene has probes, and the whole pass used to fall back to
    // one draw per copy for them. The groups keep the list's order: the static world (all the batches) first, then
    // the moving things.
    m_Instancing = ProjectSettings::Quality().GpuInstancing;
    m_S.ProbeKeys.assign(m_AnyProbes && m_Instancing ? drawList.size() : 0, 0u);
    for (std::size_t i = 0; i < m_S.ProbeKeys.size(); ++i) m_S.ProbeKeys[i] = m_In.probeArray->SelectionKey(drawList[i].Centre);
    {
        PROFILE_SCOPE("Scene: Group Instances");
        GroupInstances(
            drawList.size(),
            [&](std::uint32_t i) -> std::uint64_t {
                const DrawItem& it = drawList[i];
                if (!Batchable(it)) return kNoBatch;
                std::uint64_t seed = (std::uint64_t)(std::uintptr_t)it.Ref->ImportId() * 0x9E3779B97F4A7C15ull;
                seed ^= (std::uint64_t)it.Pass | ((std::uint64_t)it.DoubleSided << 4) | ((std::uint64_t)it.ReceiveShadows << 5) |
                        ((std::uint64_t)it.NoSsao << 6) | ((std::uint64_t)(std::uint32_t)it.LayerBit << 8) |
                        ((std::uint64_t)ProbeKey(i) << 32) ^ ((std::uint64_t)it.GIMask * 0xC2B2AE3D27D4EB4Full);
                return HashSlots(*it.Slots, seed);
            },
            [&](std::uint32_t a, std::uint32_t b) {
                const DrawItem& x = drawList[a];
                const DrawItem& y = drawList[b];
                return x.Ref->ImportId() == y.Ref->ImportId() && x.Pass == y.Pass && x.DoubleSided == y.DoubleSided &&
                       x.ReceiveShadows == y.ReceiveShadows && x.NoSsao == y.NoSsao && x.LayerBit == y.LayerBit &&
                       ProbeKey(a) == ProbeKey(b) && x.GIMask == y.GIMask && SameSlots(*x.Slots, *y.Slots);
            },
            groups);
    }
    PROFILE_SCOPE("Scene: Build Batches");
    m_S.GroupBase.assign(groups.Groups.size(), -1);
    // GPU-driven draws (InstanceBatch.h): every batchable group of an arena model - a single copy too - goes into the
    // instance buffer, and each of its opaque sub-meshes becomes one indirect command in the batch of sub-meshes that
    // bind alike: the same program, material, render state and per-object state (probes, shadows, layer, SSAO). A
    // batch draws where its first group stood in the sorted list, so the static world still goes out first.
    const bool multiDraw = m_Instancing && MultiDrawEnabled();
    m_S.GroupIndirect.assign(groups.Groups.size(), 0);
    m_S.OpaqueTags.clear();
    m_S.OpaqueItems.clear();
    m_S.OpaqueBatches.clear();
    const bool hiz = multiDraw && m_HizDepth != 0;
    std::vector<HiZCuller::Group>& hizGroups = m_S.HizGroups;
    hizGroups.clear();
    HiZCuller::Boxes().clear();
    m_HizCulled = false;

    std::vector<glm::mat4>& staging = InstanceBuffer::Staging();
    staging.clear();
    for (std::size_t g = 0; multiDraw && g < groups.Groups.size(); ++g) {
        const auto& grp = groups.Groups[g];
        const std::uint32_t leadIndex = groups.Order[grp.First];
        const DrawItem& lead = drawList[leadIndex];
        if (lead.Pass == Model::MeshPass::Transparent || !Batchable(lead) || !lead.Ref->InArena()) continue;
        m_S.GroupIndirect[g] = 1;
        const std::uint32_t base = (std::uint32_t)(staging.size() / 2);
        m_S.GroupBase[g] = (int)base;
        for (std::uint32_t k = 0; k < grp.Count; ++k) {
            const DrawItem& member = drawList[groups.Order[grp.First + k]];
            staging.push_back(member.Xform);
            staging.push_back(glm::mat4(member.Normal));
        }
        if (hiz) {
            // Foliage sways past its bounds: the box grows by the most any of its materials can move a vertex.
            float sway = 0.0f;
            for (int m = 0; m < lead.Ref->MeshCount(); ++m) {
                const MaterialAsset* ma = m < (int)lead.Slots->size() ? (*lead.Slots)[m].get() : nullptr;
                const Material& mat = ma ? ma->Mat : lead.Ref->MeshMaterial(m);
                if (mat.Foliage) sway = std::max(sway, HiZCuller::FoliageSway(std::abs(mat.WindStrength), Material::Wind().Strength));
            }
            for (std::uint32_t k = 0; k < grp.Count; ++k) {
                const DrawItem& member = drawList[groups.Order[grp.First + k]];
                HiZCuller::AddBox(member.WorldMin, member.WorldMax, member.Bounded, sway);
            }
            hizGroups.push_back({base, grp.Count, 0, 0});
        }
        // Everything the per-draw hook and the material bind read, but the transforms.
        const std::uint64_t objectKey = (std::uint64_t)lead.ReceiveShadows | ((std::uint64_t)lead.StaticReceiver << 1) |
                                        ((std::uint64_t)lead.NoSsao << 2) | ((std::uint64_t)lead.DoubleSided << 3) |
                                        ((std::uint64_t)Prepassed(lead) << 4) | ((std::uint64_t)(std::uint32_t)lead.LayerBit << 8) |
                                        ((std::uint64_t)ProbeKey(leadIndex) << 40);
        for (int m = 0; m < lead.Ref->MeshCount(); ++m) {
            const MaterialAsset* ma = m < (int)lead.Slots->size() ? (*lead.Slots)[m].get() : nullptr;
            if (ma && ma->RenderQueue == MaterialAsset::Queue::Transparent) continue;
            const MeshArena::Range& r = lead.Ref->Mesh(m).ArenaRange();
            if (!r.IndexCount) continue;
            std::uint32_t variant = 0;
            const Shader* prog = ProgramFor(ma, variant);
            const Material& mat = ma ? ma->Mat : lead.Ref->MeshMaterial(m);
            std::uint64_t key = objectKey;
            const auto mix = [&key](std::uint64_t v) { key ^= v + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2); };
            mix((std::uint64_t)(std::uintptr_t)prog);
            const std::uint64_t state = lead.Ref->MeshStateKey(m, *lead.Slots, lead.DoubleSided);
            const bool alphaTest = mat.AlphaClip || (ma && ma->RenderQueue == MaterialAsset::Queue::AlphaTest);
            const std::uint64_t shaderId = ma ? (std::uint64_t)(std::uintptr_t)ma->Shader.get() : 1ull;
            std::uint64_t matKey = mat.HashWithExtraProps();
            matKey ^= shaderId * 0x9E3779B97F4A7C15ull;
            matKey ^= alphaTest ? 0x5bd1e995ull : 0ull;
            mix(mat.HashWithExtraProps());
            mix(shaderId);
            mix(state);
            mix(r.Page);
            mix(alphaTest ? 1ull : 0ull);
            m_S.OpaqueTags.push_back({(std::uint32_t)g, (std::uint16_t)m, Prepassed(lead), prog, state, matKey, objectKey});
            m_S.OpaqueItems.push_back({key, (std::uint32_t)m_S.OpaqueTags.size() - 1,
                                       {r.IndexCount, grp.Count, r.FirstIndex, (std::int32_t)r.BaseVertex, base}});
        }
    }
    if (!m_S.OpaqueItems.empty()) BuildIndirectBatches(m_S.OpaqueItems, m_S.OpaqueCommands, m_S.OpaqueBatches);
    for (std::size_t g = 0; g < groups.Groups.size(); ++g) {
        const auto& grp = groups.Groups[g];
        if (m_S.GroupIndirect[g] || grp.Count < 2) continue;
        m_S.GroupBase[g] = (int)(staging.size() / 2);
        for (std::uint32_t k = 0; k < grp.Count; ++k) {
            const DrawItem& member = drawList[groups.Order[grp.First + k]];
            staging.push_back(member.Xform);
            staging.push_back(glm::mat4(member.Normal));
        }
    }
    InstanceBuffer::Upload();
    if (!m_S.OpaqueItems.empty()) IndirectBuffer::Upload(m_S.OpaqueCommands);
    if (hiz && !m_S.OpaqueItems.empty() && !hizGroups.empty()) {
        PROFILE_GPU_SCOPE("Occlusion Cull");
        const std::vector<IndirectCommand>& commands = m_S.OpaqueCommands;
        // Each group's commands (a command's baseInstance is its group's first slot), listed group by group.
        const std::uint32_t mdiSlots = hizGroups.back().Base + hizGroups.back().Count;
        m_S.HizSlotGroup.assign(mdiSlots, ~0u);
        for (std::uint32_t i = 0; i < (std::uint32_t)hizGroups.size(); ++i) m_S.HizSlotGroup[hizGroups[i].Base] = i;
        for (const IndirectCommand& c : commands) ++hizGroups[m_S.HizSlotGroup[c.BaseInstance]].CmdCount;
        std::uint32_t at = 0;
        for (HiZCuller::Group& g : hizGroups) {
            g.CmdFirst = at;
            at += g.CmdCount;
            g.CmdCount = 0;
        }
        m_S.HizCmdList.resize(at);
        for (std::uint32_t i = 0; i < (std::uint32_t)commands.size(); ++i) {
            HiZCuller::Group& g = hizGroups[m_S.HizSlotGroup[commands[i].BaseInstance]];
            m_S.HizCmdList[g.CmdFirst + g.CmdCount++] = i;
        }
        m_HizCulled = HiZCuller::Cull(m_HizDepth, ctx.TxHdr->Width(), ctx.TxHdr->Height(),
                                      glm::vec4(fs.vp[0], fs.vp[1], fs.vp[2], fs.vp[3]), ctx.Proj * ctx.View, ctx.ViewPos,
                                      hizGroups, m_S.HizCmdList, IndirectBuffer::Handle(), mdiSlots);
        m_ModelShader.Bind();
    }
}

void SceneRenderer::ViewPass::CountDraw(const DrawItem& it) {
    m_Stats.DrawCalls += it.Meshes;
    m_Stats.Triangles += it.Tris;
    m_Stats.Vertices += it.Verts;
}

void SceneRenderer::ViewPass::DrawOpaque() {
    const RenderFrameContext& ctx = m_Ctx;
    const std::vector<DrawItem>& drawList = m_S.DrawList;
    const InstanceGroups& groups = m_S.OpaqueGroups;
    const std::vector<SceneOpaqueTag>& tags = m_S.OpaqueTags;
    const std::vector<IndirectBatch>& batches = m_S.OpaqueBatches;
    // TAA: the opaque pass (ModelFragment.glsl) is what writes motion vectors - location 1 joins the draw
    // buffers for it alone, and leaves again before fur, the sky and the transparents.
    m_VelocityOut = ctx.TxHdr && ctx.TxHdr->VelocityEnabled();
    if (m_VelocityOut) ctx.TxHdr->SetVelocityWrites(true);
    {
    PROFILE_SCOPE("Scene: Opaque");
    PROFILE_GPU_SCOPE("Opaque");
    // What the pre-pass laid down draws without writing depth again: the model program can discard (alpha clip, hidden
    // parts), and a program that discards while writing depth loses early depth testing - every hidden layer of a
    // street or a forest was shaded in full. With writes off the test runs first, and the pre-pass depth (the same
    // clip) already holds the answer.
    GLboolean depthWrites = GL_TRUE;
    Model::DrawPassState passState; // read once for the batches below (not per double-sided batch)
    const auto floatBits = [](float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; };
    const auto drawBatch = [&](const IndirectBatch& b) {
        const SceneOpaqueTag& t = tags[b.Tag];
        const DrawItem& lead = drawList[groups.Order[groups.Groups[t.Group].First]];
        const GLboolean leadWrites = t.Prepassed ? GL_FALSE : GL_TRUE;
        if (leadWrites != depthWrites) {
            glDepthMask(depthWrites = leadWrites);
            glDepthFunc(depthWrites ? GL_LEQUAL : GL_EQUAL);
            passState.DepthMask = depthWrites == GL_TRUE; // what the batches' render-state scopes restore to
            passState.DepthFunc = depthWrites ? GL_LEQUAL : GL_EQUAL;
        }
        m_ProbeItem = &lead;
        m_InstanceBase = 0; // uInstanced on; each command's baseInstance is its group's
        m_InstanceMode = m_HizCulled ? 2 : 1;
        // Everything the per-draw hook reads for a batch's lead (batchable leads have no hiding, fade, pull or blood).
        m_PerDrawKey = t.Object ^ ((std::uint64_t)m_InstanceMode << 32) ^ ((std::uint64_t)floatBits(lead.NearHideWidth) << 13) ^
                       ((std::uint64_t)lead.GIMask * 0x9E3779B97F4A7C15ull);
        m_PerDrawKeyed = true;
        if (lead.Ref->DrawMeshIndirect(m_ModelShader, t.Mesh, *lead.Slots, m_Select, m_PerDraw, lead.DoubleSided,
                                       IndirectBuffer::Offset(b.First), (int)b.Count, &passState))
            ++m_Stats.DrawCalls;
        m_PerDrawKeyed = false;
    };
    // The pre-passed batches test GL_EQUAL against the pre-pass depth with writes off, so their order doesn't change
    // the picture: they draw together, grouped by program, render state and material (the material bind and the
    // per-draw hook then skip their uploads between batches that bind alike). The rest keep the list's order.
    std::vector<std::uint32_t>& prepassedBatches = m_S.PrepassedBatches;
    prepassedBatches.clear();
    for (std::uint32_t i = 0; i < (std::uint32_t)batches.size(); ++i)
        if (tags[batches[i].Tag].Prepassed) prepassedBatches.push_back(i);
    std::sort(prepassedBatches.begin(), prepassedBatches.end(), [&](std::uint32_t a, std::uint32_t b) {
        const SceneOpaqueTag& x = tags[batches[a].Tag];
        const SceneOpaqueTag& y = tags[batches[b].Tag];
        if (x.Prog != y.Prog) return x.Prog < y.Prog;
        if (x.State != y.State) return x.State < y.State;
        if (x.Mat != y.Mat) return x.Mat < y.Mat;
        if (x.Object != y.Object) return x.Object < y.Object;
        return a < b;
    });
    const auto countGroup = [&](const InstanceGroups::Group& grp) {
        for (std::uint32_t k = 0; k < grp.Count; ++k) {
            const DrawItem& member = drawList[groups.Order[grp.First + k]];
            m_Stats.Triangles += member.Tris;
            m_Stats.Vertices += member.Verts;
        }
    };
    bool prepassedDrawn = false;
    std::uint32_t nextBatch = 0; // the other batches draw in order, each at the first group it holds
    for (std::size_t g = 0; g < groups.Groups.size(); ++g) {
        const auto& grp = groups.Groups[g];
        const DrawItem& it = drawList[groups.Order[grp.First]];
        if (m_S.GroupIndirect[g]) {
            if (!prepassedDrawn) {
                for (std::uint32_t i : prepassedBatches) drawBatch(batches[i]);
                prepassedDrawn = true;
            }
            for (; nextBatch < batches.size() && tags[batches[nextBatch].Tag].Group == g; ++nextBatch)
                if (!tags[batches[nextBatch].Tag].Prepassed) drawBatch(batches[nextBatch]);
            countGroup(grp);
            continue;
        }
        const GLboolean wantWrites = Prepassed(it) ? GL_FALSE : GL_TRUE;
        if (wantWrites != depthWrites) {
            glDepthMask(depthWrites = wantWrites);
            glDepthFunc(depthWrites ? GL_LEQUAL : GL_EQUAL);
        }
        m_ProbeItem = &it;
        m_InstanceBase = m_S.GroupBase[g];
        m_InstanceMode = 1;
        it.Ref->DrawSelected(m_ModelShader, it.Xform, *it.Slots, m_Select, 1.0f, m_PerDraw, it.Pass, it.DoubleSided, it.Visible,
                             m_InstanceBase >= 0 ? (int)grp.Count : 1);
        m_Stats.DrawCalls += it.Meshes;
        countGroup(grp);
    }
    if (depthWrites != GL_TRUE) {
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LEQUAL);
    }
    if (m_ImpostorsOn) {
        const bool viewDepth = ctx.TxHdr && ctx.TxHdr->IsValid();
        ImpostorRenderer::Get().DrawLit(ctx.View, ctx.Proj, ctx.ViewPos, m_DepthPrepass,
                                        viewDepth ? ctx.TxHdr->MultisampleDepthTexture() : 0u,
                                        viewDepth ? ctx.TxHdr->Samples() : 0, [&](Shader& p) { m_R.ApplyFrameState(p, m_Fs); });
        m_Stats.DrawCalls += ImpostorRenderer::Get().DrawCalls();
        m_ModelShader.Bind();
    }
    }
    m_InstanceBase = -1;
    // Grass, lit (GrassRenderer): with the opaque pass, so it writes TAA motion for its sway.
    if (m_GrassOn) {
        GrassRenderer& grass = GrassRenderer::Get();
        if (!m_GrassCulled) m_GrassCulled = grass.Cull(ctx.View, ctx.Proj, ctx.ViewPos);
        if (m_GrassCulled) {
            grass.DrawLit(ctx.View, ctx.Proj, !m_GrassInPrepass, [&](Shader& p) { m_R.ApplyFrameState(p, m_Fs); });
            m_ModelShader.Bind();
        }
    }
    if (m_VelocityOut) ctx.TxHdr->SetVelocityWrites(false);
    // Nothing after this pass draws instanced: leave every program it used reading uModel again (the previews share
    // the material shaders' programs).
    for (Shader* p : m_S.AppliedPrograms) {
        p->Bind();
        p->SetInt("uInstanced", 0);
    }
    m_ModelShader.Bind();
}

void SceneRenderer::ViewPass::DrawFurAndBlood() {
    const RenderFrameContext& ctx = m_Ctx;
    // Shell fur (FurRenderer) over the opaque surfaces just drawn - the visible set only, so nothing off-screen pays
    // for it. Depth-writing and alpha-tested, so it needs no order of its own.
    if (!FurRenderer::Disabled()) {
        static std::vector<FurRenderer::Item> furItems;
        furItems.clear();
        for (const DrawItem& it : m_S.DrawList) {
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
            m_Stats.DrawCalls += FurRenderer::Get().Draw(furItems, ctx.View, ctx.Proj, ctx.ViewPos, (float)m_Fs.vp[3],
                                                         ctx.TxHdr && ctx.TxHdr->Samples() > 1,
                                                         [&](Shader& p) { m_R.ApplyFrameState(p, m_Fs); }, furTris);
            m_Stats.Triangles += (int)furTris;
            m_ModelShader.Bind();
        }
    }

    // Volumetric blood in the air: opaque fluid, so before the sky and with the
    // opaque geometry it lands on already in depth.
    m_Stats.DrawCalls += BloodRenderer::Get().DrawSprays(ctx.View, ctx.Proj,
                                                         [&](Shader& p) { m_R.ApplyFrameState(p, m_Fs); });
    m_ModelShader.Bind();
}

void SceneRenderer::ViewPass::DrawSky() {
    const RenderFrameContext& ctx = m_Ctx;
    // PR13: sky - the physical sky, HDRI cubemap or procedural gradient - at the far plane under a
    // GL_LEQUAL test, so the opaque pass has already rejected every covered pixel. Before
    // the transparent pass (it blends over the sky, and transmission captures it).
    if (ctx.GIBake == 1) {
        // A circuit's GI bake: the sky is black (the Outside layer carries it).
    } else if (m_In.skyAtmosphere) {
        m_In.skyAtmosphere->DrawSky(ctx.TxHdr);
    } else if (m_World.Render.SkySourceMode == World::SkySource::Hdri && m_In.hdriCube) {
        const float rotRad = glm::radians(m_World.Render.SkyRotationDegrees);
        m_In.sky->DrawHdri(m_In.hdriCube->Texture(), rotRad, ctx.View, ctx.Proj);
    } else {
        m_In.sky->Draw(ctx.View, ctx.Proj, m_World.Render.SkyHorizonColor, m_World.Render.SkyZenithColor);
    }
    // The volumetric fog over the sky's pixels (every surface applied it in its own shading).
    if (m_Fs.volOn) {
        m_R.m_Volumetrics->DrawSky(ctx.TxHdr, m_Fs.vp);
        GLStateCache::Invalidate();
        m_ModelShader.Bind();
    }
}

void SceneRenderer::ViewPass::SetAlphaBlend(int on) {
    for (Shader* p : m_S.AppliedPrograms) { p->Bind(); p->SetInt("uAlphaBlend", on); }
}

// PR12: resolve MSAA opaque color and copy into mipped texture for refraction. Bind the capture on unit 14 for every
// program that may run the _TRANSMISSION branch. #112 — only when a visible transparent material uses transmission;
// this resolve + mip copy used to run for any transparent object at all, per viewport per frame.
void SceneRenderer::ViewPass::BindTransmission(const FrameState& fs) {
    const RenderFrameContext& ctx = m_Ctx;
    if (!m_AnyTransmission || !ctx.TxHdr || !ctx.TxCapture) return;
    ctx.TxHdr->ResolveTo();
    ctx.TxCapture->CopyFrom(ctx.TxHdr->ResolvedColorTexture(), fs.vp[2], fs.vp[3]);
    ctx.TxHdr->BindForRender(); // rebind MSAA FBO for the transparent draw pass
    glActiveTexture(GL_TEXTURE0 + 14);
    glBindTexture(GL_TEXTURE_2D, ctx.TxCapture->Texture());
    glActiveTexture(GL_TEXTURE0); // the contract: unit 0 active on return (RenderFrameContext.h)
    for (Shader* p : m_S.AppliedPrograms) {
        p->Bind();
        p->SetInt("uOpaqueColor", 14);
        p->SetVec2("uScreenSize", glm::vec2((float)fs.vp[2], (float)fs.vp[3]));
    }
}

// Blended, no depth write, back to front: the world's transparent pass, and the view model's after its opaque arms.
void SceneRenderer::ViewPass::DrawTransparentList(std::vector<DrawItem>& list) {
    m_PassAlphaBlend = 1;
    // Every program already brought up flips to blended output; new programs picked up below get m_PassAlphaBlend via
    // the selector.
    SetAlphaBlend(1);
    BindTransmission(*m_ActiveFs);
    SortItems(list, true);

    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glBlendFuncSeparate(GL_SRC_ALPHA,  GL_ONE_MINUS_SRC_ALPHA,
                        0x0001/*GL_ONE*/, GL_ONE_MINUS_SRC_ALPHA);
    for (const DrawItem& it : list) {
        m_ProbeItem = &it;
        // Opacity comes from each transparent submesh's own slot (#112).
        it.Ref->DrawSelected(m_ModelShader, it.Xform, *it.Slots, m_Select, 1.0f, m_PerDraw, it.Pass, it.DoubleSided, it.Visible);
        CountDraw(it);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    SetAlphaBlend(0); // restore for next frame's opaque pass
}

void SceneRenderer::ViewPass::DrawEffects() {
    const RenderFrameContext& ctx = m_Ctx;
    // #177 - particles last: depth-tested against everything above, blended, no depth write.
    // The player's own muzzle flame waits for the view-model sub-pass.
    if (!ctx.ProbeCapture) { // a probe's room capture keeps only the room
        if (SceneParticles()->Draw(m_World, ctx, m_ViewModelPass) > 0) ++m_Stats.DrawCalls;
        // The Knife flipbook particles: blood mist, impact dust and debris, muzzle smoke.
        m_Stats.DrawCalls += FxSpriteRenderer::Get().Draw(ctx.View, ctx.Proj, m_Fs.vp, ctx.TxHdr, false,
                                                          [&](Shader& p) { m_R.ApplyFrameState(p, m_Fs); });
        // Labels in the world (World Text): unlit, depth-tested against everything above.
        m_Stats.DrawCalls += WorldTextRenderer::Get().Draw(m_World, ctx.View, ctx.Proj, ctx.ViewPos, ctx.EditorView);
    }
    if (ctx.WorldOverlay) {
        ctx.WorldOverlay();
        GLStateCache::Invalidate();
    }
}

// --- View-model sub-pass: tagged geometry, its own projection, after a depth clear --------
// Everything else has been written; only the tagged arms/weapon remain, and they now draw
// on top of all of it rather than fighting it for depth.
void SceneRenderer::ViewPass::DrawViewModel() {
    std::vector<DrawItem>& opaque = m_S.ViewModelList;
    std::vector<DrawItem>& transparent = m_S.ViewModelTransparentList;
    if (!m_ViewModelPass || (opaque.empty() && transparent.empty())) return;
    const RenderFrameContext& ctx = m_Ctx;
    const SceneRenderInputs& in = m_In;
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
    // projection - and so the depth that depth of field reads - unchanged.
    glEnable(0x864F /*GL_DEPTH_CLAMP, core since 3.2; not in this loader*/);

    // Re-fit the froxel lists to this projection. They were culled for the world's FOV, so
    // reading them as-is would light the arms from slices built for a different one. Safe
    // here: the world's draws have already consumed this frame's lists.
    RenderFrameContext vmCtx = ctx;
    vmCtx.Proj = m_VmProj;
    vmCtx.TaaVelocity = false; // unjittered, and it moves with the eye: zero motion (written below)
    FrameState vmFs = m_R.GatherFrameState(m_World, vmCtx, in);
    vmFs.ssrTex = 0; // traced for the world's projection, not the arms'
    // The SSAO map was built from the world's depth pre-pass, which skips the view model, so
    // sampling it here stamps whatever stands behind the gun (a character, a wall) onto it as
    // occlusion - the weapon read as see-through. The view model gets no screen-space AO.
    vmFs.ssaoOn = false;
    // The world view's decals, re-binned into these froxels too (their bounds are still uploaded) - when
    // any could land on the view model: one that takes dynamic receivers within the arms' reach of the eye,
    // and a view-model material that receives decals. Otherwise the second decal cull is skipped (its lists
    // would come out empty for every view-model pixel anyway).
    bool vmDecals = vmFs.clusterOn && m_Fs.decalCount > 0 && in.decals &&
                    in.decals->NearestDynamicReceiverDecal() < kViewModelDecalReach;
    if (vmDecals) {
        bool receives = false;
        for (const DrawItem& it : opaque) {
            if (!it.Slots || it.Slots->empty()) { receives = true; break; } // the model's own materials
            for (const auto& m : *it.Slots)
                if (!m || m->Mat.ReceiveDecals) { receives = true; break; }
            if (receives) break;
        }
        vmDecals = receives;
    }
    vmFs.decalCount = vmDecals ? m_Fs.decalCount : 0;
    if (vmFs.clusterOn) {
        PROFILE_GPU_SCOPE("View Model Cluster Cull");
        if (vmFs.decalCount > 0) in.decals->BindForCull();
        in.lightBuffer->Bind(0); // this frame's lights, as for the main view's cull
        in.clusterGrid->Cull(*in.clusterBuildShader, *in.clusterCullShader, vmCtx.View, vmCtx.Proj,
                             vmFs.clusterNearZ, vmFs.clusterFarZ, vmFs.vp[2], vmFs.vp[3], in.decalCullShader,
                             vmFs.decalCount);
    }

    // Everything below runs against the view model's frame state — uProj above all, which is
    // the entire point of the pass.
    m_ActiveFs = &vmFs;
    m_S.AppliedPrograms.clear();
    m_S.AppliedPrograms.push_back(&m_ModelShader);
    m_PassAlphaBlend = 0;
    m_R.ApplyFrameState(m_ModelShader, vmFs);
    m_ModelShader.SetInt("uAlphaBlend", 0);

    SortItems(opaque, false);
    // The arms and gun get a depth pre-pass of their own, for the world pass's reason: the model program can
    // discard, so drawn with depth writes on they lost early depth testing and every hidden layer of the gun (its
    // inner parts, the hands behind it) was shaded in full - about 1 ms of a frame for two models. Skinned pieces
    // can go in here, unlike the world's: both programs read this frame's pre-skinned vertices (Model::PreSkin).
    // Left out, as there: what the model program hides (near hide, hidden bones/vertices) and two-sided draws.
    const auto vmPrepassed = [&](const DrawItem& it) {
        return m_DepthPrepass && !(it.NearHide > 0.0f || it.HideBones || it.HideVerts || it.CollarVerts ||
                                   it.BoneMaskMode || it.LayerPull != 0.0f || it.DoubleSided);
    };
    if (m_DepthPrepass) {
        PROFILE_GPU_SCOPE("View Model Depth Pre-pass");
        std::vector<DepthCaster>& vmPrepass = m_S.ViewModelPrepass;
        vmPrepass.clear();
        for (const DrawItem& it : opaque)
            if (vmPrepassed(it)) vmPrepass.push_back({it.Ref, it.Slots, it.Visible, &it.Xform});
        if (!vmPrepass.empty()) {
            Shader& depth = *in.depthShader;
            BeginDepthDraw(depth, m_VmProj);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 4.0f);
            DrawDepthCasters(depth, vmPrepass);
            glPolygonOffset(0.0f, 0.0f);
            glDisable(GL_POLYGON_OFFSET_FILL);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            depth.SetInt("uSplitViewProj", 0);
            GLStateCache::Invalidate();
            m_ModelShader.Bind();
        }
    }
    PROFILE_GPU_SCOPE("View Model Opaque");
    if (m_VelocityOut) ctx.TxHdr->SetVelocityWrites(true); // TAA: the arms and gun write zero motion
    for (const DrawItem& it : opaque) {
        m_ProbeItem = &it;
        const bool prepassedVm = vmPrepassed(it);
        if (prepassedVm) glDepthMask(GL_FALSE);
        it.Ref->DrawSelected(m_ModelShader, it.Xform, *it.Slots, m_Select, 1.0f, m_PerDraw, it.Pass, it.DoubleSided, it.Visible);
        if (prepassedVm) glDepthMask(GL_TRUE);
        CountDraw(it);
    }
    if (m_VelocityOut) ctx.TxHdr->SetVelocityWrites(false);

    // Transparent view-model slots (a lens, glass) follow the opaque arms rather than
    // preceding them as they would in the world pass — here the opaque geometry of the same
    // object is already down, so they composite over it instead of under it.
    if (!transparent.empty()) DrawTransparentList(transparent);

    // The player's own muzzle flame, at the gun's projection, behind or in front of the arms and gun
    // as it really is.
    if (SceneParticles()->Draw(m_World, vmCtx, true, true) > 0) ++m_Stats.DrawCalls;
    m_Stats.DrawCalls += FxSpriteRenderer::Get().Draw(vmCtx.View, vmCtx.Proj, vmFs.vp, nullptr, true,
                                                      [&](Shader& p) { m_R.ApplyFrameState(p, vmFs); });

    glDisable(0x864F /*GL_DEPTH_CLAMP*/);
    m_ActiveFs = &m_Fs; // nothing below reads it; leave the selector on the caller's state
}
