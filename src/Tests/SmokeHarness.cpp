#include "Tests/SmokeHarness.h"

#include "App/LaunchOptions.h"
#include "App/PlaySession.h"
#include "AI/NpcTest.h"
#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "Camera.h"
#include "ClusterGrid.h"
#include "Components.h"
#include "Core/Env.h"
#include "GpuScene.h"
#include "GpuCull.h"
#include "Core/FrameLoads.h"
#include "Core/Paths.h"
#include "Core/Profiler.h"
#include "DecalSystem.h"
#include "EditorLayer.h"
#include "EditorSettings.h"
#include "FKeyRow.h"
#include "FirstPersonPresentation.h"
#include "Framebuffer.h"
#include "GIVolumes.h"
#include "GLDebug.h"
#include "HdrTarget.h"
#include "LightBuffer.h"
#include "LightGather.h"
#include "gl.h"
#include "Log.h"
#include "Model.h"
#include "Openable.h"
#include "LightSwitch.h"
#include "SamplingProfiler.h"
#include "SceneRenderer.h"
#include "Frustum.h"
#include "SceneSerializer.h"
#include "ShadowFrame.h"
#include "ShadowLimits.h"
#include "Tests/Cinematic.h"
#include "Tests/FrameCapture.h"
#include "Tests/SmokeChecks.h"
#include "Window.h"
#include "World.h"
#include "Renderer/MemReport.h"
#include "Renderer/TextureCache.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>

// A harness run (--smoke-test, --perf-bench, --npc-test) whose frames stop - a hang, a wait that never ends - used to
// spin until someone killed it. Like the editor tests' watchdog: a note each minute without a frame, and after
// kStallSeconds with neither a frame nor a load an abort, which writes a crash dump (the stack of the hang, with a copy
// of the log beside it) and fails the run. A scene opening on frame 0 imports its models synchronously (the Forest:
// ~250, each simplified for its LODs), which on a cold cache or a busy machine can take minutes with no frame, so every
// finished load (FrameLoads::Count) also counts as progress, up to kLoadingSeconds without a frame. After the summary
// the result is in, and a teardown that hangs (a wait on uploads that never come) ends the process with the run's own
// exit code once kShutdownSeconds pass. Its state is process-wide, not the harness's, so the detached thread never
// touches an object that is being destroyed.
namespace stall_watch {
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess();
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void* process, unsigned int exitCode);
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent();
constexpr long long kStallSeconds = 240;    // no frame and no load finished: a hang
constexpr long long kLoadingSeconds = 1800; // no frame even with loads finishing: a load loop that never ends
constexpr long long kShutdownSeconds = 90;  // teardown after the summary normally takes a few seconds
std::atomic<unsigned long long> g_Frames{0};
std::atomic<int> g_ExitCode{-1}; // set by the summary: from then on only the shutdown is watched
std::mutex g_Mutex;
std::string g_Where = "(startup)";
void SetWhere(const std::string& where) { std::lock_guard<std::mutex> lk(g_Mutex); g_Where = where; }
void Start() {
    std::thread([] {
        using clock = std::chrono::steady_clock;
        unsigned long long lastFrames = g_Frames.load(std::memory_order_relaxed);
        unsigned long long lastLoads = FrameLoads::Count(), loadsAtFrame = lastLoads;
        auto lastFrameAt = clock::now();
        auto lastProgressAt = lastFrameAt;
        auto doneAt = clock::time_point{};
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const auto now = clock::now();
            if (const int code = g_ExitCode.load(std::memory_order_relaxed); code >= 0) {
                // TerminateProcess, not exit(): ExitProcess runs DLL detach, which can block on the same lock the
                // teardown is stuck behind.
                if (doneAt == clock::time_point{}) doneAt = now;
                else if (now - doneAt >= std::chrono::seconds(kShutdownSeconds)) {
                    std::printf("[SmokeTest] WATCHDOG: the process hasn't exited %llds after the summary - ending it "
                                "with the run's exit code %d\n", kShutdownSeconds, code);
                    std::fflush(stdout);
                    TerminateProcess(GetCurrentProcess(), (unsigned)code);
                }
                continue;
            }
            const unsigned long long frames = g_Frames.load(std::memory_order_relaxed);
            const unsigned long long loads = FrameLoads::Count();
            if (frames != lastFrames) {
                lastFrames = frames;
                lastFrameAt = lastProgressAt = now;
                lastLoads = loadsAtFrame = loads;
                continue;
            }
            if (loads != lastLoads) { lastLoads = loads; lastProgressAt = now; }
            const long long stalled = std::chrono::duration_cast<std::chrono::seconds>(now - lastFrameAt).count();
            const long long idle = std::chrono::duration_cast<std::chrono::seconds>(now - lastProgressAt).count();
            std::string where;
            { std::lock_guard<std::mutex> lk(g_Mutex); where = g_Where; }
            const char* lastKind = FrameLoads::LastKind();
            char loading[160];
            std::snprintf(loading, sizeof(loading), "%llu load(s) finished since the last frame, the last %llds ago (%s)",
                          loads - loadsAtFrame, idle, lastKind ? lastKind : "none yet");
            // Under a debugger a stop at a breakpoint is not a hang: keep the minute notes, never abort.
            if ((idle >= kStallSeconds || stalled >= kLoadingSeconds) && !IsDebuggerPresent()) {
                std::printf("[SmokeTest] WATCHDOG: no frame finished for %llds in %s (frame %llu; %s) - aborting for a "
                            "crash dump\n", stalled, where.c_str(), frames, loading);
                std::fflush(stdout);
                std::abort();
            }
            if (stalled >= 60 && stalled % 60 < 5) {
                std::printf("[SmokeTest] WATCHDOG: no frame finished for %llds in %s (frame %llu; %s)\n", stalled,
                            where.c_str(), frames, loading);
                std::fflush(stdout);
            }
        }
    }).detach();
}
} // namespace stall_watch

