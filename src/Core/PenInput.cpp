#include "PenInput.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace PenInput {
namespace {
std::atomic<float> g_Real{-1.0f};
std::atomic<float> g_Synthetic{-1.0f};
std::atomic<long long> g_ReportedAtMs{0};

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void Report(float pressure) {
    g_Real.store(pressure < 0.0f ? -1.0f : std::clamp(pressure, 0.0f, 1.0f));
    g_ReportedAtMs.store(NowMs());
}

void SetSynthetic(float pressure) { g_Synthetic.store(pressure < 0.0f ? -1.0f : std::clamp(pressure, 0.0f, 1.0f)); }

float Pressure() {
    const float s = g_Synthetic.load();
    if (s >= 0.0f) return s;
    const float r = g_Real.load();
    // A pen held still sends no updates for a moment, but a reading from seconds ago is stale
    // (the pen went out of range without a leave message): drop to mouse behaviour.
    if (r >= 0.0f && NowMs() - g_ReportedAtMs.load() > 1500) return -1.0f;
    return r;
}

bool Active() { return Pressure() >= 0.0f; }

#if defined(_WIN32)
void OnPointerMessage(unsigned msg, uintptr_t wParam) {
    if (msg == WM_POINTERLEAVE || msg == WM_POINTERCAPTURECHANGED) {
        Report(-1.0f);
        return;
    }
    const UINT32 id = GET_POINTERID_WPARAM(wParam);
    POINTER_INPUT_TYPE type = PT_POINTER;
    if (!GetPointerType(id, &type) || type != PT_PEN) return;
    if (msg == WM_POINTERUP) {
        Report(-1.0f);
        return;
    }
    POINTER_PEN_INFO info{};
    if (!GetPointerPenInfo(id, &info)) return;
    if (!IS_POINTER_INCONTACT_WPARAM(wParam)) {
        Report(-1.0f); // hovering: not painting
        return;
    }
    // Pressure is 0..1024 when the digitizer reports it; otherwise treat contact as full.
    const float p = (info.penMask & PEN_MASK_PRESSURE) ? (float)info.pressure / 1024.0f : 1.0f;
    Report(p);
}
#endif

} // namespace PenInput
