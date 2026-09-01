#include "Clock.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <thread>

double Clock::s_LastFrame = 0.0;
float Clock::s_DeltaTime = 0.0f;
float Clock::s_TotalTime = 0.0f;

void Clock::Update() {
    double now = glfwGetTime();
    s_DeltaTime = static_cast<float>(now - s_LastFrame);
    // Clamp to avoid huge steps after a breakpoint/stall.
    s_DeltaTime = std::min(s_DeltaTime, 0.1f);
    s_TotalTime = static_cast<float>(now);
    s_LastFrame = now;
}

void Clock::LimitFps(int targetFps) {
    if (targetFps <= 0) return;

    const double frameEnd = s_LastFrame + 1.0 / static_cast<double>(targetFps);

    // Sleep off the bulk of the wait (cheap, but the OS scheduler can overshoot by a
    // millisecond or two), then spin the last sliver so the cap is actually hit. main.cpp
    // raises the Windows timer resolution to 1ms for the duration of the run so the sleep
    // portion stays tight.
    double remaining = frameEnd - glfwGetTime();
    if (remaining > 0.0015) {
        std::this_thread::sleep_for(std::chrono::duration<double>(remaining - 0.0015));
    }
    while (glfwGetTime() < frameEnd) { /* spin */ }
}
