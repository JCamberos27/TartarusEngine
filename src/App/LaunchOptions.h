#pragma once

#include <string>
#include <vector>

// What the command line asks of this run. Parsed once, at the top of main(), before any window or GL context exists,
// from one table of flags (LaunchOptions.cpp) that also prints `--help`. An unknown flag, a missing value, two modes
// at once or a modifier its mode doesn't use is an error: main prints it and exits 2.

// The one thing this run does. Everything but Editor runs unattended.
enum class LaunchMode {
    Editor,       // the interactive editor (or the built game when player.json sits next to the exe)
    SmokeTest,    // --smoke-test [dir]
    PerfBench,    // --perf-bench [dir|scene.json]   (the smoke harness, timed)
    NpcTest,      // --npc-test [scenario]           (the smoke harness, scripted NPC fight)
    EditorTests,  // --editor-tests [filter]
    EditorShot,   // --editor-shot <dir>
    Build,        // --build [outDir]
    UnitTests,    // --unit-tests [filter]
    ModelReport,  // --model-report <folder or model>...
    ModelNodes,   // --model-nodes <folder or fbx>...
    SceneBounds,  // --scene-bounds <scene.json|prefab dir>... --out <dir>
    BakeImpostors, // --bake-impostors <scene.json>... --out <dir> [--match <text>]
    AudioTest,    // --audio-test [scene] [--report <file>]
    BakeGI,       // --bake-gi <scene.json> [--gi-only <name>]
    CrashTest,    // --crash-test <kind> [--crash-dialog]
    Help,         // --help
};

struct LaunchOptions {
    LaunchMode mode = LaunchMode::Editor;

    // Derived from `mode` (the harness branches in main read these).
    // --smoke-test: loads every scene under the folder (default: the engine's assets/test-scenes, falling back to the
    // project's scenes/), renders a fixed number of frames of each through the editor's own render path, and fails on
    // new GL errors or a zero draw count. --perf-bench and --npc-test run on the same harness, so they set it too.
    bool smokeTestMode = false;
    // --perf-bench: the smoke harness with VSync, the FPS cap and GL debug output off, longer runs, and an averaged
    // CPU/GPU profiler breakdown per scene (edit mode, docked Play, maximized Play).
    bool perfBenchMode = false;
    // --npc-test [watch|fight|die|...]: the enemy squad against a scripted player (NpcTest) on a fixed 60 Hz step.
    bool npcTestMode = false;
    // --editor-tests [filter]: end-to-end editor UI tests (Tests/EditorUiTests.cpp) in a hidden 1920x1080 window with
    // a fresh scratch user folder (%LOCALAPPDATA%\TartarusEngine-Test).
    bool editorTestsMode = false;
    // --build [outDir]: File > Build without the UI. outDir overrides Project Settings > Build's Output Folder.
    bool buildMode = false;

    // The mode's own value.
    std::string smokeScenesDirArg;  // --smoke-test / --perf-bench [dir|scene.json]
    std::string npcTestScenario;    // --npc-test [scenario]
    std::string editorTestsFilter;  // --editor-tests [filter]
    std::string editorShotDir;      // --editor-shot <dir>
    std::string buildOutArg;        // --build [outDir]
    std::string unitTestsFilter;    // --unit-tests [filter]
    std::string audioTestScene;     // --audio-test [scene]
    std::string bakeGiScene;        // --bake-gi <scene.json>: bake its GI Volumes (GIBaker), write their .gi, exit
    std::string crashTestKind;      // --crash-test <av|overflow|abort|purecall|invalidparam|terminate>
    std::vector<std::string> inputs; // --model-report / --model-nodes / --scene-bounds inputs

    // Modifiers.
    // --weapon-lab (or TARTARUS_WEAPON_LAB): the editor on the WeaponLab scene with the Weapon Lab panel.
    bool weaponLabMode = false;
    // --perf-sample (perf-bench): a statistical main-thread CPU profile of each phase (SamplingProfiler).
    bool perfSample = false;
    // --perf-res WxH (perf-bench, editor-shot, editor-tests): the window size, for repeatable runs.
    int perfResW = 0, perfResH = 0;
    // --smoke-shots <dir> (smoke-test, npc-test, perf-bench): three Scene-view PNGs per scene.
    std::string smokeShotsDir;
    // --editor-shot-default-layout / -curves / -ide (editor-shot): start from the default dock layout; capture the
    // recoil curve widget alone; one Script IDE capture (use --project with a small fixture).
    bool editorShotDefaultLayout = false, editorShotCurves = false, editorShotIDE = false;
    // --crash-test-in-editor <kind> (editor): crash a few seconds into a normal session with the scene dirty, to
    // exercise the crash-time recovery save.
    std::string crashTestInEditor;
    // --crash-dialog (crash-test, editor with --crash-test-in-editor): show the crash dialog anyway.
    bool crashDialog = false;
    std::string sceneBoundsOut;   // --out <dir> (scene-bounds, bake-impostors)
    std::string bakeMatch;        // --match <text> (bake-impostors): the model path the LOD 0s must draw
    std::string audioTestReport;  // --report <file> (audio-test)
    std::string bakeGiOnly;       // --gi-only <name> (bake-gi): only the volumes whose name contains it
    // --project <dir> (any mode): open that project folder instead of the one next to the exe. Applied by the parse.
    std::string projectDir;
};

// Parses argv into `out`. On a bad command line returns false with `error` set (and nothing applied). On success
// also applies --project (ProjectPaths::SetRootOverride).
bool ParseLaunchOptions(int argc, char** argv, LaunchOptions& out, std::string& error);

// The flag table and the environment variables (Core/Env.h), for --help.
std::string LaunchHelpText();
