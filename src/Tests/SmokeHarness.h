#pragma once

#include "Tests/GiTour.h"
#include "Tests/PerfBench.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct LaunchOptions;
class AssetLibrary;
class Camera;
class ClusterGrid;
class DecalSystem;
class EditorLayer;
class Framebuffer;
class GIVolumes;
class HdrTarget;
class LightGather;
class NpcTest;
class ShadowFrame;
class Window;
class World;

// What the harness reads and drives of main's frame loop: references to main's own objects (so every frame sees their
// current state), and the callbacks for the few things only main can do.
struct SmokeContext {
    World& world;
    AssetLibrary& assets;
    EditorLayer& editor;
    Camera& editorCamera;
    Framebuffer& sceneFramebuffer;
    ClusterGrid& clusterGrid;
    DecalSystem& decalSystem;
    Window& window;
    NpcTest* npcTest;                // null outside --npc-test
    const bool& playing;
    const bool& playMaximized;
    const bool& playUsesPlayer;
    const Camera& playerCamera;      // the camera Play runs through when playUsesPlayer, else playSceneCamera
    const Camera& playSceneCamera;
    std::function<void()> togglePlay;           // the F1 / toolbar entry
    std::function<void(bool)> setMaximized;     // the Game view over the editor panels
    std::function<void()> warmRenderResources;  // what the editor does at startup, before its first frame
    LightGather* lightGather = nullptr;          // --gi-tour: the frame's shadow-slot holders, and its cuts (RequestSnap)
    HdrTarget* sceneHdr = nullptr;              // --gi-tour: the Scene view's depth (the static-surface mask)
    GIVolumes* giVolumes = nullptr;             // --gi-tour-view nogi: switched off for the run
    const ShadowFrame* shadowFrame = nullptr;   // --gi-tour: what the local shadow maps redrew (tour_perf.csv)
};

// --smoke-test (audit #187), and --perf-bench / --npc-test / --smoke-shots, which ride on it. Deliberately driven from
// inside the normal render loop rather than a separate loop of its own, so it exercises the exact same per-frame path
// (light gather, shadow passes, drawScene, editor.Draw/EndFrame) as an interactive session - the whole point of the
// harness is catching a regression that path could introduce, not a hand-rolled approximation of it.
//
// main calls BeginFrame() near the top of each frame (loads the scene, runs its checks, starts its Play cycles), and
// EndFrame() after the frame has presented (counts it, takes the shots, measures it, scores the scene and moves on).
class SmokeHarness {
public:
    SmokeHarness(const LaunchOptions& opt, const SmokeContext& ctx);

    // Every scene's frame quota is met: main stops the loop.
    bool AllDone() const { return m_SceneIndex >= m_ScenePaths.size(); }
    // The scene being rendered now (valid until AllDone()).
    const std::string& CurrentPath() const { return m_ScenePaths[m_SceneIndex]; }

    void BeginFrame();
    void EndFrame();
    // The end-of-run table. Returns whether every scene passed.
    bool Summary() const;

private:
    struct ShotCamera { std::string Name; glm::vec3 Position; float Yaw, Pitch; };
    struct Result { std::string scenePath; int frames; int newGlErrors; int newLogErrors;
                    int drawCalls; bool loadOk; bool pass; std::string cause; };

    void MemSnapshot(const std::string& label) const;
    void LoadScene();
    void PlayProbeCycle();
    void ShotCameraFrame();
    void CinematicFrame();
    void PoseShotFrame();
    void PerfFrame();
    void GiTourFrame();
    void GiTourLights(int waypoint);
    void ScoreScene();

    const LaunchOptions& m_Opt;
    SmokeContext m_Ctx;

