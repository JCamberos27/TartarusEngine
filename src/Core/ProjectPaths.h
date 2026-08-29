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

// The project directory: the nearest "project" folder found walking up from the working
// directory, or the working directory itself when there isn't one (so a stray exe run from
// anywhere still works, exactly as it did before). Resolved once, on first use.
const std::string& Root();

// Root() joined with `name`, as a native path string.
std::string Resolve(const std::string& name);

} // namespace ProjectPaths
