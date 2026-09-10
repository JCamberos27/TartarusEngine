#pragma once
#include <string>

// Where the engine's *shipped, read-only* assets live — GLSL shaders, the icon font, branding
// images. Distinct from ProjectPaths, which locates the *editable* project content (scenes,
// preferences) by walking up for a "project/" folder.
//
// These used to be bare relative strings ("assets/shaders", "assets/branding/splash.png")
// passed straight to fstream, so they only resolved when the working directory happened to be
// build/<Config>/. Launched from the repo root (the documented CI --smoke-test invocation) or
// any other directory, every shader read returned "" and the driver reported a misleading
// "must write to gl_Position" link error instead of "file not found". See audit BUG-102 / #355.
//
// Resolution order (first hit wins), established once at startup from argv[0]:
//   1. $TARTARUS_ASSET_ROOT/<rel>              — explicit override (packaging, tests)
//   2. <exe dir>/<rel>                          — the normal case; CMake POST_BUILD stages
//                                                assets/ next to the executable
//   3. <ancestor of exe dir>/<rel>, where the ancestor directly contains an "assets/" folder
//                                                — covers unusual out-of-tree layouts
//   4. <cwd>/<rel>                              — historical fallback; returned even when it
//                                                does not exist so diagnostics show a real path
namespace EnginePaths {

// Call once, as early in main() as possible, with argv[0]. Safe to call again (recomputes).
void Init(const char* argv0);

// The directory containing the running executable, as resolved by Init(). Empty before Init().
const std::string& ExeDir();

// Resolve a shipped-asset path (e.g. "assets/shaders", "assets/branding/splash.png") to an
// absolute native path string using the order documented above.
std::string Resolve(const std::string& relativePath);

// True if Resolve(relativePath) names something that exists on disk.
bool Exists(const std::string& relativePath);

} // namespace EnginePaths
