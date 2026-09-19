#pragma once
#include <string>
#include <vector>

// #132 - notices files added, removed, renamed or edited in the project folder while the editor
// runs (Explorer, git, an image editor saving over a texture), the way Unity's Asset Database
// does. A background thread reads the OS change journal for the folder tree
// (ReadDirectoryChangesW); the main thread drains settled changes once per frame.
//
// Changes are coalesced per path and only handed out once a path has been quiet for `settleMs`,
// so a file still being written (a big FBX copy, a PNG export) isn't read half-finished. Editor-
// only folders (Library/, screenshots/, layouts/) and temp files are never reported.
namespace ProjectWatcher {

struct Change {
    enum class Kind { Added, Removed, Modified, Renamed };
    Kind Type = Kind::Modified;
    std::string Path;     // absolute, native separators
    std::string OldPath;  // Renamed only: where it was before
};

// Starts watching `root` (recursively). Safe to call again with the same root; a different root
// restarts the watch. False when the folder can't be watched (it's then simply not watched).
bool Start(const std::string& root);
void Stop();
bool IsRunning();

// Every change whose path has been quiet for at least `settleMs`. When the OS journal
// overflowed (thousands of changes at once) `overflowed` is set: callers should rescan instead.
std::vector<Change> Drain(int settleMs, bool& overflowed);

} // namespace ProjectWatcher
