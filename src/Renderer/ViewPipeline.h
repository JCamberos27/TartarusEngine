#pragma once
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "EditorLayer.h" // EditorLayer::RenderStats
#include "InstanceBatch.h"
#include "SceneRenderer.h"
#include "Tonemapper.h" // PostSettings

class World;
class Camera;
class Window;
class Shader;
class Framebuffer;
class HdrTarget;
class TemporalAA;
class OpaqueColorCopy;
class Ssao;
class Bloom;
class Tonemapper;
class Upscaler;
class Grid;
class ColliderGizmo;
class TintOverlayRenderer;
class SelectionOutline;
class ShadowFrame;
struct ShadowFrameInputs;
struct RenderFrameContext;

// One view's offscreen targets: the Scene tab and the game camera each keep their own, sized independently
// (both can render in the same frame at different resolutions, so sharing would thrash their Resize()).
struct ViewTargets {
    HdrTarget* hdr = nullptr;            // MSAA linear HDR the world draws into
    TemporalAA* taa = nullptr;           // TAA history
    OpaqueColorCopy* opaque = nullptr;   // PR12: opaque color capture for refraction
    Ssao* ssao = nullptr;                // PR15: depth pre-pass FBO + occlusion
    Bloom* bloom = nullptr;              // PR16: threshold + mip-pyramid blur
};

// What the Scene and Game view render paths read, gathered once per frame (the Renderer/ShadowFrame pattern: pointers
// to main's long-lived objects, no ownership). `playing` and `playUsesPlayer` are pointers because Play can start or
// stop between the Scene view's render and the Game view's (the action bar's Play/Stop runs in between).
struct ViewPipelineInputs {
    World* world = nullptr;
    const Window* window = nullptr;
    EditorLayer* editor = nullptr;
    Camera* editorCamera = nullptr;
    const bool* playing = nullptr;
    const bool* playUsesPlayer = nullptr;
    float dt = 0.0f;

    // The scene draw (drawScene): the renderer, its per-frame inputs and the sun's cascades.
    SceneRenderer* sceneRenderer = nullptr;
    SceneRenderInputs* sceneInputs = nullptr; // sunShadowsReady is set per view
    ShadowFrame* shadowFrame = nullptr;
    const ShadowFrameInputs* shadowIn = nullptr;

    // Shaders
    Shader* depthShader = nullptr; // ShadowDepth: the SSAO depth pre-pass (uLightViewProj = proj * view)
    Shader* ssaoComputeShader = nullptr;
    Shader* ssaoBlurShader = nullptr;
    Shader* bloomThreshShader = nullptr;
    Shader* bloomDownsampleShader = nullptr;
    Shader* bloomUpsampleShader = nullptr;
    Shader* outlineModelShader = nullptr;
    Shader* outlineDilateShader = nullptr;
    unsigned int fsQuadVao = 0;

    // Shared passes
    Tonemapper* tonemapper = nullptr;
    ColliderGizmo* colliderGizmo = nullptr;

    // The Scene tab (editor viewport)
    ViewTargets scene;
    Framebuffer* sceneLdr = nullptr; // tonemap output the Scene tab shows via ImGui::Image
    TintOverlayRenderer* tintOverlay = nullptr;
    Grid* grid = nullptr;
    SelectionOutline* selectionOutline = nullptr;
    bool harnessCleanViews = false; // --npc-test shots: no grid or collider overlay

    // The game camera (docked Game view and maximized Play)
    ViewTargets game;
    Framebuffer* gameLdr = nullptr; // the internal-resolution tonemap target the upscaler scales from
    Upscaler* upscaler = nullptr;
    std::function<float()> viewModelFov; // FirstPersonPresentation::ViewModelFov
    // The weapon's world effects for one view (RenderFrameContext::WorldOverlay); empty = none.
    std::function<std::function<void()>(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye, int h)> weaponOverlay;
    // Into the HDR target after the world (the gravity gun's throw arc), and over the finished frame (the combat HUD).
    std::function<void(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye)> worldExtras;
    std::function<void(unsigned fbo, int w, int h, const glm::mat4& view, const glm::mat4& proj)> hud;
};

// The Scene view and game camera render paths: one pipeline both views call (HDR target + TAA set-up, the SSAO depth
// pre-pass, the world draw, the TAA resolve, bloom), with each view's own overlays and post around it.
class ViewPipeline {
public:
    // Renders the lit scene (sky + every Transform+Renderable entity) into whatever framebuffer/viewport is currently
    // bound: this view's LOD levels and sun cascades, then SceneRenderer::RenderScene. Also the probe capture's draw.
    void DrawScene(const ViewPipelineInputs& in, const RenderFrameContext& ctx, EditorLayer::RenderStats* outStats);

    // The Scene tab's offscreen pass: world, search wash, selection outline, drag ghost, bloom, collider overlay,
    // TAA, tonemap and grid, into in.sceneLdr, handed to the editor (SetSceneTexture / SetRenderStats).
    void RenderSceneView(const ViewPipelineInputs& in);

    // The game camera's frame, from the HDR clear to the HUD: drawn at rw x rh, shown at outW x outH in `outFbo`
    // (upscaled when smaller). The docked Game view and Free-Aspect maximized play differ only in what they pass:
    // `overlayInPlayOnly` keeps the physics debug overlay to Play.
    void RenderGameFrame(const ViewPipelineInputs& in, const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye,
                         int rw, int rh, unsigned outFbo, int outW, int outH, bool overlayInPlayOnly,
                         EditorLayer::RenderStats& stats);

private:
    // The shared stages, in the order each view runs them.
    glm::mat4 BeginView(const ViewPipelineInputs& in, const ViewTargets& t, int w, int h, const glm::mat4& view,
                        const glm::mat4& proj, bool& taaOn);
    bool SsaoPrepass(const ViewPipelineInputs& in, const ViewTargets& t, bool ssaoOn, int w, int h, const glm::mat4& view,
                     const glm::mat4& drawProj, bool editorView, const char* scope);
    void DrawWorld(const ViewPipelineInputs& in, const ViewTargets& t, RenderFrameContext& ctx, bool ssaoFromPrepass,
                   bool taaOn, EditorLayer::RenderStats* stats);
    unsigned int ResolveTaa(const ViewPipelineInputs& in, const ViewTargets& t, unsigned int colorTex, const char* scope);
    void ComputeBloom(const ViewPipelineInputs& in, const ViewTargets& t, unsigned int srcTex, int w, int h,
                      const char* scope, unsigned int& glowTex, float& glowIntensity);

    void SsaoCompute(const ViewPipelineInputs& in, Ssao& target, const glm::mat4& proj);
    void SsaoDepthPrepass(const ViewPipelineInputs& in, Ssao& target, int w, int h, const glm::mat4& view,
                          const glm::mat4& proj, bool editorView);

    PostSettings WithDepthEffects(PostSettings p, const World& world, const HdrTarget& hdr, const glm::mat4& proj);

    std::vector<VisibleRenderable> m_PrepassVisible; // the SSAO depth pre-pass's gather, reused frame to frame
    std::vector<DepthCaster> m_PrepassDraws;
};
