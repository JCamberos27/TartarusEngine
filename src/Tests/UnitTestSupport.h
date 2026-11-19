#pragma once

// Shared by UnitTests.cpp and the per-area test files (UnitTests_<Area>.cpp), so each area can add
// tests in its own file instead of everyone editing UnitTests.cpp and its one big test table.

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

using TestList = std::vector<std::pair<const char*, std::function<void()>>>;

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
