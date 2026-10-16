#include "GlassDragMotion.h"

#include "GlassMotion.h"
#include "GlassTheme.h" // Scale
#include "GlassTokens.h"

#include <imgui_internal.h>

#include <cstdio>
#include <cstring>

namespace Glass {

namespace {
constexpr ImGuiID kGhostFollow = 0x47485354u; // "GHST"
constexpr ImGuiID kGhostIn     = 0x47484931u; // "GHI1"
constexpr ImGuiID kGhostFly    = 0x47484631u; // "GHF1"

// The chip's content and where it is. A few bytes of fixed state, not animation state: the motion
// itself lives in Motion() entries that exist only while the chip moves or flies.
struct Ghost {
    char   glyph[8] = {};
    char   label[96] = {};
    int    frame = -1;          // the last frame a source described it
    ImVec2 pos;                 // where it was last drawn (top-left)
    ImVec2 prevTarget;          // last frame's cursor target
    ImVec2 origin;              // where the drag started
    bool   delivered = false;   // a custom zone took the drop
    ImVec2 deliveredAt;
    // The flight after release.
    bool   flying = false;
    bool   flyIn = false;       // true: into a target; false: back home
    ImVec2 from, to;
};
Ghost s_Ghost;

ImVec2 ChipSize(const Ghost& g) {
    const ImVec2 ts = ImGui::CalcTextSize(g.label);
    const ImVec2 gs = g.glyph[0] ? ImGui::CalcTextSize(g.glyph) : ImVec2(0, 0);
    const float pad = Px(8.0f), gap = g.glyph[0] ? Px(6.0f) : 0.0f;
    return ImVec2(pad * 2.0f + gs.x + gap + ts.x, ImGui::GetFrameHeight());
}

void DrawChip(const Ghost& g, ImVec2 pos, float scale, float alpha) {
    if (alpha <= 0.004f) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const ImVec2 size = ChipSize(g);
    const ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    const ImVec2 half(size.x * 0.5f * scale, size.y * 0.5f * scale);
    const ImVec2 mn(c.x - half.x, c.y - half.y), mx(c.x + half.x, c.y + half.y);
    const float r = half.y;
    // A soft shadow, the plate, its hairline.
    for (int i = 3; i >= 1; --i) {
        const float grow = Px(2.0f) * (float)i;
        dl->AddRectFilled(ImVec2(mn.x - grow, mn.y - grow + Px(2.0f)), ImVec2(mx.x + grow, mx.y + grow + Px(2.0f)),
                          IM_COL32(0, 0, 0, (int)(DragGhostShadowAlpha * alpha * 255.0f / (float)(i + 1))), r + grow);
    }
    dl->AddRectFilled(mn, mx, IM_COL32(14, 14, 14, (int)(235.0f * alpha)), r);
    dl->AddRect(mn, mx, IM_COL32(255, 255, 255, (int)(40.0f * alpha)), r, 0, 1.0f);
    if (scale < 0.75f) return; // too small for legible text: the plate alone reads as "landing"
    const float pad = Px(8.0f) * scale;
    float x = mn.x + pad;
    const ImU32 text = IM_COL32(235, 235, 235, (int)(255.0f * alpha));
    const ImU32 dim = IM_COL32(170, 170, 170, (int)(255.0f * alpha));
    const float fs = ImGui::GetFontSize() * scale;
    if (g.glyph[0]) {
        dl->AddText(ImGui::GetFont(), fs, ImVec2(x, c.y - fs * 0.5f), dim, g.glyph);
        x += ImGui::CalcTextSize(g.glyph).x * scale + Px(6.0f) * scale;
    }
    dl->AddText(ImGui::GetFont(), fs, ImVec2(x, c.y - fs * 0.5f), text, g.label);
}
} // namespace

void DragGhostLabel(const char* glyph, const char* label) {
    ImGuiContext& gc = *ImGui::GetCurrentContext();
    Ghost& g = s_Ghost;
    if (g.frame < gc.FrameCount - 1) {
        // A new drag: start from the cursor, fade in, forget any flight still running.
        g.origin = gc.IO.MouseClickedPos[0];
        g.prevTarget = gc.IO.MousePos + ImVec2(Px(DragGhostOffsetPx), Px(DragGhostOffsetPx * 0.5f));
        g.pos = g.prevTarget;
        g.delivered = false;
        g.flying = false;
        Motion().Pulse(kGhostIn, DragGhostIn.durationMs * 0.001f);
    }
    g.frame = gc.FrameCount;
    std::snprintf(g.glyph, sizeof(g.glyph), "%s", glyph ? glyph : "");
    std::snprintf(g.label, sizeof(g.label), "%s", label ? label : "");
}

void DragGhostDelivered(ImVec2 target) {
    s_Ghost.delivered = true;
    s_Ghost.deliveredAt = target;
}

void DrawDragGhost() {
    ImGuiContext& gc = *ImGui::GetCurrentContext();
    Ghost& g = s_Ghost;
    ImAnimContext& m = Motion();

    if (g.frame == gc.FrameCount) {
        // Dragging: trail the cursor.
        const ImVec2 target = gc.IO.MousePos + ImVec2(Px(DragGhostOffsetPx), Px(DragGhostOffsetPx * 0.5f));
        g.pos = m.Follow2(kGhostFollow, target, g.prevTarget, DragGhostFollow);
        g.prevTarget = target;
        const float in = EaseProgress(DragGhostIn, m.PulseProgress(kGhostIn));
        DrawChip(g, g.pos, 0.96f + 0.04f * in, in);
        return;
    }

    if (g.frame == gc.FrameCount - 1 && !g.flying) {
        // Released last frame's drag this frame: where does it go?
        const ImGuiPayload* p = ImGui::GetDragDropPayload();
        const bool delivered = g.delivered || (p && p->Delivery);
        g.flying = true;
        g.flyIn = delivered;
        g.from = g.pos;
        const ImVec2 size = ChipSize(g);
        if (delivered) {
            // Into a small target (a row, a folder tile) at its centre; into a big one (the
            // viewport, a panel's empty space) right where it was let go.
            const ImRect& tr = gc.DragDropTargetRect;
            const bool small = tr.GetWidth() > 0.0f && tr.GetWidth() < Px(320.0f) && tr.GetHeight() < Px(160.0f);
            const ImVec2 at = g.delivered ? g.deliveredAt : (small ? tr.GetCenter() : gc.IO.MousePos);
            g.to = ImVec2(at.x - size.x * 0.5f, at.y - size.y * 0.5f);
        } else {
            g.to = ImVec2(g.origin.x - size.x * 0.5f, g.origin.y - size.y * 0.5f);
        }
        m.Pulse(kGhostFly, (delivered ? DropFly : DropReturn).durationMs * 0.001f);
        g.delivered = false;
    }

    if (!g.flying) return;
    if (!m.PulseActive(kGhostFly)) { g.flying = false; return; }
    const MotionSpec& spec = g.flyIn ? DropFly : DropReturn;
    const float u = EaseProgress(spec, m.PulseProgress(kGhostFly));
    // A short arc: both control points lifted above the straight line, so the chip hops into its
    // target (or home) instead of sliding.
    const ImVec2 d = g.to - g.from;
    const float lift = Px(DropArcLiftPx);
    const ImVec2 c1(g.from.x + d.x * 0.25f, g.from.y + d.y * 0.25f - lift);
    const ImVec2 c2(g.from.x + d.x * 0.75f, g.from.y + d.y * 0.75f - lift);
    const ImVec2 p = ImBezierCubicCalc(g.from, c1, c2, g.to, u);
    const float scale = g.flyIn ? 1.0f - (1.0f - DropFlyToScale) * u : 1.0f;
    const float alpha = g.flyIn ? 1.0f - u : 1.0f - u * u;
    DrawChip(g, p, scale, alpha);
}

} // namespace Glass
