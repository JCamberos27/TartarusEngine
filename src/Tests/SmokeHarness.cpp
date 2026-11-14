#include "Tests/SmokeHarness.h"

#include "App/LaunchOptions.h"
#include "App/PlaySession.h"
#include "AI/NpcTest.h"
#include "AssetLibrary.h"
#include "Camera.h"
#include "ClusterGrid.h"
#include "Components.h"
#include "Core/Env.h"
#include "Core/Paths.h"
#include "DecalSystem.h"
#include "EditorLayer.h"
#include "Framebuffer.h"
#include "GLDebug.h"
#include "Log.h"
#include "Model.h"
#include "Openable.h"
#include "LightSwitch.h"
#include "SamplingProfiler.h"
#include "SceneRenderer.h"
#include "SceneSerializer.h"
#include "Tests/Cinematic.h"
#include "Tests/FrameCapture.h"
#include "Tests/SmokeChecks.h"
#include "Window.h"
#include "World.h"
#include "Renderer/MemReport.h"

// sscanf, as main.cpp read it (the file-wide warning off there).
#pragma warning(disable: 4996)

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>

SmokeHarness::SmokeHarness(const LaunchOptions& opt, const SmokeContext& ctx) : m_Opt(opt), m_Ctx(ctx) {
    if (const std::string cams = Env::Str(Env::Var::ShotCameras); !cams.empty() && !opt.smokeShotsDir.empty()) {
        std::stringstream all(cams);
        std::string item;
        while (std::getline(all, item, ';')) {
            const size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            ShotCamera c{item.substr(0, colon), glm::vec3(0.0f), 0.0f, 0.0f};
            if (std::sscanf(item.c_str() + colon + 1, "%f,%f,%f,%f,%f", &c.Position.x, &c.Position.y, &c.Position.z,
                            &c.Yaw, &c.Pitch) == 5)
                m_ShotCameras.push_back(c);
        }
    }
    if (Cinematic::Active() && !opt.smokeShotsDir.empty()) {
        std::string error;
        m_Cinematic = Cinematic::Load(Env::Str(Env::Var::Cinematic), error);
        if (!m_Cinematic) Log::Error("Cinematic: " + error);
    }
    m_ShotCameraStart = Env::IsSet(Env::Var::ShotStart) ? std::max(1, Env::Int(Env::Var::ShotStart, 0)) : 40;
    m_FrameBudget = opt.npcTestMode ? 60 * 60 * 6 : opt.perfBenchMode ? 3 * PerfBench::kPhaseFrames
                  : m_Cinematic ? Cinematic::Warmup() + (int)(Cinematic::Duration() * 60.0f) + 2
                  : !m_ShotCameras.empty() ? m_ShotCameraStart + kShotCameraSettle * (int)m_ShotCameras.size() + 1 : 100;
    m_MemReportDir = Env::Str(Env::Var::MemReport);
    m_OpenAll = Env::Flag(Env::Var::OpenAll);
    m_LightsMode = Env::IsSet(Env::Var::LightsOn) ? (Env::Flag(Env::Var::LightsOn) ? 1 : 0) : -1;
    m_LightsOnly = Env::Str(Env::Var::LightsOnly);

    if (opt.smokeTestMode) {
        // Scene source: explicit --smoke-test <dir> arg, else the engine's committed
        // regression scenes, else the user project's scenes/ (audit TEST-101).
        std::string scenesDir;
        if (!opt.smokeScenesDirArg.empty()) {
            scenesDir = opt.smokeScenesDirArg;
        } else {
            std::string engineScenes = EnginePaths::Resolve("assets/test-scenes");
            std::error_code exEc;
            bool haveEngineScenes = std::filesystem::is_directory(engineScenes, exEc) && !exEc;
            scenesDir = haveEngineScenes ? engineScenes : ProjectPaths::Resolve("scenes");
        }
        std::error_code dirEc;
        if (std::filesystem::is_regular_file(scenesDir, dirEc)) m_ScenePaths.push_back(scenesDir);
        else for (auto& entry : std::filesystem::directory_iterator(scenesDir, dirEc)) {
            if (dirEc) break;
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                m_ScenePaths.push_back(entry.path().string());
        }
        std::sort(m_ScenePaths.begin(), m_ScenePaths.end());
        if (opt.npcTestMode && opt.smokeScenesDirArg.empty())
            m_ScenePaths.assign(1, EnginePaths::Resolve(opt.npcTestScenario == "sandbox" ? "tests/scenes/Arena.json"
                                                          : opt.npcTestScenario == "blood" ? "tests/scenes/BloodTest.json"
                                                          : opt.npcTestScenario == "ragdoll" ? "tests/scenes/RagdollTest.json"
                                                          : opt.npcTestScenario == "revolver" ? "tests/scenes/WeaponLab.json"
                                                          : opt.npcTestScenario == "escape" || opt.npcTestScenario == "escapeshut" || opt.npcTestScenario == "aptwounds" || opt.npcTestScenario == "aptfight" ? "tests/scenes/Apartment Hallway.json"
                                                          : opt.npcTestScenario == "decals" || opt.npcTestScenario == "doors" || opt.npcTestScenario == "roam" || opt.npcTestScenario == "spider" || opt.npcTestScenario == "spiderdeath" || opt.npcTestScenario == "spiderhunt" || opt.npcTestScenario == "spiderplay" || opt.npcTestScenario == "spiderdoor" ? "tests/scenes/Apartment Hallway.json"
                                                                                       : "tests/scenes/Arena.json"));
#pragma warning(suppress : 4996)
        if (const std::string scene = Env::Str(Env::Var::NpcTestScene); opt.npcTestMode && !scene.empty()) { // e.g. doors in the Forest
            // A test scene (tests/scenes/) by that name first, else the project's scenes/.
            const std::string test = EnginePaths::Resolve("tests/scenes/" + scene + ".json");
            std::error_code tsEc;
            m_ScenePaths.assign(1, std::filesystem::exists(test, tsEc) && !tsEc ? test : ProjectPaths::Resolve("scenes/" + scene + ".json"));
        }
        std::cout << "[SmokeTest] Found " << m_ScenePaths.size() << " scene(s) under "
                  << scenesDir << std::endl;
        if (m_ScenePaths.empty()) {
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
}

void SmokeHarness::MemSnapshot(const std::string& label) const {
    if (m_MemReportDir.empty()) return;
    std::vector<const Model*> ms;
    for (const auto& m : m_Ctx.assets.Models()) ms.push_back(m.get());
    for (auto [e, rc] : m_Ctx.world.Registry.view<RenderableComponent>().each())
        if (rc.ModelRef) ms.push_back(rc.ModelRef.get());
    MemReport::Write(m_MemReportDir, label, ms);
}

void SmokeHarness::LoadScene() {
    World& world = m_Ctx.world;
    const std::string& path = m_ScenePaths[m_SceneIndex];
    // Baseline BEFORE the load, so an asset error logged *during* Load() (a
    // missing model/texture) counts against this scene instead of being folded
    // into the baseline and ignored (audit #356).
    m_BaselineGlErrors  = GLDebug::ErrorCount();
    m_BaselineLogErrors = Log::CountOf(LogLevel::Error);
    // false: the smoke-test harness only reads these scenes to render and check
    // them - it must never rewrite a committed regression fixture on disk just
    // from loading it (audit #77; tests/smoke-scenes-invalid/ in particular is
    // documented as static, hand-authored fixtures).
    if (m_SceneIndex == 0) MemSnapshot("00_start");
    m_SceneLoadOk = SceneSerializer::Load(world, m_Ctx.assets, path, /*persistMigration=*/false);
    m_Ctx.warmRenderResources(); // the editor does the same at startup, before its first frame
    std::cout << "[SmokeTest] Loading " << path
              << (m_SceneLoadOk ? "" : "  (Load() reported failure)") << std::endl;
    if (m_SceneLoadOk)
        SmokeChecks::SceneLoaded(world, m_Ctx.assets, path, [this](const std::string& label) { MemSnapshot(label); });
    m_FramesRendered = 0;
    m_SceneActive = true;
    SceneRendererDebug::ResetVariantDrawCounts(); // #354 per-scene variant tally
    m_PlayScene = path.find("smoke_play") != std::string::npos;
    m_PlayCycles = 0;
}

void SmokeHarness::BeginFrame() {
    World& world = m_Ctx.world;
    if (!m_SceneActive) LoadScene();
    const std::string& path = m_ScenePaths[m_SceneIndex];

    // #132 - a smoke_project_sync* scene: the project watcher end to end (Tests/SmokeChecks.cpp).
    if (m_SceneActive && m_FramesRendered == 15 &&
        path.find("smoke_project_sync") != std::string::npos) {
        SmokeChecks::ProjectSync(m_Ctx.editor, world, m_Ctx.assets);
    }

    // #107 - a smoke_materials* scene: every material through the material preview (Tests/SmokeChecks.cpp).
    if (m_SceneActive && m_FramesRendered == 15 &&
        path.find("smoke_materials") != std::string::npos) {
        SmokeChecks::MaterialPreviews(world);
    }

    // smoke_min: the once-per-frame GPU skinning (Model::PreSkin) against the CPU's blend. The committed scenes
    // have no skinned characters, so this is what runs its shader on CI.
    if (m_SceneActive && m_FramesRendered == 15 &&
        path.find("smoke_min") != std::string::npos) {
        std::string preSkinWhy;
        if (Model::PreSkinSelfTest(preSkinWhy)) std::cout << "[SmokeTest]   pre-skin matches the CPU skinning" << std::endl;
        else Log::Error("[SmokeTest] pre-skin: " + preSkinWhy);
    }

    // A smoke_glass* scene: the opt-in Glass editor theme on, drawn, then off (Tests/SmokeChecks.cpp).
    if (m_SceneActive && path.find("smoke_glass") != std::string::npos)
        SmokeChecks::GlassTheme(m_Ctx.editor, world, m_FramesRendered);

    // A smoke_decals* scene (docs/DECALS.md): by frame 80 the textures are resident and the delayed
    // overflow readback has landed. 574 decals in view; the 270-decal stack must trip the per-cluster
    // cap (bit 1) and nothing may fill the index list (bit 2).
    if (m_SceneActive && m_FramesRendered == 80 &&
        path.find("smoke_decals") != std::string::npos) {
        const int visible = m_Ctx.decalSystem.VisibleCount();
        const unsigned overflow = m_Ctx.clusterGrid.DecalOverflow();
        if (visible < 570) Log::Error("[SmokeTest] decals: expected >= 570 visible, got " + std::to_string(visible));
        else if ((overflow & 1u) == 0u) Log::Error("[SmokeTest] decals: the over-cap cluster was not flagged");
        else if (overflow & 2u) Log::Error("[SmokeTest] decals: the decal index list overflowed");
        else std::cout << "[SmokeTest]   decals OK (" << visible << " visible, overflow " << overflow << ")" << std::endl;
    }

    // smoke_decals_stress*: the Scene view held square on the wall, then frames 90 and 91 of the still view
    // read back and compared (Tests/SmokeChecks.cpp).
    if (m_SceneActive && path.find("smoke_decals_stress") != std::string::npos) {
        Camera& editorCamera = m_Ctx.editorCamera;
        editorCamera.Position = glm::vec3(0.0f, 3.0f, 2.5f);
        editorCamera.Yaw = -90.0f;
        editorCamera.Pitch = 0.0f;
        if (m_FramesRendered == 90 || m_FramesRendered == 91)
            SmokeChecks::DecalStress(m_Ctx.clusterGrid, m_Ctx.decalSystem.VisibleCount(), m_FramesRendered);
    }

    // #176 - a smoke_prefab* scene: a Prefab Mode round trip (Tests/SmokeChecks.cpp).
    if (m_SceneActive && m_FramesRendered == 15 &&
        path.find("smoke_prefab") != std::string::npos) {
        SmokeChecks::PrefabMode(m_Ctx.editor, world, m_Ctx.assets);
    }

    // Play-mode cycles for a smoke_play* scene: enter at 20 / 55, stop at 40 / 75,
    // within the frame budget. togglePlay() is the same entry the F1 key
    // and the toolbar button use.
    if (m_SceneActive && m_Ctx.npcTest) {
        if (m_FramesRendered == 20 && !m_Ctx.playing) { std::cout << "[NpcTest] -> Play" << std::endl; m_Ctx.togglePlay(); }
        if (m_Ctx.npcTest->Done()) m_FramesRendered = std::max(m_FramesRendered, m_FrameBudget - 1);
    }
    if (m_SceneActive && m_PlayScene) {
        const int f = m_FramesRendered;
        if ((f == 20 || f == 55) && !m_Ctx.playing) { std::cout << "[SmokeTest]   -> Play\n";  m_Ctx.togglePlay(); }
        if ((f == 40 || f == 75) &&  m_Ctx.playing) {
            SmokeChecks::PlayProbe(world, m_Ctx.playUsesPlayer ? m_Ctx.playerCamera : m_Ctx.playSceneCamera,
                                   m_Ctx.playUsesPlayer);
            std::cout << "[SmokeTest]   -> Stop\n";  m_Ctx.togglePlay(); ++m_PlayCycles;
        }
    }
}

// --smoke-shots with TARTARUS_SHOT_CAMERAS: each camera's capture, and the pose for the next frame.
void SmokeHarness::ShotCameraFrame() {
    Camera& editorCamera = m_Ctx.editorCamera;
    Framebuffer& sceneFramebuffer = m_Ctx.sceneFramebuffer;
    if (m_OpenAll && m_FramesRendered == 1) Openable::SetAll(m_Ctx.world, 1.0f);
    if (m_FramesRendered == 1 && !m_LightsOnly.empty()) {
        LightSwitch::SetAll(m_Ctx.world, false);
        std::stringstream names(m_LightsOnly);
        std::string circuit;
        while (std::getline(names, circuit, ';'))
            if (!circuit.empty() && !LightSwitch::Set(m_Ctx.world, circuit, true))
                Log::Warn("Smoke shots: no light circuit '" + circuit + "'");
    } else if (m_FramesRendered == 1 && m_LightsMode >= 0) {
        if (m_LightsMode == 1) LightSwitch::SetAll(m_Ctx.world, true);
        else LightSwitch::Update(m_Ctx.world, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), false);
    }
    const int shot = (m_FramesRendered - m_ShotCameraStart) / kShotCameraSettle;
    const bool captureFrame = m_FramesRendered >= m_ShotCameraStart &&
                              (m_FramesRendered - m_ShotCameraStart) % kShotCameraSettle == 0 &&
                              shot < (int)m_ShotCameras.size() && sceneFramebuffer.IsValid();
    // Decal maps and substrates decode in the background: a fixed frame count caught the hallway's
    // peeling paint with or without its concrete depending on how the decodes raced. Each capture
    // waits (on the same pose) until they're in, up to kShotStreamHold frames per scene.
    if (m_FramesRendered == 1) m_ShotHold = 0;
    constexpr int kShotStreamHold = 3000; // a big pack (500 maps) takes a few thousand frames of async uploads
    const bool hold = captureFrame && (m_Ctx.decalSystem.Streaming() || m_Ctx.assets.AsyncInFlight() > 0) && m_ShotHold < kShotStreamHold;
    if (hold) {
        ++m_ShotHold;
        --m_FramesRendered; // this frame again next time round
    } else if (captureFrame && m_ShotHold >= kShotStreamHold && (m_Ctx.decalSystem.Streaming() || m_Ctx.assets.AsyncInFlight() > 0)) {
        Log::Warn("Smoke shots: decal textures still streaming after " + std::to_string(kShotStreamHold) +
                  " held frames - captured anyway.");
    }
    if (captureFrame && !hold) {
        const int w = sceneFramebuffer.Width(), h = sceneFramebuffer.Height();
        const std::string stem = std::filesystem::path(m_ScenePaths[m_SceneIndex]).stem().string();
        FrameCapture::WritePngTo(m_Opt.smokeShotsDir, stem + "_" + m_ShotCameras[shot].Name, w, h,
                                 FrameCapture::GrabTopDown(sceneFramebuffer.Handle(), w, h));
    }
    // The pose for the NEXT frame: each camera holds from the previous capture up to its own.
    const int next = m_FramesRendered + 1;
    const int pose = std::clamp((next - 1 - m_ShotCameraStart) / kShotCameraSettle + (next > m_ShotCameraStart ? 1 : 0), 0,
                                (int)m_ShotCameras.size() - 1);
    editorCamera.Position = m_ShotCameras[pose].Position;
    editorCamera.Yaw = m_ShotCameras[pose].Yaw;
    editorCamera.Pitch = m_ShotCameras[pose].Pitch;
    // TARTARUS_SHOT_FOV=<vertical degrees>: match another renderer's camera (Unity reference shots)
    if (Env::IsSet(Env::Var::ShotFov)) editorCamera.Fov = Env::Float(Env::Var::ShotFov, editorCamera.Fov);
}

// TARTARUS_CINEMATIC: the camera path. Warm-up frames hold the first key (and wait for streaming); then each frame
// is written to the video and the next frame's pose and light cues are set, 1/60 s on.
void SmokeHarness::CinematicFrame() {
    Camera& cam = m_Ctx.editorCamera;
    Framebuffer& fb = m_Ctx.sceneFramebuffer;
    if (m_FramesRendered == 1) {
        m_ShotHold = 0;
        LightSwitch::Update(m_Ctx.world, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), false); // circuits as they start
    }
    const int r = m_FramesRendered - Cinematic::Warmup(); // the frame just drawn: video frame r (time r / 60)
    if (r == 0 && (m_Ctx.decalSystem.Streaming() || m_Ctx.assets.AsyncInFlight() > 0) && m_ShotHold < 3000) {
        ++m_ShotHold;
        --m_FramesRendered; // not before everything has streamed in
    } else if (r >= 0 && fb.IsValid()) {
        if (r == 0) Cinematic::Start(m_Opt.smokeShotsDir, fb.Width(), fb.Height());
        Cinematic::Write(FrameCapture::GrabTopDown(fb.Handle(), fb.Width(), fb.Height()));
        if (r >= (int)(Cinematic::Duration() * 60.0f)) Cinematic::Finish();
    }
    const float next = std::max(0, r + 1) / 60.0f;
    for (Cinematic::LightCue& c : Cinematic::Lights()) {
        if (c.Done || c.T > next) continue;
        c.Done = true;
        if (c.Circuit == "*") LightSwitch::SetAll(m_Ctx.world, c.On);
        else if (!LightSwitch::Set(m_Ctx.world, c.Circuit, c.On)) Log::Warn("Cinematic: no light circuit '" + c.Circuit + "'");
    }
    const Cinematic::Pose p = Cinematic::Evaluate(Cinematic::Keys(), next);
    cam.Position = p.Pos;
    cam.Yaw = p.Yaw;
    cam.Pitch = p.Pitch;
    cam.Fov = p.Fov;
}

