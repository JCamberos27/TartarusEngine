#include "SamplingProfiler.h"

#if defined(_WIN32) && defined(_M_X64)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SamplingProfiler {
namespace {

constexpr int kMaxDepth = 48;
constexpr size_t kMaxSamples = 200000; // ~3 min at 1 kHz

struct Sample {
    std::uint64_t Pc[kMaxDepth];
    int Depth;
    std::uint32_t Frame; // EndFrame's count when it was taken
};
std::atomic<std::uint32_t> g_Frame{0};
std::vector<float> g_FrameMs; // by frame (EndFrame), from Start

std::vector<Sample> g_Samples; // preallocated: the sampler never allocates while the target is suspended
std::atomic<size_t> g_Count{0};
std::atomic<bool> g_Run{false};
std::thread g_Thread;
HANDLE g_Target = nullptr;

// Unwinds `ctx` (the suspended thread's context) in place, recording return addresses.
int UnwindFrames(CONTEXT& ctx, std::uint64_t* out) {
    int n = 0;
    while (n < kMaxDepth && ctx.Rip) {
        out[n++] = ctx.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) { // a leaf function: the return address is on top of the stack
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
        } else {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        }
    }
    return n;
}

// A frame without unwind data (generated code, a corrupt stack) can send the walk through a bad
// pointer: the sample then ends where it got to.
int Unwind(CONTEXT ctx, std::uint64_t* out) {
    __try {
        return UnwindFrames(ctx, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1; // the leaf pc, written first, is still good
    }
}

void SamplerLoop() {
    timeBeginPeriod(1);
    while (g_Run.load(std::memory_order_relaxed)) {
        const size_t i = g_Count.load(std::memory_order_relaxed);
        if (i < g_Samples.size() && SuspendThread(g_Target) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(g_Target, &ctx)) {
                g_Samples[i].Depth = Unwind(ctx, g_Samples[i].Pc);
                g_Samples[i].Frame = g_Frame.load(std::memory_order_relaxed);
                g_Count.store(i + 1, std::memory_order_relaxed);
            }
            ResumeThread(g_Target);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
}

} // namespace

void Start() {
    Stop();
    if (g_Samples.empty()) g_Samples.resize(kMaxSamples);
    g_Count = 0;
    g_Frame = 0;
    g_FrameMs.clear();
    if (!g_Target)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_Target,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_Run = true;
    g_Thread = std::thread(SamplerLoop);
}

void Stop() {
    if (!g_Run) return;
    g_Run = false;
    if (g_Thread.joinable()) g_Thread.join();
}

void EndFrame(double frameMs) {
    if (!g_Run) return;
    g_FrameMs.push_back((float)frameMs);
    g_Frame.store((std::uint32_t)g_FrameMs.size(), std::memory_order_relaxed);
}

void Report(const char* label, int top) {
    Stop();
    size_t count = g_Count.load();
    // TARTARUS_SAMPLE_HITCH_MS: only the samples from frames slower than that (moved to the front).
    char hitch[32] = {};
    if (GetEnvironmentVariableA("TARTARUS_SAMPLE_HITCH_MS", hitch, sizeof(hitch)) > 0) {
        const float over = (float)std::atof(hitch);
        size_t kept = 0;
        int frames = 0;
        for (float ms : g_FrameMs) frames += ms > over;
        for (size_t i = 0; i < count; ++i) {
            const std::uint32_t f = g_Samples[i].Frame;
            if (f < g_FrameMs.size() && g_FrameMs[f] > over) {
                if (kept != i) g_Samples[kept] = g_Samples[i];
                ++kept;
            }
        }
        std::printf("[PerfSample] %s: %zu of %zu samples, from the %d frame(s) over %.1f ms\n", label, kept, count, frames, over);
        count = kept;
    }
    HANDLE proc = GetCurrentProcess();
    static bool symInit = false;
    if (!symInit) {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize(proc, nullptr, TRUE); // fails harmlessly when the Console's stack traces already did it
        SymRefreshModuleList(proc);
        symInit = true;
    }
    std::unordered_map<std::uint64_t, std::string> nameOf; // pc -> function
    auto name = [&](std::uint64_t pc) -> const std::string& {
        auto it = nameOf.find(pc);
        if (it != nameOf.end()) return it->second;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        std::string n;
        if (SymFromAddr(proc, pc, &disp, sym)) {
            n = sym->Name;
            IMAGEHLP_MODULE64 mod{};
            mod.SizeOfStruct = sizeof(mod);
            if (SymGetModuleInfo64(proc, pc, &mod) && std::string(mod.ModuleName) != "TartarusEngine")
                n = std::string(mod.ModuleName) + "!" + n;
        } else {
            IMAGEHLP_MODULE64 mod{};
            mod.SizeOfStruct = sizeof(mod);
            n = SymGetModuleInfo64(proc, pc, &mod) ? std::string(mod.ModuleName) + "!?" : "?";
        }
        return nameOf.emplace(pc, std::move(n)).first->second;
    };

    // Samples outside the engine (driver, OS, PhysX) are also charged to the engine function that
    // called out: "external" rows read "<external function entered> <- <engine function>".
    auto external = [](const std::string& n) { return n.find('!') != std::string::npos; };
    std::unordered_map<std::string, int> self, incl, calls;
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < count; ++i) {
        const Sample& s = g_Samples[i];
        if (s.Depth <= 0) continue;
        const std::string& leaf = name(s.Pc[0]);
        ++self[leaf];
        seen.clear();
        for (int d = 0; d < s.Depth; ++d)
            if (seen.insert(name(s.Pc[d])).second) ++incl[name(s.Pc[d])];
        if (external(leaf))
            for (int d = 1; d < s.Depth; ++d)
                if (const std::string& n = name(s.Pc[d]); !external(n)) {
                    // The external frame the engine called (e.g. opengl32!glGetIntegerv), then the caller.
                    ++calls[name(s.Pc[d - 1]) + " <- " + n];
                    break;
                }
    }
    auto print = [&](const char* kind, const std::unordered_map<std::string, int>& m) {
        std::vector<std::pair<std::string, int>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (int i = 0; i < top && i < (int)v.size(); ++i)
            std::printf("[PerfSample] %s %-9s %5.1f%%  %s\n", label, kind, 100.0 * v[i].second / std::max<size_t>(count, 1),
                        v[i].first.c_str());
    };
    std::printf("[PerfSample] %s samples=%zu\n", label, count);
    print("self", self);
    print("inclusive", incl);
    print("external", calls);

    // TARTARUS_SAMPLE_FOCUS=<function substring>: the hottest call chains (6 frames up) through it.
    char focus[256] = {};
    if (GetEnvironmentVariableA("TARTARUS_SAMPLE_FOCUS", focus, sizeof(focus)) > 0) {
        std::unordered_map<std::string, int> chains;
        for (size_t i = 0; i < count; ++i) {
            const Sample& s = g_Samples[i];
            for (int d = 0; d < s.Depth; ++d) {
                if (name(s.Pc[d]).find(focus) == std::string::npos) continue;
                std::string chain = name(s.Pc[d]);
                for (int u = d + 1; u < s.Depth && u <= d + 6; ++u) chain += " < " + name(s.Pc[u]);
                ++chains[chain];
                break;
            }
        }
        print("focus", chains);
    }
    // TARTARUS_SAMPLE_UNDER=<function substring>: what that function's time goes on - every function below it (inclusive),
    // as a share of all samples.
    // Several: separated by ';'.
    char underList[512] = {};
    GetEnvironmentVariableA("TARTARUS_SAMPLE_UNDER", underList, sizeof(underList));
    std::stringstream underItems(underList);
    for (std::string underItem; std::getline(underItems, underItem, ';');) {
        if (underItem.empty()) continue;
        const char* under = underItem.c_str();
        std::unordered_map<std::string, int> below;
        int total = 0;
        for (size_t i = 0; i < count; ++i) {
            const Sample& s = g_Samples[i];
            int at = -1;
            for (int d = s.Depth - 1; d >= 0 && at < 0; --d)
                if (name(s.Pc[d]).find(under) != std::string::npos) at = d;
            if (at < 0) continue;
            ++total;
            seen.clear();
            for (int d = at; d >= 0; --d)
                if (seen.insert(name(s.Pc[d])).second) ++below[name(s.Pc[d])];
        }
        std::printf("[PerfSample] %s under '%s': %d samples\n", label, under, total);
        print("under", below);
    }
    std::fflush(stdout);
}

} // namespace SamplingProfiler

#else

namespace SamplingProfiler {
void Start() {}
void Stop() {}
void Report(const char*, int) {}
void EndFrame(double) {}
} // namespace SamplingProfiler

#endif
