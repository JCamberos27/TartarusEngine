#pragma once

#ifdef _WIN32
#include <mutex>

// One shared dbghelp session for the process. dbghelp is single-threaded and SymInitialize may
// only be paired with one SymCleanup, so the Console stack resolver and the sampling profiler
// both go through here instead of initialising it themselves. Hold DbgHelpLock() around every
// Sym* call. Nothing calls SymCleanup; the OS tears the session down at process exit.
bool EnsureSymbols();          // initialises once (std::call_once); false if SymInitialize failed
std::mutex& DbgHelpLock();
#endif
