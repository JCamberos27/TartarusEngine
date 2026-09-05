#include "Profiler.h"
#include "gl.h"
#include <unordered_map>

namespace {
std::vector<Profiler::Entry> g_CurrentFrame;
std::vector<Profiler::Entry> g_LastFrame;

// One small ring of query objects per named GPU scope. `Next` is both the slot the next
// GpuScopeTimer for this name will use AND the oldest still-outstanding query - exactly the one
// BeginFrame() should poll before anything reuses it. kRingSize=4 covers ordinary 1-3 frame
// pipeline latency with room to spare, including scopes issued more than once per frame (e.g.
// "Scene Draw", run once for the Scene tab and once for the Game tab).
constexpr int kGpuRingSize = 4;
struct GpuRing {
    GLuint Queries[kGpuRingSize] = {};
    bool Pending[kGpuRingSize] = {};
    int Next = 0;
    bool Created = false;
};
std::unordered_map<std::string, GpuRing> g_GpuRings;
std::vector<Profiler::Entry> g_GpuLastFrame;
}

void Profiler::BeginFrame() {
    g_LastFrame = std::move(g_CurrentFrame);
    g_CurrentFrame.clear();

    // Poll every ring's oldest outstanding query before any GpuScopeTimer for this frame can
    // reuse that slot. A slot whose result isn't available yet just contributes nothing this
    // frame rather than stalling - the same "several frames late" latency the header describes.
    g_GpuLastFrame.clear();
    if (glGetQueryObjectiv && glGetQueryObjectui64v) {
        for (auto& [name, ring] : g_GpuRings) {
            int slot = ring.Next;
            if (!ring.Pending[slot]) continue;
            GLint available = 0;
            glGetQueryObjectiv(ring.Queries[slot], GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) continue;
            GLuint64 ns = 0;
            glGetQueryObjectui64v(ring.Queries[slot], GL_QUERY_RESULT, &ns);
            ring.Pending[slot] = false;
            g_GpuLastFrame.push_back({name, (float)((double)ns / 1000000.0)});
        }
    }
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

Profiler::GpuScopeTimer::GpuScopeTimer(std::string name) : m_Name(std::move(name)) {
    if (!glGenQueries || !glBeginQuery) return; // loader didn't resolve these entry points

    GpuRing& ring = g_GpuRings[m_Name];
    if (!ring.Created) {
        glGenQueries(kGpuRingSize, ring.Queries);
        ring.Created = true;
    }
    // Never re-begin over a query whose result hasn't been retrieved yet - that would silently
    // discard it. This only happens if the ring is too small for the current pipeline depth;
    // skipping the sample for one frame is harmless and self-corrects once BeginFrame catches up.
    if (ring.Pending[ring.Next]) return;

    glBeginQuery(GL_TIME_ELAPSED, ring.Queries[ring.Next]);
    m_Slot = ring.Next;
}

Profiler::GpuScopeTimer::~GpuScopeTimer() {
    if (m_Slot < 0) return;
    glEndQuery(GL_TIME_ELAPSED);
    GpuRing& ring = g_GpuRings[m_Name];
    ring.Pending[m_Slot] = true;
    ring.Next = (ring.Next + 1) % kGpuRingSize;
}