// --smoke-shots: the three fixed poses.
void SmokeHarness::PoseShotFrame() {
    Camera& editorCamera = m_Ctx.editorCamera;
    Framebuffer& sceneFramebuffer = m_Ctx.sceneFramebuffer;
    // Frames 40 / 70 / 100 are captured after 30+ frames at each pose, so
    // temporally accumulated effects have converged; the pose for the NEXT frame
    // is set here too.
    struct ShotPose { int CaptureFrame; float Yaw, Pitch; };
    static const ShotPose kPoses[3] = {{40, 180.0f, 5.0f}, {70, 0.0f, 10.0f}, {100, 90.0f, 50.0f}};
    for (int p = 0; p < 3; ++p) {
        if (m_FramesRendered != kPoses[p].CaptureFrame || !sceneFramebuffer.IsValid()) continue;
        const int w = sceneFramebuffer.Width(), h = sceneFramebuffer.Height();
        const std::string stem = std::filesystem::path(m_ScenePaths[m_SceneIndex]).stem().string();
        FrameCapture::WritePngTo(m_Opt.smokeShotsDir, stem + "_" + std::to_string(p), w, h,
                                 FrameCapture::GrabTopDown(sceneFramebuffer.Handle(), w, h));
    }
    const int next = m_FramesRendered + 1;
    const ShotPose& pose = kPoses[next <= 40 ? 0 : next <= 70 ? 1 : 2];
    editorCamera.Position = glm::vec3(0.0f, 2.0f, 12.0f);
    editorCamera.Yaw = pose.Yaw;
    editorCamera.Pitch = pose.Pitch;
}

