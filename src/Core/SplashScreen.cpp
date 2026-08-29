#include "SplashScreen.h"

#if defined(_WIN32)

#include "stb_image.h"

#include <windows.h>
#include <vector>

namespace {

const wchar_t* kClassName = L"TartarusSplash";

LRESULT CALLBACK SplashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

double NowSeconds() {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}

// The splash runs before GLFW initialises, so nothing has made this process DPI-aware yet and
// GetSystemMetrics would report a virtualised desktop (2560x1440 on a 150%-scaled 4K display),
// centring the splash noticeably up and to the left. Opt in early — GLFW sets the same
// awareness moments later and tolerates it already being set.
// Resolved dynamically because SetProcessDpiAwarenessContext needs Windows 10 1703+.
void EnsureDpiAware() {
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setCtx = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        // -4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        if (setCtx && setCtx((HANDLE)-4)) return;
    }
    SetProcessDPIAware(); // pre-1703 fallback: system-wide awareness is enough to centre correctly
}

} // namespace

void SplashScreen::Show(const std::string& imagePath, float minimumSeconds) {
    m_MinimumSeconds = minimumSeconds;
    m_ShownAt = NowSeconds();
    EnsureDpiAware();

    int w = 0, h = 0, channels = 0;
    // Forced to 4 channels: UpdateLayeredWindow only accepts a 32-bit BGRA surface.
    stbi_set_flip_vertically_on_load(false);
    unsigned char* pixels = stbi_load(imagePath.c_str(), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return; // no splash is fine — never let branding block startup
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SplashProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc); // harmless if already registered

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int x = (screenW - w) / 2;
    const int y = (screenH - h) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, // TOOLWINDOW keeps it off the taskbar
        kClassName, L"Tartarus Engine", WS_POPUP,
        x, y, w, h, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        stbi_image_free(pixels);
        return;
    }

    // Win32 DIBs are BGRA and UpdateLayeredWindow expects premultiplied alpha, so convert in
    // place from stb's straight-alpha RGBA.
    std::vector<unsigned char> bgra((size_t)w * h * 4);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        const unsigned char r = pixels[i * 4 + 0];
        const unsigned char g = pixels[i * 4 + 1];
        const unsigned char b = pixels[i * 4 + 2];
        const unsigned char a = pixels[i * 4 + 3];
        bgra[i * 4 + 0] = (unsigned char)(b * a / 255);
        bgra[i * 4 + 1] = (unsigned char)(g * a / 255);
        bgra[i * 4 + 2] = (unsigned char)(r * a / 255);
        bgra[i * 4 + 3] = a;
    }
    stbi_image_free(pixels);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // negative: top-down, matching stb's row order
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        memcpy(bits, bgra.data(), bgra.size());
        HGDIOBJ previous = SelectObject(memDC, bitmap);

        POINT topLeft = {x, y};
        SIZE size = {w, h};
        POINT origin = {0, 0};
        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        // Hands the pixels to the compositor once. Nothing further is required from this
        // thread, so the splash stays on screen through the entire blocking load.
        UpdateLayeredWindow(hwnd, screenDC, &topLeft, &size, memDC, &origin, 0, &blend, ULW_ALPHA);
        ShowWindow(hwnd, SW_SHOWNA); // SHOWNA: don't steal focus from the launching shell

        SelectObject(memDC, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    m_Handle = hwnd;
}

void SplashScreen::Close() {
    if (!m_Handle) return;

    const double elapsed = NowSeconds() - m_ShownAt;
    if (elapsed < (double)m_MinimumSeconds) {
        Sleep((DWORD)(((double)m_MinimumSeconds - elapsed) * 1000.0));
    }
    DestroyWindow((HWND)m_Handle);
    m_Handle = nullptr;
}

SplashScreen::~SplashScreen() { Close(); }

#else // non-Windows: no-ops, so callers need no platform guards of their own

void SplashScreen::Show(const std::string&, float) {}
void SplashScreen::Close() {}
SplashScreen::~SplashScreen() = default;

#endif
