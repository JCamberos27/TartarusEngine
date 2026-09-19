#include "Input.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstring>

GLFWwindow* Input::s_Window = nullptr;
double Input::s_LastX = 0.0, Input::s_LastY = 0.0;
double Input::s_DeltaX = 0.0, Input::s_DeltaY = 0.0;
double Input::s_ScrollAccum = 0.0, Input::s_ScrollY = 0.0;
double Input::s_ScrollAccumX = 0.0, Input::s_ScrollX = 0.0;
bool Input::s_FirstMouse = true;
bool Input::s_PrevKeys[512] = {};
static bool s_CurKeys[512] = {};
static constexpr int kMouseButtons = 8; // GLFW_MOUSE_BUTTON_1 .. GLFW_MOUSE_BUTTON_LAST
static bool s_CurButtons[kMouseButtons] = {};
static bool s_PrevButtons[kMouseButtons] = {};
static constexpr int kPadButtons = 15; // GLFW_GAMEPAD_BUTTON_LAST + 1
static constexpr int kPadAxes = 6;     // GLFW_GAMEPAD_AXIS_LAST + 1
static bool  s_PadConnected = false;
static bool  s_CurPad[kPadButtons] = {};
static bool  s_PrevPad[kPadButtons] = {};
static float s_PadAxes[kPadAxes] = {};
static constexpr float kStickDeadZone = 0.18f;

// Radial dead zone on one stick, rescaled so motion starts at 0 just outside it.
static void ApplyStickDeadZone(float& x, float& y) {
    const float len = std::sqrt(x * x + y * y);
    if (len <= kStickDeadZone) { x = y = 0.0f; return; }
    const float scaled = std::min((len - kStickDeadZone) / (1.0f - kStickDeadZone), 1.0f);
    x = x / len * scaled;
    y = y / len * scaled;
}

// Installed before Dear ImGui's own GLFW backend (EditorLayer::Init() runs after Input::Init()
// in main()), so ImGui_ImplGlfw_InitForOpenGL's callback-chaining picks this up as the "previous
// user callback" and still forwards scroll events to ImGui — this doesn't steal ImGui's scroll
// input, both just see every event.
void Input::ScrollCallback(GLFWwindow*, double xoffset, double yoffset) {
    s_ScrollAccum += yoffset;
    s_ScrollAccumX += xoffset;
}

void Input::Init(GLFWwindow* window) {
    s_Window = window;
    // Input is polled once per frame, so a key or button pressed AND released between two
    // polls (a quick tap at a low frame rate, or synthetic/remote input) used to be missed
    // entirely - e.g. Esc not releasing a captured Game view. Sticky mode makes glfwGetKey /
    // glfwGetMouseButton report such a tap as down for one poll. Input.cpp is their only caller.
    glfwSetInputMode(window, GLFW_STICKY_KEYS, GLFW_TRUE);
    glfwSetInputMode(window, GLFW_STICKY_MOUSE_BUTTONS, GLFW_TRUE);
    glfwGetCursorPos(window, &s_LastX, &s_LastY);
    glfwSetScrollCallback(window, ScrollCallback);
}

