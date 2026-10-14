#include "UnitTestSupport.h"
#include "../Core/JobSystem.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

// The job system (Core/JobSystem.h, docs/JOB_SYSTEM.md). Each test starts its own pool and leaves the system shut
// down (inline), the state the rest of --unit-tests runs in.

namespace {
struct PoolScope {
    explicit PoolScope(int workers) { Jobs::Init(workers); }
    ~PoolScope() { Jobs::Shutdown(); Jobs::SetShuffle(false); }
};

// Every index in [0, count) visited exactly once.
bool CoversOnce(std::uint32_t count, std::uint32_t grain) {
    std::vector<std::atomic<int>> hits(count);
    for (auto& h : hits) h.store(0);
    Jobs::ParallelFor(count, grain, [&](std::uint32_t b, std::uint32_t e) {
        for (std::uint32_t i = b; i < e; ++i) hits[i].fetch_add(1, std::memory_order_relaxed);
    });
    for (auto& h : hits) if (h.load() != 1) return false;
    return true;
}
} // namespace

static void Test_Jobs_ParallelForCoversEveryIndexOnce() {
    PoolScope pool(4);
    CHECK(Jobs::WorkerCount() == 4);
    CHECK(CoversOnce(10007, 64));
    CHECK(CoversOnce(1, 64));    // a single chunk runs inline
    CHECK(CoversOnce(128, 1));   // more chunks than workers, one index each
    CHECK(CoversOnce(0, 16));
}

static void Test_Jobs_RunsOnWorkersAndMainStaysMain() {
    PoolScope pool(3);
    CHECK(!Jobs::OnWorker());
    std::atomic<int> onWorker{0};
    Jobs::Counter c;
    // Chunks slow enough that the workers wake and take some while this thread helps in Wait.
    const auto body = [&](std::uint32_t b, std::uint32_t e) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        for (std::uint32_t i = b; i < e; ++i)
            if (Jobs::ThisWorker() >= 1 && Jobs::ThisWorker() <= 3) onWorker.fetch_add(1);
    };
    for (int round = 0; round < 50 && onWorker.load() == 0; ++round) {
        Jobs::ParallelFor(c, 64, 1, body);
        Jobs::Wait(c);
    }
    CHECK(onWorker.load() > 0);
    CHECK(!Jobs::OnWorker());
}

static void Test_Jobs_NestedParallelForDoesNotDeadlock() {
    PoolScope pool(2);
    std::atomic<std::uint32_t> total{0};
    Jobs::ParallelFor(8, 1, [&](std::uint32_t, std::uint32_t) {
        // A job waiting on its own group helps run the queue instead of blocking a worker.
        Jobs::ParallelFor(100, 7, [&](std::uint32_t b, std::uint32_t e) { total.fetch_add(e - b); });
    });
    CHECK(total.load() == 800);
}

static void Test_Jobs_InlineRunsOnCallerInOrder() {
    PoolScope pool(0);
    CHECK(Jobs::WorkerCount() == 0);
    std::vector<std::uint32_t> order;
    Jobs::ParallelFor(10, 3, [&](std::uint32_t b, std::uint32_t e) {
        CHECK(!Jobs::OnWorker());
        for (std::uint32_t i = b; i < e; ++i) order.push_back(i);
    });
    bool inOrder = order.size() == 10;
    for (std::uint32_t i = 0; inOrder && i < 10; ++i) inOrder = order[i] == i;
    CHECK(inOrder);
    int ran = 0;
    Jobs::Counter c;
    const auto job = [&] { ++ran; };
    Jobs::Run(c, job);
    CHECK(ran == 1); // inline: done before Run returns
    Jobs::Wait(c);
}

static void Test_Jobs_ShuffleCoversEveryIndexOnce() {
    PoolScope pool(4);
    Jobs::SetShuffle(true);
    CHECK(CoversOnce(1000, 9));
    std::vector<std::uint32_t> starts;
    Jobs::ParallelFor(100, 10, [&](std::uint32_t b, std::uint32_t) { starts.push_back(b); });
    bool ascending = true;
    for (size_t i = 1; i < starts.size(); ++i) ascending = ascending && starts[i] > starts[i - 1];
    CHECK(starts.size() == 10);
    CHECK(!ascending); // a fixed seed: this order is the same every run
}

static void Test_Jobs_WaitRethrowsAndCounterIsReusable() {
    PoolScope pool(2);
    Jobs::Counter c;
    std::atomic<int> ran{0};
    const auto body = [&](std::uint32_t b, std::uint32_t) {
        ran.fetch_add(1);
        if (b == 5) throw std::runtime_error("job failed");
    };
    Jobs::ParallelFor(c, 16, 1, body);
    bool threw = false;
    try { Jobs::Wait(c); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    CHECK(ran.load() == 16); // the other jobs of the group still ran
    CHECK(c.Done() && !c.Failed.load());
    std::atomic<int> again{0};
    const auto count = [&](std::uint32_t, std::uint32_t) { again.fetch_add(1); };
    Jobs::ParallelFor(c, 16, 1, count);
    Jobs::Wait(c);
    CHECK(again.load() == 16);
}

static void Test_Jobs_SubmitWithoutWorkersRunsWhenPolled() {
    // The PhysX dispatcher's path: queued, never inline, and with no workers run by whoever polls.
    PoolScope pool(0);
    std::atomic<int> ran{0};
    const Jobs::JobFn fn = [](void* d, std::uint32_t, std::uint32_t) { static_cast<std::atomic<int>*>(d)->fetch_add(1); };
    for (int i = 0; i < 5; ++i) Jobs::Submit(nullptr, fn, &ran, 0, 1);
    CHECK(ran.load() == 0);
    while (Jobs::RunOne()) {}
    CHECK(ran.load() == 5);
    Jobs::Submit(nullptr, fn, &ran, 0, 1);
    Jobs::Shutdown(); // runs what is left
    CHECK(ran.load() == 6);
}

static void Test_Jobs_ReinitChangesWorkerCount() {
    for (int n : {1, 3, 0, 2}) {
        PoolScope pool(n);
        CHECK(Jobs::WorkerCount() == (unsigned)n);
        CHECK(CoversOnce(500, 16));
    }
    CHECK(Jobs::WorkerCount() == 0);
    CHECK(Jobs::DefaultWorkerCount() >= 1);
}

void RegisterJobTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Jobs::ParallelForCoversEveryIndexOnce", Test_Jobs_ParallelForCoversEveryIndexOnce});
    tests.push_back({"Jobs::RunsOnWorkersAndMainStaysMain", Test_Jobs_RunsOnWorkersAndMainStaysMain});
    tests.push_back({"Jobs::NestedParallelForDoesNotDeadlock", Test_Jobs_NestedParallelForDoesNotDeadlock});
    tests.push_back({"Jobs::InlineRunsOnCallerInOrder", Test_Jobs_InlineRunsOnCallerInOrder});
    tests.push_back({"Jobs::ShuffleCoversEveryIndexOnce", Test_Jobs_ShuffleCoversEveryIndexOnce});
    tests.push_back({"Jobs::WaitRethrowsAndCounterIsReusable", Test_Jobs_WaitRethrowsAndCounterIsReusable});
    tests.push_back({"Jobs::SubmitWithoutWorkersRunsWhenPolled", Test_Jobs_SubmitWithoutWorkersRunsWhenPolled});
    tests.push_back({"Jobs::ReinitChangesWorkerCount", Test_Jobs_ReinitChangesWorkerCount});
}
