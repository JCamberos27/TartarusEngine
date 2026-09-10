#include "Window.h"
#include "Input.h"
#include "Clock.h"
#include "CrashHandler.h"
#include "Shader.h"
#include "Camera.h"
#include "Player.h"
#include "World.h"
#include "gl.h"

#include "AudioEngine.h"
#include "AssetLibrary.h"
#include "HotReloadGameModule.h"
#include "HotReloadEditorModule.h"
#include "EditorModuleAPI.h" // EditorConsoleState (Clear on Play / Error Pause — #236 A5)
#include "EditorLayer.h"
#include "EditorSettings.h"
#include "Shortcuts.h" // play / window keys route through the Shortcuts Manager (#236 F)
#include "Model.h"
#include "SceneSerializer.h"
#include "AnimationSystem.h"
#include "Grid.h"
#include "ColliderGizmo.h" // #185 PR 2 — collider wireframe overlay
#include "Sky.h"
#include "ShaderLibrary.h"
#include "TintOverlayRenderer.h"
#include "HdrTarget.h"
#include "OpaqueColorCopy.h"
#include "Screenshot.h"
#include "Tonemapper.h"
#include "LightBuffer.h"
#include "ClusterGrid.h"
#include "CascadedShadowMap.h"
#include "SpotShadowMap.h"
#include "PointShadowMap.h"
#include "IblProbe.h"
#include "Cubemap.h"               // PR13: HDRI environment cubemap
#include "ReflectionProbeArray.h"  // PR14: placed reflection probes
#include "Ssao.h"                  // PR15: depth pre-pass + screen-space ambient occlusion
#include "RenderFrameContext.h"    // audit #359 — per-viewport scene-draw inputs
#include "SceneRenderer.h"         // audit #359 pass 2 — host-owned scene-draw pass
#include "Bloom.h"                 // PR16: threshold + blur bloom post-process
#include "GLStateCache.h"
#include "Profiler.h"
#include "Frustum.h"
#include "PhysicsWorld.h" // #185 — PhysX world stepped during Play
#include "GameModuleAPI.h" // RaycastHit for the debug physics harness
#include "GameViewPanel.h"
#include "ProjectPaths.h"
#include "EnginePaths.h"
#include "LayerRegistry.h"
#include "ProjectSettings.h"
#include "AssetDatabase.h"
#include "SplashScreen.h"
#include "GLDebug.h"
#include "Log.h"
#include "TextureCache.h"
#include "DefaultTextures.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <filesystem>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <intrin.h>   // __cpuid — CPU brand string for the boot log
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef APIENTRY
#undef APIENTRY  // GLFW already defined it; let windows.h redefine identically without warning C4005
#endif
#include <windows.h>  // GlobalMemoryStatusEx, registry, GetLogicalProcessorInformationEx — rig info
#pragma comment(lib, "Advapi32.lib") // RegGetValueA

// Ask the vendor drivers to run this process on the discrete GPU. On a multi-adapter box
// (laptop Optimus, or a desktop with the monitor cabled to the motherboard) the GL context
// otherwise lands on the integrated GPU — observed here as Intel UHD 630 instead of an RTX
// 4060, which turned this trivial scene into a 50-100 ms frame: the CPU raced ahead and then
// blocked in SwapBuffers waiting for the iGPU to drain the HDR + MSAA + multi-light + CSM
// pipeline. These must be *exported* symbols in the .exe for the NVIDIA / AMD drivers to see them.
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 1;
    __declspec(dllexport) int   AmdPowerXpressRequestHighPerformance = 1;
}

// Selection outline (editor-only): the classic "inverted hull" technique — draw the object
// again, offset a little along its normal, with front-face culling so only the silhouette
// peeking out from behind the normal draw survives. One flat fragment shader shared by both
// variants below; only the vertex stage differs, matching each mesh's own attribute layout.
// Depth-only pass for cascaded shadow maps. Mirrors kModelVertexSrc's skinning so animated
// occluders cast a deforming shadow; writes nothing but depth.
static const char* kShadowDepthVertexSrc = R"(
#version 460 core
layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aUV;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4 aWeights;
uniform mat4 uModel;
uniform mat4 uLightViewProj;
uniform int uUseSkinning;
layout(std430, binding = 1) readonly buffer BoneBlock { mat4 uBones[]; }; // shared with the model VS (#104)
out vec2 vUV;
out vec3 vWorldPos; // used by the local-light (spot/point) depth FS; the sun FS ignores it
void main() {
    vec4 localPos = vec4(aPos, 1.0);
    if (uUseSkinning == 1) {
        mat4 skinMat = mat4(0.0);
        float tw = 0.0;
        for (int i = 0; i < 4; ++i) {
            if (aBoneIDs[i] >= 0) { skinMat += uBones[clamp(aBoneIDs[i], 0, 99)] * aWeights[i]; tw += aWeights[i]; }
        }
        if (tw <= 0.0001) skinMat = mat4(1.0);
        localPos = skinMat * localPos;
    }
    vUV = aUV;
    vec4 worldPos = uModel * localPos;
    vWorldPos = worldPos.xyz;
    gl_Position = uLightViewProj * worldPos;
}
)";
// Alpha-tested casters (foliage, chain-link, decals): when a mesh has an albedo map its alpha
// is sampled and cut below 0.5 so the shadow follows the cutout, not a solid quad (#116). Opaque
// meshes leave uAlphaTest 0 and this is a no-op. Plain hardware depth (keeps early-Z) — used
// for the cascaded SUN shadow, whose ortho projection is already linear.
static const char* kShadowDepthFragmentSrc = R"(
#version 460 core
in vec2 vUV;
uniform int uAlphaTest;
uniform sampler2D uAlbedo;
void main() {
    if (uAlphaTest == 1 && texture(uAlbedo, vUV).a < 0.5) discard;
}
)";
// Spot / point-light depth: store LINEAR distance-to-light / far rather than the perspective
// projection's non-linear depth. A constant compare bias is then uniform in world space, so a
// shadow reaches the full light Range instead of the far part of the frustum losing depth
// precision (and the shadow with it). Writing gl_FragDepth forfeits early-Z — acceptable here.
static const char* kLocalShadowDepthFragmentSrc = R"(
#version 460 core
in vec2 vUV;
in vec3 vWorldPos;
uniform int uAlphaTest;
uniform sampler2D uAlbedo;
uniform vec3 uShadowLightPos;
uniform float uShadowFar;
void main() {
    if (uAlphaTest == 1 && texture(uAlbedo, vUV).a < 0.5) discard;
    gl_FragDepth = clamp(distance(vWorldPos, uShadowLightPos) / max(uShadowFar, 1e-3), 0.0, 1.0);
}
)";

// Selection outline (editor-only): the classic "inverted hull" technique — draw the object
// again, offset a little along its normal, with front-face culling so only the silhouette
// peeking out from behind the normal draw survives. One flat fragment shader shared by both
// variants below; only the vertex stage differs, matching each mesh's own attribute layout.
static const char* kOutlineFragmentSrc = R"(
#version 460 core
out vec4 FragColor;
uniform vec3 uOutlineColor;
void main() {
    FragColor = vec4(uOutlineColor, 1.0);
}
)";

// Mirrors kModelVertexSrc's skinning block so an animated model's outline deforms with it,
// then offsets along the (skinned) normal instead of computing UV/TBN — outline doesn't need them.
static const char* kOutlineModelVertexSrc = R"(
#version 460 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4 aWeights;
layout (location = 6) in float aTangentSign;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform int uUseSkinning;
uniform mat4 uBones[100];
uniform float uThickness;

void main() {
    vec4 localPos = vec4(aPos, 1.0);
    vec3 localNormal = aNormal;

    if (uUseSkinning == 1) {
        mat4 skinMat = mat4(0.0);
        float totalWeight = 0.0;
        for (int i = 0; i < 4; ++i) {
            if (aBoneIDs[i] >= 0) {
                skinMat += uBones[aBoneIDs[i]] * aWeights[i];
                totalWeight += aWeights[i];
            }
        }
        if (totalWeight <= 0.0001) skinMat = mat4(1.0);
        localPos = skinMat * localPos;
        localNormal = mat3(skinMat) * aNormal;
    }

    vec4 world = uModel * localPos;
    vec3 worldNormal = normalize(mat3(transpose(inverse(uModel))) * localNormal);
    world.xyz += worldNormal * uThickness;
    gl_Position = uProj * uView * world;
}
)";

// Screen-space selection outline: a fullscreen pass that reads a 1-bit "is this pixel part of
// the selection" mask (rendered by the outline shader above into its own target) and paints a
// uniform-width ring in the gap just outside the silhouette. Works for any shape/orientation —
// unlike an inverted-hull, which can't widen a flat mesh's screen silhouette at all (audit #51).
static const char* kOutlineDilateVertSrc = R"(
#version 460 core
out vec2 vUV;
const vec2 kQuad[6] = vec2[](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0)
);
void main() {
    vec2 p = kQuad[gl_VertexID];
    vUV = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

static const char* kOutlineDilateFragSrc = R"(
#version 460 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uMask;
uniform vec3 uTexel;     // xy = 1.0 / mask size, in texels
uniform vec3 uColor;
uniform int uRadius;     // outline half-width, in pixels
void main() {
    float here = texture(uMask, vUV).r;
    if (here > 0.5) discard;                 // inside the selection: leave the surface alone
    float adj = 0.0;
    for (int y = -uRadius; y <= uRadius; ++y) {
        for (int x = -uRadius; x <= uRadius; ++x) {
            if (x * x + y * y > uRadius * uRadius) continue; // round brush
            adj = max(adj, texture(uMask, vUV + vec2(float(x), float(y)) * uTexel.xy).r);
        }
    }
    if (adj < 0.5) discard;                  // not adjacent to the selection
    FragColor = vec4(uColor, 1.0);
}
)";

// Simple fly-camera controls used only while the editor overlay is open. `orbitPivot`, when
// non-null, is the current selection's world-space center (see EditorLayer::GetSelectionCenter)
// — Alt+Left-drag orbits around it instead of the plain free-look that Right-drag still does.
// Returns true on any frame the fly speed was changed by scroll (so the caller can flash the
// on-screen "Fly speed: N" readout — #236 R2).
static bool UpdateEditorCamera(Camera& cam, float dt, bool allowLook, const glm::vec3* orbitPivot) {
    bool flySpeedChanged = false;
    auto trimFlySpeed = [&](double notches) {
        float& fs = EditorSettings::Get().SceneCameraFlySpeed;
        fs = std::clamp(fs * std::pow(1.15f, (float)notches), 0.5f, 200.0f);
        EditorSettings::Save();
        flySpeedChanged = true;
    };
    const bool ctrlHeld = Input::IsKeyDown(GLFW_KEY_LEFT_CONTROL) || Input::IsKeyDown(GLFW_KEY_RIGHT_CONTROL);
    // WASD/QE flythrough only while Right-drag is held — matches Unity's convention exactly,
    // and is required now that W/E/R/T also double as gizmo-tool shortcuts (EditorLayer::Draw):
    // there'd be no way to tell "pressing W to fly" from "pressing W to switch tools" otherwise.
    // A/S/D/Q were never claimed by a tool shortcut, but gating the whole block together keeps
    // the behavior simple and predictable instead of only some of WASDQE requiring Right-drag.
    bool altHeld = Input::IsKeyDown(GLFW_KEY_LEFT_ALT) || Input::IsKeyDown(GLFW_KEY_RIGHT_ALT);

    // Right-drag alone flies the camera (WASDQE + mouselook); Alt+Right-drag is claimed below
    // for dolly instead (Unity's own Scene View split of the same two mouse buttons), so this
    // excludes the Alt-held case rather than fighting it for the same drag.
    if (allowLook && !altHeld && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
        // Scroll while flying (RMB held) trims the fly speed, Unity-style — persisted (#236 R2).
        if (double sc = Input::GetScrollDeltaY(); sc != 0.0) trimFlySpeed(sc);
        float speed = EditorSettings::Get().SceneCameraFlySpeed * dt *
                      (Input::IsKeyDown(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
        glm::vec3 move{0.0f};
        if (Input::IsKeyDown(GLFW_KEY_W)) move += cam.Front();
        if (Input::IsKeyDown(GLFW_KEY_S)) move -= cam.Front();
        if (Input::IsKeyDown(GLFW_KEY_D)) move += cam.Right();
        if (Input::IsKeyDown(GLFW_KEY_A)) move -= cam.Right();
        if (Input::IsKeyDown(GLFW_KEY_E)) move += glm::vec3(0, 1, 0);
        if (Input::IsKeyDown(GLFW_KEY_Q)) move -= glm::vec3(0, 1, 0);
        if (glm::length(move) > 0.0001f) {
            cam.Position += glm::normalize(move) * speed;
        }
        cam.ProcessMouseLook((float)Input::GetMouseDeltaX(), (float)Input::GetMouseDeltaY());
    }

    // Alt+Right-drag dollies the camera along its own view direction - Unity's "hold Alt and
    // drag with the right mouse button to move closer to/further from" gesture. Works the same
    // whether or not anything's selected: it just slides the camera forward/back, same as the
    // scroll-wheel zoom below, just driven by a drag instead of notches. In orthographic mode
    // moving the position has no visual effect (same reason scroll-zoom special-cases it below),
    // so this reuses that OrthoHalfHeight path instead of silently doing nothing.
    if (allowLook && altHeld && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
        // GLFW's mouse-Y delta is screen-down-positive, so dragging UP (delta negative) should
        // dolly IN - hence the sign flip here.
        float dollyAmount = -(float)Input::GetMouseDeltaY();
        if (cam.Orthographic) {
            const float kDollyZoomFactor = 0.01f;
            cam.OrthoHalfHeight = std::clamp(cam.OrthoHalfHeight * (1.0f - dollyAmount * kDollyZoomFactor), 0.25f, 250.0f);
        } else {
            const float kDollySpeed = 0.02f;
            cam.Position += cam.Front() * (dollyAmount * kDollySpeed);
        }
    }

    if (allowLook && orbitPivot && altHeld && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT)) {
        // Same look-rotation input as free-look above, but the camera's position is then pinned
        // to a sphere of constant radius around the pivot instead of staying put — the pivot
        // (the selected object) stays centered in view as the camera swings around it.
        float radius = std::max(glm::length(cam.Position - *orbitPivot), 0.05f);
        cam.ProcessMouseLook((float)Input::GetMouseDeltaX(), (float)Input::GetMouseDeltaY());
        cam.Position = *orbitPivot - cam.Front() * radius;
    }

    // Scroll wheel zooms. Dollying the camera position (as perspective does below) has NO
    // visual effect under an orthographic projection, so orthographic mode instead shrinks/grows
    // OrthoHalfHeight — multiplicatively, so the zoom rate scales with how zoomed-in you already
    // are instead of crawling at large scales or blowing past small ones with a fixed step.
    // Ctrl+scroll (no drag needed) also trims the fly speed — Unity's binding — and shows the
    // same on-screen readout (#236 R2).
    if (allowLook && ctrlHeld && !Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
        if (double sc = Input::GetScrollDeltaY(); sc != 0.0) trimFlySpeed(sc);
    }

    // ...but not while flying (scroll-while-RMB) or trimming speed (Ctrl+scroll) — both handled above.
    if (allowLook && !(!altHeld && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) && !ctrlHeld) {
        double scroll = Input::GetScrollDeltaY();
        if (scroll != 0.0) {
            if (cam.Orthographic) {
                const float kScrollZoomFactor = 0.9f; // per wheel notch
                cam.OrthoHalfHeight = std::clamp(
                    cam.OrthoHalfHeight * powf(kScrollZoomFactor, (float)scroll), 0.25f, 250.0f);
            } else {
                const float kScrollZoomSpeed = 1.0f;
                cam.Position += cam.Front() * (float)(scroll * kScrollZoomSpeed);
            }
        }
    }

    // Middle-mouse-button drag pans the camera parallel to the view plane — same motion as
    // dragging the nav gizmo's pan button, but usable from anywhere by holding the scroll
    // wheel down, matching the Blender/Maya/Unity convention.
    if (allowLook && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_MIDDLE)) {
        const float kPanSpeed = 0.01f;
        float dx = (float)Input::GetMouseDeltaX();
        float dy = (float)Input::GetMouseDeltaY(); // inverted: up is positive
        cam.Position -= (cam.Right() * dx + cam.Up() * dy) * kPanSpeed;
    }
    return flySpeedChanged;
}

