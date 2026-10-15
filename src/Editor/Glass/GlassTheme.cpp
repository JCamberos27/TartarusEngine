#include "GlassTheme.h"

#include "EditorTheme.h" // Scale(): the UI scale recovered from the shared font

#include <imgui_internal.h> // ShadeVertsLinearUV, ImClamp

#include <cmath>

namespace Glass {

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
    DrawSoftShadow(dl, min, max, radius, 0.45f * alpha, Px(Space2));
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
    // The 25% black tint over the blur. Without a blur the plate is the opaque fallback instead, so
    // its text stays legible over anything the scene shows.
    const bool blurred = blurTex != (ImTextureID)0 && vpMax.x > vpMin.x && vpMax.y > vpMin.y;
    const ImVec4 tint = blurred ? GlassTint : GlassFallback;
    DrawSquircle(dl, min, max, U32(WithAlpha(tint, tint.w * alpha)), radius);
    // The 1 px edge of 15% white.
    DrawSquircleBorder(dl, min, max, U32(WithAlpha(GlassEdge, GlassEdge.w * alpha)), radius, 1.0f);
}

void DrawWallpaper(ImTextureID ambient) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* bg = ImGui::GetBackgroundDrawList(vp);
    const ImVec2 mn = vp->Pos, mx(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    bg->AddRectFilled(mn, mx, U32(Base));
    if (ambient == (ImTextureID)0) return;
    // The whole blurred Scene image over the whole window (v runs 1 -> 0: GL textures are bottom-up).
    bg->AddImage(ImTextureRef(ambient), mn, mx, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    bg->AddRectFilled(mn, mx, U32(Rgb(0x00, 0x00, 0x00, AmbientDim)));
}

} // namespace Glass

