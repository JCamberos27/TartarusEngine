#pragma once

#include <atomic>
#include <cstdint>
#include <exception>

// The engine's one worker pool (docs/JOB_SYSTEM.md). Fixed worker threads, one queue; a thread that waits on a
// group runs queued jobs until the group is done, so the main thread (which owns the GL context) never parks
// while there is work. Jobs never call GL, change the registry's structure, call PhysX or run scripts: the
// ownership rules are in the design doc.
//
// Init(0) (--jobs 0) runs every ParallelFor / Run inline on the calling thread, in order: the single-threaded
// reference any regression can be bisected against. SetShuffle(true) (--jobs-shuffle) is inline too, but runs a
// ParallelFor's chunks in a random order, which flushes out code that depends on chunk order without real races.
namespace Jobs {

// Called as fn(data, begin, end) for one chunk [begin, end) of a ParallelFor, or (data, 0, 1) for a single job.
using JobFn = void (*)(void* data, std::uint32_t begin, std::uint32_t end);

// A group of jobs to wait on. Not copyable; must outlive Wait() on it. Reusable once Wait() has returned.
struct Counter {
    std::atomic<std::uint32_t> Pending{0};
    std::atomic<bool> Failed{false};
    std::exception_ptr Error; // the first exception one of the group's jobs threw; Wait() rethrows it

    Counter() = default;
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;
    bool Done() const { return Pending.load(std::memory_order_acquire) == 0; }
};

// Starts `workers` worker threads (Shutdown first if already running). -1 = DefaultWorkerCount(); 0 = inline.
void Init(int workers = -1);
// Runs whatever is still queued, then stops and joins the workers. Safe to call more than once.
void Shutdown();

// One worker per physical core but the main thread's (hyper-threads add little to memory-bound work), at least 1.
unsigned DefaultWorkerCount();
// Worker threads running (0 when inline or not initialised).
unsigned WorkerCount();

void SetShuffle(bool on);
bool Shuffle();

// 0 on the main thread (and any thread that isn't a worker), 1..WorkerCount() on a worker.
unsigned ThisWorker();
inline bool OnWorker() { return ThisWorker() != 0; }

// Queues fn(data, begin, end) and counts it in `c` (may be null: fire and forget). Never runs inline, even with no
// workers: then it runs when a thread calls Wait() or RunOne(). The PhysX dispatcher uses this.
void Submit(Counter* c, JobFn fn, void* data, std::uint32_t begin, std::uint32_t end);
// Queues [0, count) as chunks of `grain`, all counted in `c`, under one lock.
void SubmitRange(Counter& c, JobFn fn, void* data, std::uint32_t count, std::uint32_t grain);

// Runs queued jobs until `c` is done, then rethrows the first exception one of them threw, if any.
void Wait(Counter& c);
// Runs one queued job on this thread; false if the queue was empty.
bool RunOne();

namespace detail {
// Inline mode (or a range that is a single chunk): run here, now. Shuffle mode randomises the chunk order.
bool RunsInline(std::uint32_t count, std::uint32_t grain);
void RunChunksInline(JobFn fn, void* data, std::uint32_t count, std::uint32_t grain);
void ReportWorkerMisuse(const char* what);

template <class F>
void RangeTrampoline(void* data, std::uint32_t begin, std::uint32_t end) { (*static_cast<const F*>(data))(begin, end); }
template <class F>
void TaskTrampoline(void* data, std::uint32_t, std::uint32_t) { (*static_cast<const F*>(data))(); }
} // namespace detail

// f(begin, end) over [0, count) in chunks of `grain`, counted in `c`. `f` is referenced, not copied: it must
// outlive Wait(c). Inline, the chunks run before this returns (and an exception propagates straight out).
template <class F>
void ParallelFor(Counter& c, std::uint32_t count, std::uint32_t grain, const F& f) {
    if (count == 0) return;
    if (grain == 0) grain = 1;
    if (detail::RunsInline(count, grain)) {
        detail::RunChunksInline(&detail::RangeTrampoline<F>, const_cast<F*>(&f), count, grain);
        return;
    }
    SubmitRange(c, &detail::RangeTrampoline<F>, const_cast<F*>(&f), count, grain);
}

// A temporary would be destroyed before the queued chunks call it: name the callable, or use the blocking form.
template <class F>
void ParallelFor(Counter& c, std::uint32_t count, std::uint32_t grain, const F&& f) = delete;

// The blocking form: returns once every chunk has run.
template <class F>
void ParallelFor(std::uint32_t count, std::uint32_t grain, const F& f) {
    Counter c;
    ParallelFor(c, count, grain, f);
    Wait(c);
}

// One job f(), counted in `c`; `f` must outlive Wait(c). Inline, it runs before this returns.
template <class F>
void Run(Counter& c, const F& f) {
    if (WorkerCount() == 0 || Shuffle()) {
        f();
        return;
    }
    Submit(&c, &detail::TaskTrampoline<F>, const_cast<F*>(&f), 0, 1);
}

template <class F>
void Run(Counter& c, const F&& f) = delete; // as ParallelFor: a temporary would not outlive the job

} // namespace Jobs

// Debug builds: logs an error (once per call site name) when code that must stay off the job workers - GL calls,
// shader compiles, bone uploads - runs on one. A logged error fails the smoke test, so CI catches it.
#ifdef NDEBUG
#define JOBS_ASSERT_NOT_WORKER(what) ((void)0)
#else
#define JOBS_ASSERT_NOT_WORKER(what) (Jobs::OnWorker() ? Jobs::detail::ReportWorkerMisuse(what) : (void)0)
#endif
