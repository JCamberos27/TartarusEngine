#include "CrashHandler.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <ctime>

namespace {

// Written entirely with async-signal-style care: no heap allocation, no CRT locks that a
// corrupted-heap crash could deadlock on. Path building uses stack buffers + wsprintfA.
LONG WINAPI WriteMiniDump(EXCEPTION_POINTERS* ep) {
    static LONG entered = 0;
    if (InterlockedCompareExchange(&entered, 1, 0) != 0)
        return EXCEPTION_EXECUTE_HANDLER; // a fault inside the handler itself — don't recurse

    char exeDir[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    // trim to the directory
    while (n > 0 && exeDir[n - 1] != '\\' && exeDir[n - 1] != '/') --n;
    exeDir[n] = '\0';

    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH] = {0};
    wsprintfA(path, "%scrash_%lu_%04u%02u%02u_%02u%02u%02u.dmp",
              exeDir, GetCurrentProcessId(),
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
            MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
        ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                               ep ? &mei : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(file);
    }

    // stderr only — Log:: may be mid-write / holding a lock at the point of the fault.
    if (ok)
        std::fprintf(stderr, "\n[CrashHandler] unhandled exception 0x%08lx at %p — minidump: %s\n",
                     ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
                     ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr,
                     path);
    else
        std::fprintf(stderr, "\n[CrashHandler] unhandled exception — minidump write FAILED (%s)\n", path);
    std::fflush(stderr);

    return EXCEPTION_EXECUTE_HANDLER; // let the process terminate
}

} // namespace

namespace CrashHandler {
void Install() {
    SetUnhandledExceptionFilter(&WriteMiniDump);
    // A few CRT fast-fails (buffer overrun, invalid parameter, pure-virtual call) bypass the
    // unhandled-exception filter; route them through the same path.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}
}

#else
namespace CrashHandler { void Install() {} }
#endif
