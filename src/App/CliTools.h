#pragma once

#include <string>

struct LaunchOptions;

// The command-line tools that run and exit before the editor starts: no window, no GL context, no audio device.

// The tools that run before anything else is set up: --unit-tests, --model-report, --model-nodes, --scene-bounds,
// --bake-impostors and --audio-test. Returns the run's exit code when opt.mode is one of them, or -1 otherwise.
int RunEarlyCliTool(const LaunchOptions& opt, const char* argv0);

// #174 - `--build [outDir]`: File > Build without the UI, on a worker thread with progress as the editor's Build button
// runs it. `outDir` (when not empty) overrides Project Settings > Build's Output Folder. Needs EnginePaths set up. Returns the exit code (0 = built).
int RunBuildTool(const std::string& outDir);
