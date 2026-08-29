#include "Window.h"
#include "gl.h"
#include <GLFW/glfw3.h>
#include <stdexcept>
#include <iostream>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>

// Present in the Windows 11 SDK's dwmapi.h, but guarded here in case an older SDK is used
// to build this — DwmSetWindowAttribute itself just ignores attributes it doesn't recognize
// on older Windows versions, so this degrades gracefully to the default title bar.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

namespace {
void ApplyBlackTitleBar(GLFWwindow* handle) {
    HWND hwnd = glfwGetWin32Window(handle);
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    COLORREF black = 0x00000000;  // 0x00BBGGRR
    COLORREF white = 0x00FFFFFF;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &black, sizeof(black));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &white, sizeof(white));
}
}
#endif

Window::Window(int width, int height, const std::string& title)
    : m_Width(width), m_Height(height) {
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    // Report the real per-monitor content scale (e.g. 2.0 at Windows' 200% scaling, common on
    // 4K displays) so the editor can bake it into font sizes and layout instead of rendering a
    // tiny fixed-pixel UI.
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    // Stay hidden until the title bar is recolored below, so it never flashes the default
    // light title bar for a frame before switching to black.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    m_Handle = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_Handle) {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

#if defined(_WIN32)
    ApplyBlackTitleBar(m_Handle);
#endif
    // Deliberately NOT shown here. Showing before anything is drawn flashes an unpainted
    // framebuffer (a white rectangle over garbage) for as long as loading takes. main() calls
    // Show() once the first real frame has been rendered; the splash covers the gap.

    glfwMakeContextCurrent(m_Handle);
    glfwSwapInterval(1); // vsync

    if (!GLLoader_Init()) {
        throw std::runtime_error("Failed to load OpenGL functions");
    }

    glfwSetWindowUserPointer(m_Handle, this);
    glfwSetFramebufferSizeCallback(m_Handle, FramebufferSizeCallback);
    glfwSetDropCallback(m_Handle, DropCallbackTrampoline);

    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_MULTISAMPLE);
}

Window::~Window() {
    if (m_Handle) {
        glfwDestroyWindow(m_Handle);
    }
    glfwTerminate();
}

bool Window::ShouldClose() const {
    return glfwWindowShouldClose(m_Handle);
}

void Window::SetShouldClose(bool close) {
    glfwSetWindowShouldClose(m_Handle, close ? GLFW_TRUE : GLFW_FALSE);
}

void Window::PollEvents() {
    glfwPollEvents();
}

void Window::SwapBuffers() {
    glfwSwapBuffers(m_Handle);
}

void Window::SetTitle(const std::string& title) {
    glfwSetWindowTitle(m_Handle, title.c_str());
}

void Window::Maximize() {
    glfwMaximizeWindow(m_Handle);
}

void Window::Show() {
    glfwShowWindow(m_Handle);
}

void Window::SetFullscreen(bool fullscreen) {
    if (fullscreen == m_IsFullscreen) return;

    if (fullscreen) {
        // Remember the windowed rect so toggling back doesn't strand the window at (0,0)
        // or the monitor's resolution.
        glfwGetWindowPos(m_Handle, &m_WindowedX, &m_WindowedY);
        glfwGetWindowSize(m_Handle, &m_WindowedW, &m_WindowedH);

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(m_Handle, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(m_Handle, nullptr, m_WindowedX, m_WindowedY, m_WindowedW, m_WindowedH, 0);
    }
    m_IsFullscreen = fullscreen;
}

void Window::SetCursorLocked(bool locked) {
    m_CursorLocked = locked;
    glfwSetInputMode(m_Handle, GLFW_CURSOR, locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void Window::FramebufferSizeCallback(GLFWwindow* window, int width, int height) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    self->m_Width = width;
    self->m_Height = height;
    glViewport(0, 0, width, height);
}

void Window::SetDropCallback(std::function<void(const std::vector<std::string>&)> callback) {
    m_DropCallback = std::move(callback);
}

void Window::DropCallbackTrampoline(GLFWwindow* window, int pathCount, const char* paths[]) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!self->m_DropCallback || pathCount <= 0) return;

    std::vector<std::string> pathList;
    pathList.reserve((size_t)pathCount);
    for (int i = 0; i < pathCount; ++i) pathList.emplace_back(paths[i]);
    self->m_DropCallback(pathList);
}
