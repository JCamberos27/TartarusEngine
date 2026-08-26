#pragma once

struct GLFWwindow;

// Static-style input service. Call Update() once per frame (before PollEvents
// consumes new state) to compute mouse deltas and edge-triggered "just pressed".
class Input {
public:
    static void Init(GLFWwindow* window);
    static void Update();

    static bool IsKeyDown(int key);
    static bool IsKeyPressed(int key); // true only on the frame the key went down

    static bool IsMouseButtonDown(int button);

    static double GetMouseDeltaX();
    static double GetMouseDeltaY();
    static double GetMouseX();
    static double GetMouseY();

private:
    static GLFWwindow* s_Window;
    static double s_LastX, s_LastY;
    static double s_DeltaX, s_DeltaY;
    static bool s_FirstMouse;
    static bool s_PrevKeys[512];
};
