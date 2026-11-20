// #173 - headless unit tests: `TartarusEngine.exe --unit-tests`.
//
// Runs before any window, GL context or audio device exists, so it works on a CI runner with no
// GPU (unlike --smoke-test). Almost everything here is pure C++: undo deltas,
// GUIDs, atomic file writes, texture-cache keys, material JSON robustness, the component
// registry, the Animator Controller, asset identity / GUID-following references and the project
// file watcher (#132). Each CHECK prints on failure; the run returns the number of failed checks (0 = pass).
//
// The one exception is PhysicsWorldSync (#201), which stands up a real PhysX world. PhysX needs
// no GL, and that check previously lived only in --smoke-test, which CI runs with
// continue-on-error for want of a GPU - so it never gated anything. It runs last, and shuts the
// PhysX core back down when it is done.
//
// Deliberately no test framework dependency: a CHECK macro and a list of functions is all this
// needs, and it keeps the engine's third-party surface unchanged.
#include "TimeService.h"
#include "UnitTestSupport.h"
#include "../Game/FirstPersonMovementInput.h"
#include "UnitTests.h"

#include "AnimatorController.h"
#include "AnimatorLint.h"
#include "RootMotion.h"
#include "FirstPersonBody.h"
#include "WeaponJankMeter.h"
#include "FirstPersonBodyContract.h"
#include "Curve.h"
#include "IK.h"
#include "FirstPersonWeaponWizard.h"
#include "ClipAnalysis.h"
#include "FirstPersonAnimation.h"
#include "WeaponPrefab.h"
#include "FirstPersonAdsCarry.h"
#include "AudioEngine.h"
#include <set>
#include "Log.h" // #178 stack traces
#include "InputMap.h"
#include "AssetDatabase.h"
#include "AssetGuid.h"
#include "Components.h"
#include "AtomicFile.h"
#include "Utf8.h"
#include "Camera.h"
#include "PhysicsWorld.h"  // #201 - the physics sync regression test
#include "Combat/Damage.h" // hit regions, damage zones
#include "Npc/NpcRagdoll.h" // the soldier's part table
#include "GameModuleAPI.h"  // QueryFilter / RaycastHit
#include <cmath>   // #202 isfinite
#include <limits>
#include "ComponentReflection.h"
#include "ComponentRegistry.h"
#include "AssetLibrary.h" // #178 preset apply
#include "MaterialAsset.h"
#include "PhysicMaterialAsset.h"
#include "PlayerConfig.h" // #174 - player.json round-trip
#include "ProjectSettings.h"
#include "Player.h"
#include "PostProcessVolume.h"
#include "Tonemapper.h"
#include "Core/Paths.h"
#include "ProjectWatcher.h"
#include "TextureCache.h"
#include "Model.h"       // texture-path resolver
#include "AssetImport.h" // folder import
#include "ImportQueueManager.h"
#include "SceneSerializer.h" // #121
#include "AnimationSystem.h"           // #123 - the spin systems, driven frame by frame below
#include "GameRules.h"
#include "HotReloadGameModule.h"      // Spin / Transform Controller run inside TartarusGame.dll, not the exe
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetModuleFileNameW: --unit-tests runs before EnginePaths::Init
#undef near            // windows.h #defines these two, and the tests use them as variable names
#undef far
#include "UndoDeltaChain.h"
#include "World.h"
#include "ImpostorLibrary.h"
#include "BulletHoles.h"
#include "ShellCasings.h"
#include "RotationMath.h"
#include "SkyAtmosphere.h"
#include "SkySettings.h"
#include "TimeOfDay.h"

#include <glm/gtc/matrix_transform.hpp>

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include "Wardrobe.h"
#include "OutfitCoverage.h"
#include "OutfitSystem.h"
#include "Texture.h"
#include "PngWrite.h" // async decode test fixture
#include "EditorLayerInternal.h" // ClipFilterMatch
#include <set>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

using json = nlohmann::json;

namespace UnitTestSupport {

int g_Failures = 0;
int g_Checks = 0;
const char* g_CurrentTest = "";
std::string g_SkipReason;

} // namespace UnitTestSupport

using UnitTestSupport::g_Checks;
using UnitTestSupport::g_CurrentTest;
using UnitTestSupport::g_Failures;
using UnitTestSupport::TempDir;
using UnitTestSupport::ReadAll;