    // --smoke-shots with TARTARUS_SHOT_CAMERAS="name:x,y,z,yaw,pitch;...": those Scene-view poses instead of the
    // three fixed ones, each saved as <scene>_<name>.png after 30 frames on it (an asset gallery, a set dressing).
    std::vector<ShotCamera> m_ShotCameras;
    // TARTARUS_SHOT_START=<frames>: first capture frame (default 40); big packs need longer for their maps to decode.
    int m_ShotCameraStart = 40;
    static constexpr int kShotCameraSettle = 30;
    int m_FrameBudget = 100;
    // --perf-bench with TARTARUS_PERF_PLAYS=N: after the maximized Play phase, Stop and Play again (N Plays in all), each
    // measured as its own phase: a later Play in the same session (what it reuses) against the first.
    int m_PerfPlays = 1, m_PerfPlay = 1;
    bool m_PerfReplay = false; // stopped: Play again next frame
    // TARTARUS_MEM_REPORT=<dir>: a memory snapshot (MemReport) before the first scene and after each one.
    std::string m_MemReportDir;
    // TARTARUS_OPEN_ALL=1: every door, drawer and lid shown open, to check each one's hinge.
    bool m_OpenAll = false;
    int m_LightsMode = -1; // TARTARUS_LIGHTS_ON: 1 every light circuit on, 0 as the switches start, unset untouched
    std::string m_LightsOnly; // TARTARUS_LIGHTS_ONLY: these circuits on, every other off

    PerfBench m_Perf;

    // --gi-tour (Tests/GiTour.h): the tour, its frames, where it is, and its outputs.
    std::unique_ptr<GiTour::Tour> m_Tour;
    std::vector<GiTour::Frame> m_TourFrames;
    int m_TourAt = 0;                  // the timeline frame the last drawn frame showed
    bool m_TourStarted = false, m_TourDone = false;
    int m_TourSavedLightView = 0;      // the F-row light view to put back (EditorSettings; the harness never saves them)
    std::unique_ptr<std::ofstream> m_TourCsv, m_TourLightsCsv;
    std::string m_TourLastSegment;
    std::uint64_t m_TourLastHash = 0;
    std::vector<unsigned char> m_TourRgba;
    std::vector<float> m_TourDepth;
    GiTour::DepthStats m_TourDepthStats;
    // tour_perf.csv (GI Phase 1 perf): per drawn frame, what the local shadow maps redrew (known at once) and the
    // frame's GPU pass times (read back a few frames late, by profiler frame serial).
    struct TourPerfRow {
        unsigned Serial = 0;
        int TourFrame = 0;
        std::string Mode, Segment;
        float WallMs = -1.0f;
        int SpotLive = 0, SpotStatic = 0, SpotDynamic = 0, SpotWhole = 0;
        int PointLive = 0, PointStaticFaces = 0, PointDynamicFaces = 0, PointKeptFaces = 0;
        int CasterDraws = 0, StaticCasterDraws = 0;
    };
    std::unique_ptr<std::ofstream> m_TourPerfCsv;
    std::vector<TourPerfRow> m_TourPerfPending;
    void TourPerfFlush(bool all);

    std::vector<std::string> m_ScenePaths;
    std::vector<Result> m_Results;
    size_t m_SceneIndex = 0;
    int m_FramesRendered = 0;
    double m_SceneStartSeconds = 0.0; // glfwGetTime() at the scene's load: its wall time and fps in the summary line
    int m_WallFrames = 0;             // frames really rendered (an npc test that is done jumps m_FramesRendered ahead)
    int m_UnfocusedFrames = 0;        // frames the visible window spent without focus (they must change nothing)
    int m_BaselineGlErrors = 0;
    int m_BaselineLogErrors = 0;
    bool m_SceneActive = false;
    bool m_SceneLoadOk = false;
    // A scene whose filename contains "smoke_play" additionally gets two Play -> Stop cycles
    // driven during its frame budget (audit #368 / #389): CI then exercises play-mode entry
    // / exit, snapshot save+restore, PhysX world create+destroy, the game module's systems
    // running from TartarusGame.dll, and - via the normal process exit afterwards - the
    // teardown ordering that #389 broke. m_PlayCycles counts completed Stop transitions.
    bool m_PlayScene = false;
    int m_PlayCycles = 0;

    // Across scenes, as the function-local statics they came from were.
    std::string m_SetupError;     // a harness setup failure (a bad camera path), charged to the first scene
    bool m_Cinematic = false;     // TARTARUS_CINEMATIC: this run records the camera path (Tests/Cinematic.h)
    int m_ShotHold = 0;           // frames the current capture has waited for decal textures to stream in
    int m_PerfHold = 0;           // --perf-bench: frames the edit phase has waited for the scene to stream in
    bool m_Sampling = false;      // --npc-test --perf-sample's profile is running
    int m_CombatPlayFrame = 0;    // the combat bench's maximized Play frames so far
    bool m_CombatReported = false;
};
