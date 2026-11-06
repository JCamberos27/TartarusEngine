#pragma once

#include <string>

// The command-line tools that run and exit before the editor starts: no window, no GL context, no audio device.

// The tools that run before anything else is set up: --unit-tests [filter], --model-report <folder or model ...>,
// --model-nodes, --scene-bounds and --audio-test [scene], tried in that order. Returns the run's exit code when argv names one of them, or -1 when it names none.
int RunEarlyCliTool(int argc, char** argv);

// #174 - `--build [outDir]`: File > Build without the UI, on a worker thread with progress as the editor's Build button
// runs it. `outDir` (when not empty) overrides Project Settings > Build's Output Folder. Needs EnginePaths set up. Returns the exit code (0 = built).
int RunBuildTool(const std::string& outDir);
