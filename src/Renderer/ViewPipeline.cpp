#include "ViewPipeline.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Bloom.h"
#include "BodyDebugDraw.h"
#include "Camera.h"
#include "ColliderGizmo.h"
#include "Components.h"
#include "EditorSettings.h"
#include "EditorTheme.h" // grid axis colours (GridColors)
#include "FKeyRow.h"
#include "Framebuffer.h"
#include "Frustum.h"
#include "GLStateCache.h"
#include "gl.h"
#include "Grid.h"
#include "HdrTarget.h"
#include "Model.h"
#include "OpaqueColorCopy.h"
#include "PostProcessVolume.h"
#include "Profiler.h"
#include "ProjectSettings.h"
#include "RenderFrameContext.h"
#include "SelectionOutline.h"
#include "ShadowFrame.h"
#include "Shader.h"
#include "Ssao.h"
#include "TemporalAA.h"
#include "TintOverlayRenderer.h"
#include "Upscaler.h"
#include "Window.h"
#include "World.h"

namespace {
// #162 - the scene's post settings for the final tonemap pass.
PostSettings MakePostSettings(const World& world, unsigned int bloomTex, float bloomIntensity,
                              float dt, int exposureSlot, const glm::vec3& cameraPos) {
    PostSettings p;
    p.ExposureEV = world.ExposureEV;
    p.Operator = world.TonemapOperator;
    p.DeltaTime = dt;
    p.ExposureSlot = exposureSlot;
    if (FKeyRow::PostFxOff()) return p; // F8: exposure and the tonemap only - no bloom, grade, lens or auto exposure
    p.BloomTexture = bloomTex;
    p.BloomIntensity = bloomIntensity;
    p.Temperature = world.GradeTemperature;
    p.Tint = world.GradeTint;
    p.Contrast = world.GradeContrast;
    p.Saturation = world.GradeSaturation;
    for (int i = 0; i < 3; ++i) p.ColorFilter[i] = world.GradeColorFilter[i];
    p.FocusDistance = world.FocusDistance;
    p.FocusRange = world.FocusRange;
    p.MaxBlur = world.DofMaxBlur;
    p.Fxaa = world.EffectiveFxaa(); // TAA replaces it
    p.AutoExposure = world.AutoExposure;
    p.AutoExposureMinEV = world.AutoExposureMinEV;
    p.AutoExposureMaxEV = world.AutoExposureMaxEV;
    p.AutoExposureSpeedUp = world.AutoExposureSpeedUp;
    p.AutoExposureSpeedDown = world.AutoExposureSpeedDown;
    p.DeltaTime = dt;
    p.ExposureSlot = exposureSlot;
    ApplyPostProcessVolumes(world, cameraPos, p); // #162 / #203 - local and global volumes
    return p;
}
} // namespace

// #162 - depth of field reads the view's resolved depth (requested only when DOF is on, so the
// depth resolve stays off otherwise, #206) and its projection. Game views only: blurring the
// Scene view would just get in the way of editing.
PostSettings ViewPipeline::WithDepthEffects(PostSettings p, const World& world, const HdrTarget& hdr,
                                            const glm::mat4& proj) {
    if (world.DepthOfField) {
        p.DepthOfField = true;
        p.ProjA = proj[2][2];
        p.ProjB = proj[3][2];
        p.Ortho = proj[2][3] == 0.0f;
    }
    if (p.DepthOfField) p.DepthTexture = hdr.ResolvedDepthTexture();
    return p;
}

