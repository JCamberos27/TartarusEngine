#pragma once

// Shared by UnitTests.cpp and the per-area test files (UnitTests_<Area>.cpp), so each area can add
// tests in its own file instead of everyone editing UnitTests.cpp and its one big test table.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace UnitTestSupport {

extern int g_Failures;
extern int g_Checks;
extern const char* g_CurrentTest;
extern std::string g_SkipReason;

// A test that can't run here (project content missing) says why and returns: the runner reports it
// as SKIP, not PASS, and counts it. A test that ran some checks before skipping the rest is
// "PASS (partly skipped: ...)".
inline void Skip(const std::string& why) { g_SkipReason = why; }

// A temp-file name of its own for this run (`stem` + a per-process stamp + `ext`), so two test runs at once
// (several worktrees, CI shards) don't overwrite each other's files.
inline std::string TempName(const std::string& stem, const std::string& ext = "") {
    static const std::string stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return stem + "_" + stamp + ext;
}

using TestList = std::vector<std::pair<const char*, std::function<void()>>>;

// A scratch folder under the system temp directory (created when missing), or its `sub` folder. The runner
// points the temp directory at a folder of its own per run, so fixed names here don't collide across runs.
inline std::filesystem::path TempDir(const std::string& sub = {}) {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec) / "TartarusUnitTests";
    if (!sub.empty()) dir /= sub;
    std::filesystem::create_directories(dir, ec);
    return dir;
}
// A whole file as bytes ("" when it can't be read).
inline std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Registers the engine components once (tests that look components up by name or serialize a scene need them).
void EnsureEngineComponents(); // UnitTests.cpp

} // namespace UnitTestSupport

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        ++UnitTestSupport::g_Checks;                                                             \
        if (!(cond)) {                                                                           \
            ++UnitTestSupport::g_Failures;                                                       \
            std::cout << "[UnitTest] FAIL " << UnitTestSupport::g_CurrentTest << ": " #cond " (" \
                      << __FILE__ << ":" << __LINE__ << ")\n";                                   \
        }                                                                                        \
    } while (0)

// Each area appends its tests to the list; RunUnitTests runs them after the core table.
void RegisterRagdollTests(UnitTestSupport::TestList& tests);   // UnitTests_Ragdoll.cpp
void RegisterAnimationTests(UnitTestSupport::TestList& tests); // UnitTests_Animation.cpp
void RegisterEditorTests(UnitTestSupport::TestList& tests);    // UnitTests_Editor.cpp
void RegisterEnhancerTests(UnitTestSupport::TestList& tests);  // UnitTests_Enhancers.cpp
void RegisterRecoilPortTests(UnitTestSupport::TestList& tests);
void RegisterSwayPortTests(UnitTestSupport::TestList& tests);
void RegisterEngineTests(UnitTestSupport::TestList& tests);    // UnitTests_Engine.cpp
void RegisterBloodTests(UnitTestSupport::TestList& tests);     // UnitTests_Blood.cpp
void RegisterGlassTests(UnitTestSupport::TestList& tests);     // UnitTests_Glass.cpp
void RegisterCharacterMoverTests(UnitTestSupport::TestList& tests); // UnitTests_CharacterMover.cpp
void RegisterLaunchOptionsTests(UnitTestSupport::TestList& tests); // UnitTests_LaunchOptions.cpp
void RegisterMotionMatchingTests(UnitTestSupport::TestList& tests); // UnitTests_MotionMatching.cpp
void RegisterMotionMatchingRuntimeTests(UnitTestSupport::TestList& tests); // UnitTests_MotionMatchingRuntime.cpp
void RegisterRenderTests(UnitTestSupport::TestList& tests); // UnitTests_Render.cpp
void RegisterGrassTests(UnitTestSupport::TestList& tests); // UnitTests_Grass.cpp
void RegisterTerrainTests(UnitTestSupport::TestList& tests); // UnitTests_Terrain.cpp
void RegisterWorldTextTests(UnitTestSupport::TestList& tests); // UnitTests_WorldText.cpp
void RegisterOpenableTests(UnitTestSupport::TestList& tests); // UnitTests_Openable.cpp
void RegisterLightSwitchTests(UnitTestSupport::TestList& tests); // UnitTests_LightSwitch.cpp
void RegisterCinematicTests(UnitTestSupport::TestList& tests); // UnitTests_Cinematic.cpp
void RegisterGameplayTests(UnitTestSupport::TestList& tests);
void RegisterEditorSoundTests(UnitTestSupport::TestList& tests); // UnitTests_EditorSound.cpp
void RegisterAudioTests(UnitTestSupport::TestList& tests); // UnitTests_Audio.cpp
void RegisterDecalRenderTests(UnitTestSupport::TestList& tests); // UnitTests_DecalRender.cpp
void RegisterDecalRuntimeTests(UnitTestSupport::TestList& tests); // UnitTests_DecalRuntime.cpp
void RegisterDecalAssetTests(UnitTestSupport::TestList& tests); // UnitTests_DecalAssets.cpp
void RegisterGraffitiTests(UnitTestSupport::TestList& tests); // UnitTests_Graffiti.cpp
void RegisterGizmoIconTests(UnitTestSupport::TestList& tests); // UnitTests_GizmoIcons.cpp
void RegisterAssetLibraryTests(UnitTestSupport::TestList& tests); // UnitTests_AssetLibrary.cpp
void RegisterFirstPersonBodyTests(UnitTestSupport::TestList& tests); // UnitTests_FirstPersonBody.cpp
void RegisterOutfitsTests(UnitTestSupport::TestList& tests); // UnitTests_Outfits.cpp
void RegisterFirstPersonWeaponsTests(UnitTestSupport::TestList& tests); // UnitTests_FirstPersonWeapons.cpp
void RegisterCoreTests(UnitTestSupport::TestList& tests); // UnitTests_Core.cpp
void RegisterAssetsTests(UnitTestSupport::TestList& tests); // UnitTests_Assets.cpp