// sscanf, as main.cpp read it (the file-wide warning off there).
#pragma warning(disable: 4996)

#include <GLFW/glfw3.h>

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
    // A camera path that can't be used fails the first scene (ScoreScene): this runs before any scene's
    // error baseline, so a logged error alone would never count and the run would exit 0 with no video.
    if (Cinematic::Active()) {
        std::string error = "TARTARUS_CINEMATIC needs --smoke-shots <dir> for the video";
        if (!opt.smokeShotsDir.empty()) m_Cinematic = Cinematic::Load(Env::Str(Env::Var::Cinematic), error);
        if (!m_Cinematic) {
            Log::Error("Cinematic: " + error);
            m_SetupError = "cinematic: " + error + "; ";
        }
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
        // --gi-tour: the one scene the tour names, for as many frames as the tour takes (GiTourFrame ends it).
        if (opt.giTourMode) {
            m_ScenePaths.clear();
            auto tour = std::make_unique<GiTour::Tour>();
            std::string error;
            std::vector<std::string> warnings;
            if (GiTour::Load(opt.giTourFile, *tour, error, &warnings)) {
                for (const std::string& w : warnings) std::cout << "[GiTour] note: " << w << std::endl;
                m_TourFrames = GiTour::BuildTimeline(*tour);
                m_ScenePaths.push_back(ProjectPaths::Resolve(tour->Scene));
                // the start's streaming hold (up to 3000 frames) on top of the timeline, then the scoring frame
                m_FrameBudget = 3000 + (int)m_TourFrames.size() + 2;
                std::cout << "[GiTour] " << tour->Waypoints.size() << " waypoint(s), " << tour->Routes.size() << " route(s), "
                          << m_TourFrames.size() << " frame(s) (" << m_TourFrames.size() / 60 << " s at 60 Hz) -> "
                          << opt.sceneBoundsOut << std::endl;
                m_Tour = std::move(tour);
            } else {
                std::cout << "[GiTour] ERROR: " << error << std::endl;
            }
        }
        if (opt.npcTestMode && opt.smokeScenesDirArg.empty())
            m_ScenePaths.assign(1, EnginePaths::Resolve(opt.npcTestScenario.empty() || opt.npcTestScenario == "labloco" || opt.npcTestScenario == "mmview" || opt.npcTestScenario == "mmdb" ||
                                                                  opt.npcTestScenario == "npcloco" || opt.npcTestScenario == "stopsteps" ||
                                                                  opt.npcTestScenario == "npcperf"
                                                              ? "tests/scenes/LocomotionLab.json" // (npcloco, stopsteps: out of the apartments, 2026-11-18)
                                                          : opt.npcTestScenario == "blood" ? "tests/scenes/BloodTest.json"
                                                          : opt.npcTestScenario == "ragdoll" ? "tests/scenes/RagdollTest.json"
                                                          : opt.npcTestScenario == "revolver" ? "tests/scenes/WeaponLab.json"
                                                          : opt.npcTestScenario == "decals" || opt.npcTestScenario == "doors" || opt.npcTestScenario == "roam"
                                                              ? "tests/scenes/DoorsLab.json" // (out of the apartments too, 2026-11-18: tools/scenes/build_doors_lab.py)
                                                                                       : "tests/scenes/Arena.json")); // (deaths, feet, flame: the Arena)
        // testbed: the animation overhaul's clean scene, a project scene (tools/scenes/build_anim_testbed.py), not a tests/ one.
        if (opt.npcTestMode && opt.smokeScenesDirArg.empty() && (opt.npcTestScenario == "testbed" || opt.npcTestScenario == "mmplayer" || opt.npcTestScenario == "mmstress"))
            m_ScenePaths.assign(1, ProjectPaths::Resolve("scenes/AnimTestbed.json"));
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
        // --perf-bench without the Drive's asset payloads (a worktree with only their .meta files) measures
        // placeholders, and one such run spun for half an hour: fail before the first scene instead.
        if (opt.perfBenchMode) {
            if (const int missing = AssetDatabase::MissingExternalCount(); missing > 0) {
                std::cout << "[PerfBench] ERROR: " << missing << " third-party asset file(s) in external_assets.csv are "
                             "missing (fetch them with tools/assets/fetch-assets.ps1, or hard-link them from a full "
                             "checkout) - failing before the first scene." << std::endl;
                m_ScenePaths.clear();
            }
        }
        stall_watch::Start();
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
    stall_watch::SetWhere(std::filesystem::path(path).filename().string());
    if (m_SceneIndex == 0) MemSnapshot("00_start");
    // The open is the longest stretch without a frame: say when it starts and what it took, so a slow or stuck open
    // reads as one in the log (the stall watchdog's notes name the same scene).
    std::cout << "[SmokeTest] Opening " << path << std::endl;
    const double openStart = glfwGetTime();
    const unsigned long long loadsBefore = FrameLoads::Count();
    const ModelCache::Stats modelsBefore = ModelCache::GetStats();
    m_SceneLoadOk = SceneSerializer::Load(world, m_Ctx.assets, path, /*persistMigration=*/false);
    const double loadedAt = glfwGetTime();
    m_Ctx.warmRenderResources(); // the editor does the same at startup, before its first frame
    const double warmedAt = glfwGetTime();
    // ... and the model cache's share (Library/Models): models read from it and imported + cooked, with their time
    // (summed over the threads that loaded them, so it can exceed the open's wall time).
    const ModelCache::Stats modelsAfter = ModelCache::GetStats();
    std::printf("[SmokeTest] Loading %s%s (open %.1f s: load %.1f s + warm-up %.1f s, %llu load(s); models: %d from the "
                "cache in %.1f s, %d imported and cooked in %.1f s)\n",
                path.c_str(), m_SceneLoadOk ? "" : "  (Load() reported failure)", warmedAt - openStart, loadedAt - openStart,
                warmedAt - loadedAt, FrameLoads::Count() - loadsBefore, modelsAfter.Hits - modelsBefore.Hits,
                (modelsAfter.HitMs - modelsBefore.HitMs) / 1000.0, modelsAfter.Cooked - modelsBefore.Cooked,
                (modelsAfter.CookMs - modelsBefore.CookMs) / 1000.0);
    std::fflush(stdout);
    // P-53 pin check: prune with the scene open, as an editor session's later prune would, under a deliberately
    // small cap. The scene's own textures (loaded or stored by this process) must all survive. Material textures
    // decode on the async loader, and an entry is pinned only once its load or store ran, so wait for it to drain.
    if (const int pruneMb = Env::Int(Env::Var::TexturePruneAfterOpenMb, 0); pruneMb > 0 && m_SceneLoadOk) {
        const double waitStart = glfwGetTime();
        while (m_Ctx.assets.AsyncInFlight() > 0 && glfwGetTime() - waitStart < 300.0) {
            m_Ctx.assets.PumpAsync(50.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const int inFlight = m_Ctx.assets.AsyncInFlight();
        const TextureCache::PruneStats ps = TextureCache::Prune(nullptr, (uint64_t)pruneMb * 1024 * 1024);
        const int missing = TextureCache::SessionEntriesMissing(); // the scene's entries no longer on disk
        constexpr double MB = 1024.0 * 1024.0;
        std::printf("[SmokeTest] Texture cache pin check: async in flight %d (waited %.1f s); %d entries %.0f MB, %d pinned "
                    "%.0f MB, cap %.0f MB; evicted %d (%.0f MB); this scene's entries missing after: %d%s\n",
                    inFlight, glfwGetTime() - waitStart, ps.Entries, ps.Bytes / MB, ps.Pinned, ps.PinnedBytes / MB,
                    ps.CapBytes / MB, ps.Evicted, ps.EvictedBytes / MB, missing,
                    (missing == 0 && inFlight == 0) ? "" : "  FAIL");
        std::fflush(stdout);
        if (missing != 0 || inFlight != 0)
            Log::Error("[SmokeTest] texture cache pin check: " + std::to_string(missing) + " of this scene's entries evicted, " +
                       std::to_string(inFlight) + " decodes still in flight");
    }
    if (m_SceneLoadOk) {
        SmokeChecks::SceneLoaded(world, m_Ctx.assets, path, [this](const std::string& label) { MemSnapshot(label); });
        FirstPersonPresentation::PrefetchPlayAssets(world, m_Ctx.assets); // as the editor's scene open
    }
    m_FramesRendered = 0;
    m_SceneStartSeconds = glfwGetTime();
    m_WallFrames = 0;
    m_UnfocusedFrames = 0;
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

    // smoke_min: the persistent GPU scene's buffers (G-1), read back slot by slot against the candidate list. Nothing
    // draws from them yet; this is what keeps them right until something does, on CI's software GL too.
    if (m_SceneActive && m_FramesRendered == 15 &&
        path.find("smoke_min") != std::string::npos) {
        std::string why;
        if (GpuScene::Get().SelfTest(world, why)) std::cout << "[SmokeTest]   GPU scene matches the candidate list" << std::endl;
        else Log::Error("[SmokeTest] GPU scene: " + why);
    }

    // smoke_min: GPU culling's stage A + ordered compaction (G-2) against GatherVisible: a camera query and one inside
    // half the scene's box. Every slot the CPU keeps must come back, in its order (extras are allowed).
    if (m_SceneActive && m_FramesRendered == 15 &&
        path.find("smoke_min") != std::string::npos && GpuScene::Enabled()) {
        const GpuCull::VerifyTotals before = GpuCull::Totals();
        VisibilityQuery cam;
        GpuCull::VerifyView(world, cam);
        glm::vec3 lo, hi;
        if (RenderableBounds(world, lo, hi)) {
            const glm::vec3 mid = (lo + hi) * 0.5f;
            const AABB half{lo, mid};
            VisibilityQuery inside;
            inside.ShadowCasters = true;
            inside.Within = &half;
            GpuCull::VerifyView(world, inside);
        }
        const GpuCull::VerifyTotals& after = GpuCull::Totals();
        const std::uint64_t views = after.Views - before.Views, missing = after.Missing - before.Missing;
        const std::uint64_t order = after.OrderBad - before.OrderBad;
        if (views > 0 && missing == 0 && order == 0)
            std::cout << "[SmokeTest]   GPU culling matches the CPU gather (" << (after.Expected - before.Expected)
                      << " kept, " << (after.Extra - before.Extra) << " extra)" << std::endl;
        else
            Log::Error("[SmokeTest] GPU culling: " + std::to_string(views) + " views, " + std::to_string(missing) +
                       " missing, " + std::to_string(order) + " out of order");
    }

    // A smoke_ssao_off* scene: rendered with SSAO off; no program may put two sampler types on one unit.
    if (m_SceneActive && m_FramesRendered == 15 &&
        path.find("smoke_ssao_off") != std::string::npos) {
        SmokeChecks::SamplerUnits();
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
        static std::vector<unsigned char> frame; // reused: a full frame per video frame (main thread only)
        FrameCapture::GrabTopDown(fb.Handle(), fb.Width(), fb.Height(), frame);
        Cinematic::Write(frame);
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

// --gi-tour: the lamps as this waypoint's camera sees them, one CSV row each (its shadow slot or -1, and its
// DistanceFade from here), with the slot holders summed up in the log.
void SmokeHarness::GiTourLights(int waypoint) {
    World& world = m_Ctx.world;
    const Camera& cam = m_Ctx.editorCamera;
    const GiTour::Waypoint& wp = m_Tour->Waypoints[waypoint];
    std::unordered_map<entt::entity, int> spotSlot, pointSlot;
    if (m_Ctx.lightGather) {
        for (const auto& [e, slot] : m_Ctx.lightGather->SpotSlotHolders()) if (world.Registry.valid(e)) spotSlot[e] = slot;
        for (const auto& [e, slot] : m_Ctx.lightGather->PointSlotHolders()) if (world.Registry.valid(e)) pointSlot[e] = slot;
    }
    auto nameOf = [&](entt::entity e) {
        const auto* n = world.Registry.try_get<NameComponent>(e);
        return n && !n->Name.empty() ? n->Name : "entity " + std::to_string((std::uint32_t)e);
    };
    const float tanHalfFov = std::tan(glm::radians(cam.Fov) * 0.5f);
    std::string spots, points;
    int lit = 0, faded = 0, gone = 0;
    for (auto e : world.Registry.view<TransformComponent, LightComponent>()) {
        const auto& lc = world.Registry.get<LightComponent>(e);
        if (lc.Kind == LightComponent::Type::Directional || world.Registry.all_of<InactiveTag>(e)) continue;
        const bool on = lc.Intensity > 0.0f && glm::dot(lc.Color, lc.Color) > 0.0f;
        const glm::vec3 pos(world.ComposeWorldTransform(e)[3]);
        const float dist = glm::length(pos - cam.Position);
        const float fade = LightBuffer::DistanceFade(lc.Range, dist, tanHalfFov);
        const bool spot = lc.Kind == LightComponent::Type::Spot;
        const auto& slots = spot ? spotSlot : pointSlot;
        const auto it = slots.find(e);
        const int slot = it != slots.end() ? it->second : -1;
        const auto* circuit = world.Registry.try_get<LightCircuitComponent>(e);
        if (on) {
            ++lit;
            if (fade <= 0.0f) ++gone;
            else if (fade < 1.0f) ++faded;
        }
        if (slot >= 0) {
            std::string& list = spot ? spots : points;
            list += (list.empty() ? "" : "; ") + std::to_string(slot) + ":" + nameOf(e);
        }
        if (m_TourLightsCsv) {
            char num[96];
            std::snprintf(num, sizeof(num), "%.3f,%.3f,%.3f,%.3f", lc.Intensity, lc.Range, dist, fade);
            *m_TourLightsCsv << GiTour::CsvField(wp.Id) << "," << GiTour::CsvField(wp.Room) << "," << GiTour::CsvField(nameOf(e))
                             << "," << (std::uint32_t)e << "," << (spot ? "spot" : "point") << ","
                             << GiTour::CsvField(circuit ? circuit->Circuit : std::string()) << "," << (on ? 1 : 0) << ","
                             << (lc.Shadow.Enabled ? 1 : 0) << "," << num << "," << slot << "\n";
        }
    }
    std::cout << "[GiTour] " << wp.Id << " (" << wp.Room << "): point slots [" << points << "] spot slots [" << spots << "]; "
              << lit << " lamp(s) on, " << faded << " fading, " << gone << " faded out" << std::endl;
}

// --gi-tour: called after each frame has presented. The frame just drawn showed timeline frame m_TourAt: take what that
// frame asks for (a shot, a CSV row), then pose the camera for the next one. The first pose waits for streaming.
// tour_perf.csv rows go out in frame order as their GPU times land (Profiler::GpuFrameScopes keeps the last few frames);
// a row whose frame was never read back in time, or every row at the end (`all`), is written with -1 for those.
void SmokeHarness::TourPerfFlush(bool all) {
    if (!m_TourPerfCsv) return;
    const unsigned now = Profiler::FrameSerial();
    size_t done = 0;
    for (; done < m_TourPerfPending.size(); ++done) {
        const TourPerfRow& p = m_TourPerfPending[done];
        const std::vector<Profiler::Entry>* gpu = Profiler::GpuFrameScopes(p.Serial);
        if (!gpu && !all && now - p.Serial < 6) break; // not read back yet: wait (in order)
        float total = -1.0f, point = -1.0f, spot = -1.0f, sun = -1.0f, depth = -1.0f, opaque = -1.0f, fog = -1.0f;
        if (gpu) {
            total = point = spot = sun = depth = opaque = fog = 0.0f;
            for (const Profiler::Entry& e : *gpu) {
                if (!e.Nested) total += e.Milliseconds;
                if (e.Name == "Point Shadow Pass") point += e.Milliseconds;
                else if (e.Name == "Spot Shadow Pass") spot += e.Milliseconds;
                else if (e.Name == "Sun Shadow Pass") sun += e.Milliseconds;
                else if (e.Name == "Depth Pre-pass") depth += e.Milliseconds;
                else if (e.Name == "Opaque") opaque += e.Milliseconds;
                else if (e.Name == "Volumetric Fog") fog += e.Milliseconds;
            }
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", p.WallMs, total, point, spot, sun, depth, opaque, fog);
        *m_TourPerfCsv << p.TourFrame << "," << p.Mode << "," << GiTour::CsvField(p.Segment) << "," << buf << "," << p.SpotLive << ","
                       << p.SpotStatic << "," << p.SpotDynamic << "," << p.SpotWhole << "," << p.PointLive << "," << p.PointStaticFaces
                       << "," << p.PointDynamicFaces << "," << p.PointKeptFaces << "," << p.CasterDraws << "," << p.StaticCasterDraws
                       << "," << m_Ctx.world.Render.MaxPointShadows << "\n";
    }
    m_TourPerfPending.erase(m_TourPerfPending.begin(), m_TourPerfPending.begin() + (std::ptrdiff_t)done);
}

void SmokeHarness::GiTourFrame() {
    namespace fs = std::filesystem;
    Camera& cam = m_Ctx.editorCamera;
    Framebuffer& fb = m_Ctx.sceneFramebuffer;
    auto pose = [&](const GiTour::Frame& f) {
        cam.Position = f.Eye;
        cam.Yaw = f.Yaw;
        cam.Pitch = f.Pitch;
        cam.Fov = m_Tour->VerticalFovDeg;
    };
    // --gi-tour-view gi / nogi: the whole run in that view, the same timeline, luminance rows only (no PNGs).
    const bool fullView = m_Opt.giTourView == "full";
    auto finish = [&]() {
        m_TourCsv.reset();       // flushed and closed
        m_TourLightsCsv.reset();
        TourPerfFlush(true);     // the last few frames' GPU times may never land: written without them
        m_TourPerfCsv.reset();
        EditorSettings::Get().FRowLightDebug = m_TourSavedLightView;
        if (m_Ctx.giVolumes) m_Ctx.giVolumes->SetEnabled(true);
        m_TourDone = true;
        m_FramesRendered = m_FrameBudget; // score the scene now
    };
    if (m_TourDone) return;
    if (!m_Tour || m_TourFrames.empty()) {
        m_SetupError = "gi-tour: no tour to run; ";
        m_FramesRendered = m_FrameBudget;
        return;
    }
    if (m_FramesRendered == 1) {
        m_ShotHold = 0;
        m_TourAt = 0;
        m_TourStarted = false;
        m_TourSavedLightView = EditorSettings::Get().FRowLightDebug;
        EditorSettings::Get().FRowLightDebug = m_Opt.giTourView == "gi" ? FKeyRow::LightGI : FKeyRow::LightOff;
        if (m_Opt.giTourView == "nogi" && m_Ctx.giVolumes) m_Ctx.giVolumes->SetEnabled(false);
        // Play's lighting state: every circuit as its switches start (Start On), as the cinematic does.
        LightSwitch::Update(m_Ctx.world, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), false);
        if (m_Tour->DoorState == "allOpen") Openable::SetAll(m_Ctx.world, 1.0f);
        std::cout << "[GiTour] measuring view: " << m_Opt.giTourView << "; doors: " << m_Tour->DoorState << "; lights: Start On; view: Scene view at "
                  << m_Ctx.sceneFramebuffer.Width() << "x" << m_Ctx.sceneFramebuffer.Height() << ", vertical FOV "
                  << m_Tour->VerticalFovDeg << std::endl;
        std::error_code ec;
        fs::create_directories(m_Opt.sceneBoundsOut, ec);
        m_TourCsv = std::make_unique<std::ofstream>(
            fs::path(m_Opt.sceneBoundsOut) / (fullView ? "tour_luminance.csv" : "tour_luminance_" + m_Opt.giTourView + ".csv"), std::ios::binary);
        if (fullView)
            m_TourLightsCsv = std::make_unique<std::ofstream>(fs::path(m_Opt.sceneBoundsOut) / "tour_lights.csv", std::ios::binary);
        if (!*m_TourCsv || (m_TourLightsCsv && !*m_TourLightsCsv)) {
            Log::Error("[GiTour] can't write to " + m_Opt.sceneBoundsOut);
            finish();
            return;
        }
        *m_TourCsv << GiTour::CsvHeader() << "\n";
        if (m_TourLightsCsv) *m_TourLightsCsv << "waypoint,room,light,entity,kind,circuit,on,shadow,intensity,range,distance,fade,slot\n";
        // TARTARUS_TOUR_MAX_POINTS: this run's point-shadow budget (the 4 / 8 / 12 sweep), never saved (the tour
        // saves nothing back).
        if (Env::IsSet(Env::Var::TourMaxPoints)) {
            m_Ctx.world.Render.MaxPointShadows = std::clamp(Env::Int(Env::Var::TourMaxPoints, 4), 0, ShadowLimits::kMaxPoints);
            std::cout << "[GiTour] max point shadows: " << m_Ctx.world.Render.MaxPointShadows << " (TARTARUS_TOUR_MAX_POINTS)" << std::endl;
        }
        m_TourPerfPending.clear();
        m_TourPerfCsv = std::make_unique<std::ofstream>(
            fs::path(m_Opt.sceneBoundsOut) / (fullView ? "tour_perf.csv" : "tour_perf_" + m_Opt.giTourView + ".csv"), std::ios::binary);
        *m_TourPerfCsv << "frame,mode,segment,wall_ms,gpu_total_ms,gpu_point_shadow_ms,gpu_spot_shadow_ms,gpu_sun_shadow_ms,"
                          "gpu_depth_prepass_ms,gpu_opaque_ms,gpu_volumetric_ms,spot_live,spot_static_redraws,spot_dynamic_passes,"
                          "spot_whole_redraws,point_live,point_static_faces,point_dynamic_faces,point_kept_faces,caster_draws,"
                          "static_caster_draws,max_point_shadows\n";
        if (m_Ctx.lightGather) m_Ctx.lightGather->RequestSnap(); // from wherever the scene opened
        pose(m_TourFrames[0]);
        return;
    }
    // Hold the first pose until the scene's textures have streamed in.
    if (!m_TourStarted) {
        if ((m_Ctx.decalSystem.Streaming() || m_Ctx.assets.AsyncInFlight() > 0) && m_ShotHold < 3000) {
            ++m_ShotHold;
            pose(m_TourFrames[0]);
            return;
        }
        if (m_ShotHold >= 3000) Log::Warn("[GiTour] still streaming after 3000 held frames - starting anyway.");
        std::cout << "[GiTour] streaming done after " << m_ShotHold << " held frame(s); the tour starts" << std::endl;
        m_TourStarted = true;
        m_TourAt = 0;
        pose(m_TourFrames[0]);
        return; // the tour's frame 0 is the next one drawn
    }

    const GiTour::Frame& f = m_TourFrames[m_TourAt];
    const GiTour::Waypoint& wp = m_Tour->Waypoints[std::max(f.Waypoint, 0)];
    const int w = fb.Width(), h = fb.Height();
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "%02d_", std::max(f.Waypoint, 0));
    if (!fullView && (f.Kind == GiTour::Step::ShotFull || f.Kind == GiTour::Step::ShotGi)) {
        // gi / nogi: no pictures; the view stays as set for the whole run
    } else if (f.Kind == GiTour::Step::ShotFull && fb.IsValid()) {
        FrameCapture::WritePngTo(m_Opt.sceneBoundsOut, prefix + wp.Id + "_full", w, h, FrameCapture::GrabTopDown(fb.Handle(), w, h));
        GiTourLights(f.Waypoint);
        EditorSettings::Get().FRowLightDebug = FKeyRow::LightGI; // debugView 8: the baked GI alone
    } else if (f.Kind == GiTour::Step::ShotGi && fb.IsValid()) {
        FrameCapture::WritePngTo(m_Opt.sceneBoundsOut, prefix + wp.Id + "_gi", w, h, FrameCapture::GrabTopDown(fb.Handle(), w, h));
        EditorSettings::Get().FRowLightDebug = FKeyRow::LightOff;
    } else if ((f.Kind == GiTour::Step::Still || f.Kind == GiTour::Step::Walk) && fb.IsValid() && m_TourCsv) {
        // The frame's colour (the tonemapped Scene view) and depth (the static-surface mask: drawn geometry, not the
        // sky), both bottom row first.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.Handle());
        m_TourRgba = FrameCapture::GrabRegion(0, 0, w, h);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        double lum = -1.0;
        int mask = 0;
        if (HdrTarget* hdr = m_Ctx.sceneHdr) {
            hdr->ResolveDepthOnly();
            const int dw = hdr->Width(), dh = hdr->Height();
            std::vector<float> depth((size_t)dw * dh);
            // Read through a depth-only framebuffer of the harness's own (the loader has no glGetTextureImage). The
            // resolved depth texture is attached each frame: a resize recreates it.
            static GLuint s_DepthFbo = 0;
            if (!s_DepthFbo) glCreateFramebuffers(1, &s_DepthFbo);
            glNamedFramebufferTexture(s_DepthFbo, GL_DEPTH_ATTACHMENT, hdr->ResolvedDepthTexture(), 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_DepthFbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, dw, dh, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            // The HDR target can be another size than the tonemapped image (render scale): the nearest depth sample.
            m_TourDepth.resize((size_t)w * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    m_TourDepth[(size_t)y * w + x] = depth[(size_t)std::min(y * dh / h, dh - 1) * dw + std::min(x * dw / w, dw - 1)];
            lum = GiTour::MaskedMeanLuminance(m_TourRgba.data(), m_TourDepth.data(), w, h, 2, 0.999999f, &mask);
            m_TourDepthStats = GiTour::MaskedDepthStats(m_TourDepth.data(), w, h, m_Ctx.editorCamera.NearPlane,
                                                        m_Ctx.editorCamera.FarPlane, 4);
        } else {
            m_TourDepthStats = {};
        }
        std::vector<std::uint32_t> spots, points;
        if (m_Ctx.lightGather) {
            for (const auto& [e, slot] : m_Ctx.lightGather->SpotSlotHolders()) if (m_Ctx.world.Registry.valid(e)) spots.push_back((std::uint32_t)e);
            for (const auto& [e, slot] : m_Ctx.lightGather->PointSlotHolders()) if (m_Ctx.world.Registry.valid(e)) points.push_back((std::uint32_t)e);
        }
        GiTour::CsvRow row;
        row.Frame = m_TourAt;
        row.Mode = f.Kind == GiTour::Step::Walk ? "walk" : "still";
        row.Segment = f.Segment;
        row.Waypoint = wp.Id;
        row.Room = wp.Room;
        row.Luminance = lum;
        row.MaskPixels = mask;
        row.MedianDepthM = m_TourDepthStats.MedianM;
        row.CloseFraction = m_TourDepthStats.CloseFraction;
        row.SlotHash = GiTour::SlotSetHash(std::move(spots), std::move(points));
        row.SlotsChanged = f.Segment == m_TourLastSegment && row.SlotHash != m_TourLastHash;
        m_TourLastSegment = f.Segment;
        m_TourLastHash = row.SlotHash;
        *m_TourCsv << GiTour::CsvLine(row) << "\n";
    }
    // tour_perf.csv: this frame's local-shadow redraws now; its GPU pass times when they land (TourPerfFlush).
    if (m_TourPerfCsv) {
        const unsigned serial = Profiler::FrameSerial();
        for (TourPerfRow& p : m_TourPerfPending) // the wall time Profiler measured for the frame before this one
            if (p.Serial + 1 == serial) p.WallMs = Profiler::LastFrameWallMs();
        TourPerfRow row;
        row.Serial = serial;
        row.TourFrame = m_TourAt;
        row.Mode = f.Kind == GiTour::Step::Walk ? "walk" : f.Kind == GiTour::Step::Still ? "still" : "other";
        row.Segment = f.Segment;
        if (m_Ctx.shadowFrame) {
            const ShadowFrame::LocalStats& s = m_Ctx.shadowFrame->LastLocalStats();
            row.SpotLive = s.SpotLive; row.SpotStatic = s.SpotStaticRedraws; row.SpotDynamic = s.SpotDynamicPasses;
            row.SpotWhole = s.SpotWholeRedraws; row.PointLive = s.PointLive; row.PointStaticFaces = s.PointStaticFaces;
            row.PointDynamicFaces = s.PointDynamicFaces; row.PointKeptFaces = s.PointKeptFaces;
            row.CasterDraws = s.CasterDraws; row.StaticCasterDraws = s.StaticCasterDraws;
        }
        m_TourPerfPending.push_back(std::move(row));
        TourPerfFlush(false);
    }
    if (++m_TourAt >= (int)m_TourFrames.size()) {
        std::cout << "[GiTour] done: " << m_TourFrames.size() << " frame(s) -> " << m_Opt.sceneBoundsOut << std::endl;
        finish();
        return;
    }
    // A jump (a cut waypoint, a route's first point): anything but a walk that moves the eye. The next frame is a cut,
    // so its shadows snap instead of fading in from the last place (the holds must not start mid-fade).
    const GiTour::Frame& next = m_TourFrames[m_TourAt];
    if (m_Ctx.lightGather && next.Kind != GiTour::Step::Walk && next.Eye != f.Eye) m_Ctx.lightGather->RequestSnap();
    pose(next);
    // Only a start held the full 3000 frames runs past the budget: keep it going to the tour's end.
    if (m_FramesRendered >= m_FrameBudget - 1) m_FrameBudget = m_FramesRendered + 2;
}

// --perf-bench: this frame's measurement, and the report at the end of a phase.
void SmokeHarness::PerfFrame() {
    NpcTest* npcTest = m_Ctx.npcTest;
    const bool& playing = m_Ctx.playing;
    const bool& playMaximized = m_Ctx.playMaximized;
    const LaunchOptions& opt = m_Opt;
    // --perf-bench <scene> --npc-test <scenario>: the scripted run in the Game view, maximized, every Play frame
    // measured (stutters included: TARTARUS_PERF_WARMUP frames skipped, 100 by default), one report when it is over.
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
        if (opt.perfSample && phaseFrame == std::max(m_Perf.Warmup(), 1)) SamplingProfiler::Start(); // warmup 0: from frame 1
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
    // A run cut short still leaves a playable file; an incomplete or empty clip fails the scene.
    const bool clipOk = !m_Cinematic || Cinematic::Finish();
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
    cause += m_SetupError;
    m_SetupError.clear(); // counted once, against the first scene
    if (!clipOk)             cause += "cinematic clip incomplete; ";
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
    // Wall time and frame rate: the frame cost of a run at unlocked FPS (TARTARUS_UNLOCKED_FPS), and how many of its
    // frames the window spent without focus.
    const double wall = std::max(1e-6, glfwGetTime() - m_SceneStartSeconds);
    char timing[96];
    std::snprintf(timing, sizeof(timing), " rendered=%d wall=%.1fs fps=%.0f unfocused=%d", m_WallFrames, wall, m_WallFrames / wall,
                  m_UnfocusedFrames);
    std::cout << "[SmokeTest] " << (pass ? "PASS" : "FAIL") << "  "
              << std::filesystem::path(path).filename().string()
              << "  frames=" << m_FramesRendered << timing
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
    stall_watch::g_Frames.fetch_add(1, std::memory_order_relaxed);
    if (!m_SceneActive) return;
    const LaunchOptions& opt = m_Opt;
    NpcTest* npcTest = m_Ctx.npcTest;
    Camera& editorCamera = m_Ctx.editorCamera;
    ++m_FramesRendered;
    ++m_WallFrames;
    if (glfwGetWindowAttrib(m_Ctx.window.Handle(), GLFW_FOCUSED) == 0) ++m_UnfocusedFrames;
    if (opt.giTourMode) {
        GiTourFrame();
    } else if (m_Cinematic && !opt.perfBenchMode) {
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
    // --npc-test --perf-sample: a CPU profile of the scripted run itself.
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
    stall_watch::SetWhere("shutdown after the summary");
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
    stall_watch::g_ExitCode = allPassed ? 0 : 1; // Finish() returns the same code
    return allPassed;
}
