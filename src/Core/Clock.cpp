#include "Clock.h"
#include <GLFW/glfw3.h>
#include <algorithm>

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
