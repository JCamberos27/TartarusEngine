#include "Profiler.h"
#include "gl.h"
#include <algorithm>

namespace {
std::vector<Profiler::Entry> g_CurrentFrame;
std::vector<Profiler::Entry> g_LastFrame;
// The entries used this frame. g_CurrentFrame keeps the slots (and their names' buffers) of the frame before last, so a
// frame's samples overwrite them in place instead of building a string each: a long scope name allocated every frame.
size_t g_CurrentCount = 0;
// Open PROFILE_SCOPEs on this thread: an entry pushed while one is open is Nested (its time is inside the outer one's).
thread_local int g_ScopeDepth = 0;
// BeginFrame's clock: the last frame's wall time, so a hitch report can tell how much of it no scope covered.
std::chrono::steady_clock::time_point g_FrameStart;
bool g_HaveFrameStart = false;
float g_LastFrameWallMs = 0.0f;

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
    unsigned Serial = 0;  // the frame (BeginFrame count) it was recorded in
    // The whole frame on the GPU (G-0): a timestamp at BeginFrame and one at EndGpuFrame (just before the swap). -1 = not taken.
    int FrameBegin = -1, FrameEnd = -1;
};
GpuFrame g_GpuFrames[kGpuFrames];
int g_GpuCurrent = 0;
unsigned g_GpuFrameSerial = 0; // bumped by BeginFrame so a scope that straddles one is detected
// The last few frames read back, by frame serial: --perf-bench attaches a slow frame's GPU scopes once they land, and
// one BeginFrame can read back more than one frame. g_GpuNewest (-1 = none yet) is what GetLastFrameGpu shows.
constexpr int kGpuHistory = 8;
struct GpuDone {
    unsigned Serial = 0;
    std::vector<Profiler::Entry> Entries;
    float FrameMs = -1.0f; // BeginFrame to EndGpuFrame on the GPU; -1 = the frame had no EndGpuFrame
};
GpuDone g_GpuHistory[kGpuHistory];
int g_GpuNewest = -1;

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
    if (!f.Pending) return false;
    int last = f.FrameEnd;
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
    float frameMs = -1.0f;
    if (f.FrameBegin >= 0 && f.FrameEnd >= 0) {
        GLuint64 t0 = 0, t1 = 0;
        glGetQueryObjectui64v(f.Pool[f.FrameBegin], GL_QUERY_RESULT, &t0);
        glGetQueryObjectui64v(f.Pool[f.FrameEnd], GL_QUERY_RESULT, &t1);
        frameMs = t1 > t0 ? (float)((double)(t1 - t0) / 1000000.0) : 0.0f;
    }
    g_GpuNewest = (g_GpuNewest + 1) % kGpuHistory;
    g_GpuHistory[g_GpuNewest] = {f.Serial, std::move(out), frameMs};
    f.Pending = false;
    return true;
}
}

void Profiler::BeginFrame() {
    // Swapped, not moved: the moved-from list lost its capacity and regrew from empty every frame.
    g_CurrentFrame.resize(g_CurrentCount); // (exactly this frame's entries go out)
    std::swap(g_LastFrame, g_CurrentFrame);
    g_CurrentCount = 0;
    const auto now = std::chrono::steady_clock::now();
    g_LastFrameWallMs = g_HaveFrameStart ? std::chrono::duration<float, std::milli>(now - g_FrameStart).count() : 0.0f;
    g_FrameStart = now;
    g_HaveFrameStart = true;
    ++g_GpuFrameSerial;

    if (!glGenQueries || !glQueryCounter || !glGetQueryObjectiv || !glGetQueryObjectui64v) return;
    // Close the frame just recorded, then read back every finished frame oldest-first (so the
    // newest finished one is what GetLastFrameGpu shows). Never waits on the GPU.
    {
        GpuFrame& done = g_GpuFrames[g_GpuCurrent];
        if (done.FrameEnd < 0) done.FrameBegin = -1; // no EndGpuFrame (an early-out frame): no whole-frame time
        done.Pending = !done.Scopes.empty() || done.FrameBegin >= 0;
    }
    for (int i = 1; i <= kGpuFrames; ++i) TryReadBack(g_GpuFrames[(g_GpuCurrent + i) % kGpuFrames]);

    // Recycle the next slot. If its results still aren't in, the GPU is more than kGpuFrames-1
    // frames behind; drop them rather than stall.
    g_GpuCurrent = (g_GpuCurrent + 1) % kGpuFrames;
    GpuFrame& next = g_GpuFrames[g_GpuCurrent];
    next.Used = 0;
    next.Scopes.clear();
    next.Pending = false;
    next.Serial = g_GpuFrameSerial;
    next.FrameEnd = -1;
    next.FrameBegin = TakeQuery(next);
    glQueryCounter(next.Pool[next.FrameBegin], GL_TIMESTAMP);
}

