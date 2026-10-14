#include "Profiler.h"
#include "JobSystem.h"
#include "gl.h"
#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

namespace {
std::vector<Profiler::Entry> g_CurrentFrame;
std::vector<Profiler::Entry> g_LastFrame;

// Static initialisation runs on the thread that runs main(), as Log.cpp relies on too.
const std::thread::id g_MainThread = std::this_thread::get_id();

// Samples from any thread but the main one: a buffer per thread (its own lock is uncontended but for BeginFrame),
// registered once. Buffers outlive their threads, so a thread exiting mid-frame keeps its samples for the merge.
struct ThreadSamples {
    std::mutex Mutex;
    std::vector<Profiler::Entry> Entries;
};
std::mutex g_ThreadSamplesMutex;
std::vector<std::shared_ptr<ThreadSamples>> g_ThreadSamples;

ThreadSamples& ThisThreadSamples() {
    thread_local std::shared_ptr<ThreadSamples> mine = [] {
        auto s = std::make_shared<ThreadSamples>();
        std::lock_guard<std::mutex> lock(g_ThreadSamplesMutex);
        g_ThreadSamples.push_back(s);
        return s;
    }();
    return *mine;
}

void PushFromAnyThread(Profiler::Entry e) {
    if (std::this_thread::get_id() == g_MainThread) {
        g_CurrentFrame.push_back(std::move(e));
        return;
    }
    e.Thread = Jobs::OnWorker() ? (int)Jobs::ThisWorker() : -1;
    ThreadSamples& s = ThisThreadSamples();
    std::lock_guard<std::mutex> lock(s.Mutex);
    s.Entries.push_back(std::move(e));
}

// #147 - GPU scopes are timed with GL_TIMESTAMP pairs, not GL_TIME_ELAPSED: elapsed-time queries
// can't nest (a nested scope made the outer one's glBeginQuery an error), timestamps can. Each
// frame in a small ring owns a pool of query objects; a scope takes two (begin/end) from the
// current frame's pool. The old design kept one ring per scope NAME and read back one result per
// ring per frame, so a scope that runs twice a frame (Scene view + Game view) dropped half its
// samples and reported whichever view happened to be read.
constexpr int kGpuFrames = 4; // 1-3 frames of driver latency, plus the frame being recorded
struct GpuScopeRecord {
    const char* Name;
    int Begin, End; // indices into GpuFrame::Pool
};
struct GpuFrame {
    std::vector<GLuint> Pool;
    int Used = 0;
    std::vector<GpuScopeRecord> Scopes;
    bool Pending = false; // recorded, results not read back yet
};
GpuFrame g_GpuFrames[kGpuFrames];
int g_GpuCurrent = 0;
std::vector<Profiler::Entry> g_GpuLastFrame;

int TakeQuery(GpuFrame& f) {
    if (f.Used == (int)f.Pool.size()) {
        const size_t old = f.Pool.size();
        f.Pool.resize(old + 16);
        glGenQueries(16, f.Pool.data() + old);
    }
    return f.Used++;
}

// Reads every scope of `f` if the GPU has finished it (queries complete in submission order,
// so the last one being available means all are). Repeated scopes are summed per name, in
// first-seen order: "Scene Draw" is the frame's total across both views.
bool TryReadBack(GpuFrame& f) {
    if (!f.Pending || f.Scopes.empty()) return false;
    int last = -1;
    for (const GpuScopeRecord& r : f.Scopes) last = std::max(last, r.End);
    if (last < 0) { f.Pending = false; return false; }
    GLint available = 0;
    glGetQueryObjectiv(f.Pool[last], GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available) return false;
    std::vector<Profiler::Entry> out;
    for (const GpuScopeRecord& r : f.Scopes) {
        if (r.End < 0) continue; // never closed (shouldn't happen)
        GLuint64 t0 = 0, t1 = 0;
        glGetQueryObjectui64v(f.Pool[r.Begin], GL_QUERY_RESULT, &t0);
        glGetQueryObjectui64v(f.Pool[r.End], GL_QUERY_RESULT, &t1);
        const float ms = t1 > t0 ? (float)((double)(t1 - t0) / 1000000.0) : 0.0f;
        auto it = std::find_if(out.begin(), out.end(), [&](const Profiler::Entry& e) { return e.Name == r.Name; });
        if (it != out.end()) it->Milliseconds += ms;
        else out.push_back({r.Name, ms});
    }
    g_GpuLastFrame = std::move(out);
    f.Pending = false;
    return true;
}
}