void Input::Update() {
    // A GLFW_CURSOR mode change (lock <-> unlock, e.g. clicking into or Esc-ing out of a
    // captured game view) makes the next reported cursor position jump: GLFW switches between
    // real screen coords and virtualized deltas, and may recenter a frame late. Feeding that
    // jump into mouse-look whips the camera. Zero the look delta for a couple of frames after
    // any such transition; s_LastX/Y still track the real position so the frame after settles
    // cleanly with no residual spike.
    static int prevCursorMode = -1;
    static int cursorSettleFrames = 0;
    int curCursorMode = glfwGetInputMode(s_Window, GLFW_CURSOR);
    if (curCursorMode != prevCursorMode) {
        prevCursorMode = curCursorMode;
        cursorSettleFrames = 2;
    }

    double x, y;
    glfwGetCursorPos(s_Window, &x, &y);
    if (s_FirstMouse) {
        s_LastX = x;
        s_LastY = y;
        s_FirstMouse = false;
    }
    s_DeltaX = x - s_LastX;
    s_DeltaY = s_LastY - y; // inverted: up is positive
    s_LastX = x;
    s_LastY = y;
    if (cursorSettleFrames > 0) {
        s_DeltaX = 0.0;
        s_DeltaY = 0.0;
        --cursorSettleFrames;
    }

    s_ScrollY = s_ScrollAccum;
    s_ScrollAccum = 0.0;
    s_ScrollX = s_ScrollAccumX;
    s_ScrollAccumX = 0.0;

    // #145 - unfocused: everything reads as up (and releases on the frame focus is lost), so a
    // key or button held while switching away can't stay stuck down for the game or the editor.
    const bool focused = glfwGetWindowAttrib(s_Window, GLFW_FOCUSED) == GLFW_TRUE;

    // Only GLFW_KEY_SPACE (32) .. GLFW_KEY_LAST (348) are valid arguments to glfwGetKey — any
    // code outside that range raises GLFW_INVALID_VALUE (previously silent; now caught by the
    // error callback). The arrays stay sized 512 and indexed by raw keycode; slots outside this
    // range are just never touched, since nothing in the engine maps a key there.
    for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; ++key) {
        s_PrevKeys[key] = s_CurKeys[key];
        s_CurKeys[key] = focused && glfwGetKey(s_Window, key) == GLFW_PRESS;
    }
    for (int b = 0; b < kMouseButtons; ++b) {
        s_PrevButtons[b] = s_CurButtons[b];
        s_CurButtons[b] = focused && glfwGetMouseButton(s_Window, b) == GLFW_PRESS;
    }

    // Gamepad: first joystick GLFW recognises as a gamepad.
    GLFWgamepadstate pad{};
    s_PadConnected = false;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST && !s_PadConnected; ++jid)
        if (glfwJoystickIsGamepad(jid) && glfwGetGamepadState(jid, &pad)) s_PadConnected = true;
    const bool padLive = s_PadConnected && focused;
    for (int b = 0; b < kPadButtons; ++b) {
        s_PrevPad[b] = s_CurPad[b];
        s_CurPad[b] = padLive && pad.buttons[b] == GLFW_PRESS;
    }
    for (int a = 0; a < kPadAxes; ++a) s_PadAxes[a] = padLive ? pad.axes[a] : 0.0f;
    // Triggers rest at -1 in GLFW; expose them as 0..1.
    for (int a : {GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER})
        s_PadAxes[a] = padLive ? std::clamp((s_PadAxes[a] + 1.0f) * 0.5f, 0.0f, 1.0f) : 0.0f;
    ApplyStickDeadZone(s_PadAxes[GLFW_GAMEPAD_AXIS_LEFT_X], s_PadAxes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
    ApplyStickDeadZone(s_PadAxes[GLFW_GAMEPAD_AXIS_RIGHT_X], s_PadAxes[GLFW_GAMEPAD_AXIS_RIGHT_Y]);
}

// #145 - reads the per-frame snapshot, like IsKeyPressed (it used to query live GLFW state, so
// the two could disagree within a frame).
bool Input::IsKeyDown(int key) {
    if (key < 0 || key >= 512) return false;
    return s_CurKeys[key];
}

bool Input::IsKeyPressed(int key) {
    // s_CurKeys / s_PrevKeys are bool[512] indexed by raw keycode. GLFW_KEY_UNKNOWN (-1) or a
    // stale rebindable-shortcut int would be an out-of-bounds read; IsKeyDown is already safe
    // because glfwGetKey range-checks internally.
    if (key < 0 || key >= 512) return false;
    return s_CurKeys[key] && !s_PrevKeys[key];
}

bool Input::IsKeyReleased(int key) {
    if (key < 0 || key >= 512) return false;
    return !s_CurKeys[key] && s_PrevKeys[key];
}

bool Input::IsMouseButtonDown(int button) {
    return button >= 0 && button < kMouseButtons && s_CurButtons[button];
}

bool Input::IsMouseButtonPressed(int button) {
    return button >= 0 && button < kMouseButtons && s_CurButtons[button] && !s_PrevButtons[button];
}

bool Input::IsMouseButtonReleased(int button) {
    return button >= 0 && button < kMouseButtons && !s_CurButtons[button] && s_PrevButtons[button];
}

double Input::GetMouseDeltaX() { return s_DeltaX; }
double Input::GetMouseDeltaY() { return s_DeltaY; }
double Input::GetMouseX() { return s_LastX; }
double Input::GetMouseY() { return s_LastY; }
double Input::GetScrollDeltaY() { return s_ScrollY; }
double Input::GetScrollDeltaX() { return s_ScrollX; }

bool Input::IsGamepadConnected() { return s_PadConnected; }
bool Input::IsGamepadButtonDown(int button) {
    return button >= 0 && button < kPadButtons && s_CurPad[button];
}
bool Input::IsGamepadButtonPressed(int button) {
    return button >= 0 && button < kPadButtons && s_CurPad[button] && !s_PrevPad[button];
}
float Input::GetGamepadAxis(int axis) {
    return axis >= 0 && axis < kPadAxes ? s_PadAxes[axis] : 0.0f;
}
