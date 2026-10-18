#pragma once
// The editor's design tokens: every colour, font role and metric the panels draw with, in one
// place. The look is an old white-phosphor terminal: pitch-black surfaces, white phosphor text, greys
// for everything secondary, and the phosphor itself (full white) as the one accent — a selection or
// the primary button is inverse video. Letter-spaced monospace capitals for headings.
//
// Header-only and dependent on nothing but ImGui, so the host (EditorLayer*.cpp) and every
// reloadable editor module (EditorModule*.cpp) include it directly and draw with the same values
// against the shared ImGuiContext. ApplyThemeStyle (EditorLayer.cpp) maps these onto ImGuiStyle;
// a panel that needs a colour ImGuiStyle has no slot for (a status tint, an axis colour, a
// per-asset-type stripe) reads it from here instead of typing a literal.

#include <imgui.h>
#include <imgui_internal.h> // ImTextCharFromUtf8, ImFontBaked

#include "EditorMotionHooks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
// The Enhancers palette's red / green / blue (Palette::Defaults), so the gizmos, the vector fields,
// the orientation gizmo and the viewport grid's axis lines carry the same spectrum as the
// Hierarchy and folder colours: clear hues, neither washed out nor pure primaries.
constexpr ImVec4 AxisX = Rgb(0xE5, 0x48, 0x4D);
constexpr ImVec4 AxisY = Rgb(0x4C, 0xC3, 0x8A);
constexpr ImVec4 AxisZ = Rgb(0x4C, 0x8D, 0xF6);

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

// --- Motion ---------------------------------------------------------------------------------------
// One timing vocabulary for every hover, open/close and slide, so nothing in the editor moves at
// its own speed. Fast: hover and press feedback. Normal: a section opening, a tab sliding. Slow: an
// overlay or page arriving.
constexpr float MotionFast   = 0.08f;
constexpr float MotionNormal = 0.14f;
constexpr float MotionSlow   = 0.20f;

namespace Ease {
inline float Clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }
// Starts quick, lands softly: things arriving.
inline float OutCubic(float t) { t = Clamp01(t); const float u = 1.0f - t; return 1.0f - u * u * u; }
// Gentle at both ends: things that open and close in place.
inline float InOutCubic(float t) {
    t = Clamp01(t);
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * 0.5f;
}
} // namespace Ease

