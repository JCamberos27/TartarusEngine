#pragma once
// Design-system primitives shared verbatim between the host (EditorLayer*.cpp) and every
// reloadable editor module (EditorModule*.cpp) — Phase 0.5 item 2 / Defect #53. Depends on
// nothing but ImGui + Font Awesome, so both sides of the DLL boundary can include this header
// directly and draw through the identical implementation, instead of each module hand-rolling
// its own copy of ActionButton/PrimaryButton against the shared ImGuiContext (as
// EditorModuleToolbar.cpp, EditorModuleAssetBrowser.cpp and EditorModuleConsole.cpp each did —
// #160's "two and only two button treatments" was only enforced by convention, not by
// construction). A module builds its own ImGui core translation units but shares the host's one
// ImGuiContext at Draw() time (API v1), so these `inline` functions — pure ImGui:: calls against
// that shared context — behave identically wherever they're compiled in.
//
// Tooltip text is routed through a caller-supplied `TooltipFn` rather than calling
// EditorUI::SetTooltip directly: that lives in EditorUIHelpers.cpp (host-only — it also reads
// EditorSettings::Get().ShowTooltips). The host passes a thin forwarder to EditorUI::SetTooltip;
// a module passes host.SetTooltip straight through — both already match this exact shape.
//
// A colour-token accessor (AccentColor) is included; a metric accessor is not — every module
// panel that positions itself already reads UI scale off EditorModuleHostAPI's existing
// GetToolbarMetrics/GetViewportRect/GetHistoryHudFrame (API v3/v4/v15), so a second, competing
// "metric token" surface would just be an unused alternative to what's already there. A
// PropertyRow primitive is deliberately not added here yet: it has no caller today (the
// Inspector body is still host-side per API v8, and stays that way until Phase 4's Inspector
// rebuild) — building it now would mean designing its layout contract against zero real call
// sites. Add it here, alongside its first consumer, when Phase 4 actually moves Inspector rows
// into a module.

#include <imgui.h>
#include <imgui_internal.h> // ImMax (kept for callers of this header that need glyph metrics)
#include <IconsFontAwesome6.h>

#include <cmath>
#include <cstdio>

namespace EditorUIPrimitives {

using TooltipFn = void (*)(const char*);

// The accent colour every "on" toggle state reads in — the same ImGui role (ImGuiCol_SliderGrab)
// the styled sliders use — kept as one named accessor rather than a raw ImGuiCol_* index
// sprinkled at every call site. Matches docs/CONVENTIONS.md's accent-discipline table: cyan =
// selection / "you are here".
inline ImVec4 AccentColor() { return ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab); }

// Status-role colours (Phase 1 item 2, Appendix B — "add danger/warning/success/info roles").
// Fixed, not theme-derived: a status colour has to mean the same thing regardless of which of
// the three themes is active, unlike AccentColor(). All four already clear WCAG 1.4.3's 4.5:1
// text floor against the darkest surface a status message sits on (#121212) by a wide margin —
// computed via the same relative-luminance method as the #34/border/text-secondary fixes above,
// not eyeballed. Existing call sites (DangerIconButton's hover red, Console's per-entry tint)
// already used colours in this range; these give that a single named source instead of each
// call site picking its own shade.
inline ImVec4 DangerColor()  { return ImVec4(1.00f, 0.42f, 0.38f, 1.0f); } // ~6.7:1
inline ImVec4 WarningColor() { return ImVec4(1.00f, 0.80f, 0.30f, 1.0f); } // ~12.5:1
inline ImVec4 SuccessColor() { return ImVec4(0.45f, 0.85f, 0.55f, 1.0f); } // ~10.9:1
inline ImVec4 InfoColor()    { return ImVec4(0.55f, 0.75f, 1.00f, 1.0f); } // ~9.9:1

