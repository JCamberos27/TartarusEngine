#include "CrashHandler.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <string>
#include <cstring>
#pragma comment(lib, "shell32.lib")

namespace {

// #148 — the dump is written by a watchdog thread created at Install(), never by the thread
// that crashed. A stack overflow leaves the faulting thread with no stack to call
// MiniDumpWriteDump on, and a corrupted heap or a held CRT lock makes allocating or printf-ing
// there a deadlock risk. The faulting thread only records what happened, wakes the watchdog and
// waits. Everything the watchdog needs (the dump folder) is resolved at Install() time.
HANDLE g_Request = nullptr;   // auto-reset: a crash happened, write the dump
HANDLE g_Done = nullptr;      // manual-reset: the watchdog finished (dump + dialog)
EXCEPTION_POINTERS* g_Pointers = nullptr;
DWORD g_FaultThreadId = 0;
const char* g_Reason = "";    // static strings only
bool g_Interactive = true;
wchar_t g_Dir[MAX_PATH] = {0}; // %LOCALAPPDATA%\TartarusEngine\Crashes\ (trailing slash)
wchar_t g_DumpPath[MAX_PATH] = {0};

void ResolveDumpDir() {
    wchar_t base[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n > 0 && n < MAX_PATH - 40) {
        wsprintfW(g_Dir, L"%s\\TartarusEngine", base);
        CreateDirectoryW(g_Dir, nullptr);
        wsprintfW(g_Dir, L"%s\\TartarusEngine\\Crashes\\", base);
        CreateDirectoryW(g_Dir, nullptr);
        return;
    }
    // No LOCALAPPDATA (shouldn't happen): fall back to the executable's folder, as before.
    n = GetModuleFileNameW(nullptr, g_Dir, MAX_PATH);
    while (n > 0 && g_Dir[n - 1] != L'\\' && g_Dir[n - 1] != L'/') --n;
    g_Dir[n] = L'\0';
}

bool WriteDump() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wsprintfW(g_DumpPath, L"%scrash_%lu_%04u%02u%02u_%02u%02u%02u.dmp", g_Dir, GetCurrentProcessId(),
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE file = CreateFileW(g_DumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = g_FaultThreadId;
    mei.ExceptionPointers = g_Pointers;
    mei.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
        MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
    const bool ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                      g_Pointers ? &mei : nullptr, nullptr, nullptr) != FALSE;
    CloseHandle(file);
    return ok;
}

// Raw WriteFile to stderr: no CRT stream locks, which the crashed thread may be holding.
void StderrLine(const char* text) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err == nullptr || err == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(err, text, (DWORD)lstrlenA(text), &written, nullptr);
}

DWORD WINAPI Watchdog(void*) {
    WaitForSingleObject(g_Request, INFINITE);
    const bool ok = WriteDump();

    char line[MAX_PATH * 2 + 128];
    wsprintfA(line, "\n[CrashHandler] %s (0x%08lx) - minidump %s: %ls\n", g_Reason,
              g_Pointers && g_Pointers->ExceptionRecord ? g_Pointers->ExceptionRecord->ExceptionCode : 0,
              ok ? "written" : "FAILED", g_DumpPath);
    StderrLine(line);

    if (g_Interactive) {
        // #148 — tell the user instead of vanishing. The editor's own log (Editor.log) sits next
        // to the Crashes folder, so opening the folder shows both.
        wchar_t msg[MAX_PATH * 2 + 256];
        if (ok)
            wsprintfW(msg, L"Tartarus Engine crashed and has to close.\n\nA crash report was saved to:\n%s\n\n"
                           L"Open the folder? Attach the .dmp and Logs\\Editor.log to a bug report.", g_DumpPath);
        else
            wsprintfW(msg, L"Tartarus Engine crashed and has to close.\n\nThe crash report couldn't be written.");
        const int choice = MessageBoxW(nullptr, msg, L"Tartarus Engine",
                                       (ok ? MB_YESNO : MB_OK) | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        if (ok && choice == IDYES) {
            wchar_t args[MAX_PATH + 16];
            wsprintfW(args, L"/select,\"%s\"", g_DumpPath);
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args, nullptr, SW_SHOWNORMAL);
        }
    }
    SetEvent(g_Done);
    return 0;
}