// A 0..1 value per ImGui id that walks toward `on` (1) or off (0) over `duration` seconds, in the
// current window's state storage, and comes back eased. For hover and selection fades: a widget
// shown for the first time starts at its target, so nothing animates in on panel open.
inline float AnimT(ImGuiID id, bool on, float duration = MotionFast) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const float target = on ? 1.0f : 0.0f;
    float v = st->GetFloat(id, -1.0f);
    if (v < 0.0f) v = target;
    const float step = duration > 0.0f ? ImGui::GetIO().DeltaTime / duration : 1.0f;
    v = v < target ? std::min(target, v + step) : std::max(target, v - step);
    st->SetFloat(id, v);
    return Ease::OutCubic(v);
}
inline ImVec4 Mix(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// --- The motion bridge (EditorMotionHooks.h) --------------------------------------------------------
// One table per binary: the host installs its own at startup, the module installs the host's every
// Draw. Every helper below falls back to AnimT when no table is installed.
inline const EditorMotionHooks*& MotionHooksSlot() { static const EditorMotionHooks* s_Hooks = nullptr; return s_Hooks; }
inline void SetMotionHooks(const EditorMotionHooks* hooks) { MotionHooksSlot() = hooks; }
inline const EditorMotionHooks* MotionHooks() { return MotionHooksSlot(); }

inline bool MotionReduced() {
    const EditorMotionHooks* h = MotionHooks();
    return h && h->ReduceMotion && h->ReduceMotion();
}
// A hover wash 0..1 (the Glass velocity-gated glow when the bridge is installed).
inline float MotionHover(ImGuiID id, bool hovered) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->HoverGlow) return h->HoverGlow(id, hovered);
    return AnimT(id, hovered);
}
// A float easing to `target` from `rest` with a token's timing.
inline float MotionAnimate(ImGuiID id, float target, float rest, EditorMotionToken token, float fallbackSeconds = MotionFast) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->Animate) return h->Animate(id, target, rest, (int)token);
    // Fallback: a 0..1 fade toward whichever end `target` is nearer, remapped onto rest..target.
    const float t = AnimT(id, std::fabs(target - rest) > 1e-4f, fallbackSeconds);
    return rest + (target - rest) * t;
}
// 0..1 openness for a flag that flips. Remembers last frame's flag in the window's state storage
// (one int per widget, ImGui's own memory) so callers don't have to.
inline float MotionToggle(ImGuiID id, bool open, EditorMotionToken token, float fallbackSeconds = MotionNormal) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID wasKey = id ^ 0x7A55E0F1u;
    const bool wasOpen = st->GetInt(wasKey, open ? 1 : 0) != 0;
    st->SetInt(wasKey, open ? 1 : 0);
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->Toggle) return h->Toggle(id, open, wasOpen, (int)token);
    return AnimT(id, open, fallbackSeconds);
}
// A glide to a moving target. Remembers last frame's target like MotionToggle. `snap`: the target
// moved for a reason that shouldn't animate (a reflow, a resize) - follow it at once; a glide
// already in flight still lands.
inline float MotionFollow(ImGuiID id, float target, EditorMotionToken token = EditorMotionToken::Glide, bool snap = false) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID prevKey = id ^ 0x3F011C0Bu;
    const float prev = snap ? target : st->GetFloat(prevKey, target);
    st->SetFloat(prevKey, target);
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->Follow) return h->Follow(id, target, prev, (int)token);
    return target;
}
// An OKLAB colour tween (a straight mix through AnimT when the bridge is missing).
inline ImVec4 MotionColor(ImGuiID id, ImVec4 target, ImVec4 rest, EditorMotionToken token) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->AnimateColor) {
        const float t4[4] = {target.x, target.y, target.z, target.w};
        const float r4[4] = {rest.x, rest.y, rest.z, rest.w};
        float o4[4] = {target.x, target.y, target.z, target.w};
        h->AnimateColor(id, t4, r4, (int)token, o4);
        return ImVec4(o4[0], o4[1], o4[2], o4[3]);
    }
    return target;
}
inline void MotionPulse(ImGuiID id, EditorMotionToken token) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->Pulse) h->Pulse(id, (int)token);
}
inline float MotionPulseProgress(ImGuiID id) {
    const EditorMotionHooks* h = MotionHooks();
    return (h && h->PulseProgress) ? h->PulseProgress(id) : 1.0f;
}
inline void MotionTriggerWiggle(ImGuiID id) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->TriggerWiggle) h->TriggerWiggle(id);
}
inline float MotionWiggle(ImGuiID id) {
    const EditorMotionHooks* h = MotionHooks();
    return (h && h->Wiggle) ? h->Wiggle(id) : 0.0f;
}
inline void MotionScrollToY(float y) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->ScrollToY) h->ScrollToY(y); else ImGui::SetScrollY(y);
}
inline void MotionScrollToBottom() {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->ScrollToBottom) h->ScrollToBottom(); else ImGui::SetScrollY(ImGui::GetScrollMaxY());
}

