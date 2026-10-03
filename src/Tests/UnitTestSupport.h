#pragma once

// Shared by UnitTests.cpp and the per-area test files (UnitTests_<Area>.cpp), so each area can add
// tests in its own file instead of everyone editing UnitTests.cpp and its one big test table.

#include <functional>
#include <iostream>
#include <utility>
#include <vector>

namespace UnitTestSupport {

extern int g_Failures;
extern int g_Checks;
extern const char* g_CurrentTest;

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
void RegisterEngineTests(UnitTestSupport::TestList& tests);    // UnitTests_Engine.cpp
void RegisterBloodTests(UnitTestSupport::TestList& tests);     // UnitTests_Blood.cpp
