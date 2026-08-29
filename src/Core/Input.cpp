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

    s_ScrollY = s_ScrollAccum;
    s_ScrollAccum = 0.0;

    for (int key = 0; key < 512; ++key) {
        s_PrevKeys[key] = s_CurKeys[key];
        s_CurKeys[key] = glfwGetKey(s_Window, key) == GLFW_PRESS;
    }
}

bool Input::IsKeyDown(int key) {
    return glfwGetKey(s_Window, key) == GLFW_PRESS;
}

bool Input::IsKeyPressed(int key) {
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
