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
