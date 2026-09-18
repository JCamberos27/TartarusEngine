#pragma once

// Last-resort crash reporting. Catches unhandled SEH exceptions (access violations, stack
// overflows) and the CRT failures that bypass them (invalid parameter, pure virtual call,
// std::terminate, abort) (#148). A watchdog thread created at Install() writes a minidump to
// %LOCALAPPDATA%\TartarusEngine\Crashes\crash_<pid>_<timestamp>.dmp, prints a line to stderr and,
// in an interactive session, shows a dialog offering to open that folder. No-op on non-Windows;
// installing twice is harmless. Call once, as early in main() as possible.
namespace CrashHandler {
void Install();

// False for non-interactive runs (--smoke-test, --resave, ...): report to stderr only, never a
// dialog nobody is there to dismiss. Defaults to true.
void SetInteractive(bool interactive);

// #148 — called once after the crash dump is written, to save what the user would otherwise
// lose (the editor writes the unsaved scene's recovery snapshot, offered on the next launch).
// Runs on a helper thread while the crashed thread waits, inside an SEH guard and with a time
// limit: after a heap corruption or with a lock held by the crashed thread it may fault or hang,
// and then the crash report simply goes ahead without it. Return true when something was saved.
// nullptr clears it (do that before whatever the callback touches is destroyed).
using EmergencySaveFn = bool (*)();
void SetEmergencySave(EmergencySaveFn fn);

// Crashes the process on purpose through one path, for checking the handler: "av", "overflow",
// "abort", "purecall", "invalidparam" or "terminate". Returns only for an unknown kind.
void CrashForTest(const char* kind);
}
