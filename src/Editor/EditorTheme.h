#pragma once
// The editor's design tokens: every colour, font role and metric the panels draw with, in one
// place. The look is an old white-phosphor terminal: pitch-black surfaces, white phosphor text, greys
// for everything secondary, and the phosphor itself (full white) as the one accent — a selection or
// the primary button is inverse video. Letter-spaced monospace capitals for headings, and
// Preferences > CRT screen draws it all through the launch screen's tube (CrtScreen.frag.glsl).
//
// Header-only and dependent on nothing but ImGui, so the host (EditorLayer*.cpp) and every
// reloadable editor module (EditorModule*.cpp) include it directly and draw with the same values
// against the shared ImGuiContext. ApplyThemeStyle (EditorLayer.cpp) maps these onto ImGuiStyle;
// a panel that needs a colour ImGuiStyle has no slot for (a status tint, an axis colour, a
// per-asset-type stripe) reads it from here instead of typing a literal.

#include <imgui.h>
#include <imgui_internal.h> // ImTextCharFromUtf8, ImFontBaked

#include <algorithm>
#include <cstring>

namespace EditorTheme {

constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
inline ImVec4 WithAlpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }
inline ImU32 U32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }

// --- Surfaces, darkest to lightest --------------------------------------------------------------
constexpr ImVec4 Base     = Rgb(0x00, 0x00, 0x00); // menu bar, title bars, dock gaps, status bar
constexpr ImVec4 Panel    = Rgb(0x05, 0x05, 0x05); // a docked panel's canvas (WindowBg)
constexpr ImVec4 Raised   = Rgb(0x0E, 0x0E, 0x0E); // component headers, cards, popups, toolbars
constexpr ImVec4 Field    = Rgb(0x00, 0x00, 0x00); // inputs, combos, sliders (FrameBg)
constexpr ImVec4 Card     = Rgb(0x09, 0x09, 0x09); // a component's body under its title bar
constexpr ImVec4 Hover    = Rgb(0x1C, 0x1C, 0x1C);
constexpr ImVec4 Pressed  = Rgb(0x2A, 0x2A, 0x2A);
constexpr ImVec4 Stripe   = Rgb(0xFF, 0xFF, 0xFF, 0.025f); // alternate list rows

// --- Lines ----------------------------------------------------------------------------------------
constexpr ImVec4 Hairline = Rgb(0x24, 0x24, 0x24);
constexpr ImVec4 Strong   = Rgb(0x3A, 0x3A, 0x3A);

// --- Text: the phosphor ---------------------------------------------------------------------------
constexpr ImVec4 Text      = Rgb(0xE6, 0xE8, 0xE6); // white phosphor, a hair off pure white
constexpr ImVec4 Secondary = Rgb(0x9C, 0x9E, 0x9C); // labels, captions, resting icons
constexpr ImVec4 Dim       = Rgb(0x6A, 0x6C, 0x6A); // hints, disabled, inactive entities

// --- The accent: the phosphor at full drive --------------------------------------------------------
// Selection, the active tool, a set toggle, focus. The primary button is a white fill with black
// text (inverse video), like a terminal's highlighted menu item.
constexpr ImVec4 Accent       = Rgb(0xF4, 0xF6, 0xF4);
constexpr ImVec4 AccentBright = Rgb(0xFF, 0xFF, 0xFF);
constexpr ImVec4 AccentDeep   = Rgb(0xB4, 0xB6, 0xB4);
constexpr ImVec4 AccentWash   = Rgb(0xFF, 0xFF, 0xFF, 0.12f); // selection, an "on" toggle's body
constexpr ImVec4 OnAccent     = Rgb(0x00, 0x00, 0x00);        // text on an accent fill

// --- Status: fixed meanings, never the accent. Muted, like a terminal's few colours. --------------
constexpr ImVec4 Danger  = Rgb(0xE8, 0x5C, 0x54);
constexpr ImVec4 Warning = Rgb(0xE6, 0xA4, 0x50);
constexpr ImVec4 Success = Rgb(0x7C, 0xD8, 0x8C);
constexpr ImVec4 Info    = Rgb(0x96, 0xB8, 0xE6);

// --- Axes (gizmos, vector fields, the orientation gizmo) ------------------------------------------
// Muted, like the status colours: still red / green / blue at a glance, without shouting off the
// black-and-white screen.
constexpr ImVec4 AxisX = Rgb(0xB8, 0x5E, 0x5E);
constexpr ImVec4 AxisY = Rgb(0x7E, 0xA8, 0x6C);
constexpr ImVec4 AxisZ = Rgb(0x66, 0x86, 0xB8);

// --- Entity / asset kinds (hierarchy icons, asset tile stripes) ------------------------------------
// Tinted greys: a hint of hue so kinds stay tellable apart, without breaking the monochrome screen.
constexpr ImVec4 KindMesh     = Rgb(0xA8, 0xBC, 0xCC);
constexpr ImVec4 KindLight    = Rgb(0xD6, 0xC8, 0x96);
constexpr ImVec4 KindCamera   = Rgb(0xB8, 0xAE, 0xD2);
constexpr ImVec4 KindPrefab   = Rgb(0x9C, 0xB8, 0xDC); // a prefab instance's name and icon
constexpr ImVec4 KindPrefabBroken = Rgb(0xE0, 0x74, 0x6C);
constexpr ImVec4 KindAudio    = Rgb(0x9C, 0xCC, 0xC2);
constexpr ImVec4 KindMaterial = Rgb(0xCE, 0xA8, 0xC2);
constexpr ImVec4 KindTexture  = Rgb(0xC6, 0xB4, 0x9A);
constexpr ImVec4 KindScene    = Accent;
constexpr ImVec4 KindAnim     = Rgb(0xA4, 0xCC, 0xA4);
constexpr ImVec4 KindScript   = Rgb(0x9C, 0x9E, 0x9C);

