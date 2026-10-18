#include "GlassMotionHooks.h"

#include "GlassFeedbackMotion.h" // StaggeredHeadline
#include "GlassMotion.h"
#include "GlassTokens.h"

namespace Glass {

namespace {

constexpr ImGuiID kCheckPath = 0x434B5031u; // "CKP1"

struct SpecPair { const MotionSpec* in; const MotionSpec* out; };

SpecPair Specs(int token) {
    switch ((EditorMotionToken)token) {
        case EditorMotionToken::Hover:   return {&HoverIn, &HoverOut};
        case EditorMotionToken::Press:   return {&PressDown, &PressRelease};
        case EditorMotionToken::Check:   return {&CheckDraw, &CheckErase};
        case EditorMotionToken::Focus:   return {&FocusIn, &FocusOut};
        case EditorMotionToken::Chevron: return {&ChevronTurn, &ChevronTurn};
        case EditorMotionToken::Glide:   return {&SelectionGlide, &SelectionGlide};
        case EditorMotionToken::Flash:   return {&RowFlash, &RowFlash};
        case EditorMotionToken::Scroll:  return {&ScrollEase, &ScrollEase};
        case EditorMotionToken::Flip:    return {&CardFlip, &CardFlip};
        case EditorMotionToken::Unfold:  return {&Unfold, &RevealClose};
        case EditorMotionToken::Fly:     return {&ReparentFly, &ReparentFly};
        case EditorMotionToken::Slide:   return {&LogSlide, &LogSlide};
        case EditorMotionToken::Pop:     return {&NodePop, &HoverOut};
        case EditorMotionToken::Sweep:   return {&LineSweep, &LineSweep};
        case EditorMotionToken::Zoom:    return {&FolderZoom, &FolderZoom};
        default:                         return {&HoverIn, &HoverOut};
    }
}

float HookAnimate(unsigned id, float target, float rest, int token) {
    const SpecPair s = Specs(token);
    return Motion().Animate(id, target, rest, *s.in, *s.out);
}
float HookToggle(unsigned id, bool open, bool wasOpen, int token) {
    const SpecPair s = Specs(token);
    return Motion().AnimateToggle(id, open, wasOpen, *s.in, *s.out);
}
float HookHoverGlow(unsigned id, bool hovered) { return Motion().HoverGlow(id, hovered); }
float HookFollow(unsigned id, float target, float previousTarget, int token) {
    return Motion().Follow(id, target, previousTarget, *Specs(token).in);
}
void HookAnimateColor(unsigned id, const float* t4, const float* r4, int token, float* o4) {
    const SpecPair s = Specs(token);
    const ImVec4 v = Motion().AnimateColor(id, ImVec4(t4[0], t4[1], t4[2], t4[3]), ImVec4(r4[0], r4[1], r4[2], r4[3]), *s.in, *s.out);
    o4[0] = v.x; o4[1] = v.y; o4[2] = v.z; o4[3] = v.w;
}
void HookPulse(unsigned id, int token) { Motion().Pulse(id, Specs(token).in->durationMs * 0.001f); }
float HookPulseProgress(unsigned id) { return Motion().PulseProgress(id); }
void HookTriggerWiggle(unsigned id) { Motion().TriggerWiggle(id, InvalidWiggle); }
float HookWiggle(unsigned id) { return Motion().Wiggle(id, InvalidWiggle); }
void HookDrawCheckmark(ImDrawList* dl, float x, float y, float size, float t, unsigned col, float thickness) {
    DrawCheckmark(dl, ImVec2(x, y), size, t, col, thickness);
}
void HookScrollToY(float y) { Motion().ScrollToY(y, ScrollEase); }
void HookScrollToBottom() { Motion().ScrollToBottom(ScrollEase); }
bool HookReduceMotion() { return Motion().ReduceMotion(); }
void HookStaggeredHeadline(unsigned id, const char* text, float x, float y, unsigned col) {
    StaggeredHeadline(id, text, ImVec2(x, y), col);
}

bool HookCategoryOn(int c) {
    return c >= 0 && c < (int)MotionCategory::Count && Motion().CategoryOn((MotionCategory)c);
}
float HookOdometer(unsigned id, ImDrawList* dl, float x, float y, double value, int decimals, unsigned col, const char* suffix) {
    return Motion().Odometer(id, dl, ImVec2(x, y), value, decimals, col, suffix);
}
void HookInertiaTrack(unsigned id, float dx, float dy) { Motion().InertiaTrack(id, ImVec2(dx, dy)); }
void HookInertiaRelease(unsigned id) { Motion().InertiaRelease(id); }
void HookInertiaDelta(unsigned id, float* dx, float* dy) {
    const ImVec2 d = Motion().InertiaDelta(id);
    if (dx) *dx = d.x;
    if (dy) *dy = d.y;
}
void HookDrawSheen(ImDrawList* dl, float x0, float y0, float x1, float y1, unsigned seed, float strength) {
    Motion().DrawSheen(dl, ImVec2(x0, y0), ImVec2(x1, y1), seed, strength);
}
void HookDrawCursorGlow(ImDrawList* dl, float x0, float y0, float x1, float y1, float cx, float cy, float strength) {
    Motion().DrawCursorGlow(dl, ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(cx, cy), strength, 0.0f);
}
float HookRippleDelay(int col, int row, int originCol, int originRow) {
    return ImAnimContext::GridRippleDelay(col, row, 0, 0, originCol, originRow, RippleStepMs, RippleCap);
}
int HookBeginLayer(ImDrawList* dl) { return ImAnimContext::BeginLayer(dl); }
void HookEndLayer(ImDrawList* dl, int start, float px, float py, float sx, float sy, float rot, float tx, float ty, float alpha) {
    LayerTransform t;
    t.pivot = ImVec2(px, py);
    t.scale = ImVec2(sx, sy);
    t.rotation = rot;
    t.translate = ImVec2(tx, ty);
    t.alpha = alpha;
    ImAnimContext::EndLayer(dl, start, t);
}

void EnsureCheckPath() {
    if (ImAnimContext::PathDefined(kCheckPath)) return;
    // A tick in a unit box: down into the elbow, then the long stroke up and out.
    const PathSeg segs[] = {
        {PathSeg::Line, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0.40f, 0.72f)},
        {PathSeg::Line, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0.80f, 0.26f)},
    };
    ImAnimContext::DefinePath(kCheckPath, ImVec2(0.18f, 0.50f), segs, 2);
}

