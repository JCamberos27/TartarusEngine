#include "Window.h"
#include "gl.h"
#include "Log.h"
#include <GLFW/glfw3.h>
#include <stdexcept>
#include <iostream>
#include <string>

namespace {
// Installed before glfwInit() so it also catches init-time failures. GLFW otherwise reports
// errors only through this callback — with none set, a failed window/context/monitor call is
// completely silent. Routed into the engine Log (which mirrors to stderr) like everything else.
void GlfwErrorCallback(int code, const char* description) {
    Log::Error("GLFW error " + std::to_string(code) + ": " + (description ? description : "(no description)"));
}
} // namespace

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#include <windowsx.h>           // GET_X_LPARAM / GET_Y_LPARAM for WM_NCHITTEST
#include <timeapi.h>            // timeBeginPeriod — tighten the scheduler quantum for the FPS cap
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")

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

// --- Borderless custom frame -------------------------------------------------------------
// The window keeps its real OS styles (WS_OVERLAPPEDWINDOW) — so Aero-snap, the drop shadow,
// resize and the maximize animation all still work — but WM_NCCALCSIZE swallows the standard
// caption/frame, and WM_NCHITTEST re-synthesises the resize borders plus a drag region. The
// editor draws its own min/max/close buttons on the top toolbar and, once per frame, tells the
// Window whether the cursor is over the toolbar's empty area (the drag handle).
Window*  g_FramedWindow  = nullptr;
WNDPROC  g_OrigWndProc   = nullptr;

int FrameBorderPx() {
    int b = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    return b > 6 ? b : 8;
}

LRESULT CALLBACK TartarusWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE: {
        if (wParam != TRUE) break;
        // Returning 0 makes the client area cover the whole window (no caption, no frame).
        // When maximized Windows grows the window rect past the monitor by the frame width, so
        // claw that back or the toolbar's top row is clipped off-screen.
        NCCALCSIZE_PARAMS* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
        if (IsZoomed(hwnd)) {
            const int bx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            const int by = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            p->rgrc[0].left   += bx;
            p->rgrc[0].right  -= bx;
            p->rgrc[0].top    += by;
            p->rgrc[0].bottom -= by;
        }
        return 0;
    }
    case WM_NCHITTEST: {
        // Let Windows' own detection resolve the left / right / bottom resize borders and
        // corners first — it honours WS_THICKFRAME and is far more reliable than reimplementing
        // it. We only add the top edge (which the missing caption would otherwise swallow) and
        // upgrade the toolbar's empty area to a draggable caption.
        const LRESULT def = CallWindowProcW(g_OrigWndProc, hwnd, msg, wParam, lParam);
        if (def != HTCLIENT && def != HTNOWHERE && def != HTCAPTION)
            return def;
        if (!IsZoomed(hwnd)) {
            const POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT wr; GetWindowRect(hwnd, &wr);
            const int b = FrameBorderPx();
            if (pt.y < wr.top + b) {
                if (pt.x < wr.left + b)   return HTTOPLEFT;
                if (pt.x >= wr.right - b) return HTTOPRIGHT;
                return HTTOP;
            }
        }
        if (def == HTCAPTION) return HTCLIENT; // no real caption exists; ignore stray hits
        if (g_FramedWindow && g_FramedWindow->TitleBarDragActive()) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        // Keep a maximized borderless window inside the monitor work area (don't cover the taskbar).
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(mon, &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top  - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right  - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        mmi->ptMinTrackSize.x = 720;
        mmi->ptMinTrackSize.y = 460;
        return 0;
    }
    default: break;
    }
    return CallWindowProcW(g_OrigWndProc, hwnd, msg, wParam, lParam);
}

