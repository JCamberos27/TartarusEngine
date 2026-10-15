#pragma once

#include <string>
#include <vector>

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
    // --weapon-test: the smoke harness on the Sandbox only, with the weapons played through by
    // script in Play (FirstPersonWeaponTest) on a fixed 60 Hz step. Exit code 0 = every check passed.
    bool weaponTestMode = false;
    // --stock-probe [remington|ak]: the same harness with the stock probe script (FirstPersonWeaponTest):
    // the weapon (default the Remington) held through pitches, turns, rounds and the sights, the butt measured
    // against the body; with --smoke-shots each capture is the Scene view (on the gun and shoulder) beside the Game view.
    bool stockProbeMode = false, stockProbeAk = false;
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
    int renderHeightArg = -1; // --render-height N: this run's render resolution (0 = native)
    // --resave <in.json> <out.json>: load a scene and immediately re-serialize it, then exit.
    // The one headless path that exercises the SAVE side of the serializer - round-trip tests
    // (prefab overrides #302 Part B, the reflected-component migrations, ...) all need it. Still
    // spins up the GL context (texture/mesh loading needs it) but never enters the main loop.
    std::string resaveIn, resaveOut;
    // --smoke-test [dir]: an optional directory of .json scenes to load. Defaults to the
    // engine's own committed regression scenes (assets/test-scenes/), falling back to the
    // user project's scenes/ folder - so the harness still means something when the user
    // project is empty (audit TEST-101).
    std::string smokeScenesDirArg;
    // --undo-bench: one-shot headless timing of SceneSerializer::SaveToString/LoadFromString at
    // synthetic scene sizes (audit ARCH-203 / #375). Investigates whether undo/redo's whole-scene
    // JSON round-trip is cheap enough to leave as-is, before committing to a diff-based redesign.
    bool undoBenchMode = false;
    // --asset-load-bench: one-shot headless timing of AssetLibrary::LoadTexture's decode/upload
    // split at synthetic library sizes (audit ARCH-201 / #375). The committed project has no
    // external texture/model assets to bench against, so this synthesizes PNGs the same way
    // --undo-bench synthesizes entities.
    bool assetLoadBenchMode = false;
    // #174 - `--build [outDir]`: File > Build without the UI (CI packaging, scripted builds).
    // Uses Project Settings > Build; outDir overrides its Output Folder. Exit 0 on success.
    // --build-cancel-at N (a test hook): cancel the build once this many files are copied (checks Cancel cleans up).
    bool buildMode = false;
    std::string buildOutArg;
    int buildCancelAt = -1;
    // --smoke-shots <dir> (with --smoke-test): saves three Scene-view PNGs per scene - toward the
    // horizon, a higher angle behind, and up at the sky - for reviewing rendering changes (the
    // physical sky's, first) without driving the editor.
    std::string smokeShotsDir;
    // --outfit-shots <dir> [scene.json|dir]: the smoke test, with the Scene view framed on the scene's
    // characters instead (CharacterOutfitComponent): the first row, then each picked character full length
    // from the front and back and its head close up - for reviewing outfit and skin-hiding changes.
    // Characters are picked spread along each row, or by name with TARTARUS_SHOT_NAMES="Name1,Name2"; each also
    // gets a "_headback" shot. TARTARUS_SHOT_POSE="<clip>@<fraction>" holds every skinned piece in that pose.
    std::string outfitShotsDir;
    // --outfit-audit [wardrobe] [report.csv]: the wardrobe's rules (presets, Randomize) and every pair of
    // outfit pieces worn one over the other, checked for clipping (OutfitAudit); --outfit-rules: the rules
    // only; --outfit-posed: the pairs in 32 animation poses too. Exit code: 0 clean, 1 problems.
    bool outfitAuditMode = false, outfitAuditGeometry = true, outfitAuditPosed = false;
    std::string outfitAuditWardrobe = "assets/Characters/Quantum/Quantum.wardrobe", outfitAuditCsv;
    // --gen-outfit-scenes: builds project/scenes/OutfitTest/*.json with the outfit system (OutfitTestScene).
    bool outfitScenesMode = false;
    // --outfit-cost [scene.json ...]: draw cost of the wardrobe's models and of each scene (OutfitAudit::RunCost).
    bool outfitCostMode = false;
    std::vector<std::string> outfitCostScenes;
    // --outfit-selftest: the OutfitSystem API end to end on the real wardrobe (OutfitAudit::RunSelfTest).
    bool outfitSelfTestMode = false;
    // --clip-report <rig> <clip or folder ...>: each animation clip attached to the rig (as the animator does) and
    // measured - travel, heading change, entry / exit direction and speed, a pivot's plant (ClipAnalysis::PrintReport).
    bool clipReportMode = false;
    std::vector<std::string> clipReportArgs;
    // --editor-shot <dir>: the interactive editor at 1920x1080 (or --perf-res), stepped through a few
    // fixed views (an entity selected, the Console, the Game view, Settings), each saved as a PNG of the
    // whole window, then closed - for reviewing editor UI changes without driving the desktop. It runs
    // on a copy of the user's editor prefs and layout (%LOCALAPPDATA%\TartarusEngine-Shot), never the
    // real ones. --editor-shot-select <name> picks the selected entity (default: Player Spawn);
    // --editor-shot-default-layout starts from the default dock layout instead of the user's;
    // --editor-shot-scale <s> sets the UI scale (as Preferences > General > UI scale would);
    // --editor-shot-glass captures with the Glass theme and its Glass Workspace panels on.
    std::string editorShotDir, editorShotSelect = "Player Spawn";
    bool editorShotDefaultLayout = false;
    bool editorShotCurves = false; // capture the real themed recoil curve widget in isolation
    bool editorShotIDE = false; // one focused IDE capture; use --project with a small fixture
    bool editorShotGlass = false;
    float editorShotScale = 0.0f;
    // --editor-tests [filter]: end-to-end tests of the editor UI (Tests/EditorUiTests.cpp) in the
    // real editor: a hidden 1920x1080 window, a fresh scratch user folder
    // (%LOCALAPPDATA%\TartarusEngine-Test), an empty in-memory scene. Exit code 0 = all passed.
    bool editorTestsMode = false;
    std::string editorTestsFilter;
};

// The options in argv. Also applies --project <dir> (#151: open that project folder instead of the one found next to
// the exe) as it goes, as the parse always has.
LaunchOptions ParseLaunchOptions(int argc, char** argv);
