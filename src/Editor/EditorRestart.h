#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Rebuild & Restart (File menu, Ctrl+Alt+R): builds the whole editor (exe + both modules) from the
// CMake tree it came from, then starts the new exe and lets this one close, handing over the open
// scene, its unsaved edits, the selection and Play through a RestartState file.
//
// The running exe can't be overwritten but can be renamed, so Request() moves it to
// <exe>.old first and the linker writes a fresh <exe>. A failed build puts the old name back.
// The new process waits for the old one to exit (--wait-pid), then deletes <exe>.old.
namespace EditorRestart {

// The exe lives in a CMake build tree with its source checkout (false in a packaged build).
bool Available();
// Why Available() is false, for the menu item's tooltip ("" when available).
std::string UnavailableReason();

// Starts the background build. False (with a log line) if one is running, the module build is busy,
// or the exe couldn't be renamed out of the linker's way.
bool Request();
bool Building();
// Call once a frame: reaps a finished build. A failure restores the exe name and logs the error.
void Poll();
// The build succeeded: the caller saves the state and calls Relaunch(), then closes.
bool ReadyToRelaunch();
// Clears ReadyToRelaunch without relaunching (the app vetoed the restart).
void Cancel();
std::string LastLog();

// Starts `exe` with this process's arguments minus any old --restore-state/--wait-pid, plus
// --restore-state <stateFile> --wait-pid <this pid>. False (nothing started, error logged) on failure:
// the caller keeps running.
bool Relaunch(const std::filesystem::path& stateFile);

// The arguments Relaunch passes, from this process's `args` (argv[1..]); exposed for the unit test.
std::vector<std::wstring> RelaunchArgs(const std::vector<std::wstring>& args, const std::filesystem::path& stateFile,
                                       unsigned long pid);
// Quotes one argument for a Windows command line (CommandLineToArgvW rules).
std::wstring QuoteArg(const std::wstring& arg);

// At startup with --wait-pid: waits (up to `timeoutMs`) for the old editor to exit, then deletes
// the <exe>.old it left. Without one, just deletes a stale <exe>.old. False when the old editor was
// still running at the timeout; it runs before Log::OpenFile, so the caller logs that once the log is open.
constexpr unsigned long kWaitOldEditorMs = 60000;
bool CleanupAtStartup(unsigned long waitPid, unsigned long timeoutMs = kWaitOldEditorMs);

// Where this editor writes its restart state: the per-instance user folder, keyed by this process's pid.
std::filesystem::path StateFile();

// Waits for a running build (editor shutdown); renames the exe back if the build never finished.
void Shutdown();

} // namespace EditorRestart
