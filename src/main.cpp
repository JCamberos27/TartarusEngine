#include "Window.h"
#include "Input.h"
#include "Clock.h"
#include "Shader.h"
#include "Camera.h"
#include "Player.h"
#include "World.h"
#include "gl.h"

#include "AudioEngine.h"
#include "AssetLibrary.h"
#include "EditorLayer.h"
#include "EditorSettings.h"
#include "Model.h"
#include "SceneSerializer.h"
#include "AnimationSystem.h"
#include "Grid.h"
#include "Sky.h"
#include "ModelShaderSource.h"
#include "TintOverlayRenderer.h"
#include "HdrTarget.h"
#include "Screenshot.h"
#include "Tonemapper.h"
#include "LightBuffer.h"
#include "ClusterGrid.h"
#include "ClusterShaderSource.h"
#include "CascadedShadowMap.h"
#include "SpotShadowMap.h"
#include "PointShadowMap.h"
#include "IblProbe.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include "Frustum.h"
#include "GameViewPanel.h"
#include "ProjectPaths.h"
#include "SplashScreen.h"
#include "GLDebug.h"
#include "Log.h"
#include "TextureCache.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <filesystem>
#include <string>
#include <algorithm>
#include <cmath>
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
static void UpdateEditorCamera(Camera& cam, float dt, bool allowLook, const glm::vec3* orbitPivot) {
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
        float speed = 8.0f * dt * (Input::IsKeyDown(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
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
    if (allowLook) {
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

int main() {
    try {
        // Up before anything else so it covers the whole startup, including the GL context
        // creation and shader compiles below. The main window stays hidden until its first
        // frame is presented (see Window::Show), so the two never overlap.
        SplashScreen splash;
        splash.Show("assets/branding/splash.png", 1.0f);

        Window window(1280, 720, "Tartarus Engine");
        // GL context + loader are live now. No-op unless a Debug build or TARTARUS_GL_DEBUG=1.
        GLDebug::Init();
        Input::Init(window.Handle());
        window.SetCursorLocked(true);
        window.Maximize(); // opens maximized (not true fullscreen, no monitor video-mode switch); F11 still enters fullscreen

        AudioEngine::Init();

        Shader modelShader(kModelVertexSrc, kModelFragmentSrc);
        Shader outlineModelShader(kOutlineModelVertexSrc, kOutlineFragmentSrc);
        Shader outlineDilateShader(kOutlineDilateVertSrc, kOutlineDilateFragSrc);
        Shader shadowShader(kShadowDepthVertexSrc, kShadowDepthFragmentSrc);
        Shader localShadowShader(kShadowDepthVertexSrc, kLocalShadowDepthFragmentSrc); // spot/point: linear depth
        Shader clusterBuildShader(kBuildClustersCompute); // #120: per-view froxel AABBs
        Shader clusterCullShader(kCullLightsCompute);     // #120: point/spot lights -> froxel lists
        unsigned int fsQuadVao = 0;
        glGenVertexArrays(1, &fsQuadVao); // attribute-less: positions come from gl_VertexID
        TintOverlayRenderer tintOverlay;
        Grid grid;
        Sky sky;
        IblProbe iblProbe; // #196: sky-baked irradiance / prefiltered specular / BRDF LUT

        World world;
        Player player;
        // Default spawn/editor-camera start: Room 2 (Lighting Test) of the Development scene,
        // facing its cluster of orbiting, colour-cycling lights - the busiest, most immediately
        // legible part of the scene rather than an arbitrary point that might sit outside any
        // room on a future layout change.
        player.Cam.Position = glm::vec3(18.0f, 2.0f, 6.0f);
        player.Cam.Yaw = -90.0f;   // faces -Z, toward the room's centre
        player.Cam.Pitch = 15.0f;  // tilted up toward the lights (they orbit up to y=7.5)

        // Resolved under the project folder (see ProjectPaths.h) rather than the working
        // directory, so the scene being edited lives alongside the source instead of inside
        // build/, where it was gitignored and a clean rebuild would delete it. Prefer the scene
        // that was open when the editor last closed, if it still exists (#95).
        EditorSettings::Load();
        std::string scenePath = ProjectPaths::Resolve("scenes/Showcase.json");
        {
            const std::string& last = EditorSettings::Get().LastScenePath;
            std::error_code sceneEc;
            if (!last.empty() && std::filesystem::exists(last, sceneEc) && !sceneEc)
                scenePath = last;
        }
        AssetLibrary assets;
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
        Tonemapper tonemapper;
        LightBuffer lightBuffer; // scene lights -> std430 SSBO the model shader reads at binding 0
        ClusterGrid clusterGrid; // #120: froxel light lists, rebuilt per rendered view each frame
        CascadedShadowMap shadowMap; // directional-sun CSM; depth array sampled by the model shader
        SpotShadowMap spotShadowMap; // perspective depth per shadow-casting spot light (#119)
        PointShadowMap pointShadowMap; // depth cube per shadow-casting point light (#119)

        Camera editorCamera;
        // Play is no longer a whole-screen mode swap. Three independent bits describe the state:
        //   editorUIVisible - the editor panels + dockspace are shown (true whenever NOT maximized)
        //   playing         - the scene is simulating; the Game panel renders from the player camera
        //   playMaximized   - the Game view fills the window and the editor panels are hidden
        // Pressing Play just flips `playing`; the game runs inside the docked Game panel with the
        // editor still fully usable. The Fullscreen button next to Stop toggles `playMaximized`.
        bool editorUIVisible = true;
        bool playing = false;
        bool playMaximized = false;
        // In-panel play is click-to-focus: the game only reads mouse/keyboard once the user has
        // clicked inside the Game view. Esc (or Stop) hands control back to the editor. Ignored
        // while maximized — that path uses the cursor-lock state directly, like the old Play mode.
        bool gameInputEngaged = false;
        editorCamera.Position = player.Cam.Position;
        editorCamera.Yaw = player.Cam.Yaw;
        editorCamera.Pitch = player.Cam.Pitch;
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
        auto startPlay = [&]() {
            if (playing) return;
            editor.OnEnterPlayMode(world);
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
            window.SetCursorLocked(false); // click the Game view to take control
        };
        auto stopPlay = [&]() {
            if (!playing) return;
            editor.OnExitPlayMode(world, assets);
            playing = false;
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

        while (true) {
            ++frameIndex;
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

            if (Input::IsKeyPressed(GLFW_KEY_F1)) togglePlay();

            if (Input::IsKeyPressed(GLFW_KEY_F11)) {
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

                UpdateEditorCamera(editorCamera, dt, allowLook || camDragActive,
                    hasSelection ? &selectionCenter : nullptr);
            } else if (camDragActive) {
                camDragActive = false; // dropped into maximized play mid-drag; it owns the cursor now
            }

            // Simulate the player whenever playing. When the game doesn't have input (in-panel
            // play, not yet clicked in), the body still falls/rests — it just doesn't walk or
            // look (see Player::Update's readInput).
            // Reap voices that have finished so repeated Play/Stop cycles don't accumulate
            // ma_sound objects and open file handles (#200).
            AudioEngine::Update();

            if (playing) {
                player.Update(dt, world, window.Handle(), gameHasInput);
                // The Play-mode camera is the ears: positional sources (#201) attenuate and pan
                // against wherever the player is looking from, updated after the move so the
                // listener matches the frame that's about to be rendered.
                AudioEngine::SetListener(player.Cam.Position, player.Cam.Front(), player.Cam.Up());
                // Procedural spin/orbit/bob/light-hue. Play-only: edit mode keeps the authored
                // pose, and the play-mode snapshot restores everything this touched on Stop.
                UpdateAnimators(world, dt);
            }

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
            // No phantom fallback light here: turning every light off (or deleting the sun) now
            // means the scene really is unlit — ambient only. `SceneSerializer` already
            // synthesises a real, visible, deletable "Directional Light" entity when a sun-less
            // scene is opened, so a fresh scene still lights up; re-injecting an invisible one
            // every frame just overrode the user when they deliberately switched it off.
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
                        renderable.ModelRef->DrawDepthOnly(shadowShader);
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
                        r.ModelRef->DrawDepthOnly(localShadowShader);
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
                            r.ModelRef->DrawDepthOnly(localShadowShader);
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
            if (iblProbe.NeedsBake(world.SkyHorizonColor, world.SkyZenithColor)) {
                PROFILE_SCOPE("IBL Bake");
                PROFILE_GPU_SCOPE("IBL Bake");
                iblProbe.Bake(world.SkyHorizonColor, world.SkyZenithColor);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, window.GetWidth(), window.GetHeight());
                GLStateCache::Invalidate();
            }

            // Renders the lit scene (sky + every Transform+Renderable entity) into whatever
            // framebuffer/viewport is currently bound. Shared by the real on-screen pass below
            // and GameViewPanel's offscreen framebuffer pass, so the two can never silently
            // diverge. Editor-only visualization (selection outline/highlight, drag-preview
            // ghost, grid, wireframe) is deliberately NOT part of this — the Game View should
            // show exactly what Play Mode does, never editor debug shading.
            auto drawScene = [&](const glm::mat4& sceneView, const glm::mat4& sceneProj,
                                  const glm::vec3& viewPos, bool unlit, EditorLayer::RenderStats* outStats) {
                const EditorSettings& gs = EditorSettings::Get();

                // Shadows for this view. Spot and point shadows only need shadows-enabled +
                // a lit pass; the cascaded SUN shadow additionally needs its once-per-frame
                // pass to have actually run (which requires a directional light). These used to
                // share one flag, so deleting the sun silently killed spot/point shadows too.
                bool shadowsOn = gs.ShadowsEnabled && !unlit;
                bool sunShadowsOn = shadowsOn && sunShadowsReady;

                sky.Draw(sceneView, sceneProj, world.SkyHorizonColor, world.SkyZenithColor);

                modelShader.Bind();
                modelShader.SetMat4("uView", sceneView);
                modelShader.SetMat4("uProj", sceneProj);
                modelShader.SetVec3("uViewPos", viewPos);

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
                    std::clamp(frameSunAngularDeg * 3.0f, 1.0f, 14.0f) * frameSunShadowSoftness);
                // Per-light sun shadow multipliers (#140 phase 2); 1.0 == pre-phase-2 output.
                modelShader.SetFloat("uSunShadowBias", frameSunShadowBias);
                modelShader.SetFloat("uSunShadowNormalBias", frameSunShadowNormalBias);
                glActiveTexture(GL_TEXTURE0 + 8);
                glBindTexture(GL_TEXTURE_2D_ARRAY, shadowMap.DepthArray());
                modelShader.SetInt("uShadowMap", 8);
                modelShader.SetFloat("uShadowMapResolution", (float)shadowMap.Resolution()); // #190

                // Spot-light shadow maps on unit 9 (#119). Count is zeroed when shadows are off
                // or in Unlit so the shader's SpotShadow() early-outs.
                int spotCountForView = shadowsOn ? spotShadowCount : 0;
                modelShader.SetInt("uSpotShadowCount", spotCountForView);
                if (spotCountForView > 0)
                    modelShader.SetMat4Array("uSpotShadowVP[0]", spotCountForView, spotShadowVP);
                for (int s = 0; s < spotCountForView; ++s) {
                    modelShader.SetVec3("uSpotShadowPos[" + std::to_string(s) + "]", spotShadowPos[s]);
                    modelShader.SetFloat("uSpotShadowFar[" + std::to_string(s) + "]", spotShadowFar[s]);
                    modelShader.SetFloat("uSpotShadowHalfTan[" + std::to_string(s) + "]", spotShadowHalfTan[s]);
                    modelShader.SetFloat("uSpotShadowBias[" + std::to_string(s) + "]", spotShadowBias[s]);
                    modelShader.SetFloat("uSpotShadowNormalBias[" + std::to_string(s) + "]", spotShadowNormalBias[s]);
                    modelShader.SetFloat("uSpotShadowSoftness[" + std::to_string(s) + "]", spotShadowSoftness[s]);
                }
                glActiveTexture(GL_TEXTURE0 + 9);
                glBindTexture(GL_TEXTURE_2D_ARRAY, spotShadowMap.DepthArray());
                modelShader.SetInt("uSpotShadowMap", 9);
                modelShader.SetFloat("uSpotShadowMapResolution", (float)spotShadowMap.Resolution()); // #190

                // Point-light cube shadow maps on unit 10 (#119).
                int pointCountForView = shadowsOn ? pointShadowCount : 0;
                modelShader.SetInt("uPointShadowCount", pointCountForView);
                for (int s = 0; s < pointCountForView; ++s) {
                    modelShader.SetFloat("uPointShadowFar[" + std::to_string(s) + "]", pointShadowFar[s]);
                    modelShader.SetFloat("uPointShadowBias[" + std::to_string(s) + "]", pointShadowBias[s]);
                    modelShader.SetFloat("uPointShadowNormalBias[" + std::to_string(s) + "]", pointShadowNormalBias[s]);
                }
                glActiveTexture(GL_TEXTURE0 + 10);
                glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, pointShadowMap.DepthCubeArray());
                modelShader.SetInt("uPointShadowMap", 10);
                modelShader.SetFloat("uPointShadowMapResolution", (float)pointShadowMap.Resolution()); // #190

                // IBL probes on units 11/12/13 (#196). The shader declares those units with
                // layout(binding=) qualifiers, so there's no SetInt here — just the bind. Unlit
                // mode skips lighting entirely, so it doesn't need them either.
                bool iblOn = iblProbe.IsValid() && !unlit;
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

                // The light SSBO (binding 0) is built once per frame above — just bind it.
                lightBuffer.Bind(0);

                // Clustered-forward light culling (#120). Dice this view's frustum and assign
                // every point/spot light to the froxels it reaches, so the fragment shader loops
                // a short per-froxel list instead of all uLightCount lights. Perspective views
                // only — the froxel AABB build assumes rays from the eye, so an orthographic
                // editor view (not perf-critical) falls back to the full loop.
                GLint vp[4] = {0, 0, 0, 0};
                glGetIntegerv(GL_VIEWPORT, vp);
                bool perspective = std::abs(sceneProj[3][3]) < 0.5f; // proj[3][3] == 1 for ortho
                bool clusterOn = perspective && !unlit && vp[2] > 0 && vp[3] > 0;
                if (clusterOn) {
                    PROFILE_GPU_SCOPE("Cluster Cull");
                    float nearZ = std::abs(sceneProj[3][2] / (sceneProj[2][2] - 1.0f));
                    float farZ  = std::abs(sceneProj[3][2] / (sceneProj[2][2] + 1.0f));
                    clusterGrid.Cull(clusterBuildShader, clusterCullShader, sceneView, sceneProj,
                                     nearZ, farZ, vp[2], vp[3]);
                    modelShader.Bind(); // Cull() left a compute program bound
                    clusterGrid.BindForShading();
                    modelShader.SetVec2("uClusterScreenSize", glm::vec2((float)vp[2], (float)vp[3]));
                    modelShader.SetVec4("uClusterZParams", ClusterGrid::ZParams(nearZ, farZ));
                }
                modelShader.SetInt("uClusterEnabled", clusterOn ? 1 : 0);

                modelShader.SetInt("uUnlit", unlit ? 1 : 0);
                // Real scene path: emit linear HDR; the shared Tonemapper pass maps it after
                // MSAA resolve. (Offscreen model thumbnails set this to 1 to self-tonemap.)
                modelShader.SetInt("uApplyTonemap", 0);

                // One draw loop for everything placed in the world — former level-geometry
                // boxes render through the exact same PBR path as imported/primitive models now,
                // since both are just entities with a Transform + Renderable.
                EditorLayer::RenderStats localStats;
                localStats.PointLights = std::max(0, frameLightCount - 1); // minus the directional sun
                localStats.LightBufferOverflowed = lightBuffer.Overflowed(); // #204
                localStats.ClusterSaturated = clusterOn && clusterGrid.Saturated(); // #204
                Frustum camFrustum = Frustum::FromViewProj(sceneProj * sceneView);
                { // scope limits PROFILE_SCOPE to just this loop, not the rest of the frame
                PROFILE_SCOPE("Scene Draw");
                PROFILE_GPU_SCOPE("Scene Draw"); // shared by both Scene-tab and Game-tab draws
                // #194: resolve once, outside the per-entity loop below.
                int modelModelLoc = modelShader.Loc("uModel");
                int modelNormalMatrixLoc = modelShader.Loc("uNormalMatrix");
                for (auto entity : world.Registry.view<TransformComponent, RenderableComponent>()) {
                    if (world.Registry.all_of<InactiveTag>(entity)) continue; // Hierarchy eye toggle / GameObject active
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

                    modelShader.SetMat4(modelModelLoc, model);
                    // Normal matrix (inverse-transpose) computed here, not per-vertex (#104).
                    modelShader.SetMat4(modelNormalMatrixLoc,
                        glm::mat4(glm::transpose(glm::inverse(glm::mat3(model)))));
                    renderable.ModelRef->Draw(modelShader);

                    localStats.DrawCalls += renderable.ModelRef->MeshCount();
                    localStats.Triangles += (int)renderable.ModelRef->TriangleCount();
                    localStats.Vertices += (int)renderable.ModelRef->VertexCount();
                }
                } // end "Scene Draw" profile scope
                if (outStats) *outStats = localStats;
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
                if (sceneWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

                EditorLayer::RenderStats sceneStats;
                drawScene(sceneViewMat, sceneProjMat, editorCamera.Position, sceneUnlit, &sceneStats);

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

                // Resolve MSAA + tonemap the linear-HDR scene into the LDR texture the Scene tab
                // displays (exposure -> curve -> gamma, once, in Tonemapper).
                sceneHdr.ResolveTo();
                {
                PROFILE_GPU_SCOPE("Tonemap");
                tonemapper.Apply(sceneHdr.ResolvedColorTexture(), sceneFramebuffer.Handle(), scW, scH,
                                 EditorSettings::Get().ExposureEV,
                                 (Tonemapper::Operator)EditorSettings::Get().TonemapOperator);
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
            }

            // The OS title bar is gone — the toolbar's empty area is the window drag handle.
            // Report it to the Window so its WM_NCHITTEST can hand that region to Windows as the
            // caption (drag + Aero-snap + double-click maximize). False whenever the editor UI is
            // hidden (maximized play) so the game view never becomes draggable.
            window.SetTitleBarDragActive(editorUIVisible && editor.WantsWindowDrag());

            // Drawn every frame in every state (unlike editor.Draw(), which is editor-UI-only) so
            // there's always an on-screen Play/Stop, not just F1. Drawn AFTER editor.Draw() so it
            // layers on top of the toolbar strip instead of being painted over by it.
            editor.DrawPlayStopButton(playing, playMaximized);
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
                EditorLayer::RenderStats gvRenderStats;
                drawScene(gvView, gvProj, gvEye, /*unlit=*/false, &gvRenderStats);

                gameHdr.ResolveTo();
                {
                PROFILE_GPU_SCOPE("Tonemap");
                tonemapper.Apply(gameHdr.ResolvedColorTexture(), gameView.GetFramebuffer().Handle(),
                                 gvWidth, gvHeight, EditorSettings::Get().ExposureEV,
                                 (Tonemapper::Operator)EditorSettings::Get().TonemapOperator);
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
                EditorLayer::RenderStats stats;
                drawScene(view, proj, gameCam->Position, /*unlit=*/false, &stats);
                editor.SetRenderStats(stats);
                gameHdr.ResolveTo();
                {
                PROFILE_GPU_SCOPE("Tonemap");
                tonemapper.Apply(gameHdr.ResolvedColorTexture(), 0, mw, mh,
                                 EditorSettings::Get().ExposureEV,
                                 (Tonemapper::Operator)EditorSettings::Get().TonemapOperator);
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
        AudioEngine::Shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        // Release builds use the GUI subsystem (see CMakeLists), so there's no console for the
        // message above to land in — without this, a failure to start would just look like the
        // engine silently doing nothing.
        Window::ShowFatalErrorDialog(std::string("Tartarus Engine failed to start.\n\n") + e.what());
        return 1;
    }

    return 0;
}
