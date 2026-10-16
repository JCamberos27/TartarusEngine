#pragma once
// The motion bridge: how code that can't include the Glass headers (the reloadable editor module,
// the shared EditorUIPrimitives widgets) reaches the host's Glass::Motion() runtime.
//
// The host fills one EditorMotionHooks table (Glass/GlassMotionHooks.cpp) and installs it on its own
// side with EditorTheme::SetMotionHooks; the module receives the same table through
// EditorModuleHostAPI::Motion (API v41) and installs it on its side every Draw. Every caller goes
// through the EditorTheme::Motion* helpers, which fall back to EditorTheme::AnimT when no table is
// installed (unit tests, a module loaded against an older host), so nothing ever stops animating
// or crashes because the bridge is missing.
//
// Plain C types only: no Glass type crosses the module boundary. A motion is named by a token and
// the host maps each token to its MotionSpec pair (GlassTokens.h), so the timing lives in one place.

struct ImDrawList;

enum class EditorMotionToken : int {
    Hover = 0,   // HoverIn / HoverOut: a row or tile wash
    Press,       // PressDown / PressRelease: a button squash and spring back
    Check,       // CheckDraw: a checkbox tick drawing on, and its fill
    Focus,       // FocusIn / FocusOut: an input's focus ring
    Chevron,     // ChevronTurn: a foldout's chevron rotating
    Glide,       // SelectionGlide: a selection sliding between items
    Flash,       // RowFlash: a one-off wash that fades (a new row, a new error)
    Scroll,      // ScrollEase: an eased scroll jump
    Count
};

struct EditorMotionHooks {
    // A float easing to `target`; `rest` is its value when idle (see Glass::ImAnimContext::Animate).
    float (*Animate)(unsigned id, float target, float rest, int token) = nullptr;
    // 0..1 openness around a flip (`open` now, `wasOpen` last frame).
    float (*Toggle)(unsigned id, bool open, bool wasOpen, int token) = nullptr;
    // The row hover wash, behind the pointer-velocity gate.
    float (*HoverGlow)(unsigned id, bool hovered) = nullptr;
    // A glide to a moving target; `previousTarget` is last frame's target.
    float (*Follow)(unsigned id, float target, float previousTarget, int token) = nullptr;
    // An OKLAB colour tween. Colours are RGBA floats.
    void (*AnimateColor)(unsigned id, const float* target4, const float* rest4, int token, float* out4) = nullptr;
    // One-shot timers: Pulse starts one for the token's duration, PulseProgress reads 0..1 (1 when
    // finished or never started).
    void (*Pulse)(unsigned id, int token) = nullptr;
    float (*PulseProgress)(unsigned id) = nullptr;
    // The invalid-input wiggle: start it, then read its horizontal offset in px (0 when idle).
    void (*TriggerWiggle)(unsigned id) = nullptr;
    float (*Wiggle)(unsigned id) = nullptr;
    // A checkmark drawn on along its path to `t` (0..1) inside the square at (x, y) of side `size`.
    void (*DrawCheckmark)(ImDrawList* dl, float x, float y, float size, float t, unsigned col, float thickness) = nullptr;
    // Ease the current window's scroll.
    void (*ScrollToY)(float y) = nullptr;
    void (*ScrollToBottom)() = nullptr;
    // Preferences > Reduce Motion.
    bool (*ReduceMotion)() = nullptr;
    // A headline whose characters fade and rise in when it first appears (static after that), in
    // the current font, into the current window's draw list at (x, y), top-left.
    void (*StaggeredHeadline)(unsigned id, const char* text, float x, float y, unsigned col) = nullptr;
};
