#include "GlassTheme.h"

#include "EditorTheme.h" // Scale(): the UI scale recovered from the shared font

#include <imgui_internal.h> // ShadeVertsLinearUV, ImClamp

#include <cmath>

namespace Glass {

float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float c) {
    c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

ImVec4 LinearCorrectOver(ImVec4 tint, float opacity, ImVec4 backdrop) {
    if (opacity <= 0.0f) return ImVec4(tint.x, tint.y, tint.z, 0.0f);
    if (opacity >= 1.0f) return ImVec4(tint.x, tint.y, tint.z, 1.0f);
    const float a = opacity;
    auto channel = [a](float c, float b) {
        // The encoded value the linear-light composite produces...
        const float target = LinearToSrgb(a * SrgbToLinear(c) + (1.0f - a) * SrgbToLinear(b));
        // ...and the source value whose encoded-space blend reaches it: a*x + (1-a)*b = target.
        const float x = (target - (1.0f - a) * b) / a;
        return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    };
    return ImVec4(channel(tint.x, backdrop.x), channel(tint.y, backdrop.y), channel(tint.z, backdrop.z), a);
}

ImVec4 Resolve(const GlassSurface& s) { return LinearCorrectOver(s.tint, s.opacity, Wallpaper); }

ImVec4 CompositeOnWallpaper(const GlassSurface& s) {
    const ImVec4 c = Resolve(s);
    const float a = c.w;
    return ImVec4(Wallpaper.x + (c.x - Wallpaper.x) * a, Wallpaper.y + (c.y - Wallpaper.y) * a,
                  Wallpaper.z + (c.z - Wallpaper.z) * a, 1.0f);
}

float Scale() { return EditorTheme::Scale(); }
float Px(float designPx) { return designPx * Scale(); }

// --- Squircle ------------------------------------------------------------------------------------
// One corner of a superellipse |x/r|^n + |y/r|^n = 1, traced from angle a0 to a1 (radians, standard
// orientation) around `centre`. The parametric form x = r * sgn(cos t) |cos t|^(2/n) keeps the
// points evenly spread in angle, which is where the eye looks for smoothness.
static void PathSuperellipseCorner(ImDrawList* dl, ImVec2 centre, float r, float a0, float a1, int segments) {
    const float e = 2.0f / SquircleExponent;
    for (int i = 0; i <= segments; ++i) {
        const float t = a0 + (a1 - a0) * ((float)i / (float)segments);
        const float c = std::cos(t), s = std::sin(t);
        const float x = r * (c < 0.0f ? -1.0f : 1.0f) * std::pow(std::fabs(c), e);
        const float y = r * (s < 0.0f ? -1.0f : 1.0f) * std::pow(std::fabs(s), e);
        dl->PathLineTo(ImVec2(centre.x + x, centre.y + y));
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
    const float pi = 3.14159265358979f;
    // Clockwise in screen space (y down) from the top-left corner, matching PathRect's winding so
    // anti-aliased fills and strokes come out the same way.
    PathSuperellipseCorner(dl, ImVec2(min.x + r, min.y + r), r, pi, pi * 1.5f, seg);
    PathSuperellipseCorner(dl, ImVec2(max.x - r, min.y + r), r, pi * 1.5f, pi * 2.0f, seg);
    PathSuperellipseCorner(dl, ImVec2(max.x - r, max.y - r), r, 0.0f, pi * 0.5f, seg);
    PathSuperellipseCorner(dl, ImVec2(min.x + r, max.y - r), r, pi * 0.5f, pi, seg);
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
                      ImVec2 vpMin, ImVec2 vpMax, float alpha) {
    if (alpha <= 0.0f) return;
    DrawSoftShadow(dl, min, max, radius, 0.55f * alpha, Px(Space2));
    if (blurTex != (ImTextureID)0 && vpMax.x > vpMin.x && vpMax.y > vpMin.y) {
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
        dl->PopTexture();
    }
    // The tint sits on the blur, so the plate keeps its value whatever the scene does behind it.
    ImVec4 tint = Resolve(SurfaceHud);
    tint.w *= alpha;
    DrawSquircle(dl, min, max, U32(tint), radius);
    // A rim lit from above: the top edge brighter than the sides, as if the glass caught light.
    DrawSquircleBorder(dl, min, max, U32(WithAlpha(GlassEdge, GlassEdge.w * alpha)), radius, 1.0f);
    dl->AddLine(ImVec2(min.x + radius, min.y + 1.0f), ImVec2(max.x - radius, min.y + 1.0f),
                U32(WithAlpha(TextPrimary, 0.10f * alpha)), 1.0f);
}

void DrawWallpaper() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* bg = ImGui::GetBackgroundDrawList(vp);
    const ImU32 top = U32(WallpaperTop), bottom = U32(WallpaperBottom);
    bg->AddRectFilledMultiColor(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), top, top, bottom, bottom);
}

} // namespace Glass

