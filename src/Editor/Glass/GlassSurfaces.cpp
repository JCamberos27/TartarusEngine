// GlassCompositor's CPU side (GlassCompositor.h, "Pure helpers"): reading a window's background back
// from its draw list, turning its solid look into glass, and the arrival pose. No GL, so the unit
// tests link it without a GPU.
#include "GlassCompositor.h"

#include <cfloat>
#include <cmath>

namespace Glass {

ImU32 FindWindowBackground(const ImDrawList* dl, const ImRect& r) {
    if (!dl || dl->VtxBuffer.Size == 0 || dl->IdxBuffer.Size == 0) return 0;
    for (const ImDrawCmd& cmd : dl->CmdBuffer) {
        if (cmd.UserCallback != nullptr) return 0; // something custom came first: not a background
        if (cmd.ElemCount == 0) continue;
        const unsigned int vi = cmd.VtxOffset + (unsigned int)dl->IdxBuffer[(int)cmd.IdxOffset];
        if ((int)vi >= dl->VtxBuffer.Size) return 0;
        const ImDrawVert& v = dl->VtxBuffer[(int)vi];
        // An anti-aliased fill's first vertex is the inner ring of its first path point: for a
        // window background that's on the left edge (0.5 px in), at or below the title bar.
        if ((v.col & IM_COL32_A_MASK) == 0) return 0;
        if (std::fabs(v.pos.x - r.Min.x) > 2.0f) return 0;
        if (v.pos.y < r.Min.y - 2.0f || v.pos.y > r.Max.y + 2.0f) return 0;
        return v.col;
    }
    return 0;
}

int ConvertToGlass(ImDrawList* dl, const ImU32* clear, int clearCount, ImU32 border, ImU32 edge) {
    if (!dl) return 0;
    constexpr ImU32 kRgb = ~IM_COL32_A_MASK;
    const ImU32 borderRgb = border & kRgb;
    const ImU32 edgeRgb = edge & kRgb;
    const unsigned int edgeA = (edge & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT;
    int cleared = 0;
    for (ImDrawVert& v : dl->VtxBuffer) {
        const ImU32 c = v.col;
        if ((c & IM_COL32_A_MASK) == 0) continue; // AA fringes are already transparent
        bool hit = false;
        for (int i = 0; i < clearCount && !hit; ++i) hit = clear[i] != 0 && c == clear[i];
        if (hit) {
            v.col = c & kRgb;
            ++cleared;
        } else if (border != 0 && (c & kRgb) == borderRgb) {
            const unsigned int a = (c & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT;
            v.col = edgeRgb | (((a * edgeA + 127u) / 255u) << IM_COL32_A_SHIFT);
        }
    }
    return cleared;
}

int GlassifySolids(ImDrawList* dl) {
    if (!dl) return 0;
    int changed = 0;
    for (ImDrawVert& v : dl->VtxBuffer) {
        const ImU32 c = v.col;
        if (((c & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT) != 255u) continue;
        const int r = (int)((c >> IM_COL32_R_SHIFT) & 0xFF);
        const int g = (int)((c >> IM_COL32_G_SHIFT) & 0xFF);
        const int b = (int)((c >> IM_COL32_B_SHIFT) & 0xFF);
        const int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if (hi - lo > 3) continue; // has a hue: content, status or axis colour
        if (hi >= GreyTextMin && hi <= GreyTextMax) {
            // Grey text and icons (Secondary, Dim): lifted so they read over a bright field.
            const int lift = LiftGreyText(hi) - hi;
            v.col = IM_COL32(r + lift, g + lift, b + lift, 255);
            ++changed;
            continue;
        }
        if (hi > 0x2C) continue;
        // Black fields and canvases: a light inset veil. Raised surfaces (component headers,
        // buttons, cards): dark glass, so they stand apart from the lighter panel. Hover: a white
        // lift. Pressed and hairlines: a brighter white edge.
        if (hi <= 0x06)      v.col = IM_COL32(0, 0, 0, 77);
        else if (hi <= 0x14) v.col = IM_COL32(0, 0, 0, 204);
        else if (hi <= 0x20) v.col = IM_COL32(255, 255, 255, 20);
        else                 v.col = IM_COL32(255, 255, 255, 36);
        ++changed;
    }
    return changed;
}

void ApplyPose(ImDrawList* dl, const SurfacePose& pose, ImU32 anchored) {
    if (!dl || pose.Identity()) return;
    const float a = pose.alpha < 0.0f ? 0.0f : (pose.alpha > 1.0f ? 1.0f : pose.alpha);
    for (ImDrawVert& v : dl->VtxBuffer) {
        if (anchored != 0 && v.col == anchored) continue;
        v.pos = pose.Apply(v.pos);
        if (a < 1.0f) {
            const unsigned int src = (v.col & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT;
            v.col = (v.col & ~IM_COL32_A_MASK) | ((ImU32)((float)src * a + 0.5f) << IM_COL32_A_SHIFT);
        }
    }
    if (!pose.moveClip) return;
    for (ImDrawCmd& cmd : dl->CmdBuffer) {
        const ImVec2 mn = pose.Apply(ImVec2(cmd.ClipRect.x, cmd.ClipRect.y));
        const ImVec2 mx = pose.Apply(ImVec2(cmd.ClipRect.z, cmd.ClipRect.w));
        cmd.ClipRect = ImVec4(mn.x, mn.y, mx.x, mx.y);
    }
}

ImRect DrawListBounds(const ImDrawList* dl) {
    ImRect r(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    if (!dl) return r;
    for (const ImDrawCmd& cmd : dl->CmdBuffer) {
        if (cmd.ElemCount == 0 && cmd.UserCallback == nullptr) continue;
        r.Add(ImRect(cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w));
    }
    return r;
}

} // namespace Glass