void ApplyGlassMaterials(ImGuiStyle& style) {
    using namespace Glass;
    ImVec4* c = style.Colors;
    const ImVec4 clear = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    const ImVec4 panel = Rgb(0x00, 0x00, 0x00, GlassPanelAlpha);
    const ImVec4 chrome = Rgb(0x00, 0x00, 0x00, GlassChromeAlpha);
    auto white = [](float a) { return Rgb(0xFF, 0xFF, 0xFF, a); };

    // Where you work: black glass over the ambient field.
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

    // Tabs sit on the strip as washes; the selected one is lit like a raised surface.
    c[ImGuiCol_Tab]                 = clear;
    c[ImGuiCol_TabHovered]          = white(0.08f);
    c[ImGuiCol_TabSelected]         = white(0.06f);
    c[ImGuiCol_TabDimmed]           = clear;
    c[ImGuiCol_TabDimmedSelected]   = white(0.04f);

    // Fields, buttons and headers lighten whatever is behind them instead of covering it.
    c[ImGuiCol_FrameBg]             = white(0.045f);
    c[ImGuiCol_FrameBgHovered]      = white(0.080f);
    c[ImGuiCol_FrameBgActive]       = white(0.110f);
    c[ImGuiCol_Button]              = white(0.060f);
    c[ImGuiCol_ButtonHovered]       = white(0.110f);
    c[ImGuiCol_ButtonActive]        = white(0.160f);
    c[ImGuiCol_HeaderHovered]       = white(0.060f);
    c[ImGuiCol_TableHeaderBg]       = white(0.050f);
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
    style.WindowBorderSize         = 1.0f;                        // hairline; 15% white once it's glass
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
    style.TabBarBorderSize         = 0.0f;                        // no rule under the strip
    style.TabBarOverlineSize       = 1.0f;                        // a quiet white line over the selected tab
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
    style.DockingSeparatorSize     = 2.0f;                        // a thin black seam between docked panels
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

    const ImVec4 clear = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // Solid where you work, glass where it floats. Floating windows and popups are drawn here
    // with solid colours (WindowBg / PopupBg / the title bar); GlassCompositor removes exactly
    // those from each floating window and lays the blurred glass underneath instead. Anywhere the
    // compositor doesn't run, the solid colours are the correct fallback.
    c[ImGuiCol_Text]                  = TextPrimary;
    c[ImGuiCol_TextDisabled]          = TextTertiary;
    c[ImGuiCol_WindowBg]              = Panel;
    c[ImGuiCol_ChildBg]               = clear;
    c[ImGuiCol_PopupBg]               = PopupSolid;
    c[ImGuiCol_Border]                = Hairline;                 // the compositor turns it into GlassEdge on glass
    c[ImGuiCol_BorderShadow]          = clear;
    c[ImGuiCol_FrameBg]               = Field;
    c[ImGuiCol_FrameBgHovered]        = FieldHover;
    c[ImGuiCol_FrameBgActive]         = FieldActive;
    c[ImGuiCol_TitleBg]               = Panel;                    // only floating windows have title bars,
    c[ImGuiCol_TitleBgActive]         = Panel;                    // and those are glass: same colour as the
    c[ImGuiCol_TitleBgCollapsed]      = Panel;                    // body so the compositor clears both
    c[ImGuiCol_MenuBarBg]             = Base;
    c[ImGuiCol_ScrollbarBg]           = clear;
    c[ImGuiCol_ScrollbarGrab]         = Rgb(0xFF, 0xFF, 0xFF, 0.12f);
    c[ImGuiCol_ScrollbarGrabHovered]  = Rgb(0xFF, 0xFF, 0xFF, 0.22f);
    c[ImGuiCol_ScrollbarGrabActive]   = Rgb(0xFF, 0xFF, 0xFF, 0.34f);
    c[ImGuiCol_CheckMark]             = OnAccent;                 // black tick on a white box
    c[ImGuiCol_CheckboxSelectedBg]    = Accent;
    c[ImGuiCol_SliderGrab]            = Rgb(0xFF, 0xFF, 0xFF, 0.70f);
    c[ImGuiCol_SliderGrabActive]      = AccentBright;
    c[ImGuiCol_Button]                = Rgb(0xFF, 0xFF, 0xFF, 0.07f);
    c[ImGuiCol_ButtonHovered]         = Rgb(0xFF, 0xFF, 0xFF, 0.12f);
    c[ImGuiCol_ButtonActive]          = Rgb(0xFF, 0xFF, 0xFF, 0.17f);
    c[ImGuiCol_Header]                = AccentWash;               // a selected row: white, washed
    c[ImGuiCol_HeaderHovered]         = WashHover;
    c[ImGuiCol_HeaderActive]          = WashPressed;
    c[ImGuiCol_Separator]             = Hairline;
    c[ImGuiCol_SeparatorHovered]      = Rgb(0xFF, 0xFF, 0xFF, 0.35f);
    c[ImGuiCol_SeparatorActive]       = Accent;
    c[ImGuiCol_ResizeGrip]            = clear;
    c[ImGuiCol_ResizeGripHovered]     = Rgb(0xFF, 0xFF, 0xFF, 0.30f);
    c[ImGuiCol_ResizeGripActive]      = Accent;
    c[ImGuiCol_InputTextCursor]       = AccentBright;
    c[ImGuiCol_TabHovered]            = Hover;
    c[ImGuiCol_Tab]                   = Base;                     // resting tabs sit on the black strip
    c[ImGuiCol_TabSelected]           = Panel;                    // the selected one joins its panel
    c[ImGuiCol_TabSelectedOverline]   = Accent;
    c[ImGuiCol_TabDimmed]             = Base;
    c[ImGuiCol_TabDimmedSelected]     = Panel;
    c[ImGuiCol_TabDimmedSelectedOverline] = clear;
    c[ImGuiCol_DockingPreview]        = Rgb(0xFF, 0xFF, 0xFF, 0.18f);
    c[ImGuiCol_DockingEmptyBg]        = Base;
    c[ImGuiCol_PlotLines]             = TextSecondary;
    c[ImGuiCol_PlotLinesHovered]      = AccentBright;
    c[ImGuiCol_PlotHistogram]         = TextSecondary;
    c[ImGuiCol_PlotHistogramHovered]  = AccentBright;
    c[ImGuiCol_TableHeaderBg]         = Raised;
    c[ImGuiCol_TableBorderStrong]     = Hairline;
    c[ImGuiCol_TableBorderLight]      = Rgb(0xFF, 0xFF, 0xFF, 0.05f);
    c[ImGuiCol_TableRowBg]            = clear;
    c[ImGuiCol_TableRowBgAlt]         = Rgb(0xFF, 0xFF, 0xFF, 0.025f);
    c[ImGuiCol_TextLink]              = AccentBright;
    c[ImGuiCol_TextSelectedBg]        = Rgb(0xFF, 0xFF, 0xFF, 0.25f);
    c[ImGuiCol_TreeLines]             = Hairline;
    c[ImGuiCol_DragDropTarget]        = Accent;
    c[ImGuiCol_DragDropTargetBg]      = Rgb(0xFF, 0xFF, 0xFF, 0.08f);
    c[ImGuiCol_UnsavedMarker]         = TextPrimary;
    c[ImGuiCol_NavCursor]             = AccentGlow;
    c[ImGuiCol_NavWindowingHighlight] = WithAlpha(TextPrimary, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = Rgb(0x00, 0x00, 0x00, 0.40f);
    c[ImGuiCol_ModalWindowDimBg]      = Rgb(0x00, 0x00, 0x00, 0.45f);

    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const ImVec4 v = c[i];
        IM_ASSERT(!(v.x == 1.0f && v.y == 0.0f && v.z == 1.0f && v.w == 1.0f) && "Glass theme: an ImGuiCol slot was left unset");
        (void)v;
    }
}
