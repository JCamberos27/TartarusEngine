#include "GlassTheme.h"

#include "EditorTheme.h" // Scale(): the UI scale recovered from the shared font

#include <imgui_internal.h> // ShadeVertsLinearUV, ImClamp

#include <array>
#include <cmath>

namespace Glass {

float Scale() { return EditorTheme::Scale(); }
float Px(float designPx) { return designPx * Scale(); }

// --- Squircle ------------------------------------------------------------------------------------
// One corner of a superellipse |x/r|^n + |y/r|^n = 1, traced from angle a0 to a1 (radians, standard
// orientation) around `centre`. The parametric form x = r * sgn(cos t) |cos t|^(2/n) keeps the
// points evenly spread in angle, which is where the eye looks for smoothness.
constexpr int kSquircleMinSegments = 4, kSquircleMaxSegments = 12;
// (|cos t|^(2/n), |sin t|^(2/n)) for t = i/seg * pi/2, i = 0..seg: one quarter, built once per segment
// count (the pow calls were per vertex, per squircle, per frame).
static const ImVec2* SquircleQuarter(int segments) {
    static const auto table = [] {
        std::array<std::array<ImVec2, kSquircleMaxSegments + 1>, kSquircleMaxSegments + 1> t{};
        const float e = 2.0f / SquircleExponent;
        for (int seg = kSquircleMinSegments; seg <= kSquircleMaxSegments; ++seg)
            for (int i = 0; i <= seg; ++i) {
                const float a = 1.57079632679f * ((float)i / (float)seg);
                t[seg][i] = ImVec2(std::pow(std::fabs(std::cos(a)), e), std::pow(std::fabs(std::sin(a)), e));
            }
        return t;
    }();
    return table[ImClamp(segments, kSquircleMinSegments, kSquircleMaxSegments)].data();
}
// The corner of quadrant q (angles q*pi/2 .. (q+1)*pi/2; 0 = bottom-right in screen space).
static void PathSuperellipseCorner(ImDrawList* dl, ImVec2 centre, float r, int q, int segments) {
    segments = ImClamp(segments, kSquircleMinSegments, kSquircleMaxSegments);
    const ImVec2* u = SquircleQuarter(segments);
    const float sx = (q == 1 || q == 2) ? -r : r, sy = q >= 2 ? -r : r;
    const bool swap = q & 1; // cos(q*pi/2 + a) is +-sin(a) in the odd quadrants
    for (int i = 0; i <= segments; ++i) {
        const ImVec2 m = swap ? ImVec2(u[i].y, u[i].x) : u[i];
        dl->PathLineTo(ImVec2(centre.x + sx * m.x, centre.y + sy * m.y));
    }
}

void PathSquircle(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius) {
    const float w = max.x - min.x, h = max.y - min.y;
    float r = radius * SquircleRadiusScale;
    r = ImMin(r, ImMin(w, h) * 0.5f);
    if (r < 0.5f) {
        dl->PathRect(min, max);
        return;
    }
    // Segment count follows the radius so a 4 px row corner isn't 12 points and a 24 px card corner
    // isn't 4. Same tessellation tolerance spirit as ImGui's own circle cache.
    const int seg = (int)ImClamp(r * 0.45f, 4.0f, 12.0f);
    // Clockwise in screen space (y down) from the top-left corner, matching PathRect's winding so
    // anti-aliased fills and strokes come out the same way.
    PathSuperellipseCorner(dl, ImVec2(min.x + r, min.y + r), r, 2, seg);
    PathSuperellipseCorner(dl, ImVec2(max.x - r, min.y + r), r, 3, seg);
    PathSuperellipseCorner(dl, ImVec2(max.x - r, max.y - r), r, 0, seg);
    PathSuperellipseCorner(dl, ImVec2(min.x + r, max.y - r), r, 1, seg);
}

void DrawSquircle(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float radius) {
    if ((col & IM_COL32_A_MASK) == 0) return;
    PathSquircle(dl, min, max, radius);
    dl->PathFillConvex(col);
}

void DrawSquircleBorder(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float radius, float thickness) {
    if ((col & IM_COL32_A_MASK) == 0) return;
    // Inset by half the stroke so the line sits inside the shape, like a CSS inner border.
    const float h = thickness * 0.5f;
    PathSquircle(dl, ImVec2(min.x + h, min.y + h), ImVec2(max.x - h, max.y - h), ImMax(0.0f, radius - h));
    dl->PathStroke(col, thickness, ImDrawFlags_Closed);
}

void DrawSoftShadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius, float alpha, float spreadPx) {
    if (alpha <= 0.0f || spreadPx <= 0.0f) return;
    // Four rings, each larger and fainter: a cheap stand-in for a Gaussian shadow that needs no
    // texture. Offset down by a third of the spread, the way a light overhead casts it.
    constexpr int kRings = 4;
    const float dy = spreadPx * 0.33f;
    for (int i = kRings; i >= 1; --i) {
        const float k = (float)i / (float)kRings;
        const float grow = spreadPx * k;
        const float a = alpha * (1.0f - k) * 0.5f + alpha * 0.08f;
        DrawSquircle(dl, ImVec2(min.x - grow, min.y - grow + dy), ImVec2(max.x + grow, max.y + grow + dy),
                     U32(WithAlpha(Shadow, a * Shadow.w)), radius + grow);
    }
}