// Shared by the Scene view, the game camera and the probe captures, so they can never silently
// diverge. Editor-only visualization (selection outline/highlight, drag-preview
// ghost, grid, wireframe) is deliberately NOT part of this — the Game View should
// show exactly what Play Mode does, never editor debug shading.
// ctx.EditorView: the editor Scene viewport (not the Game view). Only that pass applies
// editor-only visibility filters — the per-layer visibility mask (#236 A1) and the
// per-entity HiddenInSceneTag (#236 B). The running game and its Game view draw everything.
void ViewPipeline::DrawScene(const ViewPipelineInputs& in, const RenderFrameContext& ctx,
                             EditorLayer::RenderStats* outStats) {
    World& world = *in.world;
    // TAA: LOD picks and the sun's cascade fit see the unjittered projection, or the cascades would
    // re-fit (and shimmer) with every sub-pixel step.
    const glm::mat4 steadyProj = ctx.TaaVelocity ? TemporalAA::JitterProjection(ctx.Proj, -ctx.TaaJitterNdc) : ctx.Proj;
    { PROFILE_SCOPE("LOD Select"); world.LodSmooth = true; world.ApplyLod(ctx.ViewPos, steadyProj); } // #163 - this view's LOD levels (sun shadows too)
    in.sceneInputs->sunShadowsReady = in.shadowFrame->RenderSun(*in.shadowIn, ctx.View, steadyProj); // #109
    in.sceneRenderer->RenderScene(world, ctx, *in.sceneInputs, outStats);
}

// The occlusion + blur from `target`'s depth, however that depth was filled.
void ViewPipeline::SsaoCompute(const ViewPipelineInputs& in, Ssao& target, const glm::mat4& proj) {
    PROFILE_GPU_SCOPE("SSAO Compute + Blur");
    target.Compute(*in.ssaoComputeShader, proj, in.world->SsaoRadius, in.world->SsaoBias);
    target.Blur(*in.ssaoBlurShader);
    GLStateCache::Invalidate();
}

// #111 — the SSAO depth pre-pass, shared by the Scene view, the docked Game view and
// maximised Play (it was pasted three times). Uses the main pass's visibility rules:
// inactive entities, and in the editor Scene view HiddenInScene + the layer mask, are
// skipped (they used to leave AO "ghosts"); transparent-queue objects write no depth
// in the main pass so they don't here either; everything outside the view is culled.
// ViewModelTag is skipped unconditionally for the same reason those did: the arms and
// weapon are a camera-attached overlay composited over the world in their own
// sub-pass, so their depth here would only darken the world pixels hugging them.
void ViewPipeline::SsaoDepthPrepass(const ViewPipelineInputs& in, Ssao& target, int w, int h, const glm::mat4& view,
                                    const glm::mat4& proj, bool editorView) {
    World& world = *in.world;
    Shader& shadowShader = *in.depthShader;
    world.ApplyLod(glm::vec3(glm::inverse(view)[3]), proj); // #163 - same levels as the draw
    glBindFramebuffer(GL_FRAMEBUFFER, target.DepthFbo());
    glViewport(0, 0, w, h);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDisable(GL_BLEND);

    shadowShader.Bind();
    shadowShader.SetMat4("uLightViewProj", proj * view);
    shadowShader.SetInt("uUseSkinning", 0);
    const Frustum frustum = Frustum::FromViewProj(proj * view);
    // What the view sees (each view's own copy of a split pose - the player's camera the first-person
    // one), opaque, in the frustum as posed like the main pass; no view-model geometry (see note above).
    VisibilityQuery seen;
    seen.OwnerView = !editorView;
    seen.EditorView = editorView;
    seen.LayerVisibleMask = in.sceneInputs->layerVisibleMask;
    seen.SkipViewModels = true;
    // The player's own body in the camera's view: its shoulders and whatever is within Near
    // Hide are discarded in the main pass, so here they would occlude from nowhere. The main
    // pass gives these pieces no SSAO to match (SceneRenderer, DrawItem::NoSsao).
    seen.SkipPlayerBody = !editorView;
    seen.SkipTransparent = true;
    seen.Cull = &frustum;
    // Same small-object cull as the main pass (SceneRenderer), so the two agree on what stands in front.
    const glm::vec3 seenEye = glm::vec3(glm::inverse(view)[3]);
    if (std::abs(proj[3][3]) < 0.5f) {
        seen.SizeCullEye = &seenEye;
        seen.SizeCullProjScale = proj[1][1];
        seen.MinScreenHeight = ProjectSettings::Quality().SmallObjectCullHeight;
    }
    GatherVisible(world, seen, m_PrepassVisible);
    m_PrepassDraws.clear();
    for (const VisibleRenderable& v : m_PrepassVisible) {
        auto& rc = *v.Renderable;
        const auto* hide = world.Registry.try_get<OutfitHideTag>(v.Entity);
        m_PrepassDraws.push_back({rc.ModelRef.get(), &rc.Materials, hide ? hide->Visible.get() : nullptr, &v.Xform});
    }
    DrawDepthCasters(shadowShader, m_PrepassDraws);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLStateCache::Invalidate();

    SsaoCompute(in, target, proj);
}

