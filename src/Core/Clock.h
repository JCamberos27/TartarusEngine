#pragma once

class Clock {
public:
    static void Update();
    static float DeltaTime() { return s_DeltaTime; }
    static float TotalTime() { return s_TotalTime; }

private:
    static double s_LastFrame;
    static float s_DeltaTime;
    static float s_TotalTime;
};
