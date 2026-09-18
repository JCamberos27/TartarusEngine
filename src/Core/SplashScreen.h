#pragma once
#include <string>
#include <thread>

// A borderless logo window shown while the engine loads — the same affordance Unity and Ableton
// use to cover a slow start with something intentional.
//
// Deliberately NOT a second GLFW/OpenGL window. This is a Win32 layered window: its pixels are
// handed to the compositor once via UpdateLayeredWindow and the DWM keeps drawing them. It also
// avoids a second GL context fighting the engine's global function-pointer loader.
//
// The window lives on its own small thread with its own message loop (#155). Loading runs
// synchronously on the main thread and pumps nothing, so a splash owned by that thread stopped
// answering messages during a long load and Windows ghosted it as "Not Responding".
//
// Windows-only, matching the rest of the platform layer; every method is a harmless no-op if
// the image is missing or the window can't be created, so a failed splash never blocks startup.
class SplashScreen {
public:
    ~SplashScreen();

    // Decodes `imagePath` and shows it centred on the primary monitor. `minimumSeconds` is the
    // shortest time the splash will remain up — Close() waits out the remainder, so a fast
    // (warm-cache) start shows a deliberate splash rather than a flicker.
    void Show(const std::string& imagePath, float minimumSeconds = 1.0f);

    // Honours the minimum display time, then tears the window down. Safe to call when Show()
    // failed or was never called.
    void Close();

private:
    // Destroys the window and joins its thread immediately. The destructor uses this rather than
    // Close(): during exception unwinding the fatal-error dialog in main() must appear at once,
    // and only after this TOPMOST window is gone, never behind it (#155).
    void Teardown();

    void* m_Handle = nullptr; // HWND, kept opaque so windows.h stays out of this header
    std::thread m_Thread;     // owns m_Handle and runs its message loop
    double m_ShownAt = 0.0;
    float m_MinimumSeconds = 0.0f;
};