// The first active in-scene Camera entity (creation order), or entt::null. The Game view
// previews through it while editing so a shot can be framed without walking there (#36 B10).
static entt::entity FindActiveSceneCamera(const World& world) {
    entt::entity best = entt::null;
    int bestOrder = 0x7fffffff;
    for (auto e : world.Registry.view<const CameraComponent, const TransformComponent>()) {
        if (world.Registry.all_of<InactiveTag>(e)) continue;
        const auto* ord = world.Registry.try_get<OrderComponent>(e);
        int o = ord ? ord->Value : 0;
        if (o < bestOrder) { bestOrder = o; best = e; }
    }
    return best;
}

int main(int argc, char** argv) {
    // Before anything else: on an unhandled fault, drop a minidump next to the exe instead of
    // vanishing with a bare exit 139.
    CrashHandler::Install();

    // --smoke-test: headless-as-possible CI/manual smoke check (audit #187). Loads every scene
    // under project/scenes/, renders a fixed number of frames of each through the exact same
    // per-frame render path the interactive editor uses (see the `smokeTestMode` branches
    // sprinkled through the main loop below), then checks for new GL debug-callback errors and
    // a nonzero draw count before moving to the next scene. Exits 0 if every scene passed, or a
    // nonzero code if any failed — parsed here, before anything else, so it can never be
    // confused with a scene-path or other future argument.
    bool smokeTestMode = false;
    // --resave <in.json> <out.json>: load a scene and immediately re-serialize it, then exit.
    // The one headless path that exercises the SAVE side of the serializer — round-trip tests
    // (prefab overrides #302 Part B, the reflected-component migrations, ...) all need it. Still
    // spins up the GL context (texture/mesh loading needs it) but never enters the main loop.
    std::string resaveIn, resaveOut;
    // --smoke-test [dir]: an optional directory of .json scenes to load. Defaults to the
    // engine's own committed regression scenes (assets/test-scenes/), falling back to the
    // user project's scenes/ folder — so the harness still means something when the user
    // project is empty (audit TEST-101).
    std::string smokeScenesDirArg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--smoke-test") {
            smokeTestMode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') smokeScenesDirArg = argv[++i];
        }
        else if (a == "--resave" && i + 2 < argc) { resaveIn = argv[i + 1]; resaveOut = argv[i + 2]; i += 2; }
    }
    const bool resaveMode = !resaveIn.empty();
    // --smoke-test and --resave are non-interactive: no splash, and fatal errors go to stderr +
    // a nonzero exit instead of a modal MessageBox that a headless/CI desktop never dismisses
    // (audit BUG-102).
    const bool headless = smokeTestMode || resaveMode;

    // Resolve shipped engine assets (shaders, fonts, branding) relative to the executable, not
    // the working directory, so a launch from the repo root or an unrelated CWD still finds
    // them (audit #355 / BUG-102).
    EnginePaths::Init(argv[0]);

    try {
        // Up before anything else so it covers the whole startup, including the GL context
        // creation and shader compiles below. The main window stays hidden until its first
        // frame is presented (see Window::Show), so the two never overlap.
        SplashScreen splash;
        if (!headless)
            splash.Show(EnginePaths::Resolve("assets/branding/splash.png"), 1.0f);

        Window window(1280, 720, "Tartarus Engine");
        // GL context + loader are live now. No-op unless a Debug build or TARTARUS_GL_DEBUG=1.
        // The smoke test's whole job is catching GL-level regressions, so force debug output on
        // for it regardless of build config / env (audit #356) — otherwise newGlErrors is
        // structurally always 0 in a Release run and the harness only checks "did it draw".
        if (smokeTestMode) GLDebug::ForceEnable();
        GLDebug::Init();
        Input::Init(window.Handle());
        window.SetCursorLocked(true);
        window.Maximize(); // opens maximized (not true fullscreen, no monitor video-mode switch); F11 still enters fullscreen

        AudioEngine::Init();

        ShaderLibrary::Init(EnginePaths::Resolve("assets/shaders"));

        // ReadFileRequired (not ReadFile) for the core programs: a missing shader directory
        // (wrong CWD, incomplete install) throws "Required engine shader not found: <path>" at
        // startup instead of letting an empty source reach the driver as a misleading link
        // error (audit #355 / BUG-102).
        // The `debugName` third arg labels the GL program (KHR_debug) and logs its id at link
        // time — see Shader.h (audit GL-102 / #367).
        Shader modelShader(ShaderLibrary::ReadFileRequired("ModelVertex.glsl"),
                           ShaderLibrary::ReadFileRequired("ModelFragment.glsl"), "modelShader");
        Shader outlineModelShader(ShaderLibrary::ReadFileRequired("OutlineModel.vert.glsl"),
                                  ShaderLibrary::ReadFileRequired("Outline.frag.glsl"), "outlineModelShader");
        Shader outlineDilateShader(ShaderLibrary::ReadFileRequired("OutlineDilate.vert.glsl"),
                                   ShaderLibrary::ReadFileRequired("OutlineDilate.frag.glsl"), "outlineDilateShader");
        Shader shadowShader(ShaderLibrary::ReadFileRequired("ShadowDepth.vert.glsl"),
                            ShaderLibrary::ReadFileRequired("ShadowDepth.frag.glsl"), "shadowShader");
        Shader localShadowShader(ShaderLibrary::ReadFileRequired("ShadowDepth.vert.glsl"),
                                 ShaderLibrary::ReadFileRequired("ShadowDepthLocal.frag.glsl"), "localShadowShader"); // spot/point: linear depth
        Shader clusterBuildShader(ShaderLibrary::ReadFileRequired("ClusterBuild.comp.glsl"), "clusterBuildShader"); // #120: per-view froxel AABBs
        Shader clusterCullShader(ShaderLibrary::ReadFileRequired("ClusterCull.comp.glsl"), "clusterCullShader");   // #120: point/spot lights -> froxel lists
        unsigned int fsQuadVao = 0;
        glGenVertexArrays(1, &fsQuadVao); // attribute-less: positions come from gl_VertexID
        TintOverlayRenderer tintOverlay;
        Grid grid;
        ColliderGizmo colliderGizmo; // #185 PR 2
        Sky sky;
        IblProbe iblProbe; // #196: sky-baked irradiance / prefiltered specular / BRDF LUT
        // PR13: HDRI state — non-null when an HDRI is loaded, path tracks the loaded file
        std::shared_ptr<Cubemap> hdriCube;
        std::string               hdriCubePath;
        bool                      prevWasHdri = false;
        // PR14: reflection probe array — rebuilt from scene each frame, bound per drawScene
        ReflectionProbeArray probeArray;
        // PR15: SSAO — depth pre-pass FBO + compute + blur shaders (all scene-view only)
        Ssao ssao;
        Shader ssaoComputeShader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"),
                                 ShaderLibrary::ReadFile("Ssao.frag.glsl"));
        Shader ssaoBlurShader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"),
                              ShaderLibrary::ReadFile("SsaoBlur.frag.glsl"));
        // PR16: Bloom — soft-knee threshold + 5-level downsample/upsample mip pyramid.
        // Separate Scene/Game instances: both viewports can render in the same frame (Scene tab
        // + docked Game tab both visible) at different resolutions, so sharing one would thrash
        // Resize() every frame. The three shaders are stateless GLSL programs, safely shared.
        Bloom bloom;
        Bloom gameBloom;
        Shader bloomThreshShader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"),
                                 ShaderLibrary::ReadFile("BloomThreshold.frag.glsl"));
        Shader bloomDownsampleShader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"),
                                     ShaderLibrary::ReadFile("BloomDownsample.frag.glsl"));
        Shader bloomUpsampleShader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"),
                                   ShaderLibrary::ReadFile("BloomUpsample.frag.glsl"));
        // Same reasoning as gameBloom above — SSAO needs its own depth pre-pass FBO per viewport.
        Ssao gameSsao;

        World world;
        HotReloadGameModule gameModule;
        HotReloadEditorModule editorModule;
        {
            std::error_code ec;
            std::filesystem::path executable = std::filesystem::absolute(argv[0], ec);
            if (ec) executable = std::filesystem::current_path(ec) / "TartarusEngine.exe";
            // Filenames come from the build system (TARTARUS_GAME_MODULE_FILENAME /
            // TARTARUS_EDITOR_MODULE_FILENAME, see CMakeLists.txt) rather than being hardcoded
            // here — a Debug build's CMAKE_DEBUG_POSTFIX makes these "TartarusGamed.dll" /
            // "TartarusEditord.dll", not the plain names a Release build produces.
            gameModule.Initialize(executable.parent_path() / TARTARUS_GAME_MODULE_FILENAME);
            // The window handle is what native dialogs the module opens (the Console's "Save...")
            // are parented to.
            editorModule.Initialize(executable.parent_path() / TARTARUS_EDITOR_MODULE_FILENAME, window.Handle());
        }
        Player player;
        // Default spawn/editor-camera start: on the outdoor plaza, facing through the open
        // entrance toward the showroom's hero display and its walkable floor.
        player.Cam.Position = glm::vec3(0.0f, 2.0f, 11.0f);
        player.Cam.Yaw = -90.0f;  // faces -Z, toward the shape cluster at the origin
        player.Cam.Pitch = 5.0f;

        // Resolved under the project folder (see ProjectPaths.h) rather than the working
        // directory, so the scene being edited lives alongside the source instead of inside
        // build/, where it was gitignored and a clean rebuild would delete it. Prefer the scene
        // that was open when the editor last closed, if it still exists (#95).
        EditorSettings::Load();
        LayerRegistry::Load(); // LayerComponent slot names (#236 A1)
        ProjectSettings::Load(); // physics + tags (#236 A4); project/settings.json
        AssetDatabase::ScanProject(); // create .meta sidecars for existing assets (#333 PR 1)
        std::string scenePath = ProjectPaths::Resolve("scenes/Showcase.json");
        {
            const std::string& last = EditorSettings::Get().LastScenePath;
            std::error_code sceneEc;
            if (!last.empty() && std::filesystem::exists(last, sceneEc) && !sceneEc)
                scenePath = last;
        }
        AssetLibrary assets;

        if (resaveMode) {
            if (!SceneSerializer::Load(world, assets, resaveIn)) {
                std::cerr << "[Resave] FAILED to load '" << resaveIn << "'\n";
                return 2;
            }
            if (!SceneSerializer::Save(world, assets, resaveOut)) {
                std::cerr << "[Resave] FAILED to save '" << resaveOut << "'\n";
                return 3;
            }
            std::cout << "[Resave] " << resaveIn << " -> " << resaveOut << "\n";
            return 0;
        }

        bool sceneLoaded = SceneSerializer::Load(world, assets, scenePath);
        if (sceneLoaded) {
            std::cout << "Loaded scene from " << scenePath << std::endl;
        }

        // #226: TextureCache never evicted anything on its own, so a texture deleted from the
        // project (or reimported under different settings) left its old decoded-pixel entry on
        // disk forever — 229MB across 26 entries observed on a real dev machine. Sweep now, after
        // the scene above has loaded its textures and their customized import settings, so the
        // sweep knows the *current* settings for anything actually customized. An entry for a
        // texture this scene didn't touch is left alone rather than judged by stale information.
        {
            const auto& textureSettings = assets.TextureSettingsMap();
            TextureCache::Prune([&textureSettings](const std::string& sourcePath) -> std::optional<uint64_t> {
                auto it = textureSettings.find(sourcePath);
                if (it == textureSettings.end()) return std::nullopt;
                return TextureCache::HashSettings(it->second);
            });
        }

        EditorLayer editor;
        editor.Init(window.Handle());
        AudioEngine::SetMuted(EditorSettings::Get().AudioMuted); // #236 R2 — restore the View ▸ Mute Audio choice

        // One-shot rig dump: OS, CPU, RAM, GPU, driver, display, build. Collected into a block
        // for Preferences > About — no longer spammed line-by-line to the Console (it lives in
        // About now).
        std::vector<std::string> sysReport;
        {
            auto report = [&](const std::string& s) { sysReport.push_back(s); };
            constexpr GLenum kGL_VENDOR = 0x1F00, kGL_RENDERER = 0x1F01, kGL_VERSION = 0x1F02,
                             kGL_GLSL_VERSION = 0x8B8C, kGL_MAX_TEXTURE_SIZE = 0x0D33,
                             kGL_MAX_SAMPLES = 0x8D57,
                             kGL_GPU_MEM_TOTAL_NVX = 0x9048, kGL_GPU_MEM_AVAIL_NVX = 0x9049;
            auto glStr = [](GLenum e) {
                const GLubyte* s = glGetString(e);
                return s ? std::string(reinterpret_cast<const char*>(s)) : std::string("(unknown)");
            };
            auto glInt = [](GLenum e) { GLint v = 0; glGetIntegerv(e, &v); return v; };

            // --- CPU: brand string from CPUID leaves 0x80000002..4 (16 bytes each), space-padded.
            auto cpuName = []() -> std::string {
                int regs[4] = {0};
                __cpuid(regs, 0x80000000);
                if (static_cast<unsigned>(regs[0]) < 0x80000004u) return "(unknown CPU)";
                char brand[49] = {0};
                for (unsigned leaf = 0; leaf < 3; ++leaf) {
                    __cpuid(regs, 0x80000002 + leaf);
                    std::memcpy(brand + leaf * 16, regs, 16);
                }
                std::string s(brand);
                size_t a = s.find_first_not_of(' ');
                size_t b = s.find_last_not_of(' ');
                return (a == std::string::npos) ? "(unknown CPU)" : s.substr(a, b - a + 1);
            };
            // Physical core count (distinct from logical/HW-thread count).
            auto physicalCores = []() -> unsigned {
                DWORD len = 0;
                GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
                if (!len) return 0;
                std::vector<char> buf(len);
                if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
                    return 0;
                unsigned n = 0;
                for (char* p = buf.data(); p < buf.data() + len; ) {
                    auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(p);
                    if (info->Relationship == RelationProcessorCore) ++n;
                    p += info->Size;
                }
                return n;
            };
            // --- Windows edition/version from the registry (GetVersionEx lies without a manifest).
            auto regStr = [](const char* value) -> std::string {
                char b[256]; DWORD sz = sizeof(b);
                if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                        value, RRF_RT_REG_SZ, nullptr, b, &sz) == ERROR_SUCCESS)
                    return std::string(b);
                return {};
            };
            auto regDword = [](const char* value) -> DWORD {
                DWORD v = 0, sz = sizeof(v);
                RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                    value, RRF_RT_REG_DWORD, nullptr, &v, &sz);
                return v;
            };

            int fbw = 0, fbh = 0;
            glfwGetFramebufferSize(window.Handle(), &fbw, &fbh);

            unsigned threads = std::thread::hardware_concurrency();
            unsigned pcores = physicalCores();

            MEMORYSTATUSEX mem{}; mem.dwLength = sizeof(mem); GlobalMemoryStatusEx(&mem);
            double ramTotGiB = mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
            double ramAvailGiB = mem.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);

            auto gib = [](double v) {
                char b[32]; snprintf(b, sizeof(b), "%.1f", v); return std::string(b);
            };

            // Windows line
            {
                std::string prod = regStr("ProductName");                 // e.g. "Windows 10 Pro"
                std::string disp = regStr("DisplayVersion");              // e.g. "23H2"
                std::string build = regStr("CurrentBuildNumber");
                DWORD ubr = regDword("UBR");
                // Registry still says "Windows 10 ..." on 11; correct it from the build number.
                long bn = build.empty() ? 0 : std::atol(build.c_str());
                if (bn >= 22000 && prod.rfind("Windows 10", 0) == 0) prod.replace(0, 10, "Windows 11");
                std::string line = "OS: " + (prod.empty() ? "Windows" : prod);
                if (!disp.empty())  line += " " + disp;
                if (!build.empty()) line += "  (build " + build + (ubr ? "." + std::to_string(ubr) : "") + ")";
                report(line);
            }

            report("Tartarus Engine - editor up.");
            Log::Info("Tartarus Engine ready. System details: Preferences > About.");

            {
                std::string line = "CPU: " + cpuName() + "  (";
                if (pcores) line += std::to_string(pcores) + " cores / ";
                line += (threads ? std::to_string(threads) : std::string("?")) + " threads)";
                report(line);
            }
            report("RAM: " + gib(ramTotGiB) + " GiB total  (" + gib(ramAvailGiB) + " GiB free)");

            report("GPU: " + glStr(kGL_RENDERER) + "  (" + glStr(kGL_VENDOR) + ")");
            // VRAM via GL_NVX_gpu_memory_info (NVIDIA). This build's GL loader has no
            // glGetStringi to enumerate a core-profile extension list, so just probe the enum:
            // on a driver without the extension glGetIntegerv leaves the value at 0.
            {
                GLint totKiB = 0, availKiB = 0;
                glGetIntegerv(kGL_GPU_MEM_TOTAL_NVX, &totKiB);
                glGetIntegerv(kGL_GPU_MEM_AVAIL_NVX, &availKiB);
                if (totKiB > 0) {
                    char b[96];
                    snprintf(b, sizeof(b), "VRAM: %.0f MiB total  (%.0f MiB free)",
                             totKiB / 1024.0, availKiB / 1024.0);
                    report(b);
                }
            }
            report("OpenGL " + glStr(kGL_VERSION) + "  |  GLSL " + glStr(kGL_GLSL_VERSION));
            report("GL limits: max texture " + std::to_string(glInt(kGL_MAX_TEXTURE_SIZE)) +
                      " px, max MSAA " + std::to_string(glInt(kGL_MAX_SAMPLES)) + "x");

            // Display line — primary monitor mode.
            if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
                if (const GLFWvidmode* vm = glfwGetVideoMode(mon)) {
                    const char* mname = glfwGetMonitorName(mon);
                    char b[160];
                    snprintf(b, sizeof(b), "Display: %s  %d x %d @ %d Hz",
                             mname ? mname : "primary", vm->width, vm->height, vm->refreshRate);
                    report(b);
                }
            }
            report("Framebuffer: " + std::to_string(fbw) + " x " + std::to_string(fbh));

            // Build line
            {
#if defined(NDEBUG)
                const char* cfg = "Release";
#else
                const char* cfg = "Debug";
#endif
                char b[128];
                snprintf(b, sizeof(b), "Build: %s x64, MSVC %d, %s", cfg, (int)_MSC_VER, __DATE__);
                report(b);
            }
            {
                std::size_t objs = 0;
                world.Registry.view<TransformComponent>().each([&](auto...) { ++objs; });
                if (sceneLoaded)
                    Log::Info("Scene loaded from " + scenePath + " - " + std::to_string(objs) +
                              (objs == 1 ? " object." : " objects."));
                else
                    Log::Info("No scene file - started empty.");
            }
        }
        editor.SetSystemReport(sysReport); // shown in Preferences > About

        GameViewPanel gameView;
        gameView.LoadSettings();
        // The editor's own "Scene" tab renders into this rather than straight into the
        // backbuffer — see the "Scene tab offscreen pass" comment below for why.
        Framebuffer sceneFramebuffer;   // LDR: tonemap output the Scene tab shows via ImGui::Image
        Framebuffer selectionMaskFbo; // 1-bit silhouette mask for the screen-space selection outline

        // The lighting overhaul renders the scene + editor overlays into these linear RGBA16F
        // MSAA targets, then a shared Tonemapper pass resolves + maps them into the LDR
        // framebuffers above (and into FBO 0 for free-aspect maximized play).
        HdrTarget sceneHdr;
        HdrTarget gameHdr;
        OpaqueColorCopy sceneOpaqueColor;  // PR12: opaque color capture for scene-view refraction
        OpaqueColorCopy gameOpaqueColor;   // PR12: opaque color capture for game-view refraction
        Tonemapper tonemapper;
        LightBuffer lightBuffer; // scene lights -> std430 SSBO the model shader reads at binding 0
        ClusterGrid clusterGrid; // #120: froxel light lists, rebuilt per rendered view each frame
        CascadedShadowMap shadowMap; // directional-sun CSM; depth array sampled by the model shader
        SpotShadowMap spotShadowMap; // perspective depth per shadow-casting spot light (#119)
        PointShadowMap pointShadowMap; // depth cube per shadow-casting point light (#119)
        SceneRenderer sceneRenderer;  // audit #359 pass 2 — the scene-draw pass, formerly a lambda here

        Camera editorCamera;
        // Play is no longer a whole-screen mode swap. Three independent bits describe the state:
        //   editorUIVisible - the editor panels + dockspace are shown (true whenever NOT maximized)
        //   playing         - the scene is simulating; the Game panel renders from the player camera
        //   playMaximized   - the Game view fills the window and the editor panels are hidden
        // Pressing Play just flips `playing`; the game runs inside the docked Game panel with the
        // editor still fully usable. The Fullscreen button next to Stop toggles `playMaximized`.
        bool editorUIVisible = true;
        bool playing = false;
        bool paused = false;   // #236: simulation frozen, rendering continues; Step advances one frame
        bool playMaximized = false;
        // In-panel play is click-to-focus: the game only reads mouse/keyboard once the user has
        // clicked inside the Game view. Esc (or Stop) hands control back to the editor. Ignored
        // while maximized — that path uses the cursor-lock state directly, like the old Play mode.
        bool gameInputEngaged = false;
        editorCamera.Position = player.Cam.Position;
        editorCamera.Yaw = player.Cam.Yaw;
        editorCamera.Pitch = player.Cam.Pitch;
        editorCamera.Fov = EditorSettings::Get().SceneCameraFov; // #236 R2 — persisted editor camera
        editorCamera.NearPlane = EditorSettings::Get().SceneCameraNear;
        editorCamera.FarPlane = EditorSettings::Get().SceneCameraFar;
        // Deliberately NOT calling editor.FrameSceneBounds() here (audit #87's original fix for
        // "staring at empty space") - for this scene it re-frames to an exterior overview of the
        // whole building, outside every room's floor. Since Play copies its spawn straight from
        // wherever editorCamera currently is, that overview became the Play spawn point too - and
        // it's outside any walkable geometry, so the player free-fell forever. Starting inside
        // Room 2 instead means both the Scene view and an immediate Play are already somewhere
        // safe and populated.
        window.SetCursorLocked(false);

        // OS-level drag-and-drop (e.g. dragging a file in from Windows Explorer) — routes
        // through the same import logic as File > Import, filed into whichever Asset Browser
        // folder is currently open. `editorUIVisible` is captured by reference since GLFW invokes
        // this from inside PollEvents(), by which point the loop below may have flipped it.
        window.SetDropCallback([&](const std::vector<std::string>& paths) {
            editor.HandleDroppedFiles(world, assets, editorCamera, editorUIVisible, paths);
        });

        bool firstFramePresented = false; // gates the splash -> editor handoff at the loop's end

        int appliedVSyncMode = -1; // != any real mode, so the first iteration applies the saved pref
        bool camDragActive = false; // OS cursor disabled for the duration of a look/pan/orbit drag

        std::string lastScenePath;
        bool lastDirty = false;
        bool titleInitialized = false;

        // Unity's play-mode contract: entering Play snapshots the scene and leaving it restores
        // that snapshot, so gameplay (shot crates, moved objects) never silently becomes an
        // edit. EditorLayer owns the snapshot; main.cpp owns the state bits.
        int errPauseSeen = 0; // #236 A5 — error count baseline for "Error Pause", re-armed each Play
        auto startPlay = [&]() {
            if (playing) return;
            if (EditorModuleHost::ConsoleState().ClearOnPlay) Log::Clear(); // #236 A5
            editor.OnEnterPlayMode(world);
            errPauseSeen = Log::CountOf(LogLevel::Error); // ignore errors that predate this run
            playing = true;
            playMaximized = false;
            editorUIVisible = true;
            gameInputEngaged = false;
            gameView.OnPlayStateChanged(true);
            editor.RequestGameTabFocus(); // show what you just started
            // Drop the player in from wherever the editor camera is looking, so pressing Play
            // inspects the part of the level you were just working on. The editor camera is left
            // where it is — the Scene tab stays usable during play.
            player.Cam.Position = editorCamera.Position;
            player.Cam.Yaw = editorCamera.Yaw;
            player.Cam.Pitch = editorCamera.Pitch;
            player.Velocity = glm::vec3(0.0f);
            player.Gravity = ProjectSettings::Physics().Gravity.y; // #236 A4 — project-scoped
            window.SetCursorLocked(false); // click the Game view to take control
        };
        auto stopPlay = [&]() {
            if (!playing) return;
            editor.OnExitPlayMode(world, assets);
            playing = false;
            paused = false;
            playMaximized = false;
            editorUIVisible = true;
            gameInputEngaged = false;
            gameView.OnPlayStateChanged(false);
            editor.RequestSceneTabFocus();
            window.SetCursorLocked(false);
        };
        auto togglePlay = [&]() { if (playing) stopPlay(); else startPlay(); };
        // Fullscreen/Restore button next to Stop: maximize the Game view over the editor panels
        // (equivalent to the old whole-window Play look), or drop back to windowed in-panel play.
        auto setMaximized = [&](bool maximize) {
            if (!playing) return;
            playMaximized = maximize;
            editorUIVisible = !maximize;
            gameInputEngaged = false;
            // Maximized play captures the cursor immediately and lets Esc toggle it, exactly
            // like Play used to; restoring goes back to click-to-focus in the panel and keeps
            // the Game tab up so you're still looking at what you were just watching.
            window.SetCursorLocked(maximize);
            if (!maximize) editor.RequestGameTabFocus();
        };

        // On-exit "Save changes?" flow (audit #56). When the window-close request arrives with a
        // dirty, titled scene, swallow it and let the editor raise a modal; `exitApproved` is set
        // once the user has chosen Save or Don't Save so the next close request goes through.
        bool exitApproved = false;
        bool exitSkipFinalSave = false;

        // Monotonically increasing per-frame counter, used by Model::TickAnimationOnce() to
        // dedupe animation updates for models shared by more than one entity (#106) without a
        // per-frame heap allocation.
        uint64_t frameIndex = 0;

        // --smoke-test state (audit #187). Deliberately driven from inside the normal render
        // loop below rather than a separate loop of its own, so it exercises the exact same
        // per-frame path (light gather, shadow passes, drawScene, editor.Draw/EndFrame) as an
        // interactive session — the whole point of the harness is catching a regression that
        // path could introduce, not a hand-rolled approximation of it.
        constexpr int kSmokeTestFrames = 100;
        struct SmokeResult { std::string scenePath; int frames; int newGlErrors; int newLogErrors;
                             int drawCalls; bool loadOk; bool pass; std::string cause; };
        std::vector<std::string> smokeScenePaths;
        std::vector<SmokeResult> smokeResults;
        size_t smokeSceneIndex = 0;
        int smokeFramesRendered = 0;
        int smokeBaselineGlErrors = 0;
        int smokeBaselineLogErrors = 0;
        bool smokeSceneActive = false;
        bool smokeSceneLoadOk = false;
        if (smokeTestMode) {
            // Scene source: explicit --smoke-test <dir> arg, else the engine's committed
            // regression scenes, else the user project's scenes/ (audit TEST-101).
            std::string scenesDir;
            if (!smokeScenesDirArg.empty()) {
                scenesDir = smokeScenesDirArg;
            } else {
                std::string engineScenes = EnginePaths::Resolve("assets/test-scenes");
                std::error_code exEc;
                bool haveEngineScenes = std::filesystem::is_directory(engineScenes, exEc) && !exEc;
                scenesDir = haveEngineScenes ? engineScenes : ProjectPaths::Resolve("scenes");
            }
            std::error_code dirEc;
            for (auto& entry : std::filesystem::directory_iterator(scenesDir, dirEc)) {
                if (dirEc) break;
                if (entry.is_regular_file() && entry.path().extension() == ".json")
                    smokeScenePaths.push_back(entry.path().string());
            }
            std::sort(smokeScenePaths.begin(), smokeScenePaths.end());
            std::cout << "[SmokeTest] Found " << smokeScenePaths.size() << " scene(s) under "
                      << scenesDir << std::endl;
            if (smokeScenePaths.empty()) {
                std::cout << "[SmokeTest] ERROR: no scenes to test - failing." << std::endl;
            }
            if (!GLDebug::IsEnabled()) {
                // ForceEnable() above still could not wire up the callback (no KHR_debug on this
                // context, e.g. a GPU-less CI runner). GL error counts will read 0; the harness
                // still checks load success, log errors and draw count.
                std::cout << "[SmokeTest] WARNING: GL debug output could not be enabled on this "
                             "context - newGlErrors will read 0." << std::endl;
            }
        }

        while (true) {
            ++frameIndex;
            // Advance the smoke test: load the next scene (or, once every scene's frame quota is
            // met, fall through and stop the whole loop below).
            if (smokeTestMode) {
                if (smokeSceneIndex >= smokeScenePaths.size()) break;
                if (!smokeSceneActive) {
                    const std::string& path = smokeScenePaths[smokeSceneIndex];
                    // Baseline BEFORE the load, so an asset error logged *during* Load() (a
                    // missing model/texture) counts against this scene instead of being folded
                    // into the baseline and ignored (audit #356).
                    smokeBaselineGlErrors  = GLDebug::ErrorCount();
                    smokeBaselineLogErrors = Log::CountOf(LogLevel::Error);
                    smokeSceneLoadOk = SceneSerializer::Load(world, assets, path);
                    std::cout << "[SmokeTest] Loading " << path
                              << (smokeSceneLoadOk ? "" : "  (Load() reported failure)") << std::endl;
                    smokeFramesRendered = 0;
                    smokeSceneActive = true;
                }
            }
            Clock::Update();
            float dt = Clock::DeltaTime();
            Profiler::BeginFrame();
            GLStateCache::ResetFrameStats();

            // Frame-pacing preferences are live: changing VSync / FPS Limit in Preferences takes
            // effect on the very next frame. SetVSync only touches the driver on an actual change.
            if (EditorSettings::Get().VSyncMode != appliedVSyncMode) {
                appliedVSyncMode = EditorSettings::Get().VSyncMode;
                window.SetVSync(appliedVSyncMode);
            }

            window.PollEvents();
            Input::Update();

            if (window.ShouldClose()) {
                bool dirtyTitled = editor.IsDirty() && !editor.CurrentScenePath().empty();
                if (exitApproved || !dirtyTitled) break;
                window.SetShouldClose(false);          // veto this close; ask first
                if (!editor.ExitPromptActive()) editor.OpenExitPrompt();
            }

            // Rebuilding this (filesystem::path parse + string concat) is wasted work on the
            // ~99.9% of frames where neither input changed, so gate it on the inputs themselves
            // rather than on the composed string.
            const std::string& currentScenePath = editor.CurrentScenePath();
            bool dirty = editor.IsDirty();
            if (!titleInitialized || currentScenePath != lastScenePath || dirty != lastDirty) {
                std::string sceneName = std::filesystem::path(currentScenePath).filename().string();
                if (sceneName.empty()) sceneName = "Untitled"; // File > New Scene: no path yet
                std::string desiredTitle = "Tartarus Engine \xE2\x80\x94 " + sceneName +
                    (dirty ? "*" : "");
                window.SetTitle(desiredTitle);
                lastScenePath = currentScenePath;
                lastDirty = dirty;
                titleInitialized = true;
            }

            // Play / pause / step / maximize + window fullscreen are Ctx_App shortcuts now
            // (#236 F) — rebindable in Preferences ▸ Shortcuts, evaluated here via the GLFW
            // path since this runs before the ImGui frame.
            if (Shortcuts::TriggeredGlfw("play.toggle")) togglePlay();
            // Pause (F2 / toolbar) and single-frame Step (F3 / toolbar). The toolbar requests are
            // raised during the previous frame's editor draw; consuming them here folds them into
            // the same state the keys drive, one frame later.
            if (playing && (Shortcuts::TriggeredGlfw("play.pause") || editor.ConsumePauseToggleRequest()))
                paused = !paused;
            // #236 A5 — Error Pause: freeze the sim the frame a fresh error lands.
            if (playing && !paused && EditorModuleHost::ConsoleState().ErrorPause) {
                const int errNow = Log::CountOf(LogLevel::Error);
                if (errNow > errPauseSeen) {
                    paused = true;
                    Log::Info("Error Pause: simulation paused on a new error (Console \xE2\x96\xB8 Error Pause).");
                }
            }
            errPauseSeen = Log::CountOf(LogLevel::Error);
            bool stepThisFrame = playing && paused &&
                (Shortcuts::TriggeredGlfw("play.step") || editor.ConsumeStepRequest());
            // Mirrors the toolbar's Fullscreen/Restore button — maximize the Game view over
            // the editor panels (only meaningful while playing; setMaximized no-ops otherwise).
            if (playing && Shortcuts::TriggeredGlfw("play.maximize")) setMaximized(!playMaximized);

            // F6 toggles the Physics debug panel from anywhere — the Window menu that also does it
            // is hidden during maximized play (#185).
            if (Input::IsKeyPressed(GLFW_KEY_F6)) {
                EditorSettings::Get().ShowPhysicsPanel = !EditorSettings::Get().ShowPhysicsPanel;
                EditorSettings::Save();
            }
            // F5 toggles the physics debug rendering over the game view (works in maximized play,
            // where the Scene-viewport overlay isn't drawn). Turning it on with nothing selected
            // enables a sensible default set so there's immediately something to see (#185).
            // (F3 is Step One Frame; F6 is the Physics panel.)
            if (Input::IsKeyPressed(GLFW_KEY_F5)) {
                EditorSettings& es = EditorSettings::Get();
                es.PlayDebugOverlay = !es.PlayDebugOverlay;
                if (es.PlayDebugOverlay) {
                    es.ShowColliders = true;
                    if (es.PhysicsDebugDrawFlags == 0u)
                        es.PhysicsDebugDrawFlags = PhysicsWorld::PDD_Contacts |
                                                   PhysicsWorld::PDD_Raycasts |
                                                   PhysicsWorld::PDD_Velocity;
                }
                EditorSettings::Save();
            }

            if (Shortcuts::TriggeredGlfw("window.fullscreen")) {
                window.ToggleFullscreen();
            }

            if (playing) {
                if (Input::IsKeyPressed(GLFW_KEY_ESCAPE)) {
                    if (playMaximized) {
                        // Maximized play: Esc toggles the cursor, same as the old Play mode.
                        window.SetCursorLocked(!window.IsCursorLocked());
                    } else if (gameInputEngaged) {
                        // In-panel play: Esc hands control back to the editor.
                        gameInputEngaged = false;
                        window.SetCursorLocked(false);
                    }
                }

                // Safety net: if the window loses focus while the game has grabbed the cursor
                // (alt-tab, a notification steals focus), release it — otherwise you can come
                // back to a captured view with a hidden cursor and no obvious way out.
                if (gameInputEngaged && !glfwGetWindowAttrib(window.Handle(), GLFW_FOCUSED)) {
                    gameInputEngaged = false;
                    window.SetCursorLocked(false);
                }
            }

            // While the running game owns the mouse (cursor locked to it), stop ImGui from
            // tracking pointer movement — GLFW still streams virtual mouse deltas with the cursor
            // disabled, so without this the editor panels behind the Game view keep highlighting
            // rows and firing tooltips as you mouse-look. Cleared the instant input is released
            // (Esc / Stop / focus loss), so it never leaves the UI dead.
            {
                ImGuiIO& imguiIO = ImGui::GetIO();
                const bool gameOwnsMouse =
                    playing && (playMaximized ? window.IsCursorLocked() : gameInputEngaged);
                if (gameOwnsMouse) imguiIO.ConfigFlags |= ImGuiConfigFlags_NoMouse;
                else               imguiIO.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
            }

            editor.BeginFrame();

            // Cleared once, up front, regardless of mode — Scene/Game now render into their own
            // offscreen framebuffers rather than straight into this one, but ImGui panels still
            // composite on top of whatever's already here, and leaving stale pixels from a prior
            // frame in any gap between panels would show as flicker/ghosting.
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, window.GetWidth(), window.GetHeight());
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // The camera the Game view (and maximized play) renders from: the player while
            // playing, otherwise the editor's own free camera (so the Game panel still previews
            // something sensible while editing).
            Camera* gameCam = playing ? &player.Cam : &editorCamera;

            // "Does the running game own the mouse/keyboard this frame?" — the cursor-lock state
            // while maximized, the click-to-focus latch while in a panel.
            bool gameHasInput = playing && (playMaximized ? window.IsCursorLocked() : gameInputEngaged);

            // The editor camera keeps updating even during in-panel play (the Scene tab stays
            // usable) — but not while the game has grabbed the mouse, or that drag would move
            // both cameras at once.
            if (editorUIVisible) {
                glm::vec3 selectionCenter;
                bool hasSelection = editor.GetSelectionCenter(world, selectionCenter);
                bool allowLook = !editor.WantsCaptureMouse() && !editor.GizmoEngaged() && !gameHasInput;

                // Infinite drag: while a look (RMB), pan (MMB) or orbit (Alt+LMB) drag is held,
                // disable the OS cursor so mouse motion is delivered as unbounded deltas instead
                // of dead-ending at the monitor edge. Latched on the button press so a transient
                // ImGui mouse-capture flip mid-drag can't drop the lock; released once every drag
                // button is up. Input::Update() already swallows the position jump on the mode
                // switch, and Window::SetCursorLocked restores the pointer on release.
                bool altHeld = Input::IsKeyDown(GLFW_KEY_LEFT_ALT) || Input::IsKeyDown(GLFW_KEY_RIGHT_ALT);
                bool dragBtnHeld = Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)
                                || Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_MIDDLE)
                                || (altHeld && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT));
                if (!camDragActive && dragBtnHeld && allowLook) {
                    camDragActive = true;
                    window.SetCursorLocked(true);
                } else if (camDragActive && !dragBtnHeld) {
                    camDragActive = false;
                    window.SetCursorLocked(false);
                }

                if (UpdateEditorCamera(editorCamera, dt, allowLook || camDragActive,
                        hasSelection ? &selectionCenter : nullptr))
                    editor.FlashFlySpeedHud(); // #236 R2 — show the transient "Fly speed: N" readout
            } else if (camDragActive) {
                camDragActive = false; // dropped into maximized play mid-drag; it owns the cursor now
            }

            // Simulate the player whenever playing. When the game doesn't have input (in-panel
            // play, not yet clicked in), the body still falls/rests — it just doesn't walk or
            // look (see Player::Update's readInput).
            // Reap voices that have finished so repeated Play/Stop cycles don't accumulate
            // ma_sound objects and open file handles (#200).
            AudioEngine::Update();

            // Whether gameplay actually advances this frame: playing and not paused, or a
            // single-frame Step was requested while paused (#236). Everything else about play mode
            // — the Game panel, the player camera, input routing — stays live while paused, so a
            // frozen frame is still fully inspectable.
            const bool simThisFrame = (playing && !paused) || stepThisFrame;

            if (simThisFrame) {
                // Physics harness (#185): exercises the gameplay force + query API from a real
                // caller until gameplay code does. G = shockwave at the player. Right-click =
                // Half-Life-2 gravity gun: grab the dynamic body under the crosshair and carry
                // it in front of the eye; scroll while holding pulls it nearer / pushes it out;
                // left-click launches it. With nothing held, left-click shoves whatever it hits.
                // Toggle: Gizmos > Physics debug input.
                if (gameHasInput && EditorSettings::Get().PhysicsDebugInput) {
                    if (Input::IsKeyPressed(GLFW_KEY_G)) {
                        const float c[3] = {player.Cam.Position.x,
                                            player.Cam.Position.y - player.EyeHeight,
                                            player.Cam.Position.z};
                        PhysicsWorld::AddExplosionForce(c, 7.0f, 22.0f, 0.4f);
                    }

                    const glm::vec3 eye = player.Cam.Position, fwd = player.Cam.Front();
                    const float of[3] = {eye.x, eye.y, eye.z}, df[3] = {fwd.x, fwd.y, fwd.z};
                    static bool s_lmbPrev = false, s_rmbPrev = false;
                    static float s_holdDist = 3.0f;
                    const bool lmb = Input::IsMouseButtonDown(0);
                    const bool rmb = Input::IsMouseButtonDown(1);

                    if (rmb && !s_rmbPrev && !PhysicsWorld::IsGrabbing()) {
                        RaycastHit hit;
                        BodyState bs;
                        if (PhysicsWorld::Raycast(of, df, 100.0f, hit) && hit.Entity != 0xFFFFFFFFu &&
                            PhysicsWorld::GetBodyState(hit.Entity, bs) && !bs.Kinematic) {
                            PhysicsWorld::GrabBody(hit.Entity);
                            s_holdDist = glm::clamp(hit.Distance, 1.5f, 8.0f);
                        }
                    }

                    if (PhysicsWorld::IsGrabbing()) {
                        const double scroll = Input::GetScrollDeltaY();
                        if (scroll != 0.0)
                            s_holdDist = glm::clamp(s_holdDist + (float)scroll * 0.6f, 1.5f, 8.0f);
                        const glm::vec3 t = eye + fwd * s_holdDist;
                        const float tf[3] = {t.x, t.y, t.z};
                        PhysicsWorld::UpdateGrab(tf);

                        if (lmb && !s_lmbPrev) {
                            const glm::vec3 imp = fwd * 18.0f;
                            const float impf[3] = {imp.x, imp.y, imp.z};
                            PhysicsWorld::ReleaseBody(/*launch=*/true, impf);
                        } else if (!rmb && s_rmbPrev) {
                            const float zero[3] = {0.0f, 0.0f, 0.0f};
                            PhysicsWorld::ReleaseBody(/*launch=*/false, zero);
                        }
                    } else if (lmb && !s_lmbPrev) {
                        RaycastHit hit;
                        if (PhysicsWorld::Raycast(of, df, 100.0f, hit) && hit.Entity != 0xFFFFFFFFu) {
                            const float f[3] = {fwd.x * 6.0f, fwd.y * 6.0f + 2.0f, fwd.z * 6.0f};
                            PhysicsWorld::AddForceAtPosition(hit.Entity, f, hit.Point, /*Impulse*/1u);
                        }
                    }

                    s_lmbPrev = lmb;
                    s_rmbPrev = rmb;
                }
                // Push the editor's physics-debug choices into the sim (#185): draw channels +
                // slow-mo scale. Query recording auto-follows the Raycasts channel.
                {
                    const EditorSettings& es = EditorSettings::Get();
                    PhysicsWorld::SetDebugDrawFlags(es.PhysicsDebugDrawFlags);
                    PhysicsWorld::SetSimTimeScale(es.PhysicsSimTimeScale);
                    PhysicsWorld::SetQueryRecording((es.PhysicsDebugDrawFlags & PhysicsWorld::PDD_Raycasts) != 0u);
                }
                // Step the PhysX world (#185): kinematic bodies pushed from their Transforms,
                // then the sim, then dynamic bodies' poses written back into theirs.
                PhysicsWorld::Step(dt, world);
                player.Update(dt, world, window.Handle(), gameHasInput);
                // The Play-mode camera is the ears: positional sources (#201) attenuate and pan
                // against wherever the player is looking from, updated after the move so the
                // listener matches the frame that's about to be rendered.
                AudioEngine::SetListener(player.Cam.Position, player.Cam.Front(), player.Cam.Up());
                // Procedural spin/orbit/bob/light-hue. Play-only: edit mode keeps the authored
                // pose, and the play-mode snapshot restores everything this touched on Stop.
                UpdateAnimators(world, dt);
            }

            // The gameplay DLL watches its freshly-built source copy even while editing, and
            // only runs game systems during Play. Rebuilding TartarusGame swaps the module
            // without closing the editor or discarding this World.
            gameModule.Tick(world, dt, simThisFrame);

            // Animations advance whenever something is showing them: the editor viewport, or the
            // running game.
            if (editorUIVisible || playing) {
                PROFILE_SCOPE("Animation Update");
                // Advance each distinct Model once. Scene entities get their own instance
                // (AssetLibrary::InstantiateModel), so this is normally 1:1 — but dedupe
                // defensively so a future shared-Model path can't tick one player N*dt (#106).
                // Model::TickAnimationOnce() compares against its own m_LastTickedFrame instead
                // of this loop building a heap-allocated std::unordered_set<Model*> every frame.
                for (auto entity : world.Registry.view<RenderableComponent>()) {
                    Model* m = world.Registry.get<RenderableComponent>(entity).ModelRef.get();
                    if (m) m->TickAnimationOnce(frameIndex, dt);
                }
            }

            if (!editorUIVisible) {
                // Maximized play: editor.Draw() (and the real DockSpace() call inside it) doesn't
                // run, so without SOME per-frame DockSpace() call ImGui silently undocks every
                // window in that tree the instant it's submitted again on Restore/Stop. See
                // KeepDockspaceAlive's own comment for the full story.
                editor.KeepDockspaceAlive();
            }

            // Every world matrix this frame's render passes need, computed once, top-down
            // (#173). Placed here deliberately: player/animator/animation updates above have
            // finished writing transforms, and the shadow + scene passes below (which each used
            // to re-derive the same entity's parent chain per cascade, per spot, per cube face
            // and per viewport) now just read it back. Editor edits — gizmo drags, Inspector
            // fields — land in editor.Draw() further down and so take effect on the NEXT frame's
            // rebuild. That is a small behavior change for the Game-view passes, which run after
            // editor.Draw() and so used to see a mid-frame edit that the Scene pass above them
            // did not: both views now agree on one snapshot instead of tearing between them.
            // Entities SPAWNED after this point still resolve correctly — GetCachedWorldTransform
            // falls back to composing on demand for anything the cache doesn't hold.
            world.RebuildWorldTransformCache();

            glm::vec3 lightDir(-0.4f, -1.0f, -0.3f);

            // --- Sun shadow map: built ONCE per frame, from the primary view -------------------
            // This used to live inside drawScene, i.e. it ran once per viewport (Scene + Game)
            // every frame — 8 cascade renders of the entire scene when the sun and the geometry
            // are identical between the two views. Now the 4 depth slices render once, here, and
            // every drawScene() below only samples them.
            const EditorSettings& frameSettings = EditorSettings::Get();

            // Build the light SSBO ONCE per frame — it used to be rebuilt inside every
            // drawScene(), i.e. per viewport, though the light set is identical across the
            // Scene/Game views of one frame (#122). This same walk captures the first active
            // directional light's aim + disc size for the shadow pass below.
            glm::vec3 frameSunDir = lightDir;
            float frameSunAngularDeg = 0.53f; // Earth's sun; drives the penumbra width
            lightBuffer.Clear();
            bool frameHaveDirectional = false;
            bool frameSunCastShadows = false; // the active directional's per-light Shadow.Enabled
            glm::mat4 spotShadowVP[SpotShadowMap::kMaxSpots];
            glm::vec3 spotShadowPos[SpotShadowMap::kMaxSpots];
            float spotShadowFar[SpotShadowMap::kMaxSpots];
            float spotShadowHalfTan[SpotShadowMap::kMaxSpots]; // tan(half-FOV) of the map — sizes the shader's texel estimate to the real cone, not an assumed 90° (#134)
            int spotShadowCount = 0; // shadow-casting spots that got a slot this frame (#119)
            glm::vec3 pointShadowPos[PointShadowMap::kMaxPoints];
            float pointShadowFar[PointShadowMap::kMaxPoints];
            int pointShadowCount = 0; // shadow-casting point lights that got a cube this frame (#119)
            // Per-light shadow multipliers (#140 phase 2), parallel to the slot arrays above.
            // All 1.0 unless the light's ShadowSettings override them -> byte-identical to before.
            float spotShadowBias[SpotShadowMap::kMaxSpots];
            float spotShadowNormalBias[SpotShadowMap::kMaxSpots];
            float spotShadowSoftness[SpotShadowMap::kMaxSpots];
            float spotShadowNear[SpotShadowMap::kMaxSpots];
            float pointShadowBias[PointShadowMap::kMaxPoints];
            float pointShadowNormalBias[PointShadowMap::kMaxPoints];
            float pointShadowNear[PointShadowMap::kMaxPoints];
            float frameSunShadowBias = 1.0f, frameSunShadowNormalBias = 1.0f, frameSunShadowSoftness = 1.0f;
            for (auto e : world.Registry.view<TransformComponent, LightComponent>()) {
                if (lightBuffer.Count() >= LightBuffer::kMaxLights) { lightBuffer.MarkOverflowed(); break; }
                if (world.Registry.all_of<InactiveTag>(e)) continue;
                // Lights panel solo/mute is an editing aid only — Play renders every light (#140).
                if (!playing && editor.IsLightSuppressed(e)) continue;
                const auto& lc = world.Registry.get<LightComponent>(e);
                glm::mat4 m = world.ComposeWorldTransform(e);
                glm::vec3 pos = glm::vec3(m[3]);
                glm::vec3 aim = glm::normalize(glm::vec3(m * glm::vec4(0, 0, -1, 0)));
                if (lc.Kind == LightComponent::Type::Directional) {
                    lightBuffer.AddDirectional(aim, lc.Color, lc.Intensity);
                    // A zero-intensity sun contributes no light, so it must not drive the
                    // cascaded shadow pass either — it's the way a scene opts out of having a
                    // directional at all while still suppressing SceneSerializer's synthesised
                    // fallback sun (which keys off a Directional existing, not its intensity).
                    if (lc.Intensity > 0.0f) {
                        if (!frameHaveDirectional) {
                            frameSunDir = aim;
                            frameSunAngularDeg = lc.AngularSizeDegrees;
                            frameSunCastShadows = lc.Shadow.Enabled;
                            frameSunShadowBias = lc.Shadow.Bias;
                            frameSunShadowNormalBias = lc.Shadow.NormalBias;
                            frameSunShadowSoftness = lc.Shadow.Softness;
                        }
                        frameHaveDirectional = true;
                    }
                } else if (lc.Kind == LightComponent::Type::Spot) {
                    float cosOuter = cosf(glm::radians(lc.SpotAngleDegrees));
                    float cosInner = cosf(glm::radians(lc.SpotAngleDegrees * 0.9f));
                    int slot = -1;
                    if (lc.Shadow.Enabled && frameSettings.ShadowsEnabled &&
                        spotShadowCount < SpotShadowMap::kMaxSpots) {
                        slot = spotShadowCount++;
                        glm::vec3 up = std::abs(aim.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
                        float fov = glm::radians(std::min(lc.SpotAngleDegrees * 2.0f + 4.0f, 175.0f));
                        float nearP = std::max(lc.Shadow.NearPlane, 1e-3f);
                        spotShadowFar[slot] = std::max(lc.Range, 0.2f);
                        spotShadowPos[slot] = pos;
                        spotShadowHalfTan[slot] = std::tan(0.5f * fov);
                        spotShadowNear[slot] = nearP;
                        spotShadowBias[slot] = lc.Shadow.Bias;
                        spotShadowNormalBias[slot] = lc.Shadow.NormalBias;
                        spotShadowSoftness[slot] = lc.Shadow.Softness;
                        spotShadowVP[slot] = glm::perspective(fov, 1.0f, nearP, spotShadowFar[slot]) *
                                             glm::lookAt(pos, pos + aim, up);
                    }
                    lightBuffer.AddSpot(pos, aim, lc.Color, lc.Intensity, lc.Range, cosOuter, cosInner, slot);
                } else {
                    int slot = -1;
                    if (lc.Shadow.Enabled && frameSettings.ShadowsEnabled &&
                        pointShadowCount < PointShadowMap::kMaxPoints) {
                        slot = pointShadowCount++;
                        pointShadowPos[slot] = pos;
                        pointShadowFar[slot] = std::max(lc.Range, 0.2f);
                        pointShadowNear[slot] = std::max(lc.Shadow.NearPlane, 1e-3f);
                        pointShadowBias[slot] = lc.Shadow.Bias;
                        pointShadowNormalBias[slot] = lc.Shadow.NormalBias;
                    }
                    lightBuffer.AddPoint(pos, lc.Color, lc.Intensity, lc.Range, slot);
                }
            }
            // No fallback light: a scene with no lights is intentionally unlit, so deleting a
            // directional light persists and fresh scenes remain black until explicitly lit.
            lightBuffer.Upload();
            const int frameLightCount = lightBuffer.Count();

            bool sunShadowsReady = false;
            if (frameSettings.ShadowsEnabled && frameHaveDirectional && frameSunCastShadows) {
                PROFILE_SCOPE("Sun Shadow Pass");
                PROFILE_GPU_SCOPE("Sun Shadow Pass");

                // Fit the cascades to whichever camera drives this frame's main view: the game
                // camera while playing, else the editor free-cam (the Scene tab is the working
                // view). The other viewport samples the same cascades — a looser texel fit there
                // is invisible next to rebuilding the whole map a second time.
                Camera& fitCam = playing ? *gameCam : editorCamera;
                glm::vec2 fitRegion = editor.GetLastSceneContentRegion();
                if (fitRegion.x < 1.0f || fitRegion.y < 1.0f)
                    fitRegion = { (float)window.GetWidth(), (float)window.GetHeight() };
                glm::mat4 fitView = fitCam.ViewMatrix();
                glm::mat4 fitProj = fitCam.ProjectionMatrix(fitRegion.x / fitRegion.y);

                shadowMap.Configure(frameSettings.ShadowResolution, frameSettings.ShadowCascades);
                shadowMap.Update(fitView, fitProj, frameSunDir, frameSettings.ShadowDistance);

                glEnable(GL_DEPTH_TEST);
                glDepthMask(GL_TRUE);
                // Store the LIGHT-FACING surface (back-face cull), matching the spot/point passes.
                // Front-face culling stored a caster's far side, which under any convex/curved
                // occluder (sphere worst) sits far enough behind the contact that the shadow
                // detached from the object's base at grazing sun angles — the #134 sun-bay gap.
                // SunShadow()'s bias is texel-proportional and now carries the acne protection;
                // this polygon offset is just a hair of extra slack for flat lit receivers.
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(1.0f, 2.0f);

                shadowShader.Bind();
                auto casters = world.Registry.view<TransformComponent, RenderableComponent>();
                // #194: resolve these two hot uniform locations once, outside the 6-face(ish)
                // x N-caster loop below, instead of hashing "uLightViewProj"/"uModel" every call.
                int shadowLightViewProjLoc = shadowShader.Loc("uLightViewProj");
                int shadowModelLoc = shadowShader.Loc("uModel");
                for (int c = 0; c < shadowMap.Count(); ++c) {
                    shadowMap.Begin(c);
                    shadowShader.SetMat4(shadowLightViewProjLoc, shadowMap.LightViewProj(c));
                    // Cull each cascade's caster list against that cascade's own ortho frustum —
                    // the near slice covers a few metres yet used to redraw the whole level x4.
                    Frustum cascadeFrustum = Frustum::FromViewProj(shadowMap.LightViewProj(c));
                    for (auto entity : casters) {
                        if (world.Registry.all_of<InactiveTag>(entity)) continue;
                        auto& renderable = world.Registry.get<RenderableComponent>(entity);
                        glm::mat4 model = world.GetCachedWorldTransform(entity);
                        glm::vec3 bmin = renderable.ModelRef->BoundsMin();
                        glm::vec3 bmax = renderable.ModelRef->BoundsMax();
                        bool validBounds = bmin.x <= bmax.x && bmin.y <= bmax.y && bmin.z <= bmax.z;
                        // Bounds are bind-pose only (#113), so an animated limb can swing
                        // outside them — conservatively keep every skinned caster rather than
                        // risk its shadow popping at a cascade edge. Static geometry culls.
                        if (validBounds && !renderable.ModelRef->HasAnimations() &&
                            !cascadeFrustum.Intersects(AABB{bmin, bmax}.Transformed(model)))
                            continue;
                        shadowShader.SetMat4(shadowModelLoc, model);
                        renderable.ModelRef->DrawDepthOnly(shadowShader, renderable.Materials);
                    }
                }

                glDisable(GL_POLYGON_OFFSET_FILL);
                glCullFace(GL_BACK);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, window.GetWidth(), window.GetHeight());
                GLStateCache::Invalidate();
                sunShadowsReady = true;
            }

            // --- Spot-light shadow maps (#119) — once per frame, like the sun CSM ------------
            if (frameSettings.ShadowsEnabled) {
                spotShadowMap.Configure(std::min(frameSettings.ShadowResolution, 2048));
            }
            if (frameSettings.ShadowsEnabled && spotShadowCount > 0) {
                PROFILE_SCOPE("Spot Shadow Pass");
                PROFILE_GPU_SCOPE("Spot Shadow Pass");
                glEnable(GL_DEPTH_TEST);
                glDepthMask(GL_TRUE);
                // Store the LIGHT-FACING surface (back-face cull), NOT the far side. The sun CSM
                // front-face-culls fine because its rays are parallel, but a perspective spot's
                // rays diverge and a curved caster's back face sits far enough past the contact
                // that culling front faces detached the contact shadow — the sphere gap in #134.
                // gl_FragDepth is written linearly here, so glPolygonOffset does nothing; the
                // bias lives entirely in SpotShadow()'s texel-proportional `ref` term.
                glCullFace(GL_BACK);

                localShadowShader.Bind(); // linear distance-to-light depth
                auto casters = world.Registry.view<TransformComponent, RenderableComponent>();
                // #194: resolve once, outside the per-spot x per-caster loop.
                int localLightViewProjLoc = localShadowShader.Loc("uLightViewProj");
                int localLightPosLoc = localShadowShader.Loc("uShadowLightPos");
                int localFarLoc = localShadowShader.Loc("uShadowFar");
                int localModelLoc = localShadowShader.Loc("uModel");
                for (int s = 0; s < spotShadowCount; ++s) {
                    spotShadowMap.Begin(s);
                    localShadowShader.SetMat4(localLightViewProjLoc, spotShadowVP[s]);
                    localShadowShader.SetVec3(localLightPosLoc, spotShadowPos[s]);
                    localShadowShader.SetFloat(localFarLoc, spotShadowFar[s]);
                    Frustum lf = Frustum::FromViewProj(spotShadowVP[s]);
                    for (auto entity : casters) {
                        if (world.Registry.all_of<InactiveTag>(entity)) continue;
                        auto& r = world.Registry.get<RenderableComponent>(entity);
                        glm::mat4 model = world.GetCachedWorldTransform(entity);
                        glm::vec3 bmin = r.ModelRef->BoundsMin();
                        glm::vec3 bmax = r.ModelRef->BoundsMax();
                        bool vb = bmin.x <= bmax.x && bmin.y <= bmax.y && bmin.z <= bmax.z;
                        if (vb && !r.ModelRef->HasAnimations() &&
                            !lf.Intersects(AABB{bmin, bmax}.Transformed(model)))
                            continue;
                        localShadowShader.SetMat4(localModelLoc, model);
                        r.ModelRef->DrawDepthOnly(localShadowShader, r.Materials);
                    }
                }

                glDisable(GL_POLYGON_OFFSET_FILL);
                glCullFace(GL_BACK);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, window.GetWidth(), window.GetHeight());
                GLStateCache::Invalidate();
            }

            // --- Point-light cube shadow maps (#119) — six faces per casting point light ------
            if (frameSettings.ShadowsEnabled) {
                pointShadowMap.Configure(std::min(frameSettings.ShadowResolution, 1024));
            }
            if (frameSettings.ShadowsEnabled && pointShadowCount > 0) {
                PROFILE_SCOPE("Point Shadow Pass");
                PROFILE_GPU_SCOPE("Point Shadow Pass");
                // Standard GL cube-map face order: +X -X +Y -Y +Z -Z, with the conventional ups.
                static const glm::vec3 kFaceDir[6] = {
                    { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0}, {0, -1, 0}, {0, 0,  1}, {0, 0, -1} };
                static const glm::vec3 kFaceUp[6] = {
                    {0, -1, 0}, {0, -1, 0}, {0, 0,  1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0} };

                glEnable(GL_DEPTH_TEST);
                glDepthMask(GL_TRUE);
                // Store the LIGHT-FACING surface (back-face cull), matching the spot pass. Front-
                // face culling here stored a sphere's FAR hemisphere, which under this 90°
                // perspective sits well past the true contact — that was the sphere peter-panning
                // in #134. gl_FragDepth is linear, so glPolygonOffset is inert; PointShadow()'s
                // texel-proportional `ref` term carries all the bias.
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);

                localShadowShader.Bind(); // linear distance-to-light depth
                auto casters = world.Registry.view<TransformComponent, RenderableComponent>();
                // #194: resolve once, outside the per-point x 6-face x per-caster loop.
                int cubeLightPosLoc = localShadowShader.Loc("uShadowLightPos");
                int cubeFarLoc = localShadowShader.Loc("uShadowFar");
                int cubeLightViewProjLoc = localShadowShader.Loc("uLightViewProj");
                int cubeModelLoc = localShadowShader.Loc("uModel");
                for (int s = 0; s < pointShadowCount; ++s) {
                    glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, pointShadowNear[s], pointShadowFar[s]);
                    localShadowShader.SetVec3(cubeLightPosLoc, pointShadowPos[s]);
                    localShadowShader.SetFloat(cubeFarLoc, pointShadowFar[s]);
                    for (int f = 0; f < 6; ++f) {
                        pointShadowMap.BeginFace(s, f);
                        glm::mat4 vp = proj * glm::lookAt(pointShadowPos[s],
                                                         pointShadowPos[s] + kFaceDir[f], kFaceUp[f]);
                        localShadowShader.SetMat4(cubeLightViewProjLoc, vp);
                        Frustum lf = Frustum::FromViewProj(vp);
                        for (auto entity : casters) {
                            if (world.Registry.all_of<InactiveTag>(entity)) continue;
                            auto& r = world.Registry.get<RenderableComponent>(entity);
                            glm::mat4 model = world.GetCachedWorldTransform(entity);
                            glm::vec3 bmin = r.ModelRef->BoundsMin();
                            glm::vec3 bmax = r.ModelRef->BoundsMax();
                            bool vb = bmin.x <= bmax.x && bmin.y <= bmax.y && bmin.z <= bmax.z;
                            if (vb && !r.ModelRef->HasAnimations() &&
                                !lf.Intersects(AABB{bmin, bmax}.Transformed(model)))
                                continue;
                            localShadowShader.SetMat4(cubeModelLoc, model);
                            r.ModelRef->DrawDepthOnly(localShadowShader, r.Materials);
                        }
                    }
                }

                glDisable(GL_POLYGON_OFFSET_FILL);
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, window.GetWidth(), window.GetHeight());
                GLStateCache::Invalidate();
            }

            // --- IBL probes (#196) — baked from the sky, NOT per frame ------------------------
            // NeedsBake() compares against the colours the probes were last baked with, so this
            // is a couple of vec3 compares on the overwhelming majority of frames and a ~1 ms
            // burst on the frame after someone drags the sky colour (or loads a scene, or
            // undoes an edit — watching the state rather than any one writer means every path
            // that can change the sky is covered without plumbing a dirty flag through the UI).
            // PR13: HDRI sky path — load/reload cubemap when path changes; bake IBL from it.
            // Procedural sky path — bake from gradient colours as before.
            if (world.SkySourceMode == World::SkySource::Hdri) {
                if (!world.SkyHdriPath.empty() && world.SkyHdriPath != hdriCubePath) {
                    PROFILE_SCOPE("HDRI Load");
                    hdriCube     = Cubemap::LoadHdr(world.SkyHdriPath);
                    hdriCubePath = world.SkyHdriPath;
                }
                // NeedsBake(-1,-1,-1) is true unless BakeFromCubemap already ran this HDRI;
                // the sentinel set by BakeFromCubemap makes the check false until we switch source.
                if (hdriCube && iblProbe.NeedsBake(glm::vec3(-1.0f), glm::vec3(-1.0f))) {
                    PROFILE_SCOPE("IBL Bake (HDRI)");
                    PROFILE_GPU_SCOPE("IBL Bake (HDRI)");
                    iblProbe.BakeFromCubemap(hdriCube->Texture());
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glViewport(0, 0, window.GetWidth(), window.GetHeight());
                    GLStateCache::Invalidate();
                }
                prevWasHdri = true;
            } else {
                if (prevWasHdri) {
                    // Switched back to procedural — clear hdriCube so it's reloaded if HDRI is re-selected
                    hdriCube.reset();
                    hdriCubePath.clear();
                }
                prevWasHdri = false;
                if (iblProbe.NeedsBake(world.SkyHorizonColor, world.SkyZenithColor)) {
                    PROFILE_SCOPE("IBL Bake");
                    PROFILE_GPU_SCOPE("IBL Bake");
                    iblProbe.Bake(world.SkyHorizonColor, world.SkyZenithColor);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glViewport(0, 0, window.GetWidth(), window.GetHeight());
                    GLStateCache::Invalidate();
                }
            }

            // PR14: rebuild probe list from scene (cheap CPU gather, once per frame)
            probeArray.Update(world);

            // Renders the lit scene (sky + every Transform+Renderable entity) into whatever
            // framebuffer/viewport is currently bound. Shared by the real on-screen pass below
            // and GameViewPanel's offscreen framebuffer pass, so the two can never silently
            // diverge. Editor-only visualization (selection outline/highlight, drag-preview
            // ghost, grid, wireframe) is deliberately NOT part of this — the Game View should
            // show exactly what Play Mode does, never editor debug shading.
            // editorView: the editor Scene viewport (not the Game view). Only that pass applies
            // editor-only visibility filters — the per-layer visibility mask (#236 A1) and the
            // per-entity HiddenInSceneTag (#236 B). The running game and its Game view draw everything.
            // audit #359 pass 2: the ~330-line drawScene lambda that used to live here is now
            // SceneRenderer::RenderScene. Its ~25 `[&]`-captures are gathered into this explicit
            // value once per frame (after the shadow / IBL / cluster prerequisite passes above
            // have populated the frame* state), then handed to every viewport's draw call below.
            SceneRenderInputs sceneInputs;
            sceneInputs.sky                = &sky;
            sceneInputs.modelShader        = &modelShader;
            sceneInputs.clusterBuildShader = &clusterBuildShader;
            sceneInputs.clusterCullShader  = &clusterCullShader;
            sceneInputs.sunShadow          = &shadowMap;
            sceneInputs.spotShadow         = &spotShadowMap;
            sceneInputs.pointShadow        = &pointShadowMap;
            sceneInputs.iblProbe           = &iblProbe;
            sceneInputs.probeArray         = &probeArray;
            sceneInputs.lightBuffer        = &lightBuffer;
            sceneInputs.clusterGrid        = &clusterGrid;
            sceneInputs.hdriCube           = hdriCube.get();
            sceneInputs.sunShadowsReady    = sunShadowsReady;
            sceneInputs.frameLightCount    = frameLightCount;
            sceneInputs.sunAngularDeg       = frameSunAngularDeg;
            sceneInputs.sunShadowSoftness   = frameSunShadowSoftness;
            sceneInputs.sunShadowBias       = frameSunShadowBias;
            sceneInputs.sunShadowNormalBias = frameSunShadowNormalBias;
            sceneInputs.spotShadowCount      = spotShadowCount;
            sceneInputs.spotShadowVP         = spotShadowVP;
            sceneInputs.spotShadowPos        = spotShadowPos;
            sceneInputs.spotShadowFar        = spotShadowFar;
            sceneInputs.spotShadowHalfTan    = spotShadowHalfTan;
            sceneInputs.spotShadowBias       = spotShadowBias;
            sceneInputs.spotShadowNormalBias = spotShadowNormalBias;
            sceneInputs.spotShadowSoftness   = spotShadowSoftness;
            sceneInputs.pointShadowCount      = pointShadowCount;
            sceneInputs.pointShadowFar        = pointShadowFar;
            sceneInputs.pointShadowBias       = pointShadowBias;
            sceneInputs.pointShadowNormalBias = pointShadowNormalBias;
            sceneInputs.shadowsEnabled   = frameSettings.ShadowsEnabled;
            sceneInputs.ssaoEnabled      = frameSettings.SsaoEnabled;
            sceneInputs.layerVisibleMask = frameSettings.LayerVisibleMask;
            auto drawScene = [&](const RenderFrameContext& ctx, EditorLayer::RenderStats* outStats) {
                sceneRenderer.RenderScene(world, ctx, sceneInputs, outStats);
            };

            // --- Scene tab (editor viewport) offscreen pass -----------------------------------
            // Rendered into its own framebuffer rather than straight into the backbuffer, then
            // displayed via ImGui::Image inside the "Scene" window (see
            // EditorLayer::SetSceneTexture's comment for why the old raw-GL-underneath technique
            // broke once Scene became a real, named, tabbed window). Rendered whenever editing,
            // regardless of whether Scene or Game is the currently active tab — cheap enough not
            // to bother skipping, and keeps this block's shape identical to the Game View pass
            // just below it. Runs during in-panel play too — the Scene tab stays live then.
            // Capture prep: a pending Scene/clean-viewport shot renders THIS frame at the
            // requested supersample (capped so a 4K viewport can't ask for a 16K MSAA target)
            // and, for "clean", with editor overlays suppressed. The grab itself happens after
            // the frame is composed (below).
            EditorLayer::CaptureRequest capReq = editor.PeekCaptureRequest();
            int capScale = 1;
            if (capReq.pending && (capReq.mode == 1 || capReq.mode == 2)) {
                capScale = std::max(1, capReq.scale);
                editor.SetHideOverlaysThisFrame(capReq.mode == 2);
                editor.PrimeCaptureRequest();  // this frame's Scene render IS the one we grab
            }

            // Scene and Game are tabs in the same dock node - at most one is visible at a time, so
            // skip this render entirely when the Scene tab isn't the one showing (#172). Halves
            // per-frame render cost in the common case (editing in one tab or the other).
            if (editorUIVisible && editor.IsSceneViewportVisible()) {
                PROFILE_SCOPE("Scene View Render");
                glm::vec2 available = editor.GetLastSceneContentRegion();
                if (available.x < 1.0f || available.y < 1.0f) {
                    available = {(float)window.GetWidth(), (float)window.GetHeight()};
                }
                int scW = std::max((int)available.x, 1);
                int scH = std::max((int)available.y, 1);
                while (capScale > 1 && ((long long)scW * capScale > 8192 || (long long)scH * capScale > 8192))
                    capScale /= 2;
                scW *= capScale;
                scH *= capScale;

                // A pending viewport shot with a fixed Resolution preset overrides the live
                // viewport size entirely (Supersample is forced off in that case by the UI).
                if (capReq.pending && (capReq.mode == 1 || capReq.mode == 2) && capReq.resW > 0) {
                    scW = capReq.resW;
                    scH = capReq.resH;
                }

                sceneFramebuffer.Resize(scW, scH);
                sceneHdr.Resize(scW, scH, EditorSettings::Get().MsaaSamples);
                sceneHdr.BindForRender();
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                float sceneAspect = (float)scW / (float)scH;
                glm::mat4 sceneViewMat = editorCamera.ViewMatrix();
                glm::mat4 sceneProjMat = editorCamera.ProjectionMatrix(sceneAspect);

                EditorLayer::ShadingMode shading = editor.GetShadingMode();
                bool sceneWireframe = shading == EditorLayer::ShadingMode::Wireframe;
                bool sceneUnlit = shading == EditorLayer::ShadingMode::Unlit;
                int sceneDebugView = 0; // model shader uDebugView: 1 Normals, 2 Cascades, 3 Mip
                if (shading == EditorLayer::ShadingMode::Normals)  sceneDebugView = 1;
                else if (shading == EditorLayer::ShadingMode::Cascades) sceneDebugView = 2;
                else if (shading == EditorLayer::ShadingMode::Mip)      sceneDebugView = 3;
                if (sceneWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

                // PR15: SSAO depth pre-pass — runs before drawScene so the occlusion map is ready.
                // Reuses shadowShader (ShadowDepth.vert.glsl) with uLightViewProj = proj * view.
                // Skipped in unlit/wireframe modes where SSAO has no visual effect.
                if (frameSettings.SsaoEnabled && !sceneUnlit) ssao.Resize(scW, scH);
                // audit #358 — if any SSAO FBO came back incomplete, skip the passes outright
                // (the scene just renders without ambient occlusion) instead of drawing into a
                // zero/invalid framebuffer.
                if (frameSettings.SsaoEnabled && !sceneUnlit && ssao.IsValid()) {
                    PROFILE_SCOPE("SSAO Depth Pre-pass");
                    PROFILE_GPU_SCOPE("SSAO Depth Pre-pass");
                    glBindFramebuffer(GL_FRAMEBUFFER, ssao.DepthFbo());
                    glViewport(0, 0, scW, scH);
                    glClear(GL_DEPTH_BUFFER_BIT);
                    glEnable(GL_DEPTH_TEST);
                    glDepthMask(GL_TRUE);
                    glEnable(GL_CULL_FACE);
                    glCullFace(GL_BACK);
                    glDisable(GL_BLEND);

                    shadowShader.Bind();
                    shadowShader.SetMat4("uLightViewProj", sceneProjMat * sceneViewMat);
                    shadowShader.SetInt("uUseSkinning", 0); // static geometry only
                    int ssaoModelLoc = shadowShader.Loc("uModel");
                    auto ssaoRenderables = world.Registry.view<TransformComponent, RenderableComponent>();
                    for (auto entity : ssaoRenderables) {
                        if (world.Registry.all_of<InactiveTag>(entity)) continue;
                        auto& rc = world.Registry.get<RenderableComponent>(entity);
                        if (!rc.ModelRef) continue;
                        glm::mat4 model = world.GetCachedWorldTransform(entity);
                        shadowShader.SetMat4(ssaoModelLoc, model);
                        rc.ModelRef->DrawDepthOnly(shadowShader, rc.Materials);
                    }
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    GLStateCache::Invalidate();

                    ssao.Compute(ssaoComputeShader, sceneProjMat);
                    ssao.Blur(ssaoBlurShader);
                    GLStateCache::Invalidate();

                    // Restore HdrTarget for the main scene draw
                    sceneHdr.BindForRender();
                }

                EditorLayer::RenderStats sceneStats;
                drawScene(RenderFrameContext{ sceneViewMat, sceneProjMat, editorCamera.Position,
                              sceneUnlit, /*EditorView=*/true, /*DebugView=*/sceneDebugView,
                              &sceneHdr, &sceneOpaqueColor, &ssao },
                          &sceneStats);

                editor.SetRenderStats(sceneStats);
                // NB: in Wireframe mode the polygon mode stays GL_LINE through the selection
                // passes below, so the inverted-hull "outline" draws as an enlarged orange
                // wireframe over the object rather than a filled orange silhouette that buries
                // its own wireframe (#13 P2). Reset to GL_FILL before the drag-preview ghost.

                // Selection outline: screen-space. Render the selected meshes' silhouettes to a
                // 1-bit mask target, then a fullscreen pass paints a uniform-width ring in the
                // pixels just OUTSIDE that mask. No surface wash — the wash + an inverted-hull
                // that couldn't widen a flat mesh's silhouette were what turned a selected
                // flat/thin object into a solid orange blob (audit #51).
                auto selection = editor.GetSelectedItems();
                if (!selection.empty() && !editor.OverlaysHidden()) {
                    const glm::vec3 kOutlineColor(1.0f, 0.55f, 0.1f);
                    const int kOutlinePixels = 3;

                    std::vector<glm::mat4> xforms;
                    std::vector<Model*> models;
                    for (entt::entity entity : selection) {
                        if (!world.Registry.valid(entity)) continue; // may have been deleted this same frame
                        // Lights and empties have nothing to outline — they get a screen-space
                        // selection ring from EditorLayer::DrawEntityIcons instead.
                        auto* renderablePtr = world.Registry.try_get<RenderableComponent>(entity);
                        if (!renderablePtr) continue;
                        xforms.push_back(world.GetCachedWorldTransform(entity));
                        models.push_back(renderablePtr->ModelRef.get());
                    }

                    if (!models.empty()) {
                        // --- Silhouette mask pass: flat white where a selected mesh is, into its
                        // own target. Depth test off so a partly-occluded selection is still fully
                        // outlined (Unity's behaviour).
                        selectionMaskFbo.Resize(scW, scH);
                        selectionMaskFbo.Bind();
                        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                        glClear(GL_COLOR_BUFFER_BIT);
                        glDisable(GL_DEPTH_TEST);
                        glDepthMask(GL_FALSE);
                        outlineModelShader.Bind();
                        outlineModelShader.SetMat4("uView", sceneViewMat);
                        outlineModelShader.SetMat4("uProj", sceneProjMat);
                        outlineModelShader.SetFloat("uThickness", 0.0f);
                        outlineModelShader.SetVec3("uOutlineColor", glm::vec3(1.0f));
                        if (sceneWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // mask must be solid
                        for (size_t i = 0; i < models.size(); ++i) {
                            outlineModelShader.SetMat4("uModel", xforms[i]);
                            models[i]->Draw(outlineModelShader);
                        }
                        if (sceneWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

                        // --- Dilate pass: back to the scene target, paint the ring.
                        sceneHdr.BindForRender();
                        glEnable(GL_BLEND);
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                        outlineDilateShader.Bind();
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, selectionMaskFbo.ColorTexture());
                        outlineDilateShader.SetInt("uMask", 0);
                        outlineDilateShader.SetVec3("uTexel", glm::vec3(1.0f / (float)scW, 1.0f / (float)scH, 0.0f));
                        outlineDilateShader.SetVec3("uColor", kOutlineColor);
                        outlineDilateShader.SetInt("uRadius", kOutlinePixels);
                        glBindVertexArray(fsQuadVao);
                        glDrawArrays(GL_TRIANGLES, 0, 6);
                        glBindVertexArray(0);

                        // Restore.
                        glDisable(GL_BLEND);
                        glEnable(GL_DEPTH_TEST);
                        glDepthMask(GL_TRUE);
                        GLStateCache::Invalidate(); // raw shader/VAO/texture binds above bypass the cache
                    }
                }

                if (sceneWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

                // "Drag from Asset Browser" placement preview: a translucent ghost at wherever
                // the dragged model would land if released right now (raycast + grid-snap +
                // bottom-alignment, computed each frame by EditorLayer::DrawViewportDropTarget
                // while the drag is over the viewport) - see TintOverlayRenderer's own comment
                // for why this is a flat tint rather than the real PBR material shader.
                const EditorLayer::DragPreview& dragPreview = editor.GetDragPreview();
                if (dragPreview.Active && dragPreview.ModelRef) {
                    glm::mat4 ghostTransform = ComposeTransform(dragPreview.Position, glm::vec3(0.0f), glm::vec3(1.0f));
                    tintOverlay.Render(*dragPreview.ModelRef, ghostTransform, sceneViewMat, sceneProjMat,
                        glm::vec3(0.3f, 0.85f, 1.0f), 0.45f);
                }

                // Resolve MSAA now, before the grid/axis-line and collider-wireframe overlays
                // below — this snapshot is what Bloom reads. Editor-only overlays are never real
                // scene lighting, so they must never bloom/glow; capturing the bloom source here,
                // before they're drawn, guarantees that regardless of their color or opacity.
                sceneHdr.ResolveTo();

                // PR16: Bloom — soft-knee threshold + mip-pyramid blur before tonemapping.
                // Resize takes the FULL scene resolution; Bloom halves it internally for mip 0.
                unsigned int bloomGlowTex  = 0u;
                float        bloomIntensity = 0.0f;
                if (EditorSettings::Get().BloomEnabled) {
                    PROFILE_GPU_SCOPE("Bloom");
                    bloom.Resize(scW, scH);
                    // audit #358 — only run the pyramid passes if every mip FBO validated
                    // complete; otherwise the frame simply has no bloom glow added.
                    if (bloom.IsValid()) {
                        bloom.Compute(bloomThreshShader, bloomDownsampleShader, bloomUpsampleShader,
                                      sceneHdr.ResolvedColorTexture(),
                                      EditorSettings::Get().BloomThreshold,
                                      EditorSettings::Get().BloomKnee);
                        bloomGlowTex  = bloom.GlowTexture();
                        bloomIntensity = EditorSettings::Get().BloomIntensity;
                    }
                    GLStateCache::Invalidate();
                }

                // Grid / axis lines and the collider wireframe overlay — drawn AFTER the bloom
                // source snapshot above so neither ever contributes to bloom (a plain grey grid
                // line already sits above the default threshold's soft-knee floor, and the green
                // Y-axis line's luma is higher still, so without this ordering both would visibly
                // glow). Bloom::Compute (when it ran) leaves FBO 0 bound, so the MSAA HDR target
                // is rebound here; both overlays still depth-test correctly against the real
                // scene geometry already in that target's depth buffer.
                sceneHdr.BindForRender();
                if (editor.ShowGrid() && !editor.OverlaysHidden()) {
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
                    grid.Draw(sceneViewMat, sceneProjMat, editorCamera.Position,
                              gset.GridMinorSpacing, (float)gset.GridMajorEvery, gridFade,
                              gset.GridOpacity, gset.GridShowAxisLines, gset.GridAxisThickness);
                }

                // Collider wireframe overlay (#185 PR 2) — depth-tested so scene geometry
                // occludes it, no depth write so it never blocks anything drawn after. Also
                // carries the physics visual debug-draw channels (#185 A), which have their own
                // toggle, so it runs when either is on.
                {
                    const bool showShapes = EditorSettings::Get().ShowColliders;
                    const bool showDebug  = EditorSettings::Get().PhysicsDebugDrawFlags != 0u;
                    if ((showShapes || showDebug) && !editor.OverlaysHidden()) {
                        glEnable(GL_DEPTH_TEST);
                        glDepthMask(GL_FALSE);
                        colliderGizmo.Draw(sceneViewMat, sceneProjMat, world, showShapes);
                        glDepthMask(GL_TRUE);
                    }
                }

                // Resolve again now that the grid/collider overlays are drawn, so the Scene tab
                // still shows them — only the earlier, overlay-free resolve above ever reached
                // Bloom. Tonemap the linear-HDR scene into the LDR texture the Scene tab displays
                // (exposure -> curve -> gamma, once, in Tonemapper).
                sceneHdr.ResolveTo();
                {
                PROFILE_GPU_SCOPE("Tonemap");
                tonemapper.Apply(sceneHdr.ResolvedColorTexture(), sceneFramebuffer.Handle(), scW, scH,
                                 EditorSettings::Get().ExposureEV,
                                 (Tonemapper::Operator)EditorSettings::Get().TonemapOperator,
                                 bloomGlowTex, bloomIntensity);
                }

                Framebuffer::BindDefault(window.GetWidth(), window.GetHeight());
                editor.SetSceneTexture(sceneFramebuffer.ColorTexture());
            }

            {
                PROFILE_SCOPE("Editor UI Build");
                // While the game owns input, the editor viewport's own picking / gizmo keys
                // stand down (a shoot-click or strafe key shouldn't also poke the editor).
                editor.SetGameInputActive(gameHasInput);
                if (editorUIVisible) editor.Draw(world, assets, editorCamera, dt);
                else if (playing)    editor.DrawPlayModeOverlays(world); // #185 — physics panel + HUD over maximized play
                // One coalesced, atomic prefs write per frame for however many preference
                // controls changed this frame (audit CPP-206 / PERF-211).
                EditorSettings::Flush();
                // The reloadable editor module (Stats HUD, toolbar strip + menus) reads live
                // editor / world / assets / camera state through EditorModuleHostAPI; hand it
                // this frame's pointers first.
                editorModule.SetFrameContext(&editor, &world, &assets, &editorCamera);
                editorModule.Draw(editorUIVisible, dt);

                // Eyedropper (#236 R2): a colour field armed EditorLayer's viewport eyedropper
                // and HandleViewportPicking just captured a click. Read that one pixel off the
                // LDR (tonemapped) scene FBO — still holding this frame's image — and hand the
                // colour back.
                if (float ex, ey; editor.ConsumeEyedropperSample(ex, ey)) {
                    const int fbW = (int)editor.ViewportSize().x;
                    const int fbH = (int)editor.ViewportSize().y;
                    const int px = (int)(ex + 0.5f);
                    const int py = (int)(fbH - 1 - ey);       // GL sample origin is bottom-left
                    if (px >= 0 && px < fbW && py >= 0 && py < fbH) {
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneFramebuffer.Handle());
                        unsigned char rgba[4] = {0, 0, 0, 255};
                        glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                        GLStateCache::Invalidate();
                        editor.ApplyEyedropperSample(glm::vec3(rgba[0], rgba[1], rgba[2]) / 255.0f);
                    } else {
                        editor.CancelEyedropper();
                    }
                }
            }

            // The OS title bar is gone — the toolbar's empty area is the window drag handle.
            // Report it to the Window so its WM_NCHITTEST can hand that region to Windows as the
            // caption (drag + Aero-snap + double-click maximize). False whenever the editor UI is
            // hidden (maximized play) so the game view never becomes draggable.
            window.SetTitleBarDragActive(editorUIVisible && editor.WantsWindowDrag());

            // Drawn every frame in every state (unlike editor.Draw(), which is editor-UI-only) so
            // there's always an on-screen Play/Stop, not just F1. Drawn AFTER editor.Draw() so it
            // layers on top of the toolbar strip instead of being painted over by it.
            editor.DrawPlayStopButton(playing, playMaximized, paused);
            if (editor.ConsumePlayStopRequest()) togglePlay();
            if (editor.ConsumeMaximizeToggleRequest()) setMaximized(!playMaximized);

            // The Game View toolbar's Fullscreen button and "Maximize on Play" both just emit a
            // request (GameViewPanel deliberately doesn't touch Window/GLFW itself) — fulfill it
            // here, once per frame, regardless of whether the Game panel was even drawn this
            // frame (a Play Mode transition can request a change with no ImGui content at all).
            bool wantFullscreen;
            if (gameView.ConsumeFullscreenRequest(wantFullscreen)) {
                window.SetFullscreen(wantFullscreen);
            }

            // --- Game View offscreen pass -----------------------------------------------------
            // Rendered whenever the Game panel is actually the visible tab (Scene and Game share
            // one dock node, so only one of them is ever showing - #172) or maximized play has a
            // locked, non-Free aspect/resolution selected (so the letterboxed blit further down
            // has a source). Free-Aspect maximized play never needs this — the normal full-window
            // pass below already IS a free-aspect render, so skip the extra work entirely.
            bool playMaxLocked = playMaximized && gameView.GetCurrentPreset().Mode != AspectRatioMode::FreeAspect;
            bool gameTabVisible = editorUIVisible && gameView.IsVisible();
            GameViewStats gameViewStats{};
            if (gameTabVisible || playMaxLocked) {
                ImVec2 available = gameTabVisible ? gameView.GetLastAvailableRegion() : ImVec2(0.0f, 0.0f);
                if (available.x < 1.0f || available.y < 1.0f) {
                    available = ImVec2((float)window.GetWidth(), (float)window.GetHeight());
                }

                int gvWidth, gvHeight;
                gameView.ComputeTargetSize(available, gvWidth, gvHeight);
                gameView.GetFramebuffer().Resize(gvWidth, gvHeight);
                gameHdr.Resize(gvWidth, gvHeight, EditorSettings::Get().MsaaSamples);
                gameHdr.BindForRender();
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                float gvAspect = (float)gvWidth / (float)gvHeight;
                glm::mat4 gvView, gvProj;
                glm::vec3 gvEye;
                // While editing, render through a placed Camera entity if the scene has one, so
                // the Game panel previews the framed shot (#36 B10). Play mode always uses the
                // first-person controller camera.
                entt::entity sceneCamEnt = playing ? entt::null : FindActiveSceneCamera(world);
                if (sceneCamEnt != entt::null) {
                    const auto& cc = world.Registry.get<CameraComponent>(sceneCamEnt);
                    glm::mat4 camModel = world.ComposeWorldTransform(sceneCamEnt);
                    gvView = glm::inverse(camModel);
                    gvProj = glm::perspective(glm::radians(cc.FovDegrees), gvAspect, cc.NearPlane, cc.FarPlane);
                    gvEye = glm::vec3(camModel[3]);
                } else {
                    gvView = gameCam->ViewMatrix();
                    gvProj = gameCam->ProjectionMatrix(gvAspect);
                    gvEye = gameCam->Position;
                }
                // SSAO depth pre-pass for the Game view — own Ssao instance/resolution from the
                // Scene view's (see gameSsao declaration). Game view has no unlit mode.
                if (EditorSettings::Get().SsaoEnabled) gameSsao.Resize(gvWidth, gvHeight);
                if (EditorSettings::Get().SsaoEnabled && gameSsao.IsValid()) { // audit #358 — skip if any FBO incomplete
                    PROFILE_SCOPE("SSAO Depth Pre-pass (Game)");
                    PROFILE_GPU_SCOPE("SSAO Depth Pre-pass (Game)");
                    glBindFramebuffer(GL_FRAMEBUFFER, gameSsao.DepthFbo());
                    glViewport(0, 0, gvWidth, gvHeight);
                    glClear(GL_DEPTH_BUFFER_BIT);
                    glEnable(GL_DEPTH_TEST);
                    glDepthMask(GL_TRUE);
                    glEnable(GL_CULL_FACE);
                    glCullFace(GL_BACK);
                    glDisable(GL_BLEND);

                    shadowShader.Bind();
                    shadowShader.SetMat4("uLightViewProj", gvProj * gvView);
                    shadowShader.SetInt("uUseSkinning", 0);
                    int gameSsaoModelLoc = shadowShader.Loc("uModel");
                    auto gameSsaoRenderables = world.Registry.view<TransformComponent, RenderableComponent>();
                    for (auto entity : gameSsaoRenderables) {
                        if (world.Registry.all_of<InactiveTag>(entity)) continue;
                        auto& rc = world.Registry.get<RenderableComponent>(entity);
                        if (!rc.ModelRef) continue;
                        glm::mat4 model = world.GetCachedWorldTransform(entity);
                        shadowShader.SetMat4(gameSsaoModelLoc, model);
                        rc.ModelRef->DrawDepthOnly(shadowShader, rc.Materials);
                    }
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    GLStateCache::Invalidate();

                    gameSsao.Compute(ssaoComputeShader, gvProj);
                    gameSsao.Blur(ssaoBlurShader);
                    GLStateCache::Invalidate();

                    gameHdr.BindForRender(); // restore for the main scene draw below
                }

                EditorLayer::RenderStats gvRenderStats;
                drawScene(RenderFrameContext{ gvView, gvProj, gvEye, /*Unlit=*/false,
                              /*EditorView=*/false, /*DebugView=*/0,
                              &gameHdr, &gameOpaqueColor, &gameSsao },
                          &gvRenderStats);

                // Physics debug overlay over the game view (#185, F5) — depth-tested, no depth write.
                if (playing && EditorSettings::Get().PlayDebugOverlay) {
                    glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
                    colliderGizmo.Draw(gvView, gvProj, world, EditorSettings::Get().ShowColliders);
                    glDepthMask(GL_TRUE);
                }

                gameHdr.ResolveTo();

                // PR16: Bloom for the Game view — own Bloom instance (see gameBloom declaration).
                unsigned int gvBloomGlowTex   = 0u;
                float        gvBloomIntensity = 0.0f;
                if (EditorSettings::Get().BloomEnabled) {
                    PROFILE_GPU_SCOPE("Bloom (Game)");
                    gameBloom.Resize(gvWidth, gvHeight);
                    if (gameBloom.IsValid()) { // audit #358 — no glow if any mip FBO is incomplete
                        gameBloom.Compute(bloomThreshShader, bloomDownsampleShader, bloomUpsampleShader,
                                          gameHdr.ResolvedColorTexture(),
                                          EditorSettings::Get().BloomThreshold,
                                          EditorSettings::Get().BloomKnee);
                        gvBloomGlowTex   = gameBloom.GlowTexture();
                        gvBloomIntensity = EditorSettings::Get().BloomIntensity;
                    }
                    GLStateCache::Invalidate();
                }
                {
                PROFILE_GPU_SCOPE("Tonemap");
                tonemapper.Apply(gameHdr.ResolvedColorTexture(), gameView.GetFramebuffer().Handle(),
                                 gvWidth, gvHeight, EditorSettings::Get().ExposureEV,
                                 (Tonemapper::Operator)EditorSettings::Get().TonemapOperator,
                                 gvBloomGlowTex, gvBloomIntensity);
                }
                Framebuffer::BindDefault(window.GetWidth(), window.GetHeight());

                gameViewStats.FPS = dt > 0.0f ? (int)(1.0f / dt) : 0;
                gameViewStats.FrameMs = dt * 1000.0f;
                gameViewStats.DrawCalls = gvRenderStats.DrawCalls;
                gameViewStats.Triangles = gvRenderStats.Triangles;
                gameViewStats.Vertices = gvRenderStats.Vertices;
            }
            // Drawn only while the editor UI is up — maximized play has no docked panels (see
            // editor.Draw() above), and its FBO pass already rendered this frame's real output.
            if (editorUIVisible) {
                PROFILE_SCOPE("Game View UI");
                // Scene/Game go unsubmitted whenever play is maximized - reapplying Game's dock
                // assignment right before its Begin() (inside RenderUI) guards against ImGui
                // failing to remember it across that gap and popping it out into its own floating
                // window (observed after a maximize/restore cycle). ImGuiCond_Appearing makes
                // this a no-op once Game is being submitted continuously, so it never fights a
                // manual re-dock the user did.
                ImGuiID sceneGameDockId = editor.GetSceneGameDockNodeId();
                if (sceneGameDockId != 0) {
                    ImGui::SetNextWindowDockID(sceneGameDockId, ImGuiCond_Appearing);
                }
                gameView.RenderUI(&gameViewStats, window.IsFullscreen(), playing, gameInputEngaged,
                                  /*noSceneCamera=*/!playing && FindActiveSceneCamera(world) == entt::null);

                // Hand the editor this frame's Game-view image rect + framebuffer so the Play-Mode
                // Stop/Fullscreen overlay can sit over the game viewport and adapt its tint to it.
                {
                    Framebuffer& gvfb = gameView.GetFramebuffer();
                    editor.SetGameViewRect(gameView.GetViewImagePos(), gameView.GetViewImageSize(),
                                           gvfb.ColorTexture(), gvfb.Width(), gvfb.Height());
                }

                // Click inside the running Game view (while it doesn't yet own input) captures
                // the cursor for the player. Esc / Stop release it (handled above / in stopPlay).
                if (playing && !playMaximized && gameView.ConsumeEngageClick()) {
                    gameInputEngaged = true;
                    window.SetCursorLocked(true);
                }

                // Called here, after BOTH Scene's and Game's Begin() calls have run this frame,
                // deliberately - a maximize/restore or Play/Stop makes them "reappear" the same
                // frame, and ImGui's reappear-in-a-tab-bar handling otherwise leaves whichever
                // it processed LAST (Game, after Scene) as the active tab if this ran earlier.
                editor.ApplyPendingViewportTabFocus();
            } else {
                // Maximized play / editor hidden — no docked Game panel this frame; clear the rect
                // so the Stop overlay falls back to its window-anchored position.
                editor.SetGameViewRect(ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f), 0, 0, 0);
            }

            // Maximized play's actual on-screen output — the editor path never reaches here: the
            // Scene tab already rendered (and displayed, via ImGui::Image) its own content above,
            // and the Game tab (if active) already did too via RenderUI() just above.
            if (playMaxLocked) {
                // A locked, non-Free Game View aspect/resolution is selected — the pass above
                // already rendered this frame's real output into gameView's framebuffer; blit it
                // in, letterboxed, instead of re-rendering the whole scene a second time.
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                Framebuffer& gvFramebuffer = gameView.GetFramebuffer();
                ImVec2 windowSize((float)window.GetWidth(), (float)window.GetHeight());
                ResolutionManager::LetterboxRect blitRect = ResolutionManager::CalculateLetterboxRect(
                    windowSize, (float)gvFramebuffer.Width() / (float)gvFramebuffer.Height());

                // CalculateLetterboxRect's Offset is a top-left-origin centering offset, but
                // glBlitFramebuffer's dest rect is bottom-left-origin like everything else in
                // GL — no conversion needed here specifically because centering is symmetric:
                // the gap above the image equals the gap below it, so the same offset value is
                // correct measured from either edge.
                glBindFramebuffer(GL_READ_FRAMEBUFFER, gvFramebuffer.Handle());
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                glBlitFramebuffer(0, 0, gvFramebuffer.Width(), gvFramebuffer.Height(),
                    (int)blitRect.Offset.x, (int)blitRect.Offset.y,
                    (int)(blitRect.Offset.x + blitRect.Size.x), (int)(blitRect.Offset.y + blitRect.Size.y),
                    GL_COLOR_BUFFER_BIT, GL_LINEAR);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            } else if (playMaximized) {
                // Free-Aspect maximized play: render the scene into the HDR target at the
                // window's native aspect, then tonemap straight onto the backbuffer.
                int mw = window.GetWidth(), mh = window.GetHeight();
                gameHdr.Resize(mw, mh, EditorSettings::Get().MsaaSamples);
                gameHdr.BindForRender();
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                float aspect = (float)mw / (float)mh;
                glm::mat4 view = gameCam->ViewMatrix();
                glm::mat4 proj = gameCam->ProjectionMatrix(aspect);

                // SSAO depth pre-pass — same gameSsao instance the docked Game view uses; the two
                // paths are mutually exclusive per frame (if/else-if above), so no resize thrash.
                if (EditorSettings::Get().SsaoEnabled) gameSsao.Resize(mw, mh);
                if (EditorSettings::Get().SsaoEnabled && gameSsao.IsValid()) { // audit #358
                    PROFILE_SCOPE("SSAO Depth Pre-pass (Game)");
                    PROFILE_GPU_SCOPE("SSAO Depth Pre-pass (Game)");
                    glBindFramebuffer(GL_FRAMEBUFFER, gameSsao.DepthFbo());
                    glViewport(0, 0, mw, mh);
                    glClear(GL_DEPTH_BUFFER_BIT);
                    glEnable(GL_DEPTH_TEST);
                    glDepthMask(GL_TRUE);
                    glEnable(GL_CULL_FACE);
                    glCullFace(GL_BACK);
                    glDisable(GL_BLEND);

                    shadowShader.Bind();
                    shadowShader.SetMat4("uLightViewProj", proj * view);
                    shadowShader.SetInt("uUseSkinning", 0);
                    int gameSsaoModelLoc = shadowShader.Loc("uModel");
                    auto gameSsaoRenderables = world.Registry.view<TransformComponent, RenderableComponent>();
                    for (auto entity : gameSsaoRenderables) {
                        if (world.Registry.all_of<InactiveTag>(entity)) continue;
                        auto& rc = world.Registry.get<RenderableComponent>(entity);
                        if (!rc.ModelRef) continue;
                        glm::mat4 model = world.GetCachedWorldTransform(entity);
                        shadowShader.SetMat4(gameSsaoModelLoc, model);
                        rc.ModelRef->DrawDepthOnly(shadowShader, rc.Materials);
                    }
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    GLStateCache::Invalidate();

                    gameSsao.Compute(ssaoComputeShader, proj);
                    gameSsao.Blur(ssaoBlurShader);
                    GLStateCache::Invalidate();

                    gameHdr.BindForRender(); // restore for the main scene draw below
                }

                EditorLayer::RenderStats stats;
                drawScene(RenderFrameContext{ view, proj, gameCam->Position, /*Unlit=*/false,
                              /*EditorView=*/false, /*DebugView=*/0,
                              &gameHdr, &gameOpaqueColor, &gameSsao },
                          &stats);
                editor.SetRenderStats(stats);
                // Physics debug overlay over the game view (#185, F5).
                if (EditorSettings::Get().PlayDebugOverlay) {
                    glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
                    colliderGizmo.Draw(view, proj, world, EditorSettings::Get().ShowColliders);
                    glDepthMask(GL_TRUE);
                }
                gameHdr.ResolveTo();

                // PR16: Bloom — same gameBloom instance the docked Game view uses.
                unsigned int mwBloomGlowTex   = 0u;
                float        mwBloomIntensity = 0.0f;
                if (EditorSettings::Get().BloomEnabled) {
                    PROFILE_GPU_SCOPE("Bloom (Game)");
                    gameBloom.Resize(mw, mh);
                    if (gameBloom.IsValid()) { // audit #358
                        gameBloom.Compute(bloomThreshShader, bloomDownsampleShader, bloomUpsampleShader,
                                          gameHdr.ResolvedColorTexture(),
                                          EditorSettings::Get().BloomThreshold,
                                          EditorSettings::Get().BloomKnee);
                        mwBloomGlowTex   = gameBloom.GlowTexture();
                        mwBloomIntensity = EditorSettings::Get().BloomIntensity;
                    }
                    GLStateCache::Invalidate();
                }
                {
                PROFILE_GPU_SCOPE("Tonemap");
                tonemapper.Apply(gameHdr.ResolvedColorTexture(), 0, mw, mh,
                                 EditorSettings::Get().ExposureEV,
                                 (Tonemapper::Operator)EditorSettings::Get().TonemapOperator,
                                 mwBloomGlowTex, mwBloomIntensity);
                }
            }

            {
                PROFILE_SCOPE("ImGui Render");
                editor.EndFrame(); // flushes the ImGui frame (empty when not in editor mode)
            }
            // Dear ImGui's OpenGL3 backend just made its own raw glUseProgram/glBindTexture
            // calls inside EndFrame() above, bypassing GLStateCache entirely - without this,
            // the cache would start next frame still believing whatever program/texture THIS
            // frame's 3D pass last bound is still active, and wrongly skip the real rebind.
            GLStateCache::Invalidate();

            // Act on the "Save changes?" modal's outcome (see the exit-flow comment above).
            switch (editor.TakeExitDecision()) {
                case EditorLayer::ExitDecision::SaveAndExit:
                    if (!editor.CurrentScenePath().empty())
                        SceneSerializer::Save(world, assets, editor.CurrentScenePath());
                    exitApproved = true;
                    exitSkipFinalSave = true;
                    window.SetShouldClose(true);
                    break;
                case EditorLayer::ExitDecision::DiscardAndExit:
                    exitApproved = true;
                    exitSkipFinalSave = true; // user explicitly chose not to save
                    window.SetShouldClose(true);
                    break;
                case EditorLayer::ExitDecision::None:
                    break;
            }

            // Capture: PrintScreen, or a ".shot" sentinel file next to the exe (triggerable
            // without keyboard focus). Both just raise a request; it's serviced next.
            {
                bool ps = Input::IsKeyPressed(GLFW_KEY_PRINT_SCREEN);
                // The sentinel only exists to let an external script trigger a capture, so a
                // quarter-second of latency is irrelevant — stat'ing the filesystem every single
                // frame (up to 240x/sec at the FPS cap) just to poll a rarely-present file isn't
                // worth it. Throttle the check to ~4 Hz instead.
                static float shotPollAccum = 0.0f;
                shotPollAccum += dt;
                bool sentinel = false;
                if (shotPollAccum >= 0.25f) {
                    shotPollAccum = 0.0f;
                    std::error_code shotEc;
                    sentinel = std::filesystem::exists(".shot", shotEc);
                    if (sentinel) std::filesystem::remove(".shot", shotEc);
                }
                if (ps || sentinel) editor.RequestCapture();
            }

            // Service a pending capture now that the frame is fully composited on the back
            // buffer. Full-editor grabs the back buffer; the viewport modes grab the offscreen
            // FBOs they already rendered into this frame (clean/supersampled where asked).
            {
                EditorLayer::CaptureRequest cap = editor.PeekCaptureRequest();
                // Viewport modes need the Scene view re-rendered at the target size first; that
                // happens at the top of the NEXT frame, which sets `primed`. Full-editor / Game
                // modes read a buffer that's already correct, so they fire the same frame.
                const bool needsResizedRender = cap.mode == 1 || cap.mode == 2;
                if (cap.pending && needsResizedRender && !cap.primed) {
                    // wait one frame — leave the request pending, skip the grab
                } else if (cap.pending) {
                    editor.ConsumeCaptureRequest();
                    const std::string sceneName =
                        std::filesystem::path(editor.CurrentScenePath()).stem().string();
                    std::string outPath; int outW = 0, outH = 0;
                    if (cap.mode == 3) {
                        Framebuffer& gv = gameView.GetFramebuffer();
                        if (gv.IsValid()) {
                            outW = gv.Width(); outH = gv.Height();
                            glBindFramebuffer(GL_READ_FRAMEBUFFER, gv.Handle());
                            auto px = Screenshot::GrabRegion(0, 0, outW, outH);
                            outPath = Screenshot::Save(px.data(), outW, outH, true, cap.format, sceneName);
                        }
                    } else if (cap.mode == 1 || cap.mode == 2) {
                        outW = sceneFramebuffer.Width(); outH = sceneFramebuffer.Height();
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneFramebuffer.Handle());
                        auto px = Screenshot::GrabRegion(0, 0, outW, outH);
                        outPath = Screenshot::Save(px.data(), outW, outH, true, cap.format, sceneName);
                    } else { // 0 = full editor window
                        outW = window.GetWidth(); outH = window.GetHeight();
                        outPath = Screenshot::SaveBackbuffer(outW, outH, cap.format, sceneName);
                    }
                    editor.OnCaptureDone(outPath, outW, outH);
                }
                editor.SetHideOverlaysThisFrame(false);
            }

            window.SwapBuffers();

            // Software frame cap. Runs whatever the VSync mode is, but it's really for VSync Off
            // (with VSync On the driver already blocks in SwapBuffers). 0 = uncapped.
            Clock::LimitFps(EditorSettings::Get().FpsLimit);

            // The editor has now actually presented a frame, so revealing the window shows
            // finished content rather than an unpainted framebuffer. Ordered swap -> show ->
            // close so the splash never disappears before there's something to replace it.
            if (!firstFramePresented) {
                firstFramePresented = true;
                window.Show();
                splash.Close(); // blocks out any remainder of the minimum display time
            }

            // Count this frame toward the active scene's quota; once it's rendered enough,
            // score it (new GL errors + draw count) and advance to the next scene.
            if (smokeTestMode && smokeSceneActive) {
                ++smokeFramesRendered;
                if (smokeFramesRendered >= kSmokeTestFrames) {
                    int newErrors    = GLDebug::ErrorCount() - smokeBaselineGlErrors;
                    int newLogErrors = Log::CountOf(LogLevel::Error) - smokeBaselineLogErrors;
                    int drawCalls    = editor.GetRenderStats().DrawCalls;
                    // A scene is only viable end-to-end if it loaded, introduced no new GL or
                    // log errors across load+render, and actually drew something (audit #356).
                    std::string cause;
                    if (!smokeSceneLoadOk)   cause += "Load() failed; ";
                    if (newLogErrors > 0)    cause += std::to_string(newLogErrors) + " new log error(s); ";
                    if (newErrors > 0)       cause += std::to_string(newErrors) + " new GL error(s); ";
                    if (drawCalls <= 0)      cause += "zero draw calls; ";
                    bool pass = cause.empty();
                    const std::string& path = smokeScenePaths[smokeSceneIndex];
                    smokeResults.push_back({path, smokeFramesRendered, newErrors, newLogErrors,
                                            drawCalls, smokeSceneLoadOk, pass, cause});
                    std::cout << "[SmokeTest] " << (pass ? "PASS" : "FAIL") << "  "
                              << std::filesystem::path(path).filename().string()
                              << "  frames=" << smokeFramesRendered
                              << " loadOk=" << (smokeSceneLoadOk ? 1 : 0)
                              << " newGlErrors=" << newErrors
                              << " newLogErrors=" << newLogErrors
                              << " drawCalls=" << drawCalls
                              << (pass ? "" : ("  cause: " + cause)) << std::endl;
                    ++smokeSceneIndex;
                    smokeSceneActive = false;
                }
            }
        }

        if (smokeTestMode) {
            // Deliberately skip the normal exit path entirely (no play-mode revert, no
            // save-on-exit) — smoke-tested scenes were never really "open" from the user's
            // point of view and must not get written back to disk.
            std::cout << "\n[SmokeTest] ==== Summary ====" << std::endl;
            // No scenes discovered is itself a failure — an empty scene folder must not read
            // as a green run (audit #356 / TEST-101).
            bool allPassed = !smokeResults.empty() && smokeScenePaths.size() == smokeResults.size();
            for (const auto& r : smokeResults) {
                std::cout << "[SmokeTest] " << (r.pass ? "PASS" : "FAIL") << "  "
                          << std::filesystem::path(r.scenePath).filename().string()
                          << "  frames=" << r.frames
                          << " loadOk=" << (r.loadOk ? 1 : 0)
                          << " newGlErrors=" << r.newGlErrors
                          << " newLogErrors=" << r.newLogErrors
                          << " drawCalls=" << r.drawCalls
                          << (r.pass ? "" : ("  cause: " + r.cause)) << std::endl;
                if (!r.pass) allPassed = false;
            }
            std::cout << "[SmokeTest] " << smokeResults.size() << " scene(s) - "
                      << (allPassed ? "ALL PASSED" : "FAILURES DETECTED") << std::endl;
            editor.Shutdown();
            editorModule.Shutdown();
            AudioEngine::Shutdown();
            return allPassed ? 0 : 1;
        }

        // Closing mid-play would otherwise auto-save the transient play state — revert to the
        // snapshot first, same as pressing Stop.
        if (playing) editor.OnExitPlayMode(world, assets);
        // An untitled scene (File > New Scene, never Saved As) has no path — do NOT write it
        // anywhere on exit, or it would overwrite whatever scene.json last held. The user has to
        // explicitly Save As to give it a home.
        if (!exitSkipFinalSave && !editor.CurrentScenePath().empty()) {
            SceneSerializer::Save(world, assets, editor.CurrentScenePath());
        }

        editor.Shutdown();
        editorModule.Shutdown();
        AudioEngine::Shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        // Non-interactive runs (--smoke-test / --resave) must never block: a modal MessageBox on
        // a headless or CI desktop is never dismissed and the process hangs until the job's
        // timeout (audit BUG-102). stderr + a nonzero exit is the whole contract there.
        if (!headless) {
            // Release builds use the GUI subsystem (see CMakeLists), so there's no console for
            // the message above to land in — without this, a failure to start would just look
            // like the engine silently doing nothing.
            Window::ShowFatalErrorDialog(std::string("Tartarus Engine failed to start.\n\n") + e.what());
        }
        return 1;
    }

    return 0;
}