// --perf-bench: this frame's measurement, and the report at the end of a phase.
void SmokeHarness::PerfFrame() {
    NpcTest* npcTest = m_Ctx.npcTest;
    const bool& playing = m_Ctx.playing;
    const bool& playMaximized = m_Ctx.playMaximized;
    const LaunchOptions& opt = m_Opt;
    // --perf-bench <scene> --npc-test <scenario>: the combat bench - the scripted fight in the Game view,
    // maximized, every Play frame measured (stutters included: TARTARUS_PERF_WARMUP frames skipped, 100 by
    // default), one report when the fight is over.
    if (opt.perfBenchMode && npcTest) {
        const double frameMs = m_Perf.TakeFrameMs();
        if (playing && !playMaximized) m_Ctx.setMaximized(true);
        if (!m_CombatReported && playing && playMaximized && ++m_CombatPlayFrame > m_Perf.Warmup())
            m_Perf.Sample(frameMs, m_CombatPlayFrame);
        if (!m_CombatReported && m_Perf.Samples() > 0 && (m_FramesRendered >= m_FrameBudget - 1 || npcTest->Done())) {
            m_CombatReported = true;
            m_Perf.ReportCombat(std::filesystem::path(m_ScenePaths[m_SceneIndex]).filename().string(),
                                m_Ctx.editor.GetRenderStats().DrawCalls, m_Ctx.editor.GetRenderStats().Triangles,
                                m_Ctx.window.GetWidth(), m_Ctx.window.GetHeight());
        }
    } else if (opt.perfBenchMode) {
        const double frameMs = m_Perf.TakeFrameMs();
        const int phaseFrame = (m_FramesRendered - 1) % PerfBench::kPhaseFrames + 1;
        if (opt.perfSample && phaseFrame == m_Perf.Warmup()) SamplingProfiler::Start();
        if (phaseFrame > m_Perf.Warmup()) m_Perf.Sample(frameMs, phaseFrame);
        if (phaseFrame == PerfBench::kPhaseFrames) {
            m_Perf.ReportPhase(std::filesystem::path(m_ScenePaths[m_SceneIndex]).filename().string(),
                               !playing ? "edit" : playMaximized ? "play-max" : "play",
                               m_Ctx.editor.GetRenderStats().DrawCalls, m_Ctx.editor.GetRenderStats().Triangles,
                               m_Ctx.window.GetWidth(), m_Ctx.window.GetHeight(), opt.perfSample);
            if (m_FramesRendered < m_FrameBudget) {
                if (!playing) m_Ctx.togglePlay();
                else if (!playMaximized) m_Ctx.setMaximized(true);
            }
        }
    }
}