// Two — and only two — button treatments across the whole editor (#160; see
// docs/CONVENTIONS.md). ActionButton: flat, no body at rest, faint wash on hover; `active` gives
// an accent-tinted body + a 2px bottom keyline for toggles that are "on". This is every toolbar
// tool, every panel toggle, every low-frequency icon action.
inline bool ActionButton(const char* icon, const char* tooltip, TooltipFn tooltipFn,
                          bool active = false, ImVec2 size = ImVec2(0, 0)) {
    const ImVec4 acc = AccentColor();
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(acc.x, acc.y, acc.z, 0.22f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(acc.x, acc.y, acc.z, 0.34f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(acc.x, acc.y, acc.z, 0.46f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(acc.x, acc.y, acc.z, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
    }
    // Button() folds its label into its ID, so two buttons that ever show the same glyph would
    // collide — scope the ID to the (unique) tooltip string instead.
    ImGui::PushID(tooltip);
    bool clicked = ImGui::Button(icon, size);
    ImGui::PopID();
    if (active) {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const float y = mx.y - 2.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(mn.x + 3.0f, y), ImVec2(mx.x - 3.0f, mx.y - 1.0f),
                                                  ImGui::ColorConvertFloat4ToU32(acc), 1.0f);
    }
    ImGui::PopStyleColor(active ? 4 : 3);
    if (tooltipFn && ImGui::IsItemHovered()) tooltipFn(tooltip);
    return clicked;
}

// Same skeleton as ActionButton, the red-on-hover palette for destructive icon actions (Delete,
// the component-remove x).
inline bool DangerIconButton(const char* icon, const char* tooltip, TooltipFn tooltipFn,
                              ImVec2 size = ImVec2(0, 0)) {
    const ImVec4 danger = DangerColor();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(danger.x, danger.y, danger.z, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(danger.x, danger.y, danger.z, 1.00f));
    ImGui::PushID(tooltip);
    bool clicked = ImGui::Button(icon, size);
    ImGui::PopID();
    ImGui::PopStyleColor(3);
    if (tooltipFn && ImGui::IsItemHovered()) tooltipFn(tooltip);
    return clicked;
}

// Defect #20 / #73 — an unchecked checkbox's frame meets this theme's 3:1 WCAG floor (FrameBg vs
// WindowBg — see EditorLayer.cpp's #34 comment) but this theme runs FrameBorderSize 0 everywhere,
// so it has no outline at all: a small, low-contrast fill with no edge to define it, easy to miss
// entirely at a glance (confirmed live via raw-pixel sampling against a running build — it does
// render, just imperceptibly). Checked boxes don't have this problem: ImGuiCol_CheckboxSelectedBg
// and the CheckMark tick are both far more saturated than a plain unchecked fill. First fixed
// locally in the Console module (Defect #20) with a full manual reimplementation; promoted here,
// simplified to a thin wrapper, once the same pattern turned up editor-wide (#73) — real
// ImGui::Checkbox handles every bit of actual behaviour (click/hover/id/tri-state/keyboard nav/
// disabled), this only adds the one outline it's missing, drawn over its own item rect afterward.
inline bool Checkbox(const char* label, bool* v) {
    const bool changed = ImGui::Checkbox(label, v);
    const ImVec2 mn = ImGui::GetItemRectMin();
    const float sz = ImGui::GetFrameHeight(); // Checkbox()'s own box is always this tall/wide, at the item's top-left
    ImGui::GetWindowDrawList()->AddRect(mn, ImVec2(mn.x + sz, mn.y + sz),
        ImGui::GetColorU32(ImGuiCol_TextDisabled), ImGui::GetStyle().FrameRounding, 0, 1.0f);
    return changed;
}

// The one filled treatment — theme accent body, for prominent/rare actions only (modal-dialog
// buttons: Save / Don't Save / Restore / Cancel …).
inline bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::GetStyleColorVec4(ImGuiCol_Header));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

// #4 item 2 — a small inline marker for a control that writes into the *scene* file (pushes an
// undo step, dirties the scene) rather than editor_prefs.json or a project file. Phase 2 item 1
// already removed the one case where this ambiguity was an outright bug (Preferences > Environment
// silently dirtying the scene); this is for panels that legitimately mix both kinds of state in
// one window, like Window > Lighting's Environment section sitting next to its own
// Post-processing/Shadows sections (which are editor_prefs.json, not scene data) — so it's a
// disclosure, not a warning: InfoColor, not WarningColor. Draws as its own item — call
// ImGui::SameLine() first if the caller wants it beside a preceding label instead of on its own
// line (SeparatorText, for one, already ends its row, so a badge marking a section it titles
// reads better on the next line than fighting the separator rule for the same row).
inline void SceneDataBadge(TooltipFn tooltipFn) {
    const ImVec4 info = InfoColor();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(info.x, info.y, info.z, 0.85f));
    ImGui::TextUnformatted(ICON_FA_FILM "  SCENE");
    ImGui::PopStyleColor();
    if (tooltipFn && ImGui::IsItemHovered())
        tooltipFn("Saved in the scene file, not your editor preferences \xe2\x80\x94 editing this dirties the scene and can be undone (Ctrl+Z).");
}

