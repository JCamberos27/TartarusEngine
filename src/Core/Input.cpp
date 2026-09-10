#include "Input.h"
#include <GLFW/glfw3.h>
#include <cstring>

GLFWwindow* Input::s_Window = nullptr;
double Input::s_LastX = 0.0, Input::s_LastY = 0.0;
double Input::s_DeltaX = 0.0, Input::s_DeltaY = 0.0;
double Input::s_ScrollAccum = 0.0, Input::s_ScrollY = 0.0;
bool Input::s_FirstMouse = true;
bool Input::s_PrevKeys[512] = {};
static bool s_CurKeys[512] = {};

// Installed before Dear ImGui's own GLFW backend (EditorLayer::Init() runs after Input::Init()
// in main()), so ImGui_ImplGlfw_InitForOpenGL's callback-chaining picks this up as the "previous
// user callback" and still forwards scroll events to ImGui — this doesn't steal ImGui's scroll
// input, both just see every event.
void Input::ScrollCallback(GLFWwindow*, double, double yoffset) {
    s_ScrollAccum += yoffset;
}

void Input::Init(GLFWwindow* window) {
    s_Window = window;
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

    // Only GLFW_KEY_SPACE (32) .. GLFW_KEY_LAST (348) are valid arguments to glfwGetKey — any
    // code outside that range raises GLFW_INVALID_VALUE (previously silent; now caught by the
    // error callback). The arrays stay sized 512 and indexed by raw keycode; slots outside this
    // range are just never touched, since nothing in the engine maps a key there.
    for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; ++key) {
        s_PrevKeys[key] = s_CurKeys[key];
        s_CurKeys[key] = glfwGetKey(s_Window, key) == GLFW_PRESS;
    }
}

bool Input::IsKeyDown(int key) {
    return glfwGetKey(s_Window, key) == GLFW_PRESS;
}

bool Input::IsKeyPressed(int key) {
    // s_CurKeys / s_PrevKeys are bool[512] indexed by raw keycode. GLFW_KEY_UNKNOWN (-1) or a
    // stale rebindable-shortcut int would be an out-of-bounds read; IsKeyDown is already safe
    // because glfwGetKey range-checks internally.
    if (key < 0 || key >= 512) return false;
    return s_CurKeys[key] && !s_PrevKeys[key];
}

bool Input::IsMouseButtonDown(int button) {
    return glfwGetMouseButton(s_Window, button) == GLFW_PRESS;
}

double Input::GetMouseDeltaX() { return s_DeltaX; }
double Input::GetMouseDeltaY() { return s_DeltaY; }
double Input::GetMouseX() { return s_LastX; }
double Input::GetMouseY() { return s_LastY; }
double Input::GetScrollDeltaY() { return s_ScrollY; }
