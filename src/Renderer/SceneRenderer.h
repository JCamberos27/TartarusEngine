#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <entt/entity/fwd.hpp>
#include "RenderStats.h"

// Per-run tally of meshes drawn through each ShaderAsset variant key (audit #354). Key 0 means
// the default modelShader / a zero-keyword variant. Read by --smoke-test to report which lobes
// actually rendered; reset it per scene.
namespace SceneRendererDebug {
const std::unordered_map<std::uint32_t, std::uint64_t>& VariantDrawCounts();
void ResetVariantDrawCounts();
}

class World;
class Model;
struct Frustum;
struct RenderableComponent;

// The one culling walk every pass shares (the camera view, the SSAO pre-pass, the sun and local shadow gathers):
// each active, LOD-visible renderable with a model, in registry order, filtered by what the pass asks for.
struct VisibilityQuery {
    // Shadow casters: Cast Shadows not Off, and every copy of a split pose but the first-person one (the world copy
    // casts in every view). Otherwise what a camera sees: not Shadows Only, and the view's own copy of a split pose.
    bool ShadowCasters = false;
    bool OwnerView = false;          // camera views: the player's own camera (sees the first-person copy)
    bool EditorView = false;         // the editor Scene view: its per-entity hide and layer visibility apply
    unsigned LayerVisibleMask = ~0u; //   which layers it shows
    bool SkipViewModels = false;     // ViewModelTag geometry left out
    bool SkipPlayerBody = false;     // PlayerBodyTag pieces left out
    bool SkipTransparent = false;    // a transparent first material slot left out
    const Frustum* Cull = nullptr;   // when set, anything with bounds outside it is left out
    // Small-object culling: when SizeCullEye is set, anything whose bounding sphere, seen from there, is shorter than
    // MinScreenHeight of the viewport is left out. Only camera views set it (the main view and the SSAO pre-pass);
    // the shadow passes leave it unset, so a culled prop still casts.
    const glm::vec3* SizeCullEye = nullptr;
    float SizeCullProjScale = 0.0f;  // the camera projection's [1][1] (1 / tan(fovY / 2))
    float MinScreenHeight = 0.0f;
    // A coarse test of whole ground-plan cells of renderables (see GatherVisible), for queries whose per-object test
    // happens later: a cell is skipped when its bounds miss every one of these CellCullCount frusta. Callers pass a
    // superset of what their own test accepts. Animated, unbounded, view-model and player-body renderables are never
    // in a cell, so they always come through.
    const Frustum* CellCull = nullptr;
    int CellCullCount = 0;
};
struct VisibleRenderable {
    entt::entity Entity;
    RenderableComponent* Renderable;
    glm::mat4 Xform;              // the cached world transform
    glm::vec3 LocalMin, LocalMax; // the model's bounds as posed (Model::PosedBounds)
    // Those bounds in world space; Bounded is false when the model has none (a never-populated pair), which draws or
    // casts without a frustum test.
    glm::vec3 WorldMin, WorldMax;
    bool Bounded;
};
// Clears `out` and fills it with what passes `q`.
void GatherVisible(World& world, const VisibilityQuery& q, std::vector<VisibleRenderable>& out);
// A box holding every renderable GatherVisible knows (a superset of any query's result), cheaply: the sun's cascades
// fit their near planes to it before the casters are gathered against them. False when there are none.
bool RenderableBounds(World& world, glm::vec3& outMin, glm::vec3& outMax);
// VisibilityQuery's small-object test: the box's bounding sphere seen from `eye` is shorter than `minScreenHeight`
// (a share of the viewport height). Never true from inside the sphere, or with the test off (a value <= 0).
bool TooSmallOnScreen(const glm::vec3& worldMin, const glm::vec3& worldMax, const glm::vec3& eye, float projScaleY,
                      float minScreenHeight);

class Sky;
class SkyAtmosphere;
class Shader;
class Cubemap;
class CascadedShadowMap;
class SpotShadowMap;
class PointShadowMap;
class IblProbe;
class ReflectionProbeArray;
class LightBuffer;
class ClusterGrid;
class DecalSystem;
struct RenderFrameContext;
class VolumetricFog;

// Engine resources + this frame's light/shadow state that the scene-draw pass needs, gathered
// into one explicit value instead of the ~25 `[&]`-captures the old main.cpp lambda relied on
// (audit #359, pass 2). Pointers to the stable subsystems are the same every frame; the scalars
// and the shadow arrays are refreshed per frame by main.cpp's light-gather + shadow passes
// before RenderScene() runs. The shadow-array pointers alias main.cpp's fixed-size stack arrays
// (SpotShadowMap::kMaxSpots / PointShadowMap::kMaxPoints entries); only the first *Count entries
// are read.
struct SceneRenderInputs {
    // --- stable engine resources ---
    Sky*                 sky            = nullptr;
    SkyAtmosphere*       skyAtmosphere  = nullptr; // non-null => the physical sky draws and lights this frame
    Shader*              modelShader    = nullptr;
    // ShadowDepth: the opaque pass's depth pre-pass (Project Settings > Quality > Depth pre-pass); null skips it.
    Shader*              depthShader    = nullptr;
    Shader*              clusterBuildShader = nullptr;
    Shader*              clusterCullShader  = nullptr;
    CascadedShadowMap*   sunShadow      = nullptr;
    SpotShadowMap*       spotShadow     = nullptr;
    PointShadowMap*      pointShadow    = nullptr;
    IblProbe*            iblProbe       = nullptr;
    ReflectionProbeArray* probeArray    = nullptr;
    LightBuffer*         lightBuffer    = nullptr;
    ClusterGrid*         clusterGrid    = nullptr;
    // Clustered decals (docs/DECALS.md): gathered per view in RenderScene and binned by the cluster
    // cull. Either null = no decals.
    DecalSystem*         decals          = nullptr;
    Shader*              decalCullShader = nullptr;

