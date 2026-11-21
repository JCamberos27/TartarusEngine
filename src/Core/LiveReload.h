#pragma once

// Whether runtime systems should watch files for changes and reload them (animator controllers,
// weapon presentation assets, weapon prefabs, shaders, the game module). On in the editor and in
// development builds; a shipped game turns it off at startup so it never polls the disk for
// edits nobody will make.
namespace LiveReload {
// Out of line (LiveReload.cpp): a function-local static in an inline function gets one copy per module, so with
// the core built as TartarusCore.dll the exe's SetEnabled(false) would not reach the core's watchers.
bool& EnabledFlag();
inline bool Enabled() { return EnabledFlag(); }
inline void SetEnabled(bool on) { EnabledFlag() = on; }
}
