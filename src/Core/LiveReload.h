#pragma once

// Whether runtime systems should watch files for changes and reload them (animator controllers,
// weapon presentation assets, weapon prefabs, shaders, the game module). On in the editor and in
// development builds; a shipped game turns it off at startup so it never polls the disk for
// edits nobody will make.
namespace LiveReload {
inline bool& EnabledFlag() {
    static bool enabled = true;
    return enabled;
}
inline bool Enabled() { return EnabledFlag(); }
inline void SetEnabled(bool on) { EnabledFlag() = on; }
}
