#pragma once
#include <functional>
#include <string>

// Files changed on disk while the editor runs, handed to whoever caches them. ProjectWatcher
// notices the change; EditorLayer::SyncProjectChanges (main thread, once per frame) does the
// editor's own work (GUIDs, the Asset Library, scene references) and then calls Dispatch, which
// runs every hook registered for the file's asset type (AssetDatabase::AssetType) or for that
// exact file. A cache that registers here can stop polling its files' mtimes while Watching()
// holds; it keeps its poll as the fallback when nothing watches (a built game, headless runs).
//
// Hooks run on the main thread, for edits made outside the editor and the editor's own saves
// alike (Event::BySelf tells them apart), so a hook that replaces a poll misses nothing.
namespace AssetChangeHooks {

struct Event {
    enum class Kind { Added, Removed, Modified, Moved, Rescan };
    Kind Type = Kind::Modified;
    std::string Path;      // absolute; empty for Rescan
    std::string OldPath;   // Moved only: where it was before
    std::string AssetType; // AssetDatabase::AssetType(Path); empty for a non-asset file
    bool BySelf = false;   // the editor wrote it itself (AtomicFile) or renamed it itself
};

// Returns true when it applied something (a reload, a cache drop): counted, and the editor
// plays its "sync done" chime for changes made outside it.
using Hook = std::function<bool(const Event&)>;

// Every change to a file of `assetType` ("texture", "animatorcontroller", ...). Returns an id for
// Unregister. Rescan (the OS journal overflowed: anything may have changed) reaches every hook.
int Register(const std::string& assetType, Hook hook);
// Every change to one file (absolute, or relative to the project folder), asset or not
// (settings.json, editor_ui_styles.json). A relative path is resolved at each Dispatch, so it
// follows the open project. A move reaches it when either end is that file.
int RegisterFile(const std::string& path, Hook hook);
void Unregister(int id);

// Runs the matching hooks; returns how many reported applying something. A hook registered from
// inside a hook runs from the next Dispatch; one unregistered from inside a hook stops at once (it
// isn't called later in the same Dispatch).
int Dispatch(const Event& e);

// True while ProjectWatcher is watching the project folder (the editor): changes arrive here,
// so a poll covered by a hook can be skipped.
bool Watching();

} // namespace AssetChangeHooks
