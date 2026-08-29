#include "Profiler.h"

namespace {
std::vector<Profiler::Entry> g_CurrentFrame;
std::vector<Profiler::Entry> g_LastFrame;
}

void Profiler::BeginFrame() {
    g_LastFrame = std::move(g_CurrentFrame);
    g_CurrentFrame.clear();
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
