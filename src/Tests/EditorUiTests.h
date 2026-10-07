#pragma once
#include <string>

class EditorLayer;
class World;
class AssetLibrary;
class Camera;

// --editor-tests [filter]: end-to-end tests of the editor UI, run in the real editor (hidden
// window, scratch user folder, an empty in-memory scene). Each test is a list of steps run one
// per frame, before the frame's input is processed: steps queue synthetic mouse / keyboard input
// through ImGui's input queue, find widgets by label through ImGui's test-engine item hooks, and
// check the editor's state. See EditorUiTests.cpp.
namespace EditorUiTests {

// Turns the harness on; tests whose name doesn't contain `filter` (case-insensitive) are skipped.
void Configure(const std::string& filter);
bool Enabled();
// main.cpp, once per frame right before EditorLayer::BeginFrame(). True once every test ran.
bool BeforeFrame(EditorLayer& editor, World& world, AssetLibrary& assets, Camera& camera);
// 0 when every test passed (valid after BeforeFrame returned true).
int ExitCode();

} // namespace EditorUiTests