constexpr ImGuiID kPlayPath = 0x504C5931u; // "PLY1"
constexpr ImGuiID kStopPath = 0x53545031u; // "STP1"

void EnsureTransportPaths() {
    if (ImAnimContext::PathDefined(kPlayPath)) return;
    // Both clockwise from the top-left, so the morph turns corner into corner. The triangle's
    // centroid sits a touch right of centre, like the Font Awesome glyph it replaces.
    const PathSeg play[] = {
        {PathSeg::Line, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0.90f, 0.50f)},
        {PathSeg::Line, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0.22f, 0.90f)},
        {PathSeg::Line, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0.22f, 0.10f)},
    };
    ImAnimContext::DefinePath(kPlayPath, ImVec2(0.22f, 0.10f), play, 3);
    const PathSeg stop[] = {
        {PathSeg::Line, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0.86f, 0.14f)},
        {PathSeg::Line, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0.86f, 0.86f)},
        {PathSeg::Line, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0.14f, 0.86f)},
        {PathSeg::Line, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0.14f, 0.14f)},
    };
    ImAnimContext::DefinePath(kStopPath, ImVec2(0.14f, 0.14f), stop, 4);
}

} // namespace

void DrawTransportGlyph(ImDrawList* dl, ImVec2 c, float size, float stopT, ImU32 col) {
    if (!dl) return;
    EnsureTransportPaths();
    stopT = stopT < 0.0f ? 0.0f : (stopT > 1.0f ? 1.0f : stopT);
    constexpr int kSamples = 32;
    ImVec2 pts[kSamples];
    const ImVec2 o(c.x - size * 0.5f, c.y - size * 0.5f);
    for (int i = 0; i < kSamples; ++i) {
        const float t = (float)i / (float)kSamples;
        const ImVec2 p = stopT <= 0.0f ? ImAnimContext::PathPoint(kPlayPath, t)
                       : stopT >= 1.0f ? ImAnimContext::PathPoint(kStopPath, t)
                                       : ImAnimContext::PathMorph(kPlayPath, kStopPath, t, stopT);
        pts[i] = ImVec2(o.x + p.x * size, o.y + p.y * size);
    }
    dl->AddConvexPolyFilled(pts, kSamples, col);
}

void DrawCheckmark(ImDrawList* dl, ImVec2 pos, float size, float t, ImU32 col, float thickness) {
    if (t <= 0.0f || !dl) return;
    EnsureCheckPath();
    t = t > 1.0f ? 1.0f : t;
    // The pen's tip is the path at arc-length fraction t (constant speed along the stroke); behind
    // it, the start and, once passed, the elbow, so the corner stays sharp.
    constexpr float kElbow = 0.338f; // the first stroke's share of the tick's length
    auto at = [&](float u) { const ImVec2 p = ImAnimContext::PathPoint(kCheckPath, u); return ImVec2(pos.x + p.x * size, pos.y + p.y * size); };
    ImVec2 pts[3];
    int n = 0;
    pts[n++] = at(0.0f);
    if (t > kElbow) pts[n++] = at(kElbow);
    pts[n++] = at(t);
    dl->AddPolyline(pts, n, col, ImDrawFlags_None, thickness);
}

const EditorMotionHooks& HostMotionHooks() {
    static const EditorMotionHooks s_Hooks = [] {
        EditorMotionHooks h;
        h.Animate        = &HookAnimate;
        h.Toggle         = &HookToggle;
        h.HoverGlow      = &HookHoverGlow;
        h.Follow         = &HookFollow;
        h.AnimateColor   = &HookAnimateColor;
        h.Pulse          = &HookPulse;
        h.PulseProgress  = &HookPulseProgress;
        h.TriggerWiggle  = &HookTriggerWiggle;
        h.Wiggle         = &HookWiggle;
        h.DrawCheckmark  = &HookDrawCheckmark;
        h.ScrollToY      = &HookScrollToY;
        h.ScrollToBottom = &HookScrollToBottom;
        h.ReduceMotion   = &HookReduceMotion;
        h.StaggeredHeadline = &HookStaggeredHeadline;
        h.CategoryOn     = &HookCategoryOn;
        h.Odometer       = &HookOdometer;
        h.InertiaTrack   = &HookInertiaTrack;
        h.InertiaRelease = &HookInertiaRelease;
        h.InertiaDelta   = &HookInertiaDelta;
        h.DrawSheen      = &HookDrawSheen;
        h.DrawCursorGlow = &HookDrawCursorGlow;
        h.RippleDelay    = &HookRippleDelay;
        h.BeginLayer     = &HookBeginLayer;
        h.EndLayer       = &HookEndLayer;
        return h;
    }();
    return s_Hooks;
}

} // namespace Glass
