#include "DbgSymbols.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>

bool EnsureSymbols() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        std::lock_guard<std::mutex> lock(DbgHelpLock());
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        ok = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    });
    return ok;
}

std::mutex& DbgHelpLock() {
    static std::mutex m;
    return m;
}
#endif
