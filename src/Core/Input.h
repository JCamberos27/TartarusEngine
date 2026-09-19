#pragma once

struct GLFWwindow;

// Static-style input service. Call Update() once per frame, right after PollEvents, to take this
// frame's snapshot: every query below reads that snapshot (#145), so within one frame IsKeyDown,
// IsKeyPressed and IsKeyReleased always agree. While the window is unfocused everything reads as
// up, so a key held while alt-tabbing away can't stay "down".
class Input {
public:
    static void Init(GLFWwindow* window);
    static void Update();

    static bool IsKeyDown(int key);
    static bool IsKeyPressed(int key);  // true only on the frame the key went down
    static bool IsKeyReleased(int key); // true only on the frame the key went up

    static bool IsMouseButtonDown(int button);
    static bool IsMouseButtonPressed(int button);  // frame the button went down
    static bool IsMouseButtonReleased(int button); // frame the button went up

    static double GetMouseDeltaX();
    static double GetMouseDeltaY();
    static double GetMouseX();
    static double GetMouseY();

    // Scroll wheel movement accumulated since the last Update() (GLFW's scroll callback can fire
    // zero, one, or several times between polls). Y: positive is scroll away from the user (the
    // "zoom in" direction by convention). X: horizontal wheel / trackpad, positive is right.
    static double GetScrollDeltaY();
    static double GetScrollDeltaX();

    // Gamepad (#145) - the first connected joystick with a standard (SDL_GameControllerDB)
    // mapping, polled once per Update(). Buttons / axes are GLFW_GAMEPAD_BUTTON_* / _AXIS_*.
    // Stick axes have a radial dead zone applied (0 inside it, rescaled to 0..1 outside);
    // stick Y is +1 DOWN as GLFW reports it. Triggers read 0 (released) .. 1 (fully pulled).
    static bool   IsGamepadConnected();
    static bool   IsGamepadButtonDown(int button);
    static bool   IsGamepadButtonPressed(int button); // frame the button went down
    static bool   IsGamepadButtonReleased(int button); // frame the button went up
    static float  GetGamepadAxis(int axis);

private:
    static GLFWwindow* s_Window;
    static double s_LastX, s_LastY;
    static double s_DeltaX, s_DeltaY;
    static double s_ScrollAccum, s_ScrollY;
    static double s_ScrollAccumX, s_ScrollX;
    static bool s_FirstMouse;
    static bool s_PrevKeys[512];
    static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);
};
