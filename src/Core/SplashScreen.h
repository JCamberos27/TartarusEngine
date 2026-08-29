#pragma once
#include <string>

// A borderless logo window shown while the engine loads — the same affordance Unity and Ableton
// use to cover a slow start with something intentional.
//
// Deliberately NOT a second GLFW/OpenGL window. Loading runs synchronously on the main thread,
// so a splash that needed a message pump or a redraw would freeze mid-load. This is a Win32
// layered window instead: its pixels are handed to the compositor once via UpdateLayeredWindow
// and the DWM keeps drawing them without any further cooperation from this thread, which is
// exactly the behaviour a blocking load needs. It also avoids a second GL context fighting the
// engine's global function-pointer loader.
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
    void* m_Handle = nullptr; // HWND, kept opaque so windows.h stays out of this header
    double m_ShownAt = 0.0;
    float m_MinimumSeconds = 0.0f;
};