namespace {



// #162 / #203 - Post-process Volumes: full weight inside the box, linear fade over the blend
// distance, nothing beyond; overrides blend in priority order.
void TestPostProcessVolume() {
    const glm::mat4 at = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    const glm::vec3 size(4.0f);
    CHECK(PostVolumeWeight(false, size, 2.0f, 1.0f, at, glm::vec3(10.0f, 0.0f, 0.0f)) == 1.0f);
    CHECK(PostVolumeWeight(false, size, 2.0f, 1.0f, at, glm::vec3(11.9f, 0.0f, 0.0f)) == 1.0f);
    CHECK(std::abs(PostVolumeWeight(false, size, 2.0f, 1.0f, at, glm::vec3(13.0f, 0.0f, 0.0f)) - 0.5f) < 1e-4f);
    CHECK(PostVolumeWeight(false, size, 2.0f, 1.0f, at, glm::vec3(20.0f, 0.0f, 0.0f)) == 0.0f);
    CHECK(PostVolumeWeight(true, size, 2.0f, 0.4f, at, glm::vec3(1000.0f)) == 0.4f);
    const glm::mat4 scaled = glm::scale(at, glm::vec3(2.0f)); // box now 8 m wide
    CHECK(PostVolumeWeight(false, size, 0.0f, 1.0f, scaled, glm::vec3(13.5f, 0.0f, 0.0f)) == 1.0f);

    World world;
    const entt::entity a = world.Registry.create();
    world.Registry.emplace<TransformComponent>(a);
    auto& va = world.Registry.emplace<PostProcessVolumeComponent>(a);
    va.Global = true; va.OverrideSaturation = true; va.Saturation = -100.0f; va.Priority = 1;
    const entt::entity b = world.Registry.create();
    world.Registry.emplace<TransformComponent>(b);
    auto& vb = world.Registry.emplace<PostProcessVolumeComponent>(b);
    vb.Global = true; vb.OverrideSaturation = true; vb.Saturation = 50.0f; vb.OverrideExposure = true; vb.ExposureEV = 2.0f;
    vb.Weight = 0.5f; vb.Priority = 0;
    world.RebuildWorldTransformCache();
    PostSettings p;
    p.Saturation = 0.0f; p.ExposureEV = 0.0f;
    ApplyPostProcessVolumes(world, glm::vec3(0.0f), p);
    CHECK(p.Saturation == -100.0f);                  // b (priority 0) then a (priority 1, weight 1) wins
    CHECK(std::abs(p.ExposureEV - 1.0f) < 1e-5f);    // only b overrides exposure, at weight 0.5
    world.Registry.emplace<DeactivatedTag>(a);
    world.RebuildWorldTransformCache();
    p.Saturation = 0.0f;
    ApplyPostProcessVolumes(world, glm::vec3(0.0f), p);
    CHECK(std::abs(p.Saturation - 25.0f) < 1e-4f);   // inactive volumes don't apply
}

// The clip pickers' search: every word, any case, anywhere in the path.
void TestClipFilterMatch() {
    using EditorInternal::ClipFilterMatch;
    const std::string path = "Animations/Mocap/AF_Walk/Walk_Fwd_NoRM.fbx";
    CHECK(ClipFilterMatch(nullptr, path) && ClipFilterMatch("", path) && ClipFilterMatch("   ", path));
    CHECK(ClipFilterMatch("walk", path) && ClipFilterMatch("WALK fwd", path) && ClipFilterMatch("  norm  af_", path));
    CHECK(!ClipFilterMatch("run", path) && !ClipFilterMatch("walk run", path) && !ClipFilterMatch("fwdx", path));
}











// Every test writes under temp_directory_path(), and several runs can go at once (worktrees, CI-style queues): two runs
// sharing %TEMP% wiped each other's fixture folders mid-test (the watcher and asset-identity tests failed "under load").
// Each run gets its own folder under the user's temp folder, removed when the run ends.
struct PrivateTempDir {
    std::filesystem::path Dir;
    std::wstring OldTmp, OldTemp;
    bool HadTmp = false, HadTemp = false;
    static bool Get(const wchar_t* name, std::wstring& out) {
        const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
        if (n == 0) return false;
        out.resize(n);
        out.resize(GetEnvironmentVariableW(name, out.data(), n));
        return true;
    }
    PrivateTempDir() {
        std::error_code ec;
        Dir = std::filesystem::temp_directory_path(ec) / ("TartarusUnitTests-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(Dir, ec);
        if (!std::filesystem::create_directories(Dir, ec) || ec) { Dir.clear(); return; }
        HadTmp = Get(L"TMP", OldTmp);
        HadTemp = Get(L"TEMP", OldTemp);
        SetEnvironmentVariableW(L"TMP", Dir.wstring().c_str());
        SetEnvironmentVariableW(L"TEMP", Dir.wstring().c_str());
    }
    ~PrivateTempDir() {
        if (Dir.empty()) return;
        SetEnvironmentVariableW(L"TMP", HadTmp ? OldTmp.c_str() : nullptr);
        SetEnvironmentVariableW(L"TEMP", HadTemp ? OldTemp.c_str() : nullptr);
        std::error_code ec;
        std::filesystem::remove_all(Dir, ec);
    }
    PrivateTempDir(const PrivateTempDir&) = delete;
    PrivateTempDir& operator=(const PrivateTempDir&) = delete;
};
} // namespace

void UnitTestSupport::EnsureEngineComponents() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
}

