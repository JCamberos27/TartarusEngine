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
// main.cpp, once per frame after the editor has drawn into the default framebuffer and before
// SwapBuffers: takes the screenshot a Shot() step asked for, if any.
void AfterFrame(int fbWidth, int fbHeight);
// 0 when every test passed (valid after BeforeFrame returned true).
int ExitCode();
// Flushes the output and ends the process with `code` without running exit(): on CI the CRT's
// ExitProcess first kills every other thread (the watchdog too), then DLL detach can stall for
// many minutes or forever, so nothing is left to end it. Call once the editor has shut down.
[[noreturn]] void EndProcess(int code);

} // namespace EditorUiTests
