#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ProjectSettings { struct BuildSettings; }

// #174 - File > Build: exports a standalone, runnable copy of the game.
//
// Output layout (<out> = Build Settings > Output Folder, default <repo>/Builds/<Product>):
//   <out>/<Product>.exe     the engine exe; player.json beside it makes it start as the game
//   <out>/*.dll             runtime + game module (PhysX, TartarusGame, ...)
//   <out>/assets/           engine assets (shaders, fonts, branding)
//   <out>/project/          the project's content, minus editor-only folders (Library cache,
//                           screenshots, layouts) and with only the scenes that are in the build
//   <out>/player.json       product name, scene list, resolution, fullscreen, vsync
//
// Not yet: a separate editor-free player exe, dependency-walked asset cooking, archives.
namespace BuildPipeline {

struct Report {
    bool Ok = false;
    std::string Message;   // one line: what happened, or why it failed
    std::string OutputDir;
    std::string ExePath;
    int FileCount = 0;
    std::uint64_t TotalBytes = 0;
    std::vector<std::pair<std::string, std::uint64_t>> BytesByGroup;   // "Engine", "Scenes", ...
    std::vector<std::pair<std::string, std::uint64_t>> LargestFiles;  // relative path, bytes (top 10)
    double Seconds = 0.0;
    int ShadersChecked = 0; // #208 - project .shader descriptors whose stages/includes were validated
};

// Where a build with these settings lands (resolves the empty default).
std::string ResolveOutputDir(const ProjectSettings::BuildSettings& settings);

// Project-relative paths of every scene under project/scenes, sorted.
std::vector<std::string> FindProjectScenes();

Report Build(const ProjectSettings::BuildSettings& settings);

// Starts the built exe with its own folder as the working directory. False if it couldn't.
bool Run(const std::string& exePath);

} // namespace BuildPipeline
