#pragma once
#include <string>

// Where the engine's editable content lives (the scene being edited, editor preferences).
//
// These used to be bare relative paths ("scene.json"), which resolved against the working
// directory — i.e. build/Release/ when running the built exe. That put authored content inside
// a build output directory: gitignored, and destroyed by a clean rebuild. Everything here now
// resolves against a "project/" folder committed alongside the source instead.
//
// Deliberately minimal: this is the seed of a real project system (a project root that also owns
// Assets/, Scenes/, and a baked import cache), not that system itself. It exists today only to
// keep authored content out of build/.
namespace ProjectPaths {

// The project directory: `--project <dir>` if given (SetRootOverride), else the nearest
// "project" folder walking up from the executable (#151), else from the working directory,
// else the working directory itself (so a stray exe run from anywhere still works). Resolved
// once, on first use — after EnginePaths::Init().
const std::string& Root();

// #174 - a built player pins the project to the folder shipped next to its exe instead of
// walking up from the working directory. Must be called before the first Root().
void SetRootOverride(const std::string& root);

// Root() joined with `name`, as a native path string.
std::string Resolve(const std::string& name);

// The inverse of Resolve(), for display: an absolute path made relative to Root() with forward
// slashes (`std::filesystem`'s portable `generic_string()` form), or the input unchanged if it
// isn't under Root() at all. Defect #18 — Console log lines used to print full native absolute
// paths, and different call sites picked different separators (one used `.string()`, another
// `.generic_string()`), so the same session showed both `c:\...\Showcase.json` and
// `c:/.../Showcase_...png`. Use this at any log call site that would otherwise embed a resolved
// project path.
std::string Relativize(const std::string& absolutePath);

} // namespace ProjectPaths
