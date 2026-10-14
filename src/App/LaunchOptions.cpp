#include "App/LaunchOptions.h"

#include "ProjectPaths.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>

LaunchOptions ParseLaunchOptions(int argc, char** argv) {
    LaunchOptions o;
    {
#pragma warning(suppress : 4996)
        const char* lab = std::getenv("TARTARUS_WEAPON_LAB"); // run-weapon-lab.cmd (its launcher starts the exe without arguments)
        o.weaponLabMode = lab && *lab && *lab != '0';
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--smoke-test" || a == "--perf-bench") {
            o.smokeTestMode = true;
            o.perfBenchMode = a == "--perf-bench";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.smokeScenesDirArg = argv[++i];
        }
        else if (a == "--perf-sample") { o.perfSample = true; }
        else if (a == "--perf-res" && i + 1 < argc) {
#pragma warning(suppress : 4996)
            if (std::sscanf(argv[++i], "%dx%d", &o.perfResW, &o.perfResH) != 2 || o.perfResW <= 0 || o.perfResH <= 0)
                o.perfResW = o.perfResH = 0;
        }
        else if (a == "--perf-csv" && i + 1 < argc) { o.perfCsv = argv[++i]; }
        else if (a == "--jobs" && i + 1 < argc) { o.jobWorkers = std::max(0, std::atoi(argv[++i])); }
        else if (a == "--jobs-shuffle") { o.jobsShuffle = true; }
        else if (a == "--fixed-step") { o.fixedStep = true; }
        else if (a == "--render-height" && i + 1 < argc) { o.renderHeightArg = std::max(0, std::atoi(argv[++i])); }
        else if (a == "--weapon-test") { o.smokeTestMode = true; o.weaponTestMode = true; }
        else if (a == "--weapon-lab") { o.weaponLabMode = true; }
        else if (a == "--npc-test") {
            o.smokeTestMode = true; o.npcTestMode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.npcTestScenario = argv[++i];
        }
        else if (a == "--stock-probe") {
            o.smokeTestMode = true; o.weaponTestMode = true; o.stockProbeMode = true;
            if (i + 1 < argc && (std::string(argv[i + 1]) == "ak" || std::string(argv[i + 1]) == "remington")) o.stockProbeAk = std::string(argv[++i]) == "ak";
        }
        else if (a == "--smoke-shots" && i + 1 < argc) { o.smokeShotsDir = argv[++i]; }
        else if (a == "--editor-shot" && i + 1 < argc) { o.editorShotDir = argv[++i]; }
        else if (a == "--editor-tests") {
            o.editorTestsMode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.editorTestsFilter = argv[++i];
        }
        else if (a == "--editor-shot-select" && i + 1 < argc) { o.editorShotSelect = argv[++i]; }
        else if (a == "--editor-shot-default-layout") { o.editorShotDefaultLayout = true; }
        else if (a == "--editor-shot-curves") { o.editorShotCurves = true; }
        else if (a == "--editor-shot-ide") { o.editorShotIDE = true; }
        else if (a == "--editor-shot-scale" && i + 1 < argc) { o.editorShotScale = (float)std::atof(argv[++i]); }
        else if (a == "--outfit-shots" && i + 1 < argc) {
            o.smokeTestMode = true;
            o.outfitShotsDir = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') o.smokeScenesDirArg = argv[++i];
        }
        else if (a == "--resave" && i + 2 < argc) { o.resaveIn = argv[i + 1]; o.resaveOut = argv[i + 2]; i += 2; }
        else if (a == "--undo-bench") { o.undoBenchMode = true; }
        else if (a == "--gen-outfit-scenes") { o.outfitScenesMode = true; }
        else if (a == "--outfit-audit" || a == "--outfit-rules" || a == "--outfit-posed") {
            // --outfit-posed: --outfit-audit plus each pair skinned through walk/run/crouch/... poses.
            o.outfitAuditMode = true;
            o.outfitAuditGeometry = a != "--outfit-rules";
            o.outfitAuditPosed = a == "--outfit-posed";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.outfitAuditWardrobe = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') o.outfitAuditCsv = argv[++i];
        }
        else if (a == "--outfit-selftest") { o.outfitSelfTestMode = true; }
        else if (a == "--clip-report") {
            o.clipReportMode = true;
            while (i + 1 < argc && argv[i + 1][0] != '-') o.clipReportArgs.push_back(argv[++i]);
        }
        else if (a == "--outfit-cost") {
            o.outfitCostMode = true;
            while (i + 1 < argc && argv[i + 1][0] != '-') o.outfitCostScenes.push_back(argv[++i]);
        }
        else if (a == "--asset-load-bench") { o.assetLoadBenchMode = true; }
        else if (a == "--build") {
            o.buildMode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.buildOutArg = argv[++i];
        }
        else if (a == "--build-cancel-at" && i + 1 < argc) { o.buildCancelAt = std::atoi(argv[++i]); }
        else if (a == "--project" && i + 1 < argc) {
            std::error_code ec;
            const std::filesystem::path dir = std::filesystem::absolute(argv[++i], ec);
            if (!ec && std::filesystem::is_directory(dir, ec))
                ProjectPaths::SetRootOverride(dir.lexically_normal().string());
            else
                std::cerr << "--project: '" << argv[i] << "' is not a folder; ignoring it." << std::endl;
        }
    }
    return o;
}
