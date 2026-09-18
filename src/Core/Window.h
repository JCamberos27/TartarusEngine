#pragma once
#include <string>
#include <vector>
#include <functional>

struct GLFWwindow;

class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    bool ShouldClose() const;
    void PollEvents();
    void SwapBuffers();
    void SetShouldClose(bool close);

    int GetWidth() const { return m_Width; }
    int GetHeight() const { return m_Height; }
    GLFWwindow* Handle() const { return m_Handle; }

    void SetCursorLocked(bool locked);
    bool IsCursorLocked() const { return m_CursorLocked; }

    void SetTitle(const std::string& title);
    void Maximize();

    // #143: the window's restored ("normal") rect plus whether it is maximized, so the editor
    // can reopen where the user left it. Coordinates are OS workspace pixels on Windows
    // (GetWindowPlacement's own space, so a save/restore round-trip is exact); elsewhere they
    // are GLFW screen coordinates. Valid is false when there is nothing usable (fullscreen,
    // minimized-only, or no native window).
    struct Placement {
        bool Valid = false;
        int X = 0, Y = 0, Width = 0, Height = 0;
        bool Maximized = false;
    };
    Placement GetPlacement() const;
    // Applies a saved placement to the (still hidden) window. Returns false, leaving the window
    // untouched, when the rect is degenerate or no longer overlaps any connected monitor (a
    // laptop undocked from the screen it was on) - the caller then falls back to its default.
    bool ApplyPlacement(const Placement& placement);

    // The OS title bar is removed (Win32 custom frame — see Window.cpp). The editor draws its
    // own min/max/close buttons on the top toolbar and reports, once per frame, whether the
    // cursor is over the toolbar's empty area — that region acts as the drag handle
    // (double-click to maximize/restore). No-op off Windows.
    void SetTitleBarDragActive(bool active) { m_TitleBarDragActive = active; }
    bool TitleBarDragActive() const { return m_TitleBarDragActive; }

    // Swap-interval control for the current GL context. mode: 0 = off, 1 = on (sync to
    // refresh), 2 = adaptive (late-swap tear). Adaptive silently degrades to plain vsync on
    // drivers without EXT_swap_control_tear. Safe to call every frame; only hits the driver
    // when the mode actually changes.
    void SetVSync(int mode);
    int VSyncMode() const { return m_VSyncMode; }

    // The window is created hidden and stays hidden until this is called — see the constructor.
    // Call it only after a frame has actually been presented, so it appears already painted
    // instead of flashing an undefined framebuffer.
    void Show();

    // A native modal error dialog. Static, and usable before any Window exists, because the
    // failures it reports are mostly startup ones (no GL context, no window). Release builds
    // have no console for a stderr message to reach, so without this a failed launch would
    // look like the engine silently doing nothing. No-op off Windows.
    static void ShowFatalErrorDialog(const std::string& message);

    // #154 — Unity's FullScreenWindow / ExclusiveFullScreen. Borderless (the default) covers the
    // chosen monitor with the window itself: instant alt-tab, other monitors keep working, no
    // video-mode switch. Exclusive takes over the monitor's video mode. Leaving fullscreen puts
    // the window back exactly as it was (position, size, maximized).
    enum class FullscreenMode { Borderless = 0, Exclusive = 1 };
    // `monitorIndex` indexes glfwGetMonitors(); -1 = the monitor the window is currently on.
    void SetFullscreenOptions(FullscreenMode mode, int monitorIndex) { m_FsMode = mode; m_FsMonitor = monitorIndex; }
    void SetFullscreen(bool fullscreen);
    void ToggleFullscreen() { SetFullscreen(!m_IsFullscreen); }
    bool IsFullscreen() const { return m_IsFullscreen; }
    // Connected monitor names, in glfwGetMonitors() order (for the Preferences picker).
    static std::vector<std::string> MonitorNames();

    // Fires when the OS reports files dropped onto this window (e.g. dragged in from Windows
    // Explorer) — GLFW hands over the full path list in one call, one per actual drop gesture.
    // Only one callback at a time; setting a new one replaces whatever was there before.
    void SetDropCallback(std::function<void(const std::vector<std::string>&)> callback);

private:
    // Undoes everything the constructor acquired. Shared by the destructor and the constructor's
    // own failure path, since a destructor never runs for an object whose constructor threw (#192).
    void ReleaseResources();

    GLFWwindow* m_Handle = nullptr;
    bool m_TimerPeriodRaised = false; // timeBeginPeriod(1) is active and needs its timeEndPeriod
    int m_Width, m_Height;
    bool m_CursorLocked = false;
    bool m_TitleBarDragActive = false; // updated per-frame by the editor; read by the Win32 WndProc

    int m_VSyncMode = 1;
    bool m_IsFullscreen = false;
    FullscreenMode m_FsMode = FullscreenMode::Borderless;
    int m_FsMonitor = -1;
    FullscreenMode m_ActiveFsMode = FullscreenMode::Borderless; // how the current fullscreen was entered
    int m_WindowedX = 0, m_WindowedY = 0, m_WindowedW = 0, m_WindowedH = 0;
    Placement m_PreFullscreen;           // borderless: restored on exit (includes maximized)

    std::function<void(const std::vector<std::string>&)> m_DropCallback;

    static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void DropCallbackTrampoline(GLFWwindow* window, int pathCount, const char* paths[]);
};
