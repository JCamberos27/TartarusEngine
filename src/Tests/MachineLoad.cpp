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
        const char* const* hit = std::find_if(std::begin(kWatched), std::end(kWatched),
                                              [&](const char* name) { return SameNameNoCase(exe, name); });
        if (hit == std::end(kWatched)) continue;
        Snapshot::Proc p;
        p.Name = *hit;
        p.Pid = pe.th32ProcessID;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
            if (GetProcessTimes(h, &c, &e, &pk, &pu)) { p.Cpu = U64(pk) + U64(pu); p.CpuKnown = true; }
            CloseHandle(h);
        }
        s.Procs.push_back(std::move(p));
    }
    CloseHandle(snap);
    s.Ok = true;
#endif
    return s;
}

Result Between(const Snapshot& start, const Snapshot& end, int cores) {
    Result r;
    if (!start.Ok || !end.Ok || end.SysTotal <= start.SysTotal) return r;
    r.Ok = true;
    const double total = (double)(end.SysTotal - start.SysTotal);
    const double busy = end.SysBusy >= start.SysBusy ? (double)(end.SysBusy - start.SysBusy) : 0.0;
    const double self = end.SelfCpu >= start.SelfCpu ? (double)(end.SelfCpu - start.SelfCpu) : 0.0;
    r.OtherCpuPct = std::clamp(100.0 * (busy - self) / total, 0.0, 100.0);
    // Busy: took more than kBusyCorePct of one core, started or ended during the phase (a compile), or its time
    // couldn't be read (not provably idle).
    const double busyLine = kBusyCorePct / 100.0 * total / std::max(cores, 1);
    auto find = [](const Snapshot& s, const Snapshot::Proc& p) -> const Snapshot::Proc* {
        for (const Snapshot::Proc& q : s.Procs)
            if (q.Pid == p.Pid && q.Name == p.Name) return &q;
        return nullptr;
    };
    auto list = [](std::string& out, const char* name, int n) {
        if (n == 0) return;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%s x%d", out.empty() ? "" : ", ", name, n);
        out += buf;
    };
    for (const char* name : kWatched) {
        int nBusy = 0, nIdle = 0;
        for (const Snapshot::Proc& p : end.Procs) {
            if (p.Name != name) continue;
            const Snapshot::Proc* was = find(start, p);
            const bool idle = was && was->CpuKnown && p.CpuKnown && (double)(p.Cpu - std::min(p.Cpu, was->Cpu)) <= busyLine;
            ++(idle ? nIdle : nBusy);
        }
        for (const Snapshot::Proc& p : start.Procs)
            if (p.Name == name && !find(end, p)) ++nBusy; // ended during the phase
        list(r.Busy, name, nBusy);
        list(r.Idle, name, nIdle);
    }
    r.Contended = !r.Busy.empty() || r.OtherCpuPct >= kContendedCpuPct;
    return r;
}

std::string Describe(const Result& r, int cores) {
    if (!r.Ok) return "";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "other CPU %.0f%% (%d cores); busy: ", r.OtherCpuPct, cores);
    return buf + (r.Busy.empty() ? std::string("-") : r.Busy) + "; idle: " + (r.Idle.empty() ? std::string("-") : r.Idle) +
           (r.Contended ? " -> CONTENDED" : " -> quiet");
}

} // namespace MachineLoad
