#pragma once
#include <filesystem>
#include <string>

// Rebuilds the reloadable C++ modules (TartarusGame, TartarusEditor) from inside the editor with the
// same CMake build tree the editor came from. The running editor picks the new DLLs up through
// HotReloadGameModule / HotReloadEditorModule, so a save in the Script IDE plays without a restart.
namespace GameModuleBuild {
// The engine's source checkout and CMake build tree both exist (false in a packaged build).
bool Available();
std::filesystem::path SourceRoot();
// Starts a background `cmake --build` of the reloadable modules. False if one is running or unavailable.
bool Start();
bool Running();
// Queues a rebuild for when the current one finishes (several saves in a row become one more build).
void Request();
// Call once a frame: reaps a finished build and starts a queued one.
void Poll();
bool LastSucceeded();
// Combined compiler/linker output of the last finished build.
std::string LastLog();
// Waits for a running build (editor shutdown); a std::thread must not outlive the process.
void Shutdown();
// True for C++ sources that are compiled into a reloadable module (a save can be hot reloaded).
bool IsReloadableSource(const std::filesystem::path& path);
}
