#include "Tests/MachineLoad.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef APIENTRY
#undef APIENTRY
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace MachineLoad {
namespace {

// Processes that take the CPU or GPU in bursts: other engines (editor tests, benches) and build / encode tools.
constexpr const char* kWatched[] = {"TartarusEngine", "cl", "link", "ninja", "MSBuild", "cmake", "ffmpeg", "blender"};

bool SameNameNoCase(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return i == a.size() && !b[i];
}

#ifdef _WIN32
uint64_t U64(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }
#endif

} // namespace

int Cores() { return std::max(1u, std::thread::hardware_concurrency()); }

Snapshot Take() {
    Snapshot s;
#ifdef _WIN32
    FILETIME idle, kernel, user, c, e, pk, pu;
    if (!GetSystemTimes(&idle, &kernel, &user)) return s;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu)) return s;
    s.SysTotal = U64(kernel) + U64(user); // kernel time includes idle
    s.SysBusy = s.SysTotal - U64(idle);
    s.SelfCpu = U64(pk) + U64(pu);
    for (const char* name : kWatched) s.Procs.emplace_back(name, 0);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return s;
    const DWORD self = GetCurrentProcessId();
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self) continue;
        std::string exe; // the watched names are ASCII: other characters only need to not match
        for (const wchar_t* w = pe.szExeFile; *w; ++w) exe += *w < 128 ? (char)*w : '?';
        if (exe.size() > 4 && SameNameNoCase(exe.substr(exe.size() - 4), ".exe")) exe.resize(exe.size() - 4);
        for (auto& [name, count] : s.Procs)
            if (SameNameNoCase(exe, name.c_str())) ++count;
    }
    CloseHandle(snap);
    s.Ok = true;
#endif
    return s;
}

Result Between(const Snapshot& start, const Snapshot& end) {
    Result r;
    if (!start.Ok || !end.Ok || end.SysTotal <= start.SysTotal) return r;
    r.Ok = true;
    const double total = (double)(end.SysTotal - start.SysTotal);
    const double busy = end.SysBusy >= start.SysBusy ? (double)(end.SysBusy - start.SysBusy) : 0.0;
    const double self = end.SelfCpu >= start.SelfCpu ? (double)(end.SelfCpu - start.SelfCpu) : 0.0;
    r.OtherCpuPct = std::clamp(100.0 * (busy - self) / total, 0.0, 100.0);
    bool anyProc = false;
    for (const auto& [name, n0] : start.Procs) {
        int n = n0;
        for (const auto& [name1, n1] : end.Procs)
            if (name1 == name) n = std::max(n, n1);
        if (n == 0) continue;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%s x%d", r.Procs.empty() ? "" : ", ", name.c_str(), n);
        r.Procs += buf;
        anyProc = true;
    }
    r.Contended = anyProc || r.OtherCpuPct >= kContendedCpuPct;
    return r;
}

std::string Describe(const Result& r, int cores) {
    if (!r.Ok) return "";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "other CPU %.0f%% (%d cores); running: ", r.OtherCpuPct, cores);
    return buf + (r.Procs.empty() ? std::string("-") : r.Procs) + (r.Contended ? " -> CONTENDED" : " -> quiet");
}

} // namespace MachineLoad
