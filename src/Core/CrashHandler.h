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

// Crashes the process on purpose through one path, for checking the handler: "av", "overflow",
// "abort", "purecall", "invalidparam" or "terminate". Returns only for an unknown kind.
void CrashForTest(const char* kind);
}
