#pragma once

class Clock {
public:
    static void Update();
    static float DeltaTime() { return s_DeltaTime; }
    static float TotalTime() { return s_TotalTime; }

    // Busy-waits/sleeps until this frame has lasted at least 1/targetFps seconds, measured from
    // the last Update() (i.e. the start of the current frame). No-op when targetFps <= 0. Call
    // once per frame after presenting; the next Update() then sees a delta that reflects the cap.
    static void LimitFps(int targetFps);

private:
    static double s_LastFrame;
    static float s_DeltaTime;
    static float s_TotalTime;
};
