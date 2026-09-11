#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_map>
#include "RenderStats.h"

// Per-run tally of meshes drawn through each ShaderAsset variant key (audit #354). Key 0 means
// the default modelShader / a zero-keyword variant. Read by --smoke-test to report which lobes
// actually rendered; reset it per scene.
namespace SceneRendererDebug {
const std::unordered_map<std::uint32_t, std::uint64_t>& VariantDrawCounts();
void ResetVariantDrawCounts();
}

class World;
class Sky;
class Shader;
class Cubemap;
class CascadedShadowMap;
class SpotShadowMap;
class PointShadowMap;
class IblProbe;
class ReflectionProbeArray;
class LightBuffer;
class ClusterGrid;
struct RenderFrameContext;

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
    Shader*              modelShader    = nullptr;
    Shader*              clusterBuildShader = nullptr;
    Shader*              clusterCullShader  = nullptr;
    CascadedShadowMap*   sunShadow      = nullptr;
    SpotShadowMap*       spotShadow     = nullptr;
    PointShadowMap*      pointShadow    = nullptr;
    IblProbe*            iblProbe       = nullptr;
    ReflectionProbeArray* probeArray    = nullptr;
    LightBuffer*         lightBuffer    = nullptr;
    ClusterGrid*         clusterGrid    = nullptr;

    // --- this frame's environment / light state ---
    const Cubemap* hdriCube = nullptr; // non-null => draw the HDRI sky instead of the gradient
    bool  sunShadowsReady   = false;   // the once-per-frame cascade pass actually ran
    int   frameLightCount   = 0;       // entries in lightBuffer this frame (incl. the sun)

    // Global render toggles (mirrors of EditorSettings, passed as plain values so the renderer
    // boundary doesn't depend on the editor). layerVisibleMask is only consulted for ctx.EditorView.
    bool     shadowsEnabled   = true;
    bool     ssaoEnabled      = false;
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
    int  spotCountForView = 0, pointCountForView = 0;
    float clusterNearZ = 0.0f, clusterFarZ = 0.0f;
    int  vp[4] = {0, 0, 0, 0};
};

// Host-owned scene-draw pass, extracted from main.cpp's `drawScene` lambda (audit #359, pass 2).
// No behavior change: same GL calls, same uniform order. main.cpp still owns lifecycle, the
// shadow/IBL/cluster prerequisite passes, framebuffer binding, and the viewport.
class SceneRenderer {
public:
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
};
