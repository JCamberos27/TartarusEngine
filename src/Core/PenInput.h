#pragma once
// Pen (Wacom / Surface) pressure. On Windows the main window's procedure forwards the Pointer Input
// API's WM_POINTER* messages here (Window.cpp); the message is then passed on untouched, so GLFW
// still sees the pen as a mouse. Everything else (a mouse, other platforms) reports "no pen".
// Tests can inject a synthetic pressure with SetSynthetic().
#include <cstdint>

namespace PenInput {

// Pressure 0..1 of a pen touching the surface right now, or a negative value when there is no pen
// contact (a mouse, a hovering pen, a stale reading): callers fall back to their mouse behaviour.
float Pressure();
// True when a real pen is in contact or injected pressure is set.
bool Active();
// Tests: force a pressure (0..1); a negative value removes the override.
void SetSynthetic(float pressure);
// Feed a reading (0..1 in contact, negative = pen left contact). The Windows hook calls this; tests may too.
void Report(float pressure);

#if defined(_WIN32)
// The WM_POINTERDOWN / UPDATE / UP / LEAVE hook: wParam is the message's WPARAM. Returns nothing:
// the caller always passes the message on.
void OnPointerMessage(unsigned msg, uintptr_t wParam);
#endif

} // namespace PenInput
