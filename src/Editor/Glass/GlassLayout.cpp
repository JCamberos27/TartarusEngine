#include "GlassLayout.h"

#include "GlassMotion.h"
#include "GlassTheme.h" // Px

#include <cfloat>

namespace Glass {

namespace {
// Salts so a reveal's stagger timer doesn't share an id with its height tween.
constexpr ImGuiID kStaggerSalt = 0x53544147u; // "STAG"

struct RevealFrame {
    ImGuiID id;
    float   t;            // 0..1 openness this frame
    bool    moving;
    bool    clipped;      // a ClipRect was pushed
    bool    alphaPushed;
    ImVec2  start;        // screen pos where the body starts
    ImVec2  savedMaxPos;  // window->DC.CursorMaxPos before the body (restored: hidden rows mustn't grow the scroll range)
    float   staggerElapsed; // seconds into the row cascade, FLT_MAX when not cascading
};
ImVector<RevealFrame> s_Reveals;

bool s_LayoutInMotion = false;     // this frame
bool s_LayoutInMotionNext = false; // carried into the next frame (widgets drawn before the mover)
} // namespace

bool LayoutInMotion() { return s_LayoutInMotion; }
void MarkLayoutInMotion() { s_LayoutInMotion = true; s_LayoutInMotionNext = true; }
void LayoutMotionNewFrame() {
    s_LayoutInMotion = s_LayoutInMotionNext;
    s_LayoutInMotionNext = false;
    if (s_Reveals.Size == 0) s_Reveals.clear(); // nothing nested is open across frames: free it
}

ImGuiButtonFlags MotionButtonFlags(ImGuiButtonFlags base) {
    if ((base & ImGuiButtonFlags_PressedOnMask_) != 0) return base; // caller chose a press policy
    return base | (LayoutInMotion() ? ImGuiButtonFlags_PressedOnClickReleaseAnywhere
                                    : ImGuiButtonFlags_PressedOnClickRelease);
}

bool MotionButton(ImGuiID id, const ImRect& rest, const ImRect& drawn, bool* outHovered, bool* outHeld,
                  ImGuiButtonFlags flags) {
    ImGui::ItemSize(rest.GetSize());
    const ImRect hit = MotionHitRect(rest, drawn);
    if (!ImGui::ItemAdd(hit, id)) {
        if (outHovered) *outHovered = false;
        if (outHeld) *outHeld = false;
        return false;
    }
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(hit, id, &hovered, &held, MotionButtonFlags(flags));
    if (outHovered) *outHovered = hovered;
    if (outHeld) *outHeld = held;
    return pressed;
}

bool BeginReveal(ImGuiID id, bool open, bool wasOpen) {
    ImAnimContext& m = Motion();
    const float t = m.AnimateToggle(id, open, wasOpen, RevealOpen, RevealClose);
    const ImGuiID staggerId = id ^ kStaggerSalt;
    if (open && !wasOpen) {
        const float cascade = StaggerDelay(RevealStaggerCap, RevealStaggerMs, RevealStaggerCap) + RevealRowFadeMs * 0.001f;
        m.Pulse(staggerId, cascade);
    }
    // Once per frame, so the timer is released the frame it completes (PulseProgress removes it).
    const bool cascading = m.PulseProgress(staggerId) < 1.0f;

    if (!open && t <= 0.0f) return false; // closed and at rest: the body costs nothing

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    RevealFrame f;
    f.id = id;
    f.t = t;
    f.moving = m.Live(id); // an entry exists exactly while the openness is in flight
    f.clipped = false;
    f.alphaPushed = false;
    f.start = window->DC.CursorPos;
    f.savedMaxPos = window->DC.CursorMaxPos;
    f.staggerElapsed = cascading ? m.PulseElapsed(staggerId, FLT_MAX) : FLT_MAX;

    if (f.moving) {
        MarkLayoutInMotion();
        // The clip height uses the natural height measured last frame. On the very frame motion
        // starts there's no measurement yet, but then t is exactly where it came from (0 when
        // opening, 1 when closing), so "nothing" or "everything" is the right answer.
        const float natural = m.Aux(id, -1.0f);
        const float clipH = natural >= 0.0f ? natural * t : (t >= 0.5f ? FLT_MAX * 0.25f : 0.0f);
        ImGui::PushClipRect(ImVec2(window->ClipRect.Min.x, f.start.y),
                            ImVec2(window->ClipRect.Max.x, f.start.y + clipH), true);
        f.clipped = true;
        // Content fades in over the later part of an open and out over the early part of a close,
        // so a half-open section reads as "arriving" rather than "cropped".
        const float fade = ImClamp((t - 0.15f) / 0.85f, 0.0f, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * fade);
        f.alphaPushed = true;
    }
    s_Reveals.push_back(f);
    return true;
}

void EndReveal() {
    IM_ASSERT(s_Reveals.Size > 0 && "EndReveal without BeginReveal");
    if (s_Reveals.Size == 0) return;
    const RevealFrame f = s_Reveals.back();
    s_Reveals.pop_back();

    if (f.alphaPushed) ImGui::PopStyleVar();
    if (f.clipped) ImGui::PopClipRect();
    if (!f.moving) return; // at rest and open: laid out normally, nothing to correct

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    // The body was laid out at full size; its natural height is how far the cursor travelled
    // (including the item spacing after its last row, exactly what an un-animated body occupies).
    const float natural = window->DC.CursorPos.y - f.start.y;
    Motion().SetAux(f.id, natural);
    const float animH = natural * f.t;

    // Take back what the hidden part of the body added to the window's content size, then occupy
    // exactly the animated height. ItemSize(0) advances by one ItemSpacing.y, so start that much
    // above the target: the cursor lands on start + animH with no double spacing at either end.
    window->DC.CursorMaxPos = f.savedMaxPos;
    const float sy = ImGui::GetStyle().ItemSpacing.y;
    window->DC.CursorPos = ImVec2(f.start.x, f.start.y + animH - sy);
    window->DC.IsSameLine = false;
    ImGui::ItemSize(ImVec2(0.0f, 0.0f)); // also grows CursorMaxPos to start + animH - spacing, ImGui's convention
}

float CurrentRevealT() { return s_Reveals.Size ? s_Reveals.back().t : 1.0f; }
bool CurrentRevealMoving() { return s_Reveals.Size ? s_Reveals.back().moving : false; }

RevealRow::RevealRow(int index) {
    if (s_Reveals.Size == 0) return;
    const RevealFrame& f = s_Reveals.back();
    if (f.staggerElapsed == FLT_MAX) return;
    const float delay = StaggerDelay(index, RevealStaggerMs, RevealStaggerCap);
    const float p = EaseProgress(HoverIn /* out-cubic */, (f.staggerElapsed - delay) / (RevealRowFadeMs * 0.001f));
    if (p >= 1.0f) return;
    m_Shift = Px(RevealRowShiftPx) * (1.0f - p);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + m_Shift);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * p);
    m_PushedAlpha = true;
}

RevealRow::~RevealRow() {
    if (m_PushedAlpha) ImGui::PopStyleVar();
    // Take the shift back out of the layout directly. SetCursorPosY would flag the move as a
    // SetCursorPos with no item after it: the shift is smaller than ItemSpacing, so the cursor
    // stays past CursorMaxPos and ImGui asserts if the row was the last thing in its window,
    // group or table.
    if (m_Shift != 0.0f) ImGui::GetCurrentWindow()->DC.CursorPos.y -= m_Shift;
}

} // namespace Glass