void Profiler::BeginFrame() {
    // Other threads' samples belong to the frame that just ended.
    {
        std::lock_guard<std::mutex> lock(g_ThreadSamplesMutex);
        for (const std::shared_ptr<ThreadSamples>& t : g_ThreadSamples) {
            std::lock_guard<std::mutex> tl(t->Mutex);
            for (Profiler::Entry& e : t->Entries) g_CurrentFrame.push_back(std::move(e));
            t->Entries.clear();
        }
    }
    // Swapped, not moved: the moved-from list lost its capacity and regrew from empty every frame.
    std::swap(g_LastFrame, g_CurrentFrame);
    g_CurrentFrame.clear();

    if (!glQueryCounter || !glGetQueryObjectiv || !glGetQueryObjectui64v) return;
    // Close the frame just recorded, then read back every finished frame oldest-first (so the
    // newest finished one is what GetLastFrameGpu shows). Never waits on the GPU.
    g_GpuFrames[g_GpuCurrent].Pending = !g_GpuFrames[g_GpuCurrent].Scopes.empty();
    for (int i = 1; i <= kGpuFrames; ++i) TryReadBack(g_GpuFrames[(g_GpuCurrent + i) % kGpuFrames]);

    // Recycle the next slot. If its results still aren't in, the GPU is more than kGpuFrames-1
    // frames behind; drop them rather than stall.
    g_GpuCurrent = (g_GpuCurrent + 1) % kGpuFrames;
    GpuFrame& next = g_GpuFrames[g_GpuCurrent];
    next.Used = 0;
    next.Scopes.clear();
    next.Pending = false;
}

void Profiler::PushSample(const std::string& name, float milliseconds) {
    PushFromAnyThread({name, milliseconds});
}

void Profiler::PushSample(const char* name, float milliseconds) {
    PushFromAnyThread({std::string(name), milliseconds});
}

const std::vector<Profiler::Entry>& Profiler::GetLastFrame() {
    return g_LastFrame;
}

Profiler::ScopeTimer::ScopeTimer(const char* name)
    : m_Name(name), m_Start(std::chrono::steady_clock::now()) {}

Profiler::ScopeTimer::~ScopeTimer() {
    auto elapsed = std::chrono::steady_clock::now() - m_Start;
    float ms = std::chrono::duration<float, std::milli>(elapsed).count();
    Profiler::PushSample(m_Name, ms);
}

const std::vector<Profiler::Entry>& Profiler::GetLastFrameGpu() {
    return g_GpuLastFrame;
}

Profiler::GpuScopeTimer::GpuScopeTimer(const char* name) {
    JOBS_ASSERT_NOT_WORKER("PROFILE_GPU_SCOPE");
    if (!glGenQueries || !glQueryCounter) return; // loader didn't resolve these entry points
    GpuFrame& f = g_GpuFrames[g_GpuCurrent];
    const int begin = TakeQuery(f);
    glQueryCounter(f.Pool[begin], GL_TIMESTAMP);
    m_Record = (int)f.Scopes.size();
    f.Scopes.push_back({name, begin, -1});
}

Profiler::GpuScopeTimer::~GpuScopeTimer() {
    if (m_Record < 0) return;
    GpuFrame& f = g_GpuFrames[g_GpuCurrent];
    if (m_Record >= (int)f.Scopes.size()) return; // frame rolled over mid-scope (BeginFrame inside it)
    const int end = TakeQuery(f);
    glQueryCounter(f.Pool[end], GL_TIMESTAMP);
    f.Scopes[m_Record].End = end;
}