// HUD plates drawn over the 3D view (tool palette, status, overlays): black, mostly opaque, so the
// phosphor text on them is legible over anything the scene shows.
constexpr ImVec4 HudPlate = Rgb(0x00, 0x00, 0x00, 0.86f);

// --- Fonts ----------------------------------------------------------------------------------------
// Loaded by EditorLayer::Init and found again by name, so a module DLL (which shares the
// ImGuiContext but not the host's pointers) gets the same faces.
constexpr const char* kUiFontName   = "TE-UI";
constexpr const char* kMonoFontName = "TE-Mono";
constexpr float kBaseFontPx = 15.0f; // the UI font at 1x UI scale

inline ImFont* FindFont(const char* name) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    for (ImFont* f : atlas->Fonts)
        if (std::strncmp(f->GetDebugName(), name, std::strlen(name)) == 0) return f;
    return nullptr;
}
inline ImFont* UiFont()   { static ImFont* f = nullptr; if (!f) f = FindFont(kUiFontName); return f ? f : ImGui::GetFont(); }
inline ImFont* MonoFont() { static ImFont* f = nullptr; if (!f) f = FindFont(kMonoFontName); return f ? f : UiFont(); }

// The UI scale (monitor content scale or the Preferences override), recovered from the UI font's
// baked size so modules don't need it passed in.
inline float Scale() {
    ImFont* f = UiFont();
    return f && f->LegacySize > 0.0f ? f->LegacySize / kBaseFontPx : 1.0f;
}
// A design-space pixel length (authored at 1x) in screen pixels.
inline float Px(float v) { return v * Scale(); }

// Font roles. Sizes are the UI font's base size times a ratio, so they follow the UI scale.
inline float BodySize()    { return Px(kBaseFontPx); }
inline float SmallSize()   { return Px(13.0f); }
inline float HeadingSize() { return Px(11.5f); }  // tracked mono capitals
inline float TitleSize()   { return Px(18.0f); }

inline void PushBody()    { ImGui::PushFont(UiFont(), BodySize()); }
inline void PushSmall()   { ImGui::PushFont(UiFont(), SmallSize()); }
inline void PushMono()    { ImGui::PushFont(MonoFont(), Px(13.5f)); }
inline void PushMonoSmall() { ImGui::PushFont(MonoFont(), Px(12.0f)); }
inline void PushHeading() { ImGui::PushFont(MonoFont(), HeadingSize()); }
inline void PushTitle()   { ImGui::PushFont(UiFont(), TitleSize()); }
inline void PopFont()     { ImGui::PopFont(); }

// --- Tracked text ---------------------------------------------------------------------------------
// The launcher's "B U I L D I N G": capitals with extra space between letters. ImGui has no
// letter-spacing, so this lays the glyphs out one by one with the current font.
inline ImVec2 CalcTrackedSize(const char* text, float tracking) {
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    float w = 0.0f;
    int n = 0;
    for (const char* p = text; *p;) {
        unsigned int c = 0;
        const int len = ImTextCharFromUtf8(&c, p, nullptr);
        if (len <= 0) break;
        w += font->GetFontBaked(size)->GetCharAdvance((ImWchar)c);
        p += len;
        ++n;
    }
    if (n > 1) w += tracking * (float)(n - 1);
    return ImVec2(w, size);
}
inline void DrawTracked(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, float tracking) {
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    ImFontBaked* baked = font->GetFontBaked(size);
    for (const char* p = text; *p;) {
        unsigned int c = 0;
        const int len = ImTextCharFromUtf8(&c, p, nullptr);
        if (len <= 0) break;
        char buf[8] = {};
        std::memcpy(buf, p, (size_t)len);
        dl->AddText(font, size, pos, col, buf);
        pos.x += baked->GetCharAdvance((ImWchar)c) + tracking;
        p += len;
    }
}
// An item: tracked text at the cursor in `col`.
inline void TrackedText(const char* text, ImVec4 col, float tracking) {
    const ImVec2 size = CalcTrackedSize(text, tracking);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    DrawTracked(ImGui::GetWindowDrawList(), pos, U32(col), text, tracking);
    ImGui::Dummy(size);
}
inline float HeadingTracking() { return Px(1.6f); }

// --- The property grid ----------------------------------------------------------------------------
// Every Inspector row (PropertyLabel, the vector rows, PropertyRows) shares one label column: a
// share of the row's width, clamped so a narrow panel still leaves room for the value and a wide
// one doesn't strand the value far from its name. Call at the start of the row.
inline float PropertyLabelWidth() {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float f = ImGui::GetFontSize();
    return std::clamp(avail * 0.38f, f * 5.5f, f * 10.0f);
}
// A row's label: the secondary text colour, vertically centred on a frame-height row, cut with an
// ellipsis when it doesn't fit `width` (returns true then, so the caller can show the full text
// as a tooltip). Leaves the label as the last item, for hover / popup checks.
inline bool PropertyLabelText(const char* label, float width, ImVec4 col = Secondary) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    const float maxW = std::max(1.0f, width - ImGui::GetStyle().ItemSpacing.x);
    const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    ImGui::Dummy(ImVec2(std::min(ts.x, maxW), h));
    const float y = pos.y + (h - ts.y) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(pos.x, y), ImVec2(pos.x + maxW, y + ts.y),
                              pos.x + maxW, label, nullptr, &ts);
    ImGui::PopStyleColor();
    return ts.x > maxW;
}

} // namespace EditorTheme
