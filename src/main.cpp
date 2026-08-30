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
#include "Model.h"
#include "SceneSerializer.h"
#include "Grid.h"
#include "Sky.h"
#include "ModelShaderSource.h"
#include "TintOverlayRenderer.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include "Frustum.h"
#include "GameViewPanel.h"
#include "ProjectPaths.h"
#include "SplashScreen.h"
#include "GLDebug.h"
#include "Log.h"

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

// Selection outline (editor-only): the classic "inverted hull" technique — draw the object
// again, offset a little along its normal, with front-face culling so only the silhouette
// peeking out from behind the normal draw survives. One flat fragment shader shared by both
// variants below; only the vertex stage differs, matching each mesh's own attribute layout.
static const char* kOutlineFragmentSrc = R"(
#version 330 core
out vec4 FragColor;
uniform vec3 uOutlineColor;
void main() {
    FragColor = vec4(uOutlineColor, 1.0);
}
)";

// Mirrors kModelVertexSrc's skinning block so an animated model's outline deforms with it,
// then offsets along the (skinned) normal instead of computing UV/TBN — outline doesn't need them.
static const char* kOutlineModelVertexSrc = R"(
#version 330 core
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
#version 330 core
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
#version 330 core
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
        unsigned int fsQuadVao = 0;
        glGenVertexArrays(1, &fsQuadVao); // attribute-less: positions come from gl_VertexID
        TintOverlayRenderer tintOverlay;
        Grid grid;
        Sky sky;

        World world;
        Player player;
        player.Cam.Position = glm::vec3(0, 2.0f, 0);

        // Resolved under the project folder (see ProjectPaths.h) rather than the working
        // directory, so the scene being edited lives alongside the source instead of inside
        // build/, where it was gitignored and a clean rebuild would delete it.
        const std::string scenePath = ProjectPaths::Resolve("scene.json");
        AssetLibrary assets;
        bool sceneLoaded = SceneSerializer::Load(world, assets, scenePath);
        if (sceneLoaded) {
            std::cout << "Loaded scene from " << scenePath << std::endl;
        }

        EditorLayer editor;
        editor.Init(window.Handle());

        // One-shot rig dump: OS, CPU, RAM, GPU, driver, display, build. Mirrored to the Console
        // on launch (audit #82) AND kept as a block for Preferences > About (each line goes to
        // both via report()).
        std::vector<std::string> sysReport;
        {
            auto report = [&](const std::string& s) { sysReport.push_back(s); Log::Info(s); };
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
        Framebuffer sceneFramebuffer;
        Framebuffer selectionMaskFbo; // 1-bit silhouette mask for the screen-space selection outline

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
        // Open framed on the scene instead of staring at empty space beside it (audit #87).
        editor.FrameSceneBounds(world, editorCamera);
        window.SetCursorLocked(false);

        // OS-level drag-and-drop (e.g. dragging a file in from Windows Explorer) — routes
        // through the same import logic as File > Import, filed into whichever Asset Browser
        // folder is currently open. `editorUIVisible` is captured by reference since GLFW invokes
        // this from inside PollEvents(), by which point the loop below may have flipped it.
        window.SetDropCallback([&](const std::vector<std::string>& paths) {
            editor.HandleDroppedFiles(world, assets, editorCamera, editorUIVisible, paths);
        });

        bool firstFramePresented = false; // gates the splash -> editor handoff at the loop's end
        bool prevF1 = false;
        bool prevF11 = false;
        bool prevEscape = false;

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

        while (true) {
            Clock::Update();
            float dt = Clock::DeltaTime();
            Profiler::BeginFrame();
            GLStateCache::ResetFrameStats();

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
            const std::string& scenePath = editor.CurrentScenePath();
            bool dirty = editor.IsDirty();
            if (!titleInitialized || scenePath != lastScenePath || dirty != lastDirty) {
                std::string sceneName = std::filesystem::path(scenePath).filename().string();
                if (sceneName.empty()) sceneName = "Untitled"; // File > New Scene: no path yet
                std::string desiredTitle = "Tartarus Engine \xE2\x80\x94 " + sceneName +
                    (dirty ? "*" : "");
                window.SetTitle(desiredTitle);
                lastScenePath = scenePath;
                lastDirty = dirty;
                titleInitialized = true;
            }

            bool f1Now = Input::IsKeyDown(GLFW_KEY_F1);
            if (f1Now && !prevF1) togglePlay();
            prevF1 = f1Now;

            bool f11Now = Input::IsKeyDown(GLFW_KEY_F11);
            if (f11Now && !prevF11) {
                window.ToggleFullscreen();
            }
            prevF11 = f11Now;

            if (playing) {
                bool escNow = Input::IsKeyDown(GLFW_KEY_ESCAPE);
                if (escNow && !prevEscape) {
                    if (playMaximized) {
                        // Maximized play: Esc toggles the cursor, same as the old Play mode.
                        window.SetCursorLocked(!window.IsCursorLocked());
                    } else if (gameInputEngaged) {
                        // In-panel play: Esc hands control back to the editor.
                        gameInputEngaged = false;
                        window.SetCursorLocked(false);
                    }
                }
                prevEscape = escNow;

                // Safety net: if the window loses focus while the game has grabbed the cursor
                // (alt-tab, a notification steals focus), release it — otherwise you can come
                // back to a captured view with a hidden cursor and no obvious way out.
                if (gameInputEngaged && !glfwGetWindowAttrib(window.Handle(), GLFW_FOCUSED)) {
                    gameInputEngaged = false;
                    window.SetCursorLocked(false);
                }
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
                UpdateEditorCamera(editorCamera, dt,
                    !editor.WantsCaptureMouse() && !editor.GizmoEngaged() && !gameHasInput,
                    hasSelection ? &selectionCenter : nullptr);
            }

            // Simulate the player whenever playing. When the game doesn't have input (in-panel
            // play, not yet clicked in), the body still falls/rests — it just doesn't walk or
            // look (see Player::Update's readInput).
            if (playing) {
                player.Update(dt, world, window.Handle(), gameHasInput);
            }

            // Animations advance whenever something is showing them: the editor viewport, or the
            // running game.
            if (editorUIVisible || playing) {
                PROFILE_SCOPE("Animation Update");
                for (auto entity : world.Registry.view<RenderableComponent>()) {
                    world.Registry.get<RenderableComponent>(entity).ModelRef->UpdateAnimation(dt);
                }
            }

            if (!editorUIVisible) {
                // Maximized play: editor.Draw() (and the real DockSpace() call inside it) doesn't
                // run, so without SOME per-frame DockSpace() call ImGui silently undocks every
                // window in that tree the instant it's submitted again on Restore/Stop. See
                // KeepDockspaceAlive's own comment for the full story.
                editor.KeepDockspaceAlive();
            }

            glm::vec3 lightDir(-0.4f, -1.0f, -0.3f);

            // Renders the lit scene (sky + every Transform+Renderable entity) into whatever
            // framebuffer/viewport is currently bound. Shared by the real on-screen pass below
            // and GameViewPanel's offscreen framebuffer pass, so the two can never silently
            // diverge. Editor-only visualization (selection outline/highlight, drag-preview
            // ghost, grid, wireframe) is deliberately NOT part of this — the Game View should
            // show exactly what Play Mode does, never editor debug shading.
            auto drawScene = [&](const glm::mat4& sceneView, const glm::mat4& sceneProj,
                                  const glm::vec3& viewPos, bool unlit, EditorLayer::RenderStats* outStats) {
                sky.Draw(sceneView, sceneProj, world.SkyHorizonColor, world.SkyZenithColor);

                modelShader.Bind();
                modelShader.SetMat4("uView", sceneView);
                modelShader.SetMat4("uProj", sceneProj);
                modelShader.SetVec3("uLightDir", lightDir);
                modelShader.SetVec3("uLightColor", glm::vec3(3.0f));
                modelShader.SetVec3("uViewPos", viewPos);

                // Gather every active LightComponent into the shader's fixed-size arrays.
                // Inactive entities are skipped so the Hierarchy's eye toggle turns a light off
                // for real.
                int pointLightCount = 0;
                for (auto entity : world.Registry.view<TransformComponent, LightComponent>()) {
                    if (pointLightCount >= kMaxPointLights) break;
                    if (world.Registry.all_of<InactiveTag>(entity)) continue;

                    const auto& light = world.Registry.get<LightComponent>(entity);
                    glm::mat4 lightModel = world.ComposeWorldTransform(entity);

                    char name[64];
                    snprintf(name, sizeof(name), "uPointLightPos[%d]", pointLightCount);
                    modelShader.SetVec3(name, glm::vec3(lightModel[3]));
                    snprintf(name, sizeof(name), "uPointLightColor[%d]", pointLightCount);
                    modelShader.SetVec3(name, light.Color * light.Intensity);
                    snprintf(name, sizeof(name), "uPointLightRange[%d]", pointLightCount);
                    modelShader.SetFloat(name, light.Range);

                    // A spot aims along its own -Z (the usual "forward" convention), so rotating
                    // the entity aims the cone. -1 in the cutoff slot marks an omnidirectional
                    // point light.
                    snprintf(name, sizeof(name), "uPointLightDir[%d]", pointLightCount);
                    modelShader.SetVec3(name, glm::normalize(glm::vec3(lightModel * glm::vec4(0, 0, -1, 0))));
                    snprintf(name, sizeof(name), "uPointLightCosCutoff[%d]", pointLightCount);
                    modelShader.SetFloat(name, light.Kind == LightComponent::Type::Spot
                        ? cosf(glm::radians(light.SpotAngleDegrees)) : -1.0f);

                    pointLightCount++;
                }
                modelShader.SetInt("uPointLightCount", pointLightCount);
                modelShader.SetInt("uUnlit", unlit ? 1 : 0);

                // One draw loop for everything placed in the world — former level-geometry
                // boxes render through the exact same PBR path as imported/primitive models now,
                // since both are just entities with a Transform + Renderable.
                EditorLayer::RenderStats localStats;
                localStats.PointLights = pointLightCount;
                Frustum camFrustum = Frustum::FromViewProj(sceneProj * sceneView);
                { // scope limits PROFILE_SCOPE to just this loop, not the rest of the frame
                PROFILE_SCOPE("Scene Draw");
                for (auto entity : world.Registry.view<TransformComponent, RenderableComponent>()) {
                    if (world.Registry.all_of<InactiveTag>(entity)) continue; // Hierarchy eye toggle / GameObject active
                    auto& renderable = world.Registry.get<RenderableComponent>(entity);
                    glm::mat4 model = world.ComposeWorldTransform(entity);

                    // Frustum culling: skip the draw call entirely for anything outside the
                    // camera's view. Bounds come from the same Model::BoundsMin/Max already used
                    // for gizmo framing and Snap to Ground - an invalid (never-populated, e.g. a
                    // failed import) bounds pair draws unconditionally rather than risk hiding it.
                    glm::vec3 boundsMin = renderable.ModelRef->BoundsMin();
                    glm::vec3 boundsMax = renderable.ModelRef->BoundsMax();
                    bool validBounds = boundsMin.x <= boundsMax.x && boundsMin.y <= boundsMax.y && boundsMin.z <= boundsMax.z;
                    if (validBounds) {
                        AABB worldBounds = AABB{boundsMin, boundsMax}.Transformed(model);
                        if (!camFrustum.Intersects(worldBounds)) {
                            localStats.Culled++;
                            continue;
                        }
                    }

                    modelShader.SetMat4("uModel", model);
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
            if (editorUIVisible) {
                PROFILE_SCOPE("Scene View Render");
                glm::vec2 available = editor.GetLastSceneContentRegion();
                if (available.x < 1.0f || available.y < 1.0f) {
                    available = {(float)window.GetWidth(), (float)window.GetHeight()};
                }
                int scW = std::max((int)available.x, 1);
                int scH = std::max((int)available.y, 1);

                sceneFramebuffer.Resize(scW, scH);
                sceneFramebuffer.Bind();
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
                if (!selection.empty()) {
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
                        xforms.push_back(world.ComposeWorldTransform(entity));
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
                        sceneFramebuffer.Bind();
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

                if (editor.ShowGrid()) {
                    // The grid shader fades lines by literal world-space distance from the
                    // camera. That distance is meaningless in orthographic mode — scroll-zoom
                    // there resizes OrthoHalfHeight without moving the camera (see
                    // UpdateEditorCamera), so "how zoomed in you are" and "how far the camera
                    // physically sits" are decoupled, and a fixed fade radius would fade the grid
                    // out at high zoom for no visual reason. Orthographic views don't need the
                    // radial fade anyway (no perspective depth cue to blend into), so just push
                    // it out far enough to never kick in.
                    float gridFade = editorCamera.Orthographic ? 100000.0f : 80.0f;
                    grid.Draw(sceneViewMat, sceneProjMat, editorCamera.Position, editor.GridMinorSpacing(), 10.0f, gridFade);
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
            // Rendered whenever the Game panel is on screen (editor UI visible, so its docked tab
            // has something current to show) or maximized play has a locked, non-Free aspect/
            // resolution selected (so the letterboxed blit further down has a source). Free-Aspect
            // maximized play never needs this — the normal full-window pass below already IS a
            // free-aspect render, so skip the extra work entirely.
            bool playMaxLocked = playMaximized && gameView.GetCurrentPreset().Mode != AspectRatioMode::FreeAspect;
            GameViewStats gameViewStats{};
            if (editorUIVisible || playMaxLocked) {
                ImVec2 available = editorUIVisible ? gameView.GetLastAvailableRegion() : ImVec2(0.0f, 0.0f);
                if (available.x < 1.0f || available.y < 1.0f) {
                    available = ImVec2((float)window.GetWidth(), (float)window.GetHeight());
                }

                int gvWidth, gvHeight;
                gameView.ComputeTargetSize(available, gvWidth, gvHeight);
                gameView.GetFramebuffer().Resize(gvWidth, gvHeight);
                gameView.GetFramebuffer().Bind();
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
                // Free-Aspect maximized play: render straight to the backbuffer at the window's
                // own native aspect.
                glViewport(0, 0, window.GetWidth(), window.GetHeight());
                float aspect = (float)window.GetWidth() / (float)window.GetHeight();
                glm::mat4 view = gameCam->ViewMatrix();
                glm::mat4 proj = gameCam->ProjectionMatrix(aspect);
                EditorLayer::RenderStats stats;
                drawScene(view, proj, gameCam->Position, /*unlit=*/false, &stats);
                editor.SetRenderStats(stats);
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

            window.SwapBuffers();

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