// Every crash path funnels here. Runs on the faulting thread: records, wakes, waits, dies.
[[noreturn]] void Report(EXCEPTION_POINTERS* ep, const char* reason) {
    static LONG entered = 0;
    if (InterlockedCompareExchange(&entered, 1, 0) == 0 && g_Request) {
        g_Pointers = ep;
        g_FaultThreadId = GetCurrentThreadId();
        g_Reason = reason;
        SetEvent(g_Request);
        WaitForSingleObject(g_Done, g_Interactive ? INFINITE : 60000);
    } else {
        // A second thread crashing concurrently (or one inside the handler): let the first
        // report finish, then go.
        if (g_Done) WaitForSingleObject(g_Done, 60000);
    }
    TerminateProcess(GetCurrentProcess(), 0xC0DEDEAD);
    for (;;) {} // not reached
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    Report(ep, code == EXCEPTION_STACK_OVERFLOW ? "stack overflow" : "unhandled exception");
}

// The CRT failures below don't raise a structured exception, so SetUnhandledExceptionFilter
// never sees them (the old SetErrorMode comment claimed otherwise). Capture a context so the
// dump still shows the failing stack.
[[noreturn]] void ReportHere(const char* reason) {
    CONTEXT ctx = {};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec = {};
    rec.ExceptionCode = 0xE0000001; // application-defined: "CRT failure"
    rec.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
    EXCEPTION_POINTERS ep = {&rec, &ctx};
    Report(&ep, reason);
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    ReportHere("invalid parameter passed to a CRT function");
}
void OnPureCall() { ReportHere("pure virtual function call"); }
void OnTerminate() { ReportHere("std::terminate (uncaught C++ exception)"); }
void OnAbort(int) { ReportHere("abort()"); }

} // namespace

namespace CrashHandler {
void Install() {
    if (g_Request) return; // installing twice is harmless
    ResolveDumpDir();
    g_Request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_Done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 256 * 1024, &Watchdog, nullptr, 0, nullptr)) CloseHandle(t);

    SetUnhandledExceptionFilter(&OnUnhandledException);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_invalid_parameter_handler(&OnInvalidParameter);
    _set_purecall_handler(&OnPureCall);
    std::set_terminate(&OnTerminate);
    std::signal(SIGABRT, &OnAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // no CRT "abort() called" box
    // Keeps enough stack in reserve on this (main) thread for the handler to run after an overflow.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
}

void SetInteractive(bool interactive) { g_Interactive = interactive; }
} // namespace CrashHandler

namespace {
struct PureBase {
    PureBase() { CallPure(); } // calls the pure virtual while the derived part doesn't exist yet
    void CallPure() { Pure(); }
    virtual void Pure() = 0;
    virtual ~PureBase() = default;
};
struct PureDerived : PureBase { void Pure() override {} };

#pragma warning(push)
#pragma warning(disable: 4717) // recursive on all control paths: that's the point
__declspec(noinline) int Recurse(volatile int depth) {
    volatile char pad[4096];
    pad[0] = (char)depth;
    return Recurse(depth + 1) + pad[0];
}
#pragma warning(pop)
} // namespace

namespace CrashHandler {
void CrashForTest(const char* kind) {
    const std::string k = kind ? kind : "";
    if (k == "av") { volatile int* p = nullptr; *p = 1; }
    else if (k == "overflow") { Recurse(0); }
    else if (k == "abort") { std::abort(); }
    else if (k == "purecall") { PureDerived d; (void)d; }
    else if (k == "invalidparam") { char buf[4]; strcpy_s(buf, sizeof(buf), "too long for this"); }
    else if (k == "terminate") { std::terminate(); }
}
} // namespace CrashHandler

#else
namespace CrashHandler { void Install() {} void SetInteractive(bool) {} void CrashForTest(const char*) {} }
#endif