void InstallCustomFrame(Window* self, GLFWwindow* handle) {
    HWND hwnd = glfwGetWin32Window(handle);
    g_FramedWindow = self;
    g_OrigWndProc  = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(TartarusWndProc)));
    // Guarantee the styles that make Windows treat this as a normal resizable/snappable window
    // (the caption is only hidden visually, via WM_NCCALCSIZE — the style bit stays).
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style |= WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU |
             WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    // A 1px bottom sheet keeps the DWM drop shadow without a visible caption line.
    MARGINS m{ 0, 0, 1, 0 };
    DwmExtendFrameIntoClientArea(hwnd, &m);
    // Force WM_NCCALCSIZE to re-run now that the proc is hooked.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void RemoveCustomFrame(GLFWwindow* handle) {
    if (!g_OrigWndProc) return;
    HWND hwnd = glfwGetWin32Window(handle);
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_OrigWndProc));
    g_OrigWndProc = nullptr;
    g_FramedWindow = nullptr;
}
} // namespace
#endif

Window::Window(int width, int height, const std::string& title)
    : m_Width(width), m_Height(height) {
    glfwSetErrorCallback(GlfwErrorCallback);

    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    // Ask for a debug context so KHR_debug output is available (GLDebug wires the callback when
    // a Debug build or TARTARUS_GL_DEBUG=1 turns it on). Free on a release driver when unused.
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
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
    InstallCustomFrame(this, m_Handle); // remove the OS caption; editor draws its own controls
#endif
    // Deliberately NOT shown here. Showing before anything is drawn flashes an unpainted
    // framebuffer (a white rectangle over garbage) for as long as loading takes. main() calls
    // Show() once the first real frame has been rendered; the splash covers the gap.

    glfwMakeContextCurrent(m_Handle);
    SetVSync(m_VSyncMode); // vsync on by default; main() re-applies the saved preference

#if defined(_WIN32)
    // Without this, this_thread::sleep_for rounds up to the default ~15.6ms tick, which makes
    // any FPS cap wildly inaccurate. 1ms for the whole session; paired timeEndPeriod in dtor.
    timeBeginPeriod(1);
#endif

    if (!GLLoader_Init()) {
        throw std::runtime_error("Failed to load OpenGL functions");
    }

    // The loader's pointers resolve on any 3.3+ context, but every shader is `#version 460`.
    // Without this check a downgraded context (RDP, llvmpipe, an old driver) dies later with a
    // cryptic "Shader compile error: ... version 460" instead of naming the real problem (#105).
    {
        int glMajor = glfwGetWindowAttrib(m_Handle, GLFW_CONTEXT_VERSION_MAJOR);
        int glMinor = glfwGetWindowAttrib(m_Handle, GLFW_CONTEXT_VERSION_MINOR);
        if (glMajor < 4 || (glMajor == 4 && glMinor < 6)) {
            const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            throw std::runtime_error(
                "Tartarus needs an OpenGL 4.6 core context. This GPU/driver reports OpenGL " +
                std::to_string(glMajor) + "." + std::to_string(glMinor) +
                (ver ? std::string(" (\"") + ver + "\")" : std::string()) +
                ".\n\nUpdate your graphics driver, or run on a machine with a GPU that supports OpenGL 4.6.");
        }
    }

    glfwSetWindowUserPointer(m_Handle, this);
    glfwSetFramebufferSizeCallback(m_Handle, FramebufferSizeCallback);
    glfwSetDropCallback(m_Handle, DropCallbackTrampoline);

    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS); // smooth filtering across point-shadow cube face edges (#119)
}

Window::~Window() {
#if defined(_WIN32)
    timeEndPeriod(1);
    if (m_Handle) RemoveCustomFrame(m_Handle);
#endif
    if (m_Handle) {
        glfwDestroyWindow(m_Handle);
    }
    glfwTerminate();
}

void Window::SetVSync(int mode) {
    m_VSyncMode = mode;
    // 0 -> no sync, 1 -> sync to refresh, 2 -> adaptive (negative interval = late-swap tear).
    int interval = mode == 0 ? 0 : (mode == 2 ? -1 : 1);
    glfwSwapInterval(interval);
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

void Window::ShowFatalErrorDialog(const std::string& message) {
#if defined(_WIN32)
    MessageBoxA(nullptr, message.c_str(), "Tartarus Engine", MB_ICONERROR | MB_OK);
#else
    (void)message;
#endif
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