    // --- this frame's environment / light state ---
    const Cubemap* hdriCube = nullptr; // non-null => draw the HDRI sky instead of the gradient
    bool  sunShadowsReady   = false;   // the once-per-frame cascade pass actually ran
    int   frameLightCount   = 0;       // entries in lightBuffer this frame (incl. the sun)

    // Global render toggles (mirrors of EditorSettings, passed as plain values so the renderer
    // boundary doesn't depend on the editor). layerVisibleMask is only consulted for ctx.EditorView.
    bool     shadowsEnabled   = true;
    bool     ssaoEnabled      = false;
    float    ssaoIntensity    = 1.0f;  // #160 — occlusion exponent (World::SsaoIntensity)
    bool     postFxOff        = false; // F8: no SSAO, no fog (distance or volumetric) in any view
    unsigned layerVisibleMask = 0xFFFFFFFFu;

    // sun shadow tuning (from the active directional light this frame)
    float sunAngularDeg       = 0.53f;
    float sunShadowSoftness   = 1.0f;
    float sunShadowBias       = 1.0f;
    float sunShadowNormalBias = 1.0f;

    // spot-light shadow maps allocated this frame (arrays alias main.cpp stack storage)
    int              spotShadowCount      = 0;
    const glm::mat4* spotShadowVP         = nullptr;
    const glm::vec3* spotShadowPos        = nullptr;
    const float*     spotShadowFar        = nullptr;
    const float*     spotShadowHalfTan    = nullptr;
    const float*     spotShadowBias       = nullptr;
    const float*     spotShadowNormalBias = nullptr;
    const float*     spotShadowSoftness   = nullptr;

    // point-light cube shadow maps allocated this frame
    int          pointShadowCount      = 0;
    const float* pointShadowFar        = nullptr;
    const float* pointShadowBias       = nullptr;
    const float* pointShadowNormalBias = nullptr;
};

// The model-shader program state for one frame + view — computed once by RenderScene (the
// viewport read, the shadow/IBL/SSAO/cluster enable flags, the cluster near/far), then applied
// to whichever program(s) the draw loop selects via ApplyFrameState(). Non-owning: the pointers
// alias RenderScene's own arguments for the lifetime of the call (audit #354: this is the seam
// that lets a material select a ShaderAsset variant and still get the common per-frame state).
struct FrameState {
    const RenderFrameContext* ctx   = nullptr;
    const SceneRenderInputs*  in    = nullptr;
    World*                    world = nullptr; // SkyAmbientIntensity for uIBLIntensity

    bool shadowsOn = false, sunShadowsOn = false;
    bool iblOn = false, ssaoOn = false, clusterOn = false;
    bool fogOn = true;  // distance fog (F8 Post FX off clears it)
    bool volOn = false; // this view builds and applies the volumetric fog (VolumetricFog::WantedFor)
    float ssaoIntensity = 1.0f; // #160
    int  spotCountForView = 0, pointCountForView = 0;
    float clusterNearZ = 0.0f, clusterFarZ = 0.0f;
    int  vp[4] = {0, 0, 0, 0};
    int  decalCount = 0; // decals visible in this view (uDecalCount); 0 without clustering
};

// Host-owned scene-draw pass, extracted from main.cpp's `drawScene` lambda (audit #359, pass 2).
// No behavior change: same GL calls, same uniform order. main.cpp still owns lifecycle, the
// shadow/IBL/cluster prerequisite passes, framebuffer binding, and the viewport.
class SceneRenderer {
public:
    SceneRenderer();
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // Compiles the volumetric fog programs and builds its noise now (load-time warm-up).
    void WarmUpVolumetrics();

    // Compiles, at load, every shader-variant program the world's materials will draw through
    // (otherwise each compiles on the first frame that draws it). Returns how many programs it touched.
    static int WarmShaderVariants(World& world);
    // The same for one entity and everything under it: a body built mid-play (an NPC spawn, a new outfit
    // piece) would otherwise compile its new variants on the frame that first draws it.
    static int WarmShaderVariants(World& world, entt::entity root);
    // The combat effects' programs (particles, FX sprites, blood sprays and decals) compiled now: each used to compile
    // on its first use, mid-fight.
    static void WarmEffects();

    // Draws sky + the opaque and transparent scene geometry for one viewport. `ctx` carries the
    // per-view params (see RenderFrameContext.h and its GL state contract); `in` carries the
    // engine resources + this frame's light/shadow state. `outStats` (optional) receives the
    // draw-call / triangle / cull / overflow counts for the Stats overlay.
    void RenderScene(World& world, const RenderFrameContext& ctx, const SceneRenderInputs& in,
                     RenderStats* outStats);

private:
    // Compute this frame's model-shader state (viewport, enable flags, cluster near/far). No GL
    // writes beyond reading GL_VIEWPORT.
    FrameState GatherFrameState(World& world, const RenderFrameContext& ctx,
                                const SceneRenderInputs& in) const;

    // Bind `program` and push the whole common per-frame uniform + texture-unit set onto it
    // (camera, cascaded/spot/point shadows, IBL, reflection probes, SSAO, clustered lighting,
    // unlit/tonemap flags). Safe to call once per distinct program used in a frame.
    void ApplyFrameState(Shader& program, const FrameState& fs) const;

    // Froxel volumetric fog: per-view volumes, built in RenderScene before the opaque pass.
    std::unique_ptr<VolumetricFog> m_Volumetrics;
};