void SmokeHarness::ScoreScene() {
    if (m_Cinematic) Cinematic::Finish(); // a run cut short still leaves a playable file
    NpcTest* npcTest = m_Ctx.npcTest;
    int newErrors    = GLDebug::ErrorCount() - m_BaselineGlErrors;
    int newLogErrors = Log::CountOf(LogLevel::Error) - m_BaselineLogErrors;
    int drawCalls    = m_Ctx.editor.GetRenderStats().DrawCalls;
    // A scene is only viable end-to-end if it loaded, introduced no new GL or
    // log errors across load+render, and actually drew something (audit #356).
    // Leave play mode before scoring / advancing, so the scene snapshot is
    // restored and the next scene loads from a clean editor state.
    if (m_Ctx.playing) { m_Ctx.togglePlay(); ++m_PlayCycles; }

    std::string cause;
    if (!m_SceneLoadOk)      cause += "Load() failed; ";
    if (newLogErrors > 0)    cause += std::to_string(newLogErrors) + " new log error(s); ";
    if (npcTest) {
        if (!npcTest->Done()) cause += "npc test didn't finish; ";
        if (npcTest->Failures() > 0)
            cause += std::to_string(npcTest->Failures()) + " of " + std::to_string(npcTest->Checks()) + " npc check(s) failed; ";
        std::cout << "[NpcTest] " << npcTest->Checks() << " checks, " << npcTest->Failures() << " failure(s)" << std::endl;
    }
    if (newErrors > 0)       cause += std::to_string(newErrors) + " new GL error(s); ";
    if (drawCalls <= 0)      cause += "zero draw calls; ";
    // A smoke_play* scene must actually have completed its Play -> Stop cycles.
    if (m_PlayScene && m_PlayCycles < 2)
        cause += "play-mode cycles incomplete (" + std::to_string(m_PlayCycles) + "/2); ";
    bool pass = cause.empty();
    const std::string& path = m_ScenePaths[m_SceneIndex];
    MemSnapshot(std::to_string(m_SceneIndex + 1) + "_" + std::filesystem::path(path).stem().string());
    m_Results.push_back({path, m_FramesRendered, newErrors, newLogErrors,
                         drawCalls, m_SceneLoadOk, pass, cause});
    // #354: which ShaderAsset variant keys drew this scene (0 = default program).
    std::string variants;
    for (const auto& [key, n] : SceneRendererDebug::VariantDrawCounts()) {
        char vbuf[48];
        std::snprintf(vbuf, sizeof(vbuf), " 0x%02x=%llu", key, (unsigned long long)n);
        variants += vbuf;
    }
    std::cout << "[SmokeTest] " << (pass ? "PASS" : "FAIL") << "  "
              << std::filesystem::path(path).filename().string()
              << "  frames=" << m_FramesRendered
              << " loadOk=" << (m_SceneLoadOk ? 1 : 0)
              << " newGlErrors=" << newErrors
              << " newLogErrors=" << newLogErrors
              << " drawCalls=" << drawCalls
              << (m_PlayScene ? ("  playCycles=" + std::to_string(m_PlayCycles)) : "")
              << (variants.empty() ? "" : ("  variants:" + variants))
              << (pass ? "" : ("  cause: " + cause)) << std::endl;
    ++m_SceneIndex;
    m_SceneActive = false;
}

