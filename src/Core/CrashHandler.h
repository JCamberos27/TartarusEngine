#pragma once

// Last-resort crash reporting. Installs a process-wide unhandled-exception filter that writes a
// minidump next to the executable (crash_<pid>_<timestamp>.dmp) and a one-line marker to stderr
// before the process dies, so a segfault in an interactive session leaves something openable in
// a debugger instead of vanishing with a bare exit 139. No-op on non-Windows; installing twice
// is harmless. Call once, as early in main() as possible.
namespace CrashHandler {
void Install();
}
