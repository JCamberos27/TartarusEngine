#pragma once
#include <string>
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

    // True fullscreen (takes over the monitor's own video mode), not a borderless-window
    // fake. Toggling back to windowed restores whatever position/size the window had
    // before it went fullscreen.
    void SetFullscreen(bool fullscreen);
    void ToggleFullscreen() { SetFullscreen(!m_IsFullscreen); }
    bool IsFullscreen() const { return m_IsFullscreen; }

private:
    GLFWwindow* m_Handle = nullptr;
    int m_Width, m_Height;
    bool m_CursorLocked = false;

    bool m_IsFullscreen = false;
    int m_WindowedX = 0, m_WindowedY = 0, m_WindowedW = 0, m_WindowedH = 0;

    static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
};
