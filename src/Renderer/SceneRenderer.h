#pragma once

#include <glm/glm.hpp>
#include "RenderStats.h"

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
};