// The view's HDR target sized, cleared and bound, TAA's velocity attachment and jitter set up. Returns the projection
// the world pass draws through (jittered under TAA); picking, gizmos, the grid and outlines keep `proj`.
glm::mat4 ViewPipeline::BeginView(const ViewPipelineInputs& in, const ViewTargets& t, int w, int h,
                                  const glm::mat4& view, const glm::mat4& proj, bool& taaOn) {
    HdrTarget& hdr = *t.hdr;
    hdr.Resize(w, h, in.world->EffectiveMsaaSamples());
    // TAA: a velocity attachment and the resolved depth for the resolve; jitter goes on below.
    taaOn = in.world->TaaEnabled() && hdr.IsValid();
    hdr.SetVelocityEnabled(taaOn);
    if (taaOn) hdr.ResolvedDepthTexture(); // provisioned before this frame's ResolveTo
    else t.taa->Reset();
    const glm::mat4 drawProj = taaOn ? t.taa->BeginFrame(view, proj, w, h) : proj;
    hdr.BindForRender();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (taaOn) hdr.ClearVelocity(TemporalAA::kVelocityUnknown);
    return drawProj;
}

// PR15: SSAO depth pre-pass — runs before the world draw so the occlusion map is ready.
// Reuses the depth shader (ShadowDepth.vert.glsl) with uLightViewProj = proj * view.
// audit #358 — if any SSAO FBO came back incomplete, skip the passes outright
// (the scene just renders without ambient occlusion) instead of drawing into a
// zero/invalid framebuffer. True = SSAO takes the depth pre-pass's depth instead (RenderFrameContext::SsaoFromPrepass).
bool ViewPipeline::SsaoPrepass(const ViewPipelineInputs& in, const ViewTargets& t, bool ssaoOn, int w, int h,
                               const glm::mat4& view, const glm::mat4& drawProj, bool editorView, const char* scope) {
    Ssao& ssao = *t.ssao;
    if (ssaoOn) ssao.Resize(w, h);
    const bool fromPrepass = ssaoOn && ssao.IsValid() && ProjectSettings::Quality().DepthPrepass;
    if (ssaoOn && ssao.IsValid() && !fromPrepass) {
        PROFILE_SCOPE(scope);
        PROFILE_GPU_SCOPE(scope);
        SsaoDepthPrepass(in, ssao, w, h, view, drawProj, editorView);

        t.hdr->BindForRender(); // restore for the main scene draw
    }
    return fromPrepass;
}

// The world pass into the view's HDR target: ctx carries the view; this adds SSAO-from-prepass and TAA's motion vectors.
void ViewPipeline::DrawWorld(const ViewPipelineInputs& in, const ViewTargets& t, RenderFrameContext& ctx,
                             bool ssaoFromPrepass, bool taaOn, EditorLayer::RenderStats* stats) {
    if (ssaoFromPrepass) {
        Ssao* ssao = t.ssao;
        const glm::mat4 proj = ctx.Proj;
        ctx.SsaoFromPrepass = [this, &in, ssao, proj] { SsaoCompute(in, *ssao, proj); };
    }
    ctx.TaaVelocity = taaOn;
    ctx.TaaJitterNdc = t.taa->JitterNdc();
    ctx.TaaPrevViewProj = t.taa->PrevViewProj();
    DrawScene(in, ctx, stats);
}

unsigned int ViewPipeline::ResolveTaa(const ViewPipelineInputs& in, const ViewTargets& t, unsigned int colorTex,
                                      const char* scope) {
    PROFILE_SCOPE(scope);
    PROFILE_GPU_SCOPE(scope);
    return t.taa->Resolve(colorTex, t.hdr->ResolvedDepthTexture(), t.hdr->VelocityTexture(), in.world->TaaSharpness);
}

