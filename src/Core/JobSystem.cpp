#include "JobSystem.h"
#include "Log.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

struct Job {
    Jobs::JobFn Fn = nullptr;
    void* Data = nullptr;
    std::uint32_t Begin = 0, End = 0;
    Jobs::Counter* Group = nullptr;
};

// One queue under one mutex. With the tens of jobs a frame queues this is not the bottleneck; per-worker
// work-stealing deques can replace it behind the same API once a profile says so (docs/JOB_SYSTEM.md).
std::mutex g_Mutex;
std::condition_variable g_Wake;
std::deque<Job> g_Queue;
std::atomic<std::uint32_t> g_Queued{0}; // g_Queue.size(), readable without the lock
bool g_Stop = false;
std::vector<std::thread> g_Workers;
std::atomic<unsigned> g_WorkerCount{0};
std::atomic<bool> g_Shuffle{false};

thread_local unsigned t_Worker = 0;

bool PopLocked(Job& out) {
    if (g_Queue.empty()) return false;
    out = g_Queue.front();
    g_Queue.pop_front();
    g_Queued.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

bool TryPop(Job& out) {
    if (g_Queued.load(std::memory_order_relaxed) == 0) return false;
    std::lock_guard<std::mutex> lock(g_Mutex);
    return PopLocked(out);
}

void Execute(const Job& j) {
    try {
        j.Fn(j.Data, j.Begin, j.End);
    } catch (...) {
        // Ungrouped jobs (PhysX tasks) have nobody to hand the error to: crash here, where the dump shows it.
        if (!j.Group) std::terminate();
        if (!j.Group->Failed.exchange(true, std::memory_order_acq_rel)) j.Group->Error = std::current_exception();
    }
    // The last touch of the group: once Pending reaches zero its owner may return from Wait and destroy it.
    if (j.Group) j.Group->Pending.fetch_sub(1, std::memory_order_acq_rel);
}

void WorkerMain(unsigned index) {
    t_Worker = index;
#if defined(_WIN32)
    // Named for the debugger's Threads window and for crash dumps.
    const std::wstring name = L"Tartarus Job " + std::to_wstring(index);
    SetThreadDescription(GetCurrentThread(), name.c_str());
#endif
    for (;;) {
        Job j;
        // A short spin before sleeping: a frame's job groups arrive in bursts, and waking from the condition
        // variable costs more than a few yields.
        bool got = false;
        for (int spin = 0; spin < 64 && !got; ++spin) {
            got = TryPop(j);
            if (!got) std::this_thread::yield();
        }
        if (!got) {
            std::unique_lock<std::mutex> lock(g_Mutex);
            g_Wake.wait(lock, [] { return g_Stop || !g_Queue.empty(); });
            if (!PopLocked(j)) return; // stopping, and the queue is drained
        }
        Execute(j);
    }
}

unsigned PhysicalCoreCount() {
#if defined(_WIN32)
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len) {
        std::vector<char> buf(len);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
            unsigned n = 0;
            for (char* p = buf.data(); p < buf.data() + len;) {
                auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(p);
                if (info->Relationship == RelationProcessorCore) ++n;
                p += info->Size;
            }
            if (n) return n;
        }
    }
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}

} // namespace

namespace Jobs {

unsigned DefaultWorkerCount() {
    const unsigned cores = PhysicalCoreCount();
    return std::clamp(cores > 1 ? cores - 1 : 1u, 1u, 63u);
}

void Init(int workers) {
    Shutdown();
    const unsigned n = workers < 0 ? DefaultWorkerCount() : std::min((unsigned)workers, 63u);
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Stop = false;
    }
    g_Workers.reserve(n);
    for (unsigned i = 0; i < n; ++i) g_Workers.emplace_back(WorkerMain, i + 1);
    g_WorkerCount.store(n, std::memory_order_release);
    Log::Info("Jobs: " + (n ? std::to_string(n) + " worker thread(s)" : std::string("inline (no worker threads)")) +
              (g_Shuffle.load() ? ", shuffled" : ""));
}

void Shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Stop = true;
    }
    g_Wake.notify_all();
    for (std::thread& t : g_Workers) t.join();
    g_Workers.clear();
    g_WorkerCount.store(0, std::memory_order_release);
    // Workers drain the queue before they exit; with none, whatever is left runs here.
    while (RunOne()) {}
}

unsigned WorkerCount() { return g_WorkerCount.load(std::memory_order_acquire); }

void SetShuffle(bool on) { g_Shuffle.store(on); }
bool Shuffle() { return g_Shuffle.load(std::memory_order_relaxed); }

unsigned ThisWorker() { return t_Worker; }

void Submit(Counter* c, JobFn fn, void* data, std::uint32_t begin, std::uint32_t end) {
    if (c) c->Pending.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Queue.push_back({fn, data, begin, end, c});
        g_Queued.fetch_add(1, std::memory_order_relaxed);
    }
    g_Wake.notify_one();
}

void SubmitRange(Counter& c, JobFn fn, void* data, std::uint32_t count, std::uint32_t grain) {
    if (count == 0) return;
    if (grain == 0) grain = 1;
    const std::uint32_t chunks = (count + grain - 1) / grain;
    c.Pending.fetch_add(chunks, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (std::uint32_t b = 0; b < count; b += grain)
            g_Queue.push_back({fn, data, b, std::min(count, b + grain), &c});
        g_Queued.fetch_add(chunks, std::memory_order_relaxed);
    }
    if (chunks == 1) g_Wake.notify_one();
    else g_Wake.notify_all();
}

bool RunOne() {
    Job j;
    if (!TryPop(j)) return false;
    Execute(j);
    return true;
}

void Wait(Counter& c) {
    while (!c.Done()) {
        if (!RunOne()) std::this_thread::yield(); // the group's last chunks are running on other threads
    }
    if (c.Failed.load(std::memory_order_acquire)) {
        std::exception_ptr e = c.Error;
        c.Error = nullptr;
        c.Failed.store(false, std::memory_order_relaxed);
        std::rethrow_exception(e);
    }
}

namespace detail {

bool RunsInline(std::uint32_t count, std::uint32_t grain) {
    return WorkerCount() == 0 || Shuffle() || count <= grain;
}

void RunChunksInline(JobFn fn, void* data, std::uint32_t count, std::uint32_t grain) {
    if (!Shuffle()) {
        for (std::uint32_t b = 0; b < count; b += grain) fn(data, b, std::min(count, b + grain));
        return;
    }
    std::vector<std::uint32_t> starts((count + grain - 1) / grain);
    for (std::size_t i = 0; i < starts.size(); ++i) starts[i] = (std::uint32_t)i * grain;
    static thread_local std::mt19937 rng{0x7a57a5u}; // fixed seed: a failing order reproduces run to run
    std::shuffle(starts.begin(), starts.end(), rng);
    for (std::uint32_t b : starts) fn(data, b, std::min(count, b + grain));
}

void ReportWorkerMisuse(const char* what) {
    static std::mutex m;
    static std::set<std::string> reported;
    {
        std::lock_guard<std::mutex> lock(m);
        if (!reported.insert(what).second) return;
    }
    Log::Error(std::string("Jobs: ") + what + " ran on job worker " + std::to_string(ThisWorker()) +
               " - it must stay on the main thread (docs/JOB_SYSTEM.md, Ownership rules)");
}

} // namespace detail

} // namespace Jobs