void Profiler::EndGpuFrame() {
    if (!glQueryCounter) return;
    GpuFrame& f = g_GpuFrames[g_GpuCurrent];
    if (f.FrameBegin < 0 || f.FrameEnd >= 0) return; // no BeginFrame yet this slot, or already ended
    f.FrameEnd = TakeQuery(f);
    glQueryCounter(f.Pool[f.FrameEnd], GL_TIMESTAMP);
}

namespace {
template <typename Name> void Push(const Name& name, float milliseconds) {
    if (g_CurrentCount < g_CurrentFrame.size()) {
        Profiler::Entry& e = g_CurrentFrame[g_CurrentCount];
        e.Name.assign(name);
        e.Milliseconds = milliseconds;
        e.Nested = g_ScopeDepth > 0;
    } else {
        g_CurrentFrame.push_back({std::string(name), milliseconds, g_ScopeDepth > 0});
    }
    ++g_CurrentCount;
}
} // namespace

void Profiler::PushSample(const std::string& name, float milliseconds) { Push(name, milliseconds); }

void Profiler::PushSample(const char* name, float milliseconds) { Push(name, milliseconds); }

const std::vector<Profiler::Entry>& Profiler::GetLastFrame() {
    return g_LastFrame;
}

float Profiler::LastFrameWallMs() { return g_LastFrameWallMs; }

float Profiler::LastFrameUnscopedMs() {
    float scoped = 0.0f;
    for (const Entry& e : g_LastFrame)
        if (!e.Nested) scoped += e.Milliseconds;
    return std::max(0.0f, g_LastFrameWallMs - scoped);
}

Profiler::ScopeTimer::ScopeTimer(const char* name)
    : m_Name(name), m_Start(std::chrono::steady_clock::now()) {
    ++g_ScopeDepth;
}

Profiler::ScopeTimer::~ScopeTimer() {
    --g_ScopeDepth; // before the push: this scope's own entry is top-level when it is the outermost
    auto elapsed = std::chrono::steady_clock::now() - m_Start;
    float ms = std::chrono::duration<float, std::milli>(elapsed).count();
    Profiler::PushSample(m_Name, ms);
}

const std::vector<Profiler::Entry>& Profiler::GetLastFrameGpu() {
    static const std::vector<Entry> s_None;
    return g_GpuNewest < 0 ? s_None : g_GpuHistory[g_GpuNewest].Entries;
}

unsigned Profiler::FrameSerial() { return g_GpuFrameSerial; }

void Profiler::ForEachGpuFrameAfter(unsigned serial, const std::function<void(const GpuFrameResult&)>& fn) {
    if (g_GpuNewest < 0) return;
    // Oldest first: the slot after the newest is the oldest in the ring.
    for (int i = 1; i <= kGpuHistory; ++i) {
        const GpuDone& d = g_GpuHistory[(g_GpuNewest + i) % kGpuHistory];
        if (d.Serial > serial) fn({d.Serial, d.FrameMs, &d.Entries});
    }
}

const std::vector<Profiler::Entry>* Profiler::GpuFrameScopes(unsigned serial) {
    if (g_GpuNewest < 0) return nullptr;
    for (const GpuDone& d : g_GpuHistory)
        if (d.Serial == serial && !d.Entries.empty()) return &d.Entries;
    return nullptr;
}

Profiler::GpuScopeTimer::GpuScopeTimer(const char* name) {
    if (!glGenQueries || !glQueryCounter) return; // loader didn't resolve these entry points
    GpuFrame& f = g_GpuFrames[g_GpuCurrent];
    const int begin = TakeQuery(f);
    glQueryCounter(f.Pool[begin], GL_TIMESTAMP);
    m_Record = (int)f.Scopes.size();
    m_Slot = g_GpuCurrent;
    m_Serial = g_GpuFrameSerial;
    f.Scopes.push_back({name, begin, -1});
}

Profiler::GpuScopeTimer::~GpuScopeTimer() {
    if (m_Record < 0) return;
    if (m_Serial != g_GpuFrameSerial) return; // frame rolled over mid-scope (BeginFrame inside it)
    GpuFrame& f = g_GpuFrames[m_Slot];
    if (m_Record >= (int)f.Scopes.size()) return;
    const int end = TakeQuery(f);
    glQueryCounter(f.Pool[end], GL_TIMESTAMP);
    f.Scopes[m_Record].End = end;
}