void SmokeHarness::EndFrame() {
    // Count this frame toward the active scene's quota; once it's rendered enough,
    // score it (new GL errors + draw count) and advance to the next scene.
    if (!m_SceneActive) return;
    const LaunchOptions& opt = m_Opt;
    NpcTest* npcTest = m_Ctx.npcTest;
    Camera& editorCamera = m_Ctx.editorCamera;
    ++m_FramesRendered;
    if (m_Cinematic && !opt.perfBenchMode) {
        CinematicFrame();
    } else if (!opt.smokeShotsDir.empty() && !opt.perfBenchMode && !m_ShotCameras.empty()) {
        ShotCameraFrame();
    } else if (!opt.smokeShotsDir.empty() && !opt.perfBenchMode) {
        PoseShotFrame();
    }
    // --perf-bench: a scene with a Camera entity (tests/perf-scenes) holds the Scene view on it too, so the edit
    // phase measures the same fixed view as the two Play phases.
    if (opt.perfBenchMode && !npcTest && !m_Ctx.playing) {
        const entt::entity cam = FindActiveSceneCamera(m_Ctx.world);
        if (cam != entt::null) {
            const glm::mat4 m = m_Ctx.world.ComposeWorldTransform(cam);
            editorCamera.Position = glm::vec3(m[3]);
            YawPitchFromWorld(m, editorCamera.Yaw, editorCamera.Pitch);
            editorCamera.Fov = m_Ctx.world.Registry.get<CameraComponent>(cam).FovDegrees;
        }
    }
    if (npcTest) {
        glm::vec3 p;
        float yaw, pitch;
        if (npcTest->SceneCamera(p, yaw, pitch)) {
            editorCamera.Position = p;
            editorCamera.Yaw = yaw;
            editorCamera.Pitch = pitch;
        }
    }
    // --npc-test --perf-sample: a CPU profile of the fight itself (the squad up and shooting).
    if (npcTest && opt.perfSample) {
        if (m_FramesRendered == 900) { SamplingProfiler::Start(); m_Sampling = true; }
        if (m_Sampling && (m_FramesRendered >= m_FrameBudget - 1 || npcTest->Done())) {
            SamplingProfiler::Stop();
            SamplingProfiler::Report("npc-test", 80);
            m_Sampling = false;
        }
    }
    PerfFrame();
    if (m_FramesRendered >= m_FrameBudget) ScoreScene();
}

bool SmokeHarness::Summary() const {
    // Deliberately skip the normal exit path entirely (no play-mode revert, no
    // save-on-exit) - smoke-tested scenes were never really "open" from the user's
    // point of view and must not get written back to disk.
    std::cout << "\n[SmokeTest] ==== Summary ====" << std::endl;
    // No scenes discovered is itself a failure - an empty scene folder must not read
    // as a green run (audit #356 / TEST-101).
    bool allPassed = !m_Results.empty() && m_ScenePaths.size() == m_Results.size();
    for (const auto& r : m_Results) {
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
    std::cout << "[SmokeTest] " << m_Results.size() << " scene(s) - "
              << (allPassed ? "ALL PASSED" : "FAILURES DETECTED") << std::endl;
    return allPassed;
}