// --- Viewport HUD legibility (Defect #54 / Phase 1 item 3) ---------------------------------
// Every viewport-overlay HUD (Stats, History, the Play/Stop button, the status bar, the
// nav-gizmo cluster, the Game-view overlays) used to sample the rendered scene's luminance
// behind it (an async GPU readback, throttled ~10 Hz, per element) and ease its text between
// white-on-dark and black-on-light so it stayed readable over arbitrary content. That machinery
// — AsyncLuminanceReadback, SampleTextureLuminance, ContrastForLuminance, 7+ scratch FBOs, 14+
// PBOs, a glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING) call that bypassed GLStateCache — is gone.
// A HUD element only ever needs to be legible over ANYTHING; a fixed light text colour on a
// fixed opaque-ish dark plate is legible by construction, with zero runtime GPU cost and zero
// "wrong colour this frame" flicker while a readback catches up. Use these two together: draw
// the plate first, then the text in kHudTextColor on top.
inline constexpr ImU32 kHudTextColor         = IM_COL32(235, 235, 235, 255);
inline constexpr ImU32 kHudTextDisabledColor = IM_COL32(235, 235, 235, 150);
inline constexpr ImU32 kHudPlateColor        = IM_COL32(10, 10, 10, 190);

// Fills `mn`..`mx` with the standard HUD plate colour. Pass the tight bounding box of the
// content that sits on top (e.g. from ImGui::CalcTextSize / ImFont::CalcTextSizeA), already
// padded — this does not add its own margin, since callers pad differently (a text hint vs. a
// multi-line stats block).
inline void DrawHudPlate(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding = 4.0f) {
    dl->AddRectFilled(mn, mx, kHudPlateColor, rounding);
}

// --- Startup contrast assert (Phase 1 item 1) -----------------------------------------------
// WCAG relative-luminance / contrast-ratio math, header-only so both the host's ApplyThemeStyle
// and (if a module ever needs it) a module panel can check a colour pair against a floor without
// eyeballing it or re-deriving the sRGB formula ad hoc — the same math used by hand for the #34
// FrameBg fix and the border/text-secondary floor raise above, now a reusable, checkable function
// instead of a one-off comment showing the arithmetic.
inline float SrgbChannelToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float RelativeLuminance(ImVec4 c) {
    return 0.2126f * SrgbChannelToLinear(c.x) + 0.7152f * SrgbChannelToLinear(c.y) +
           0.0722f * SrgbChannelToLinear(c.z);
}

// WCAG 2.x contrast ratio, order-independent (always >=1.0): (L_lighter+0.05)/(L_darker+0.05).
inline float ContrastRatio(ImVec4 a, ImVec4 b) {
    const float la = RelativeLuminance(a), lb = RelativeLuminance(b);
    const float hi = ImMax(la, lb), lo = ImMin(la, lb);
    return (hi + 0.05f) / (lo + 0.05f);
}

// Straight (non-premultiplied) alpha composite of `fg` over opaque `bg`, done in encoded sRGB
// space (i.e. on the raw 0..1 channel values, no linearize/re-encode round trip) — this engine
// never enables GL_FRAMEBUFFER_SRGB, so that's the space ImGui's own blending actually happens
// in, and it's what a contrast check needs to match to mean anything. Returns an opaque colour
// so the result can go straight into ContrastRatio/AssertContrastFloor. An alpha-blended role
// (Border, Separator — anything under 1.0 alpha) MUST be composited before checking: comparing
// its raw (un-composited) channel values against a background is meaningless, since e.g. Border's
// rgb is literally white regardless of how transparent it actually reads on screen.
inline ImVec4 CompositeOver(ImVec4 fg, ImVec4 bg) {
    const float a = fg.w;
    return ImVec4(bg.x + (fg.x - bg.x) * a, bg.y + (fg.y - bg.y) * a, bg.z + (fg.z - bg.z) * a, 1.0f);
}

// Logs (does not crash — a failed floor should ship visible-but-flagged, not take the editor
// down) via the caller-supplied `warnFn` if `fg` against `bg` doesn't clear `floor`. `role` names
// the pair in the message ("FrameBg vs WindowBg") so a regression is actionable from the log
// alone. Call once per theme switch, not per frame — this is a correctness check, not a
// runtime-adaptive system (that machinery was deliberately removed, see kHudTextColor above).
using ContrastWarnFn = void (*)(const char* message);
inline void AssertContrastFloor(const char* role, ImVec4 fg, ImVec4 bg, float floor, ContrastWarnFn warnFn) {
    const float ratio = ContrastRatio(fg, bg);
    if (ratio + 0.005f < floor) { // small epsilon so a floor computed to land exactly on the line doesn't nag
        char buf[192];
        snprintf(buf, sizeof(buf), "Contrast floor missed: %s is %.2f:1, needs >=%.1f:1.", role, ratio, floor);
        if (warnFn) warnFn(buf);
    }
}

} // namespace EditorUIPrimitives
