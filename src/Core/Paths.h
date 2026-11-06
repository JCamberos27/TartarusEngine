#pragma once
#include <string>

// The engine's three path roots, in one place. Every path here is a std::string holding UTF-8:
// the exe manifest sets the process code page to UTF-8 (src/TartarusEngine.manifest), so narrow
// Win32, CRT and std::filesystem calls all read these strings the same way.
//
// EnginePaths  - the *shipped, read-only* assets (shaders, icon font, branding), next to the exe.
// ProjectPaths - the *editable* project content (scenes, layers.json, project settings).
// UserPaths    - per-user, per-machine editor state (prefs, shortcuts), under %LOCALAPPDATA%.
//
// Startup order: the overrides (ProjectPaths::SetRootOverride, UserPaths::SetAppName) come before
// the first Root() of that kind. A late override can't move a root other code already used, so it
// is ignored and logged as an error instead of silently splitting files across two folders.

namespace EnginePaths {

// Call once, early in main(). Finds the running exe through the OS (GetModuleFileNameW), so a
// launch through PATH, a shortcut or another working directory still finds the right folder.
// `argv0` is only the fallback when that fails. Safe to call again (recomputes).
void Init(const char* argv0);

// The directory holding the running executable. Empty before Init().
const std::string& ExeDir();

// A shipped-asset path ("assets/shaders", "assets/fonts/fa-solid-900.ttf") made absolute.
// First hit wins:
//   1. $TARTARUS_ASSET_ROOT/<rel>   explicit override (packaging, tests)
//   2. <exe dir>/<rel>              the normal case; CMake POST_BUILD stages assets/ by the exe
//   3. <ancestor of exe dir>/<rel>  an ancestor that directly holds the first component ("assets/")
//   4. <cwd>/<rel>                  returned even when missing, so diagnostics show a real path
// (Shaders used to be read from bare relative paths and only resolved from build/<Config>/; #355.)
std::string Resolve(const std::string& relativePath);

} // namespace EnginePaths

namespace ProjectPaths {

// The project directory: `--project <dir>` (SetRootOverride), else the nearest "project" folder
// walking up from the exe (#151), else from the working directory, else the working directory.
// Pinned on first use, after EnginePaths::Init().
const std::string& Root();

// `--project`, or a built player's shipped project/ folder (#174). Must come before the first Root().
void SetRootOverride(const std::string& root);

// Root() joined with `name`.
std::string Resolve(const std::string& name);

// The inverse of Resolve(), for display: relative to Root() with forward slashes, or the input
// unchanged when it isn't under Root(). Use it at log sites that would print a resolved path (#18).
std::string Relativize(const std::string& absolutePath);

} // namespace ProjectPaths

namespace UserPaths {

// %LOCALAPPDATA%\<app name>, created on first use; the working directory when there's no
// LOCALAPPDATA. Pinned on first use. (Prefs used to live in project/ and clashed across clones; #42.)
const std::string& Root();

// %LOCALAPPDATA% itself (no app name), or "" when unset. Doesn't pin Root(), so a harness can
// find the real prefs folder before switching to a scratch one.
std::string BaseDir();

// A built player (#174) or a test harness uses its own folder name. Must come before the first Root().
void SetAppName(const std::string& name);

// Root() joined with `name`.
std::string Resolve(const std::string& name);

} // namespace UserPaths