void DrawFrostedPlate(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius, ImTextureID blurTex,
                      ImVec2 vpMin, ImVec2 vpMax, float alpha, int* blurVtx) {
    if (blurVtx) blurVtx[0] = blurVtx[1] = 0;
    if (alpha <= 0.0f) return;
    const bool blurred = blurTex != (ImTextureID)0 && vpMax.x > vpMin.x && vpMax.y > vpMin.y;
    if (blurred) {
        // The blurred scene, sampled under exactly this rect: the plate's corners map to the same
        // spot of the blurred texture the sharp scene image shows there, so the glass lines up
        // with the 3D view behind it to the pixel. v runs 1 -> 0 (GL textures are bottom-up),
        // matching how the Scene window shows the sharp image.
        const ImVec2 uvMin((min.x - vpMin.x) / (vpMax.x - vpMin.x), 1.0f - (min.y - vpMin.y) / (vpMax.y - vpMin.y));
        const ImVec2 uvMax((max.x - vpMin.x) / (vpMax.x - vpMin.x), 1.0f - (max.y - vpMin.y) / (vpMax.y - vpMin.y));
        dl->PushTexture(ImTextureRef(blurTex));
        const int vtxStart = dl->VtxBuffer.Size;
        PathSquircle(dl, min, max, radius);
        dl->PathFillConvex(IM_COL32(255, 255, 255, (int)(255.0f * alpha)));
        ImGui::ShadeVertsLinearUV(dl, vtxStart, dl->VtxBuffer.Size, min, max, uvMin, uvMax, true);
        if (blurVtx) { blurVtx[0] = vtxStart; blurVtx[1] = dl->VtxBuffer.Size; }
        dl->PopTexture();
    }
    // The chrome's tint over the blur (the same glass as the action bar and tool rail). Without a
    // blur the plate is the opaque fallback instead, so its text stays legible over anything.
    const ImVec4 tint = blurred ? OverlayTint : GlassFallback;
    DrawSquircle(dl, min, max, U32(WithAlpha(tint, tint.w * alpha)), radius);
    // The 1 px edge of 15% white.
    DrawSquircleBorder(dl, min, max, U32(WithAlpha(GlassEdge, GlassEdge.w * alpha)), radius, 1.0f);
}

void DrawWallpaper() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 mn = vp->Pos, mx(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    ImGui::GetBackgroundDrawList(vp)->AddRectFilled(mn, mx, U32(Base));
}

} // namespace Glass

void ApplyGlassMaterials(ImGuiStyle& style) {
    using namespace Glass;
    ImVec4* c = style.Colors;
    const ImVec4 clear = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    const ImVec4 panel = Rgb(0x00, 0x00, 0x00, GlassPanelAlpha);
    const ImVec4 chrome = Rgb(0x00, 0x00, 0x00, GlassChromeAlpha);
    auto white = [](float a) { return Rgb(0xFF, 0xFF, 0xFF, a); };

    // Where you work: black glass over the canvas.
    c[ImGuiCol_WindowBg]            = panel;
    c[ImGuiCol_ChildBg]             = clear;
    c[ImGuiCol_MenuBarBg]           = chrome;
    c[ImGuiCol_DockingEmptyBg]      = clear;
    // A docked node's tab strip is drawn in the title colours; on a floating window the compositor
    // removes these and lays the blur underneath instead.
    c[ImGuiCol_TitleBg]             = chrome;
    c[ImGuiCol_TitleBgActive]       = chrome;
    c[ImGuiCol_TitleBgCollapsed]    = chrome;
    // What floats: drawn solid here, glass once the compositor runs (GlassFallback otherwise).
    c[ImGuiCol_PopupBg]             = PopupSolid;

    auto black = [](float a) { return Rgb(0x00, 0x00, 0x00, a); };
    // Tabs, headers and selections are dark glass: they stand apart from the lighter panel and the
    // white text on them gets the most contrast. Hover lifts with white, as everywhere.
    c[ImGuiCol_Tab]                 = clear;
    c[ImGuiCol_TabHovered]          = white(0.08f);
    c[ImGuiCol_TabSelected]         = black(0.85f);
    c[ImGuiCol_TabDimmed]           = clear;
    c[ImGuiCol_TabDimmedSelected]   = black(0.70f);
    c[ImGuiCol_Header]              = black(0.75f); // a selected row
    c[ImGuiCol_HeaderHovered]       = white(0.07f);
    c[ImGuiCol_HeaderActive]        = black(0.85f);
    c[ImGuiCol_TableHeaderBg]       = black(0.75f);

    // Fields sit in the glass as dark insets; buttons are dark glass that lifts under the pointer.
    c[ImGuiCol_FrameBg]             = black(0.55f);
    c[ImGuiCol_FrameBgHovered]      = black(0.65f);
    c[ImGuiCol_FrameBgActive]       = black(0.75f);
    c[ImGuiCol_Button]              = black(0.70f);
    c[ImGuiCol_ButtonHovered]       = white(0.10f);
    c[ImGuiCol_ButtonActive]        = black(0.85f);
    c[ImGuiCol_TableBorderLight]    = white(0.050f);

    // Hints and placeholders a step brighter than Tartarus's Dim: glass sits lighter than black.
    c[ImGuiCol_TextDisabled]        = Rgb(0x86, 0x88, 0x86);

    // Thin, quiet edges.
    c[ImGuiCol_Border]              = white(0.10f);
    c[ImGuiCol_Separator]           = white(0.08f);
    c[ImGuiCol_TableBorderStrong]   = white(0.10f);

    style.WindowRounding = GlassWindowRounding; // floating windows only: docked ones are never rounded
    style.PopupRounding  = GlassPopupRounding;
}
