#pragma once

#include <string>

// What the command line asks of this run: the harness and tool modes and their arguments. Parsed once, at the top of
// main(), before any window or GL context exists. (Tools that run and exit before the editor starts are in CliTools.h.)
struct LaunchOptions {
    // --smoke-test: headless-as-possible CI/manual smoke check (audit #187). Loads every scene
    // under project/scenes/, renders a fixed number of frames of each through the exact same
    // per-frame render path the interactive editor uses (see the `smokeTestMode` branches
    // sprinkled through the main loop), then checks for new GL debug-callback errors and
    // a nonzero draw count before moving to the next scene. Exits 0 if every scene passed, or a
    // nonzero code if any failed.
    bool smokeTestMode = false;
    // --weapon-lab (run-weapon-lab.cmd, or TARTARUS_WEAPON_LAB set): the editor on the WeaponLab scene, the Scene view
    // (the world body) beside the Game view, and the Weapon Lab panel (WeaponLab) - the third-person weapon animations
    // worked on by hand.
    bool weaponLabMode = false;
    // --npc-test [watch|fight|die]: the smoke harness on scenes/Arena.json with the enemy squad played
    // against a scripted player (NpcTest) on a fixed 60 Hz step; with --smoke-shots the Scene view
    // follows the soldiers beside the Game view. Exit code 0 = every check passed.
    bool npcTestMode = false;
    std::string npcTestScenario;
    // --perf-bench [dir|scene.json]: the smoke-test harness with VSync, the FPS cap and GL debug output
    // off, a longer per-scene run, and an averaged CPU/GPU profiler breakdown printed per scene: edit
    // mode, then Play in the docked Game view, then Play maximized. --perf-res WxH sizes the window
    // (default: maximized) so runs at a given resolution are repeatable. --perf-sample adds a
    // statistical main-thread CPU profile of each phase (SamplingProfiler): the hottest functions.
    bool perfBenchMode = false, perfSample = false;
    int perfResW = 0, perfResH = 0;
    // --smoke-test [dir]: an optional directory of .json scenes to load. Defaults to the
    // engine's own committed regression scenes (assets/test-scenes/), falling back to the
    // user project's scenes/ folder - so the harness still means something when the user
    // project is empty (audit TEST-101).
    std::string smokeScenesDirArg;
    // #174 - `--build [outDir]`: File > Build without the UI (CI packaging, scripted builds).
    // Uses Project Settings > Build; outDir overrides its Output Folder. Exit 0 on success.
    bool buildMode = false;
    std::string buildOutArg;
    // --smoke-shots <dir> (with --smoke-test): saves three Scene-view PNGs per scene - toward the
    // horizon, a higher angle behind, and up at the sky - for reviewing rendering changes (the
    // physical sky's, first) without driving the editor.
    std::string smokeShotsDir;
    // --editor-shot <dir>: the interactive editor at 1920x1080 (or --perf-res), stepped through a few
    // fixed views (an entity selected, the Console, the Game view, Settings), each saved as a PNG of the
    // whole window, then closed - for reviewing editor UI changes without driving the desktop. It runs
    // on a copy of the user's editor prefs and layout (%LOCALAPPDATA%\TartarusEngine-Shot), never the
    // real ones. The selected entity is Player Spawn;
    // --editor-shot-default-layout starts from the default dock layout instead of the user's.
    std::string editorShotDir;
    bool editorShotDefaultLayout = false;
    bool editorShotCurves = false; // capture the real themed recoil curve widget in isolation
    bool editorShotIDE = false; // one focused IDE capture; use --project with a small fixture
    // --editor-tests [filter]: end-to-end tests of the editor UI (Tests/EditorUiTests.cpp) in the
    // real editor: a hidden 1920x1080 window, a fresh scratch user folder
    // (%LOCALAPPDATA%\TartarusEngine-Test), an empty in-memory scene. Exit code 0 = all passed.
    bool editorTestsMode = false;
    std::string editorTestsFilter;
};

// The options in argv. Also applies --project <dir> (#151: open that project folder instead of the one found next to
// the exe) as it goes, as the parse always has.
LaunchOptions ParseLaunchOptions(int argc, char** argv);
