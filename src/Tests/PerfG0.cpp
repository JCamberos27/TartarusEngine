#include "Tests/PerfG0.h"

#include "Core/Env.h"
#include "Core/Profiler.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace {
// The render CPU scopes the P-48 ranking reads (SceneViewPass, ShadowFrame, SceneRenderer). Nested in Game View Render.
const char* const kP48Scopes[] = {
    "Game View Render", "Gather Visible", "Scene Draw", "Scene: Classify Visible", "Scene: Depth Pre-pass",
    "Scene: Group Instances", "Scene: Build Batches", "Scene: Opaque", "Decal Prepare", "View Model Draw",
    "Sun Shadow Pass", "Sun: Fit + Gather", "Sun: Sort + Masks", "Sun: Draw Casters", "Sun: Grass",
    "Local Shadow Gather", "Spot Shadow Pass", "Point Shadow Pass",
};
// What the CPU spent waiting on the GPU at the end of the frame: the present, or the hidden run's pacing fence
// (Application.cpp: "Pace Wait" and "Pace Flush" are nested in it).
constexpr const char* kWaitScope = "Swap Buffers";

bool Seen(const std::vector<std::string>& seen, const std::string& key) {
    return std::find(seen.begin(), seen.end(), key) != seen.end();
}
} // namespace

std::string PerfG0::KeyOf(const std::string& scopeName) {
    std::string key;
    for (char c : scopeName) {
        if (std::isalnum((unsigned char)c)) key += (char)std::tolower((unsigned char)c);
        else if (!key.empty() && key.back() != '_') key += '_';
    }
    while (!key.empty() && key.back() == '_') key.pop_back();
    return key;
}

void PerfG0::AddSeen(const AddFn& add, const std::string& key, double ms, std::vector<std::string>& seen, bool present) {
    if (present) {
        if (!Seen(seen, key)) seen.push_back(key);
        add(key, ms);
    } else if (Seen(seen, key)) {
        add(key, 0.0); // ran earlier this phase, not this frame: a zero keeps its percentiles per frame
    }
}

void PerfG0::BeginPhase() {
    m_Started = true;
    // GetLastFrame is FrameSerial()-1; anything recorded before that frame belongs to the warm-up.
    const unsigned now = Profiler::FrameSerial();
    m_LastGpuSerial = now >= 2 ? now - 2 : 0;
    m_CpuBound = m_GpuBound = 0;
    m_SeenCpu.clear();
    m_SeenGpu.clear();
}

void PerfG0::Sample(const AddFn& add) {
    if (!m_Started) BeginPhase();
    const std::vector<Profiler::Entry>& cpu = Profiler::GetLastFrame();
    // Which half bound the frame, and the CPU half: the frame's wall time less what it spent waiting on the GPU.
    double waitMs = 0.0;
    for (const Profiler::Entry& e : cpu)
        if (!e.Nested && e.Name == kWaitScope) waitMs += e.Milliseconds;
    const double wallMs = Profiler::LastFrameWallMs();
    if (wallMs > 0.0) {
        add("half.cpu", std::max(0.0, wallMs - waitMs));
        (waitMs >= kGpuBoundWaitMs ? m_GpuBound : m_CpuBound)++;
    }
    // P-48: each listed scope summed over the frame (Scene Draw runs once per view).
    for (const char* name : kP48Scopes) {
        double ms = 0.0;
        bool present = false;
        for (const Profiler::Entry& e : cpu)
            if (e.Name == name) { ms += e.Milliseconds; present = true; }
        AddSeen(add, "p48." + KeyOf(name), ms, m_SeenCpu, present);
    }
    // The GPU half: every frame read back since the last call (each exactly once).
    Profiler::ForEachGpuFrameAfter(m_LastGpuSerial, [&](const Profiler::GpuFrameResult& f) {
        m_LastGpuSerial = std::max(m_LastGpuSerial, f.Serial);
        if (f.FrameMs >= 0.0f) add("gpu.frame", f.FrameMs);
        std::vector<std::string> here;
        for (const Profiler::Entry& e : *f.Scopes) {
            const std::string key = "gpu." + KeyOf(e.Name);
            here.push_back(key);
            AddSeen(add, key, e.Milliseconds, m_SeenGpu, true);
        }
        const std::vector<std::string> seen = m_SeenGpu; // AddSeen may grow it
        for (const std::string& key : seen)
            if (!Seen(here, key)) add(key, 0.0);
    });
}

void PerfG0::Report(const std::string& sceneFile, const char* mode) {
    const int n = m_CpuBound + m_GpuBound;
    if (n > 0) {
        std::printf("[PerfBench]   BIND cpu-bound=%d gpu-bound=%d of %d (gpu-bound = waited >= %.1f ms at %s)\n", m_CpuBound,
                    m_GpuBound, n, kGpuBoundWaitMs, kWaitScope);
        // TARTARUS_PERF_CSV: row "bind", rank cpu / gpu, the frame count in "frames" (PerfBench's columns).
        const std::string path = Env::Str(Env::Var::PerfCsv);
        if (!path.empty()) {
            if (std::ofstream out{path, std::ios::app}) {
                auto row = [&](const char* half, int count) {
                    // 18 columns: scene,phase,row,rank, 9 empty (frame..over33), frames, 4 empty (cpu..gpu_starved).
                    out << '"' << sceneFile << "\"," << mode << ",bind," << half << ",,,,,,,,,," << count << ",,,,\n";
                };
                row("cpu", m_CpuBound);
                row("gpu", m_GpuBound);
            }
        }
    }
    m_Started = false;
    m_CpuBound = m_GpuBound = 0;
}