// PR16: Bloom — soft-knee threshold + mip-pyramid blur before tonemapping.
// Resize takes the FULL view resolution; Bloom halves it internally for mip 0.
void ViewPipeline::ComputeBloom(const ViewPipelineInputs& in, const ViewTargets& t, unsigned int srcTex, int w, int h,
                                const char* scope, unsigned int& glowTex, float& glowIntensity) {
    World& world = *in.world;
    glowTex = 0u;
    glowIntensity = 0.0f;
    if (!world.BloomEnabled || FKeyRow::PostFxOff()) return; // F8 off: no bloom
    PROFILE_GPU_SCOPE(scope);
    Bloom& bloom = *t.bloom;
    bloom.Resize(w, h);
    // audit #358 — only run the pyramid passes if every mip FBO validated
    // complete; otherwise the frame simply has no bloom glow added.
    if (bloom.IsValid()) {
        bloom.Compute(*in.bloomThreshShader, *in.bloomDownsampleShader, *in.bloomUpsampleShader,
                      srcTex,
                      world.BloomThreshold,
                      world.BloomKnee);
        glowTex = bloom.GlowTexture();
        glowIntensity = world.BloomIntensity;
    }
    GLStateCache::Invalidate();
}

// --- Scene tab (editor viewport) offscreen pass -----------------------------------
// Rendered into its own framebuffer rather than straight into the backbuffer, then
// displayed via ImGui::Image inside the "Scene" window (see
// EditorLayer::SetSceneTexture's comment for why the old raw-GL-underneath technique
// broke once Scene became a real, named, tabbed window).
void ViewPipeline::RenderSceneView(const ViewPipelineInputs& in) {
    PROFILE_SCOPE("Scene View Render");
    PROFILE_GPU_SCOPE("Scene View Render");
    World& world = *in.world;
    EditorLayer& editor = *in.editor;
    Camera& editorCamera = *in.editorCamera;
    const ViewTargets& t = in.scene;
    HdrTarget& sceneHdr = *t.hdr;
    Framebuffer& sceneFramebuffer = *in.sceneLdr;
    const int windowW = in.window->GetWidth(), windowH = in.window->GetHeight();

    glm::vec2 available = editor.GetLastSceneContentRegion();
    if (available.x < 1.0f || available.y < 1.0f) {
        available = {(float)windowW, (float)windowH};
    }
    const int scW = std::max((int)available.x, 1);
    const int scH = std::max((int)available.y, 1);

    sceneFramebuffer.Resize(scW, scH);
    float sceneAspect = (float)scW / (float)scH;
    glm::mat4 sceneViewMat = editorCamera.ViewMatrix();
    glm::mat4 sceneProjMat = editorCamera.ProjectionMatrix(sceneAspect);
    // The world pass draws through the jittered projection; picking, gizmos, the grid and outlines keep sceneProjMat.
    bool sceneTaaOn = false;
    const glm::mat4 sceneDrawProj = BeginView(in, t, scW, scH, sceneViewMat, sceneProjMat, sceneTaaOn);

    EditorLayer::ShadingMode shading = editor.GetShadingMode();
    bool sceneWireframe = shading == EditorLayer::ShadingMode::Wireframe;
    bool sceneUnlit = shading == EditorLayer::ShadingMode::Unlit;
    const int sceneDebugView = FKeyRow::DebugViewFor((int)shading); // model shader uDebugView (FKeyRow.h)
    if (sceneWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    // Skipped in unlit/wireframe modes where SSAO has no visual effect.
    const bool sceneSsaoFromPrepass = SsaoPrepass(in, t, world.SsaoEnabled && !sceneUnlit, scW, scH, sceneViewMat,
                                                  sceneDrawProj, /*editorView=*/true, "SSAO Depth Pre-pass");

    EditorLayer::RenderStats sceneStats;
    {
        RenderFrameContext sceneCtx{ sceneViewMat, sceneDrawProj, editorCamera.Position,
                                     sceneUnlit, /*EditorView=*/true, /*DebugView=*/sceneDebugView,
                                     &sceneHdr, t.opaque, t.ssao };
        DrawWorld(in, t, sceneCtx, sceneSsaoFromPrepass, sceneTaaOn, &sceneStats);
    }

    editor.SetRenderStats(sceneStats);

    // #178 - Search in Scene: while the Hierarchy search is active, wash every visible
    // object that doesn't match it in flat grey, so the matches stand out (Unity).
    if (editor.SceneSearchActive() && !editor.OverlaysHidden()) {
        if (sceneWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        for (auto e : world.Registry.view<const TransformComponent, const RenderableComponent>()) {
            if (world.Registry.any_of<InactiveTag, LodCulledTag, HiddenInSceneTag, PoseSourceTag, OwnerViewOnlyTag>(e)) continue;
            if (editor.MatchesSceneSearch(world, e)) continue;
            const auto& r = world.Registry.get<const RenderableComponent>(e);
            if (!r.ModelRef || r.CastShadows == RenderableComponent::ShadowCasting::ShadowsOnly) continue;
            in.tintOverlay->Render(*r.ModelRef, world.GetCachedWorldTransform(e), sceneViewMat, sceneProjMat,
                                   glm::vec3(0.18f), 0.85f);
        }
        if (sceneWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        GLStateCache::Invalidate();
    }
    // NB: in Wireframe mode the polygon mode stays GL_LINE through the selection
    // passes below, so the inverted-hull "outline" draws as an enlarged orange
    // wireframe over the object rather than a filled orange silhouette that buries
    // its own wireframe (#13 P2). Reset to GL_FILL before the drag-preview ghost.

    // Selection outline (Renderer/SelectionOutline): the hover pre-highlight and the selection's ring.
    if (!editor.OverlaysHidden() && !editor.CleanShots()) {
        SelectionOutlineInputs outlineIn;
        outlineIn.world = &world;
        outlineIn.editor = &editor;
        outlineIn.editorCamera = &editorCamera;
        outlineIn.maskShader = in.outlineModelShader;
        outlineIn.dilateShader = in.outlineDilateShader;
        outlineIn.fsQuadVao = in.fsQuadVao;
        outlineIn.target = &sceneHdr;
        outlineIn.view = sceneViewMat;
        outlineIn.proj = sceneProjMat;
        outlineIn.width = scW;
        outlineIn.height = scH;
        outlineIn.wireframe = sceneWireframe;
        outlineIn.playing = *in.playing;
        in.selectionOutline->Render(outlineIn);
    }

    if (sceneWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // "Drag from Asset Browser" placement preview: a translucent ghost at wherever
    // the dragged model would land if released right now (raycast + grid-snap +
    // bottom-alignment, computed each frame by EditorLayer::DrawViewportDropTarget
    // while the drag is over the viewport) - see TintOverlayRenderer's own comment
    // for why this is a flat tint rather than the real PBR material shader.
    const EditorLayer::DragPreview& dragPreview = editor.GetDragPreview();
    if (dragPreview.Active) {
        const glm::mat4 pose = glm::translate(glm::mat4(1.0f), dragPreview.Position) *
                               glm::mat4_cast(dragPreview.Rotation);
        for (const auto& part : dragPreview.Parts)
            if (part.ModelRef)
                in.tintOverlay->Render(*part.ModelRef, pose * part.Local, sceneViewMat, sceneProjMat,
                    glm::vec3(0.3f, 0.85f, 1.0f), dragPreview.Alpha);
    }

    // Resolve MSAA now, before the grid/axis-line and collider-wireframe overlays
    // below — this snapshot is what Bloom reads. Editor-only overlays are never real
    // scene lighting, so they must never bloom/glow; capturing the bloom source here,
    // before they're drawn, guarantees that regardless of their color or opacity.
    sceneHdr.ResolveTo();

    unsigned int bloomGlowTex  = 0u;
    float        bloomIntensity = 0.0f;
    ComputeBloom(in, t, sceneHdr.ResolvedColorTexture(), scW, scH, "Bloom", bloomGlowTex, bloomIntensity);

    // The collider wireframe overlay — drawn AFTER the bloom source snapshot above so it
    // never contributes to bloom. Bloom::Compute (when it ran) leaves FBO 0 bound, so the
    // MSAA HDR target is rebound here; the overlay still depth-tests correctly against
    // the real scene geometry already in that target's depth buffer.
    sceneHdr.BindForRender();
    // Collider wireframe overlay (#185 PR 2) — depth-tested so scene geometry
    // occludes it, no depth write so it never blocks anything drawn after. Also
    // carries the physics visual debug-draw channels (#185 A), which have their own
    // toggle, so it runs when either is on.
    {
        const bool showShapes = EditorSettings::Get().ShowColliders;
        const bool showDebug  = EditorSettings::Get().PhysicsDebugDrawFlags != 0u || BodyDebug::Enabled();
        if ((showShapes || showDebug) && !editor.OverlaysHidden() && !editor.CleanShots() && !in.harnessCleanViews) {
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            // Selected-only: just the selection's colliders and its descendants' (F3's Play overlay below shows all).
            const std::function<bool(entt::entity)> inSelection = [&](entt::entity e) { return editor.InSelectionTree(world, e); };
            in.colliderGizmo->Draw(sceneViewMat, sceneProjMat, world, showShapes,
                                   EditorSettings::Get().CollidersSelectedOnly ? &inSelection : nullptr);
            glDepthMask(GL_TRUE);
        }
    }

    // Resolve again now that the collider overlay is drawn, so the Scene tab
    // still shows them — only the earlier, overlay-free resolve above ever reached
    // Bloom. Tonemap the linear-HDR scene into the LDR texture the Scene tab displays
    // (exposure -> curve -> gamma, once, in Tonemapper).
    sceneHdr.ResolveTo();
    unsigned int sceneColorTex = sceneHdr.ResolvedColorTexture();
    if (sceneTaaOn) sceneColorTex = ResolveTaa(in, t, sceneColorTex, "TAA");
    {
    PROFILE_GPU_SCOPE("Tonemap");
    in.tonemapper->Apply(sceneColorTex, sceneFramebuffer.Handle(), scW, scH,
                         MakePostSettings(world, bloomGlowTex, bloomIntensity, in.dt, 0, editorCamera.Position));
    }

    // Grid / axis lines — drawn AFTER the tonemap, straight into the LDR image the Scene
    // tab shows, so their colours are the editor's own sRGB values (EditorTheme::Axis*,
    // the traffic lights' family) rather than linear light pushed through exposure, the
    // tonemap curve, grading and gamma, which washed them out toward white. Also keeps
    // them out of bloom. The grid quad sits at a constant depth and
    // never occluded against the scene, so no depth test is lost by drawing here.
    sceneFramebuffer.Bind();
    if (editor.ShowGrid() && !editor.OverlaysHidden() && !in.harnessCleanViews) { // (no grid lines through a test's shots)
        const GLboolean gridHadDepthTest = glIsEnabled(GL_DEPTH_TEST);
        glDisable(GL_DEPTH_TEST);
        // The grid shader fades lines by literal world-space distance from the
        // camera. That distance is meaningless in orthographic mode — scroll-zoom
        // there resizes OrthoHalfHeight without moving the camera (see
        // UpdateEditorCamera), so "how zoomed in you are" and "how far the camera
        // physically sits" are decoupled, and a fixed fade radius would fade the grid
        // out at high zoom for no visual reason. Orthographic views don't need the
        // radial fade anyway (no perspective depth cue to blend into), so just push
        // it out far enough to never kick in.
        const EditorSettings& gset = EditorSettings::Get();
        float gridFade = editorCamera.Orthographic ? 100000.0f : gset.GridFadeDistance;
        auto toVec3 = [](ImVec4 c) { return glm::vec3(c.x, c.y, c.z); };
        GridColors gridColors;
        gridColors.AxisX = toVec3(EditorTheme::AxisX);
        gridColors.AxisY = toVec3(EditorTheme::AxisY);
        gridColors.AxisZ = toVec3(EditorTheme::AxisZ);
        in.grid->Draw(sceneViewMat, sceneProjMat, editorCamera.Position,
                      gset.GridMinorSpacing, (float)gset.GridMajorEvery, gridFade,
                      gset.GridOpacity, gset.GridShowAxisLines, gset.GridAxisThickness,
                      gridColors);
        if (gridHadDepthTest) glEnable(GL_DEPTH_TEST);
    }

    Framebuffer::BindDefault(windowW, windowH);
    editor.SetSceneTexture(sceneFramebuffer.ColorTexture(), sceneFramebuffer.Width(), sceneFramebuffer.Height());
}

void ViewPipeline::RenderGameFrame(const ViewPipelineInputs& in, const glm::mat4& view, const glm::mat4& proj,
                                   const glm::vec3& eye, int rw, int rh, unsigned outFbo, int outW, int outH,
                                   bool overlayInPlayOnly, EditorLayer::RenderStats& stats) {
    World& world = *in.world;
    const ViewTargets& t = in.game;
    HdrTarget& gameHdr = *t.hdr;
    const bool playing = *in.playing;
    const bool scaled = rw != outW || rh != outH;
    // TAA: the world pass (and its SSAO pre-pass) draws jittered; the view-model sub-pass builds its own
    // unjittered projection (SceneRenderer), and the overlays below keep `proj`.
    bool gameTaaOn = false;
    const glm::mat4 drawProj = BeginView(in, t, rw, rh, view, proj, gameTaaOn);
    // SSAO depth pre-pass - the Game view's own Ssao instance/resolution, apart from the Scene view's (see the
    // gameSsao declaration). The two callers are mutually exclusive per frame, so no resize thrash.
    const bool gameSsaoOn = world.SsaoEnabled && !FKeyRow::PostFxOff(); // F8
    const bool gameSsaoFromPrepass = SsaoPrepass(in, t, gameSsaoOn, rw, rh, view, drawProj, /*editorView=*/false,
                                                 "SSAO Depth Pre-pass (Game)");

    // F5's draw mode and F7's light view reach the game camera too (FKeyRow.h).
    const EditorLayer::ShadingMode gameShading = in.editor->GetShadingMode();
    const bool gameWire = gameShading == EditorLayer::ShadingMode::Wireframe;
    if (gameWire) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    {
        const bool ownerView = playing && *in.playUsesPlayer;
        RenderFrameContext gameCtx{ view, drawProj, eye, /*Unlit=*/gameShading == EditorLayer::ShadingMode::Unlit,
                                    /*EditorView=*/false, /*DebugView=*/FKeyRow::DebugViewFor((int)gameShading),
                                    &gameHdr, t.opaque, t.ssao,
                                    in.viewModelFov(),
                                    in.weaponOverlay(view, drawProj, eye, rh),
                                    /*OwnerView=*/ownerView };
        DrawWorld(in, t, gameCtx, gameSsaoFromPrepass, gameTaaOn, &stats);
    }
    if (gameWire) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Physics debug overlay over the game view (#185, F6 Physics) - depth-tested, no depth write.
    if ((playing || !overlayInPlayOnly) && EditorSettings::Get().PlayDebugOverlay) {
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        in.colliderGizmo->Draw(view, proj, world, EditorSettings::Get().ShowColliders);
        glDepthMask(GL_TRUE);
    }
    if (in.worldExtras) in.worldExtras(view, proj, eye);

    gameHdr.ResolveTo();
    unsigned int gameColorTex = gameHdr.ResolvedColorTexture();
    if (gameTaaOn) gameColorTex = ResolveTaa(in, t, gameColorTex, "TAA (Game)");

    // PR16: Bloom for the Game view - own Bloom instance (see gameBloom declaration).
    unsigned int bloomGlowTex   = 0u;
    float        bloomGlowIntensity = 0.0f;
    ComputeBloom(in, t, gameColorTex, rw, rh, "Bloom (Game)", bloomGlowTex, bloomGlowIntensity);
    {
    PROFILE_GPU_SCOPE("Tonemap");
    const PostSettings post = WithDepthEffects(MakePostSettings(world, bloomGlowTex, bloomGlowIntensity, in.dt, 1, eye), world, gameHdr, proj);
    if (scaled) {
        in.gameLdr->Resize(rw, rh);
        in.tonemapper->Apply(gameColorTex, in.gameLdr->Handle(), rw, rh, post);
    } else {
        in.tonemapper->Apply(gameColorTex, outFbo, outW, outH, post);
    }
    }
    if (scaled) {
        PROFILE_GPU_SCOPE("Upscale");
        in.upscaler->Apply(in.gameLdr->ColorTexture(), rw, rh, outFbo, 0, 0, outW, outH, EditorSettings::Get().RenderSharpness);
    }
    if (in.hud) in.hud(outFbo, outW, outH, view, proj);
}