// --- Motion v2 (API v44): each falls back to the static look when the bridge is missing ----------
// Categories mirror Glass::MotionCategory.
enum class MotionCat : int { ThemeTweens = 0, Inertia, Gradients, RollingNumbers, Viewport, Startup };
inline bool MotionCategoryOn(MotionCat c) {
    const EditorMotionHooks* h = MotionHooks();
    return h && h->CategoryOn && h->CategoryOn((int)c);
}
// Rolling digits at (x, y) top-left; plain text without the bridge. Returns the width drawn.
inline float MotionOdometer(ImGuiID id, ImDrawList* dl, ImVec2 pos, double value, int decimals, ImU32 col, const char* suffix = nullptr) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->Odometer) return h->Odometer(id, dl, pos.x, pos.y, value, decimals, col, suffix);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f%s", decimals, value, suffix ? suffix : "");
    dl->AddText(pos, col, buf);
    return ImGui::CalcTextSize(buf).x;
}
inline void MotionInertiaTrack(ImGuiID id, ImVec2 delta) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->InertiaTrack) h->InertiaTrack(id, delta.x, delta.y);
}
inline void MotionInertiaRelease(ImGuiID id) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->InertiaRelease) h->InertiaRelease(id);
}
inline ImVec2 MotionInertiaDelta(ImGuiID id) {
    const EditorMotionHooks* h = MotionHooks();
    ImVec2 d(0, 0);
    if (h && h->InertiaDelta) h->InertiaDelta(id, &d.x, &d.y);
    return d;
}
inline void MotionSheen(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImGuiID seed, float strength) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->DrawSheen) h->DrawSheen(dl, mn.x, mn.y, mx.x, mx.y, seed, strength);
}
inline void MotionCursorGlow(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImVec2 cursor, float strength) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->DrawCursorGlow) h->DrawCursorGlow(dl, mn.x, mn.y, mx.x, mx.y, cursor.x, cursor.y, strength);
}
inline float MotionRippleDelay(int col, int row, int originCol, int originRow) {
    const EditorMotionHooks* h = MotionHooks();
    return (h && h->RippleDelay) ? h->RippleDelay(col, row, originCol, originRow) : 0.0f;
}
inline int MotionBeginLayer(ImDrawList* dl) {
    const EditorMotionHooks* h = MotionHooks();
    return (h && h->BeginLayer) ? h->BeginLayer(dl) : dl->VtxBuffer.Size;
}
// Scale `scale` and rotate `rot` about `pivot`, translate by `translate`, multiply alpha.
inline void MotionEndLayer(ImDrawList* dl, int vtxStart, ImVec2 pivot, ImVec2 scale, float rot = 0.0f,
                           ImVec2 translate = ImVec2(0, 0), float alpha = 1.0f) {
    const EditorMotionHooks* h = MotionHooks();
    if (h && h->EndLayer) h->EndLayer(dl, vtxStart, pivot.x, pivot.y, scale.x, scale.y, rot, translate.x, translate.y, alpha);
}

// --- User colours (row styles, folder colours, tab and favourite tints) ---------------------------
// The screen is monochrome; a colour the user picked is a signal, never a fill. It shows as a thin
// stripe, a tinted glyph and a faint wash - and a selection or hover always reads over it.
inline float UserStripeW()  { return Px(3.0f); }
constexpr float UserWashAlpha = 0.10f; // gradient wash, at its strongest (left) end
constexpr float UserFlatAlpha = 0.13f; // flat wash
// Black Glass is monochrome: the colours picked for rows, folders, component headers and tabs show
// in white there (the stripe, the glyph and the wash keep their shape, just not their hue). The
// Glass style is the one whose panels are translucent, which both the host and TartarusEditor.dll
// can see through the shared ImGui context.
inline bool MonochromeUserColors() {
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w < 0.999f;
}
// A packed user colour as it should draw: itself, or (Black Glass) white with its alpha kept.
inline unsigned UserPacked(unsigned packed) {
    return (packed != 0 && MonochromeUserColors()) ? ((packed & 0xFF000000u) | 0x00E6E8E6u) : packed;
}
// An IM_COL32 user colour as a float colour, alpha forced opaque.
inline ImVec4 UserColor(unsigned packed) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(UserPacked(packed));
    c.w = 1.0f;
    return c;
}
// The same colour lifted until it reads as a glyph on the near-black panels: a dark pick (navy,
// maroon) is mixed toward the phosphor white until its luminance clears a floor.
inline ImVec4 UserGlyphColor(unsigned packed) {
    ImVec4 c = UserColor(packed);
    const float lum = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    constexpr float kFloor = 0.42f;
    if (lum < kFloor) c = Mix(c, Text, (kFloor - lum) / (1.0f - lum + 1e-4f));
    return c;
}

} // namespace EditorTheme
