#pragma once
#include <string>

// Where per-user, per-machine editor state lives — preferences and keyboard-shortcut overrides
// (editor_prefs.json, shortcuts.json). Distinct from ProjectPaths, which locates the *shared,
// authored* project content (scenes, layers.json, project settings) meant to be committed and
// carried across machines/teammates via version control.
//
// Defect #42 (Phase 0.5): these two files used to resolve under ProjectPaths — i.e. the
// version-controlled project/ folder — so every clone shared one prefs file, and two machines
// (or two teammates) editing the same project fought over UI scale, theme, window layout and
// keybindings on every commit. They now resolve here instead: %LOCALAPPDATA%\TartarusEngine\ on
// Windows, created on first use.
namespace UserPaths {

// The per-user state directory. Resolved once, on first use.
const std::string& Root();

// #174 - a built player keeps its own logs/prefs under %LOCALAPPDATA%\<product> instead of the
// editor's folder. Must be called before the first Root().
void SetAppName(const std::string& name);

// Root() joined with `name`, as a native path string.
std::string Resolve(const std::string& name);

} // namespace UserPaths
