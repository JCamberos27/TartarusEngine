#include "SplashScreen.h"

#if defined(_WIN32)

#include "stb_image.h"

#include <windows.h>
#include <vector>
#include <algorithm>
#include <cmath>

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

    int srcW = 0, srcH = 0, channels = 0;
    // Forced to 4 channels: UpdateLayeredWindow only accepts a 32-bit BGRA surface.
    stbi_set_flip_vertically_on_load(false);
    unsigned char* pixels = stbi_load(imagePath.c_str(), &srcW, &srcH, &channels, 4);
    if (!pixels || srcW <= 0 || srcH <= 0) {
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

    // Size the splash to a fixed fraction of the monitor instead of blitting the PNG 1:1 — a
    // 720px image is a modest 19% of a 4K width but 38% of 1080p, so a raw blit visibly grows
    // when the project moves to a smaller display. Target ~20% of the screen width, never
    // upscaling the source more than 1.5x (raster softens past that) and never below a floor.
    const double kTargetFrac = 0.20;
    int w = (int)std::lround(std::clamp((double)screenW * kTargetFrac,
                                        260.0, (double)srcW * 1.5));
    int h = (int)std::lround((double)w * srcH / srcW);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

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

    // Resample to the target size and, in the same pass, key the PNG's flat black background out
    // to transparent (the source is RGB with no alpha) so the mark floats on the desktop rather
    // than sitting in a black rectangle. Luminance below `kBlackLo` is fully cut; the ramp up to
    // `kBlackHi` keeps the mark's own anti-aliased edges soft. Output is premultiplied BGRA,
    // which is what UpdateLayeredWindow's ULW_ALPHA path expects.
    const float kBlackLo = 8.0f, kBlackHi = 64.0f;
    std::vector<unsigned char> bgra((size_t)w * h * 4);
    for (int dy = 0; dy < h; ++dy) {
        // Bilinear sample position in source space (pixel-center mapping).
        float fy = (dy + 0.5f) * srcH / h - 0.5f;
        int y0 = (int)std::floor(fy);
        float wy = fy - y0;
        int y0c = std::clamp(y0, 0, srcH - 1);
        int y1c = std::clamp(y0 + 1, 0, srcH - 1);
        for (int dx = 0; dx < w; ++dx) {
            float fx = (dx + 0.5f) * srcW / w - 0.5f;
            int x0 = (int)std::floor(fx);
            float wx = fx - x0;
            int x0c = std::clamp(x0, 0, srcW - 1);
            int x1c = std::clamp(x0 + 1, 0, srcW - 1);

            auto tap = [&](int px, int py, int ch) -> float {
                return pixels[((size_t)py * srcW + px) * 4 + ch];
            };
            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                float top = tap(x0c, y0c, c) * (1 - wx) + tap(x1c, y0c, c) * wx;
                float bot = tap(x0c, y1c, c) * (1 - wx) + tap(x1c, y1c, c) * wx;
                rgb[c] = top * (1 - wy) + bot * wy;
            }
            float lum = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
            float t = std::clamp((lum - kBlackLo) / (kBlackHi - kBlackLo), 0.0f, 1.0f);
            float alpha = t * t * (3.0f - 2.0f * t); // smoothstep
            unsigned char a = (unsigned char)std::lround(alpha * 255.0f);

            size_t o = ((size_t)dy * w + dx) * 4;
            bgra[o + 0] = (unsigned char)std::lround(rgb[2] * alpha); // B, premultiplied
            bgra[o + 1] = (unsigned char)std::lround(rgb[1] * alpha); // G
            bgra[o + 2] = (unsigned char)std::lround(rgb[0] * alpha); // R
            bgra[o + 3] = a;
        }
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
