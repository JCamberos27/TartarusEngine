#pragma once
#include <string>

// Live motion matching rebake (hot reload, H-3): a saved .mmconfig re-runs `--bake-motion` on it in a child process (the
// running editor's own exe), off the UI thread. When the bake succeeds, its database's cached loads are dropped
// (SharedMotionDatabase::Invalidate) and a body playing it restarts on the new one. A rewritten `.mmanim` / `.mmdb` (a bake
// run from the command line) is picked up the same way, without a bake.
namespace MotionRebake {
// `path` is a motion matching bake config (.mmconfig).
bool IsConfig(const std::string& path);
// `path` is a baked motion database file (.mmanim / .mmdb); `base` gets the path without the extension.
bool IsBakedDatabase(const std::string& path, std::string* base = nullptr);
// Starts (or queues behind a running one) a background bake of `configPath`. False when the config doesn't load.
bool Request(const std::string& configPath);
bool Running();
// `<base>.mmanim` / `.mmdb` changed on disk (not by a bake of ours): invalidated once both have been quiet a moment.
void NotifyDatabaseChanged(const std::string& base);
// Call once a frame: reaps a finished bake (invalidating its database on success) and starts a queued one.
void Poll();
// Waits for a running bake (editor shutdown); a std::thread must not outlive the process.
void Shutdown();
}
