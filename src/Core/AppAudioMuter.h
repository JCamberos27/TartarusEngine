#pragma once
#include <string>
#include <vector>

// Mutes other apps' audio while asked to: the editor uses it to silence a keyboard-sound app (Mechvibes) while the keyboard
// is driving the view (the Scene camera flying, or Play mode holding the input), so WASD isn't a wall of key clicks. Windows
// only, through each app's own audio session (the per-app mute in the volume mixer); a no-op elsewhere. Best effort: the
// COM work runs on a worker thread, so SetMuted never blocks the frame.
//
// Only sessions this muted are unmuted again (one the user muted themself stays muted). Windows remembers an app's mute
// across runs, so while anything is muted a marker file lists it, and RecoverStale on the next start unmutes what a crash
// left muted.
namespace AppAudioMuter {

// The apps to mute: exe names, matched case-insensitively against each session's process image ("Mechvibes.exe").
void Configure(const std::vector<std::string>& exeNames);
// Muted while true. Cheap; call it every frame.
void SetMuted(bool muted);
// Unmutes and stops the worker (blocks until it has).
void Shutdown();
// Start-up: unmutes the apps a previous run's marker file lists (it crashed while muting them) and deletes the marker.
void RecoverStale();

// Pure helpers (unit-tested).
bool NameMatches(const std::string& imagePath, const std::vector<std::string>& exeNames);
std::vector<std::string> ParseNameList(const std::string& commaSeparated);
std::string JoinNameList(const std::vector<std::string>& names);

} // namespace AppAudioMuter
