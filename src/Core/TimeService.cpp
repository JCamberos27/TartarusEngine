#include "TimeService.h"
#include "ProjectSettings.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace {

double g_FrameStart = -1.0; // glfwGetTime() at the last BeginFrame; <0 before the first frame
float  g_RealDelta = 0.0f;
float  g_UnscaledDelta = 0.0f;
float  g_Delta = 0.0f;
double g_Time = 0.0;
double g_UnscaledTime = 0.0;
std::uint64_t g_FrameCount = 0;
float  g_TimeScale = 1.0f;
float  g_DebugTimeScale = 1.0f;
float  g_MaxDelta = 0.1f;
double g_RefreshPeriod = 0.0; // SetRefreshPeriod; 0 = deltas as measured
double g_Unpaced = 0.0;       // measured time not yet handed out as whole refreshes (+ ahead, - behind)
double g_AvgDelta = 0.0;      // the measured delta, averaged over the last ~20 frames

#if defined(_WIN32)
// A high-resolution waitable timer (Windows 10 1803+) wakes within ~0.5 ms of its due time,
// unlike Sleep, which rounds to the scheduler tick. Null when unavailable - LimitFps then
// sleeps with a wider margin and spins a little longer.
HANDLE FrameTimer() {
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                 TIMER_ALL_ACCESS);
    return timer;
}
#endif

} // namespace

namespace Time {

double PaceDelta(double measured, double period, double& unpaced) {
    unpaced += measured;
    double paced = period * (double)std::max(1L, std::lround(unpaced / period));
    unpaced -= paced;
    // A little of what is left over goes back every frame: the refresh rate GLFW reports is
    // rounded (59.94 Hz reads 60), and the game clock must not drift off the real one.
    const double bleed = unpaced * 0.02;
    paced += bleed;
    unpaced -= bleed;
    // Frames not following the refresh at all (a variable-refresh display, a stall): as measured.
    if (std::abs(unpaced) > 2.0 * period) {
        paced += unpaced;
        unpaced = 0.0;
    }
    return paced;
}

void BeginFrame() {
    const double now = glfwGetTime();
    g_RealDelta = g_FrameStart < 0.0 ? 0.0f : (float)(now - g_FrameStart);
    if (g_RealDelta < 0.0f) g_RealDelta = 0.0f;
    g_FrameStart = now;
    g_UnscaledDelta = std::min(g_RealDelta, g_MaxDelta);
    g_AvgDelta += ((double)g_UnscaledDelta - g_AvgDelta) * 0.05;
    // Frames coming faster than the refresh: the sync isn't holding them (the driver overrides it), so as measured.
    if (const double period = g_RefreshPeriod; period > 0.0 && g_UnscaledDelta > 0.0f && g_AvgDelta > period * 0.9) {
        const double paced = PaceDelta(g_UnscaledDelta, period, g_Unpaced);
        g_UnscaledDelta = (float)std::clamp(paced, 0.0, (double)g_MaxDelta);
    }
    g_Delta = g_UnscaledDelta * EffectiveTimeScale();
    g_UnscaledTime += g_UnscaledDelta;
    g_Time += g_Delta;
    ++g_FrameCount;
}

void SetRefreshPeriod(double seconds) {
    seconds = seconds > 0.0 ? seconds : 0.0;
    if (seconds != g_RefreshPeriod) {
        g_Unpaced = 0.0;
        g_AvgDelta = seconds;
    }
    g_RefreshPeriod = seconds;
}

float DeltaTime() { return g_Delta; }
float UnscaledDeltaTime() { return g_UnscaledDelta; }
float RealDeltaTime() { return g_RealDelta; }

double TimeSinceStartup() { return g_Time; }
double UnscaledTimeSinceStartup() { return g_UnscaledTime; }
double RealtimeSinceStartup() { return glfwGetTime(); }

std::uint64_t FrameCount() { return g_FrameCount; }

float TimeScale() { return g_TimeScale; }
void SetTimeScale(float scale) { g_TimeScale = std::clamp(scale, 0.0f, 100.0f); }

void SetDebugTimeScale(float scale) { g_DebugTimeScale = std::clamp(scale, 0.0f, 2.0f); }
float DebugTimeScale() { return g_DebugTimeScale; }
float EffectiveTimeScale() { return g_TimeScale * g_DebugTimeScale; }

float MaximumDeltaTime() { return g_MaxDelta; }
void SetMaximumDeltaTime(float seconds) { g_MaxDelta = std::clamp(seconds, 0.01f, 1.0f); }

float FixedDeltaTime() {
    const float t = ProjectSettings::Physics().FixedTimestep;
    return t > 0.0f ? t : 1.0f / 60.0f;
}

void LimitFps(int targetFps) {
    if (targetFps <= 0 || g_FrameStart < 0.0) return;
    const double frameEnd = g_FrameStart + 1.0 / (double)targetFps;

    // Coarse wait, leaving a margin the wake-up can't overshoot, then spin (yielding) the rest.
    // Window.cpp raises the Windows timer resolution to 1 ms, which tightens the Sleep fallback.
#if defined(_WIN32)
    HANDLE timer = FrameTimer();
    const double margin = timer ? 0.0006 : 0.0015;
#else
    const double margin = 0.0015;
#endif
    const double remaining = frameEnd - glfwGetTime();
    if (remaining > margin) {
        const double wait = remaining - margin;
#if defined(_WIN32)
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(wait * 1.0e7); // relative, in 100 ns units
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else
            std::this_thread::sleep_for(std::chrono::duration<double>(wait));
#else
        std::this_thread::sleep_for(std::chrono::duration<double>(wait));
#endif
    }
    while (glfwGetTime() < frameEnd) std::this_thread::yield();
}

} // namespace Time