// =================================================================================================
// The theme
// =================================================================================================
void ConfigureMacEngineTheme(ImGuiStyle& style) {
    using namespace Glass;

    // --- Metrics: the 8 px law (raw 1x values; the caller runs ScaleAllSizes once) --------------
    style.Alpha                    = 1.0f;
    style.DisabledAlpha            = 0.42f;
    style.WindowPadding            = ImVec2(Space2, Space2);      // 16, 16: nothing crowds a window edge
    style.WindowRounding           = RadiusWindow;                // floating windows
    style.WindowBorderSize         = 1.0f;                        // a light hairline rim, not a dark outline
    style.WindowMinSize            = ImVec2(U * 12.0f, U * 8.0f);
    style.WindowTitleAlign         = ImVec2(0.5f, 0.5f);          // centred titles, the macOS way
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding            = RadiusCard;
    style.ChildBorderSize          = 0.0f;                        // a child is layout, not a card
    style.PopupRounding            = RadiusWindow;
    style.PopupBorderSize          = 1.0f;
    style.FramePadding             = ImVec2(Space1h, Space1);     // 12, 8: generous, 31 px frames at 1x
    style.FrameRounding            = RadiusControl;               // input boxes, buttons, combos
    style.FrameBorderSize          = 0.0f;                        // fields are fills; borders are for glass
    style.ItemSpacing              = ImVec2(Space1, Space1);      // 8, 8
    style.ItemInnerSpacing         = ImVec2(Space1, SpaceHalf);   // 8, 4: icon to label (intra-control)
    style.CellPadding              = ImVec2(Space1, SpaceHalf);   // 8, 4: table cells
    style.TouchExtraPadding        = ImVec2(0.0f, 0.0f);
    style.IndentSpacing            = Space2;                      // 16: one tree level
    style.ColumnsMinSpacing        = Space1;
    style.ScrollbarSize            = Space1 + SpaceHalf;          // 12: a slim overlay-style track
    style.ScrollbarRounding        = RadiusControl;               // pill
    style.ScrollbarPadding         = 2.0f;
    style.GrabMinSize              = Space1 + SpaceHalf;          // 12
    style.GrabRounding             = RadiusGrab;
    style.LogSliderDeadzone        = 4.0f;
    style.ImageRounding            = RadiusRow;
    style.ImageBorderSize          = 0.0f;
    style.TabRounding              = RadiusControl;
    style.TabBorderSize            = 0.0f;
    style.TabBarBorderSize         = 0.0f;                        // no rule under the strip: glass floats
    style.TabBarOverlineSize       = 2.0f;                        // the accent underline/overline
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;                // close glyph on hover only
    style.TreeLinesFlags           = ImGuiTreeNodeFlags_DrawLinesNone;
    style.TreeLinesSize            = 1.0f;
    style.TreeLinesRounding        = 0.0f;
    style.MenuItemRounding         = RadiusRow;
    style.SelectableRounding       = RadiusRow;
    style.DragDropTargetRounding   = RadiusControl;
    style.DragDropTargetBorderSize = 2.0f;
    style.DragDropTargetPadding    = SpaceHalf;
    style.ButtonTextAlign          = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign      = ImVec2(0.0f, 0.5f);
    style.SeparatorSize            = 1.0f;
    style.SeparatorTextBorderSize  = 1.0f;
    style.SeparatorTextAlign       = ImVec2(0.0f, 0.5f);
    style.SeparatorTextPadding     = ImVec2(Space2, SpaceHalf);
    style.DisplayWindowPadding     = ImVec2(Space3, Space3);
    style.DisplaySafeAreaPadding   = ImVec2(SpaceHalf, SpaceHalf);
    style.DockingSeparatorSize     = SpaceHalf;                   // 4 px of wallpaper between docked panels
    style.DockingNodeHasCloseButton = true;
    style.AntiAliasedLines         = true;
    style.AntiAliasedLinesUseTex   = true;
    style.AntiAliasedFill          = true;
    style.CurveTessellationTol     = 1.0f;
    style.CircleTessellationMaxError = 0.20f;                     // rounder corners at 10-12 px radii
    style.HoverStationaryDelay     = 0.15f;
    style.HoverDelayShort          = 0.15f;
    style.HoverDelayNormal         = 0.40f;

    // --- Colours ---------------------------------------------------------------------------------
    // Every slot is written. A sentinel pass first means a slot a newer ImGui adds (and this list
    // doesn't know yet) shows up as obvious magenta in a debug build instead of a silent default.
    ImVec4* c = style.Colors;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) c[i] = ImVec4(1.0f, 0.0f, 1.0f, 1.0f);

    const ImVec4 panel  = Resolve(SurfacePanel);
    const ImVec4 raised = Resolve(SurfaceRaised);
    const ImVec4 floatg = Resolve(SurfaceFloat);
    const ImVec4 title  = Resolve(SurfaceTitle);
    const ImVec4 clear  = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_Text]                  = TextPrimary;
    c[ImGuiCol_TextDisabled]          = TextTertiary;
    c[ImGuiCol_WindowBg]              = panel;
    c[ImGuiCol_ChildBg]               = clear;
    c[ImGuiCol_PopupBg]               = floatg;
    c[ImGuiCol_Border]                = Hairline;
    c[ImGuiCol_BorderShadow]          = clear;
    c[ImGuiCol_FrameBg]               = Field;
    c[ImGuiCol_FrameBgHovered]        = FieldHover;
    c[ImGuiCol_FrameBgActive]         = FieldActive;
    c[ImGuiCol_TitleBg]               = title;
    c[ImGuiCol_TitleBgActive]         = title;
    c[ImGuiCol_TitleBgCollapsed]      = WithAlpha(title, title.w * 0.75f);
    c[ImGuiCol_MenuBarBg]             = title;
    c[ImGuiCol_ScrollbarBg]           = clear;
    c[ImGuiCol_ScrollbarGrab]         = Rgb(0xFF, 0xFF, 0xFF, 0.16f);
    c[ImGuiCol_ScrollbarGrabHovered]  = Rgb(0xFF, 0xFF, 0xFF, 0.26f);
    c[ImGuiCol_ScrollbarGrabActive]   = Rgb(0xFF, 0xFF, 0xFF, 0.36f);
    c[ImGuiCol_CheckMark]             = OnAccent;
    c[ImGuiCol_CheckboxSelectedBg]    = Accent;
    c[ImGuiCol_SliderGrab]            = Rgb(0xFF, 0xFF, 0xFF, 0.80f);
    c[ImGuiCol_SliderGrabActive]      = OnAccent;
    c[ImGuiCol_Button]                = Rgb(0xFF, 0xFF, 0xFF, 0.08f);
    c[ImGuiCol_ButtonHovered]         = Rgb(0xFF, 0xFF, 0xFF, 0.13f);
    c[ImGuiCol_ButtonActive]          = Rgb(0xFF, 0xFF, 0xFF, 0.18f);
    c[ImGuiCol_Header]                = AccentWash;               // a selected row: the accent, washed
    c[ImGuiCol_HeaderHovered]         = WashHover;
    c[ImGuiCol_HeaderActive]          = WithAlpha(Accent, 0.32f);
    c[ImGuiCol_Separator]             = Separator;
    c[ImGuiCol_SeparatorHovered]      = WithAlpha(Accent, 0.60f);
    c[ImGuiCol_SeparatorActive]       = Accent;
    c[ImGuiCol_ResizeGrip]            = clear;
    c[ImGuiCol_ResizeGripHovered]     = WithAlpha(Accent, 0.45f);
    c[ImGuiCol_ResizeGripActive]      = Accent;
    c[ImGuiCol_InputTextCursor]       = Accent;
    c[ImGuiCol_TabHovered]            = Rgb(0xFF, 0xFF, 0xFF, 0.10f);
    c[ImGuiCol_Tab]                   = clear;                    // resting tabs float on the strip
    c[ImGuiCol_TabSelected]           = raised;
    c[ImGuiCol_TabSelectedOverline]   = Accent;
    c[ImGuiCol_TabDimmed]             = clear;
    c[ImGuiCol_TabDimmedSelected]     = WithAlpha(raised, raised.w * 0.70f);
    c[ImGuiCol_TabDimmedSelectedOverline] = clear;
    c[ImGuiCol_DockingPreview]        = WithAlpha(Accent, 0.32f);
    c[ImGuiCol_DockingEmptyBg]        = clear;                    // the wallpaper shows through
    c[ImGuiCol_PlotLines]             = TextSecondary;
    c[ImGuiCol_PlotLinesHovered]      = Accent;
    c[ImGuiCol_PlotHistogram]         = Accent;
    c[ImGuiCol_PlotHistogramHovered]  = AccentBright;
    c[ImGuiCol_TableHeaderBg]         = raised;
    c[ImGuiCol_TableBorderStrong]     = Hairline;
    c[ImGuiCol_TableBorderLight]      = Rgb(0xFF, 0xFF, 0xFF, 0.05f);
    c[ImGuiCol_TableRowBg]            = clear;
    c[ImGuiCol_TableRowBgAlt]         = Rgb(0xFF, 0xFF, 0xFF, 0.025f);
    c[ImGuiCol_TextLink]              = AccentBright;
    c[ImGuiCol_TextSelectedBg]        = WithAlpha(Accent, 0.35f);
    c[ImGuiCol_TreeLines]             = Separator;
    c[ImGuiCol_DragDropTarget]        = Accent;
    c[ImGuiCol_DragDropTargetBg]      = WithAlpha(Accent, 0.12f);
    c[ImGuiCol_UnsavedMarker]         = TextPrimary;
    c[ImGuiCol_NavCursor]             = AccentGlow;
    c[ImGuiCol_NavWindowingHighlight] = WithAlpha(TextPrimary, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = Rgb(0x00, 0x00, 0x00, 0.40f);
    c[ImGuiCol_ModalWindowDimBg]      = Rgb(0x00, 0x00, 0x00, 0.48f);

    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const ImVec4 v = c[i];
        IM_ASSERT(!(v.x == 1.0f && v.y == 0.0f && v.z == 1.0f && v.w == 1.0f) && "Glass theme: an ImGuiCol slot was left unset");
        (void)v;
    }
}
