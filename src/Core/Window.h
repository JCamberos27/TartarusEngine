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

    // The window is created hidden and stays hidden until this is called — see the constructor.
    // Call it only after a frame has actually been presented, so it appears already painted
    // instead of flashing an undefined framebuffer.
    void Show();

    // A native modal error dialog. Static, and usable before any Window exists, because the
    // failures it reports are mostly startup ones (no GL context, no window). Release builds
    // have no console for a stderr message to reach, so without this a failed launch would
    // look like the engine silently doing nothing. No-op off Windows.
    static void ShowFatalErrorDialog(const std::string& message);

    // True fullscreen (takes over the monitor's own video mode), not a borderless-window
    // fake. Toggling back to windowed restores whatever position/size the window had
    // before it went fullscreen.
    void SetFullscreen(bool fullscreen);
    void ToggleFullscreen() { SetFullscreen(!m_IsFullscreen); }
    bool IsFullscreen() const { return m_IsFullscreen; }

    // Fires when the OS reports files dropped onto this window (e.g. dragged in from Windows
    // Explorer) — GLFW hands over the full path list in one call, one per actual drop gesture.
    // Only one callback at a time; setting a new one replaces whatever was there before.
    void SetDropCallback(std::function<void(const std::vector<std::string>&)> callback);

private:
    GLFWwindow* m_Handle = nullptr;
    int m_Width, m_Height;
    bool m_CursorLocked = false;

    bool m_IsFullscreen = false;
    int m_WindowedX = 0, m_WindowedY = 0, m_WindowedW = 0, m_WindowedH = 0;

    std::function<void(const std::vector<std::string>&)> m_DropCallback;

    static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void DropCallbackTrampoline(GLFWwindow* window, int pathCount, const char* paths[]);
};
