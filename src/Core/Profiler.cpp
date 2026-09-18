#include "Profiler.h"
#include "gl.h"
#include <algorithm>

namespace {
std::vector<Profiler::Entry> g_CurrentFrame;
std::vector<Profiler::Entry> g_LastFrame;

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
    g_LastFrame = std::move(g_CurrentFrame);
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
    g_CurrentFrame.push_back({name, milliseconds});
}

const std::vector<Profiler::Entry>& Profiler::GetLastFrame() {
    return g_LastFrame;
}

Profiler::ScopeTimer::ScopeTimer(std::string name)
    : m_Name(std::move(name)), m_Start(std::chrono::steady_clock::now()) {}

Profiler::ScopeTimer::~ScopeTimer() {
    auto elapsed = std::chrono::steady_clock::now() - m_Start;
    float ms = std::chrono::duration<float, std::milli>(elapsed).count();
    Profiler::PushSample(m_Name, ms);
}

const std::vector<Profiler::Entry>& Profiler::GetLastFrameGpu() {
    return g_GpuLastFrame;
}

Profiler::GpuScopeTimer::GpuScopeTimer(const char* name) {
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