int RunUnitTests(const char* filter) {
    const PrivateTempDir privateTemp;
    UnitTestSupport::TestList tests = {
        {"PostProcessVolume", TestPostProcessVolume},
        {"ClipFilterMatch", TestClipFilterMatch},
    };
    RegisterRagdollTests(tests);
    RegisterLaunchOptionsTests(tests);
    RegisterAnimationTests(tests);
    RegisterMotionMatchingTests(tests); // UnitTests_MotionMatching.cpp
    RegisterMotionMatchingRuntimeTests(tests); // UnitTests_MotionMatchingRuntime.cpp
    RegisterEditorTests(tests);
    RegisterEnhancerTests(tests);
    RegisterRecoilPortTests(tests);
    RegisterSwayPortTests(tests);
    RegisterRenderTests(tests); // UnitTests_Render.cpp
    RegisterGrassTests(tests); // UnitTests_Grass.cpp
    RegisterWorldTextTests(tests); // UnitTests_WorldText.cpp
    RegisterOpenableTests(tests); // UnitTests_Openable.cpp
    RegisterLightSwitchTests(tests); // UnitTests_LightSwitch.cpp
    RegisterCinematicTests(tests); // UnitTests_Cinematic.cpp
    RegisterFirstPersonWeaponsTests(tests); // UnitTests_FirstPersonWeapons.cpp
    RegisterOutfitsTests(tests); // UnitTests_Outfits.cpp
    RegisterFirstPersonBodyTests(tests); // UnitTests_FirstPersonBody.cpp
    RegisterCharacterMoverTests(tests); // UnitTests_CharacterMover.cpp
    RegisterEngineTests(tests);
    RegisterGameplayTests(tests);
    RegisterBloodTests(tests);
    RegisterGlassTests(tests);
    RegisterEditorSoundTests(tests); // UnitTests_EditorSound.cpp
    RegisterAudioTests(tests); // UnitTests_Audio.cpp
    RegisterDecalRenderTests(tests); // UnitTests_DecalRender.cpp
    RegisterDecalRuntimeTests(tests); // UnitTests_DecalRuntime.cpp
    RegisterDecalAssetTests(tests); // UnitTests_DecalAssets.cpp
    RegisterGraffitiTests(tests); // UnitTests_Graffiti.cpp
    RegisterGizmoIconTests(tests); // UnitTests_GizmoIcons.cpp
    RegisterAssetLibraryTests(tests); // UnitTests_AssetLibrary.cpp
    RegisterAssetsTests(tests); // UnitTests_Assets.cpp
    RegisterCoreTests(tests); // UnitTests_Core.cpp
    {
        std::set<std::string> names;
        for (const auto& t : tests)
            if (!names.insert(t.first).second) {
                std::cout << "[UnitTest] FAIL two tests are both named \"" << t.first << "\"\n";
                ++g_Failures;
            }
    }
    int ran = 0, skipped = 0;
    for (const auto& [name, fn] : tests) {
        if(filter && std::string(name).find(filter)==std::string::npos) continue;
        ++ran;
        g_CurrentTest = name;
        // Named before it runs, with both streams flushed (tests and the engine also print through printf and the
        // log), so a test that hangs until a CI timeout is the last line in the log.
        std::cout << "[UnitTest] RUN  " << name << std::endl;
        std::fflush(stdout);
        UnitTestSupport::g_SkipReason.clear();
        const int before = g_Failures, checksBefore = g_Checks;
        const auto started = std::chrono::steady_clock::now();
        try {
            fn();
        } catch (const std::exception& e) {
            ++g_Failures;
            std::cout << "[UnitTest] FAIL " << name << ": threw " << e.what() << "\n";
        } catch (...) {
            ++g_Failures;
            std::cout << "[UnitTest] FAIL " << name << ": threw a non-std exception\n";
        }
        const std::string& skip = UnitTestSupport::g_SkipReason;
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const std::string took = " (" + std::to_string(ms) + " ms)";
        if (g_Failures != before) std::cout << "[UnitTest] FAIL " << name << took << "\n";
        else if (!skip.empty() && g_Checks == checksBefore) { ++skipped; std::cout << "[UnitTest] SKIP " << name << ": " << skip << took << "\n"; }
        else if (!skip.empty()) std::cout << "[UnitTest] PASS " << name << " (partly skipped: " << skip << ")" << took << "\n";
        else std::cout << "[UnitTest] PASS " << name << took << "\n";
        std::cout.flush();
        std::fflush(stdout);
    }
    // A filter that matches nothing is a typo, not a pass.
    if (filter && ran == 0) {
        std::cout << "[UnitTest] FAIL no test name contains \"" << filter << "\"\n";
        ++g_Failures;
    }
    std::cout << "[UnitTest] " << g_Checks << " checks, " << g_Failures << " failure(s), " << ran << " test(s) run, " << skipped
              << " skipped\n";
    std::cout.flush();
    return g_Failures;
}
