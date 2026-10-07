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

#include "EditorTheme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace EditorUIPrimitives {

using TooltipFn = void (*)(const char*);

// The accent every "on" state, selection and focus reads in: the phosphor at full drive
// (EditorTheme.h). Kept as named accessors so a call site never picks its own shade.
inline ImVec4 AccentColor() { return EditorTheme::Accent; }
inline ImVec4 ActiveAccentColor() { return EditorTheme::AccentBright; }

// Status-role colours (Phase 1 item 2): fixed meanings, never the accent. All clear WCAG 1.4.3's
// 4.5:1 text floor against the Panel surface (checked in ApplyThemeStyle).
inline ImVec4 DangerColor()  { return EditorTheme::Danger; }
inline ImVec4 WarningColor() { return EditorTheme::Warning; }
inline ImVec4 SuccessColor() { return EditorTheme::Success; }
inline ImVec4 InfoColor()    { return EditorTheme::Info; }

// The hover / press wash every flat control shares.
inline ImVec4 FlatHover()   { return ImVec4(1.0f, 1.0f, 1.0f, 0.07f); }
inline ImVec4 FlatPressed() { return ImVec4(1.0f, 1.0f, 1.0f, 0.12f); }

// Three button treatments across the whole editor (#160), plus a destructive variant:
//   ActionButton    flat; no body at rest, a faint wash on hover. Toolbar tools, panel toggles,
//                   low-frequency icon actions. `active` = a toggle that is on: accent glyph on an
//                   accent wash with an accent keyline.
//   SecondaryButton a raised body with a hairline: ordinary text buttons (Cancel, Import...,
//                   Add Component).
//   PrimaryButton   a white body with black text (inverse video): the one confirming action of a dialog.
//   DangerIconButton flat, red on hover: Delete, the component-remove x.
inline bool ActionButton(const char* icon, const char* tooltip, TooltipFn tooltipFn,
                          bool active = false, ImVec2 size = ImVec2(0, 0)) {
    const ImVec4 acc = AccentColor();
    if (!active) {
        // A resting icon is drawn in the secondary text colour and brightens under the cursor, so a
        // row of icons reads as buttons before the pointer arrives. Approximates Button()'s own
        // hit-test since the colour has to be pushed before the button is submitted.
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 iconSize = ImGui::CalcTextSize(icon, nullptr, true);
        const ImVec2 pad = ImGui::GetStyle().FramePadding;
        const ImVec2 btnSize(size.x > 0.0f ? size.x : iconSize.x + pad.x * 2.0f,
                              size.y > 0.0f ? size.y : iconSize.y + pad.y * 2.0f);
        const bool willHover = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseHoveringRect(cursor, ImVec2(cursor.x + btnSize.x, cursor.y + btnSize.y));
        ImGui::PushStyleColor(ImGuiCol_Text, willHover ? EditorTheme::Text : EditorTheme::Secondary);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, FlatHover());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  FlatPressed());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text,          EditorTheme::AccentBright);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(acc.x, acc.y, acc.z, 0.16f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(acc.x, acc.y, acc.z, 0.24f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(acc.x, acc.y, acc.z, 0.32f));
    }
    // FrameBorderSize is on editor-wide (a hairline around fields); a flat button has no frame.
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    // Button() folds its label into its ID, so two buttons showing the same glyph would collide;
    // scope the ID to the (unique) tooltip string instead.
    ImGui::PushID(tooltip);
    const bool clicked = ImGui::Button(icon, size);
    ImGui::PopID();
    ImGui::PopStyleVar();
    if (active) {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const float k = EditorTheme::Px(2.0f);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(mn.x + k * 1.5f, mx.y - k), ImVec2(mx.x - k * 1.5f, mx.y),
                                                  EditorTheme::U32(acc), k * 0.5f);
    }
    ImGui::PopStyleColor(4);
    if (tooltipFn && ImGui::IsItemHovered()) tooltipFn(tooltip);
    return clicked;
}

// Quiet at rest (the dim glyph, no body); under the cursor the glyph turns red over a faint red
// wash, so a destructive control never shouts until it is about to be used.
inline bool DangerIconButton(const char* icon, const char* tooltip, TooltipFn tooltipFn,
                              ImVec2 size = ImVec2(0, 0)) {
    const ImVec4 danger = DangerColor();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 iconSize = ImGui::CalcTextSize(icon, nullptr, true);
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const ImVec2 btnSize(size.x > 0.0f ? size.x : iconSize.x + pad.x * 2.0f,
                          size.y > 0.0f ? size.y : iconSize.y + pad.y * 2.0f);
    const bool willHover = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseHoveringRect(cursor, ImVec2(cursor.x + btnSize.x, cursor.y + btnSize.y));
    ImGui::PushStyleColor(ImGuiCol_Text,          willHover ? danger : EditorTheme::Dim);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(danger.x, danger.y, danger.z, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(danger.x, danger.y, danger.z, 0.26f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushID(tooltip);
    const bool clicked = ImGui::Button(icon, size);
    ImGui::PopID();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (tooltipFn && ImGui::IsItemHovered()) tooltipFn(tooltip);
    return clicked;
}

// Defect #20 / #73: an unchecked box needs a visible edge. The frame's own hairline is faint on
// the near-black panels, so this draws a stronger one over the item afterwards (real
// ImGui::Checkbox handles all behaviour).
inline bool Checkbox(const char* label, bool* v) {
    const bool changed = ImGui::Checkbox(label, v);
    const ImVec2 mn = ImGui::GetItemRectMin();
    const float sz = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddRect(mn, ImVec2(mn.x + sz, mn.y + sz),
        EditorTheme::U32(*v ? EditorTheme::AccentDeep : EditorTheme::Strong), ImGui::GetStyle().FrameRounding, 0, 1.0f);
    return changed;
}

// The raised text button: everything that isn't a toolbar icon or a dialog's confirm.
inline bool SecondaryButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        EditorTheme::Raised);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  EditorTheme::Pressed);
    ImGui::PushStyleColor(ImGuiCol_Border,        EditorTheme::Strong);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// The one filled treatment: a white body with black text, for a dialog's confirming action (Save,
// Create, Apply, Restore...). Rare by design, so it always reads as "the" action.
inline bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        EditorTheme::Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::AccentBright);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  EditorTheme::AccentDeep);
    ImGui::PushStyleColor(ImGuiCol_Border,        EditorTheme::AccentDeep);
    ImGui::PushStyleColor(ImGuiCol_Text,          EditorTheme::OnAccent);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    return clicked;
}

// A segmented control: `count` mutually exclusive options drawn as one joined strip, the chosen one
// accent. Labels may be icons or short words. Returns true when `*current` changed.
inline bool Segmented(const char* id, int* current, const char* const* labels, int count, TooltipFn tooltipFn = nullptr,
                      const char* const* tooltips = nullptr, float itemWidth = 0.0f) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool on = *current == i;
        ImGui::PushStyleColor(ImGuiCol_Button,        on ? EditorTheme::AccentWash : EditorTheme::Field);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? EditorTheme::WithAlpha(EditorTheme::Accent, 0.20f) : EditorTheme::Hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  on ? EditorTheme::WithAlpha(EditorTheme::Accent, 0.28f) : EditorTheme::Pressed);
        ImGui::PushStyleColor(ImGuiCol_Text,          on ? EditorTheme::AccentBright : EditorTheme::Secondary);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushID(i);
        if (ImGui::Button(labels[i], ImVec2(itemWidth, 0.0f)) && !on) { *current = i; changed = true; }
        ImGui::PopID();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        if (tooltipFn && tooltips && tooltips[i] && ImGui::IsItemHovered()) tooltipFn(tooltips[i]);
    }
    const ImVec2 end = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(start, end, EditorTheme::U32(EditorTheme::Strong),
                                        EditorTheme::Px(3.0f), 0, 1.0f);
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return changed;
}

// A section heading in the launch screen's voice: tracked monospace capitals in the secondary text
// colour, an optional icon in the accent colour, and a hairline running out to the right edge. Replaces
// SeparatorText. `text` is set in capitals here; a Font Awesome glyph at its front (the old
// SeparatorText(ICON_FA_X "  Title") form) becomes the icon.
inline void SectionHeader(const char* rawText, const char* icon = nullptr) {
    char iconBuf[8] = {};
    if (!icon && rawText && (unsigned char)rawText[0] >= 0xE0) {
        unsigned int c = 0;
        const int len = ImTextCharFromUtf8(&c, rawText, nullptr);
        if (len > 0 && len < (int)sizeof(iconBuf) && c >= 0xE000 && c <= 0xF8FF) { // the icon font's private-use range
            std::memcpy(iconBuf, rawText, (size_t)len);
            icon = iconBuf;
            rawText += len;
            while (*rawText == ' ') ++rawText;
        }
    }
    char upper[160];
    {
        size_t n = 0;
        for (const char* s = rawText ? rawText : ""; *s && n + 1 < sizeof(upper); ++s)
            upper[n++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 'a' + 'A') : *s;
        upper[n] = 0;
    }
    const char* text = upper;
    ImGui::Dummy(ImVec2(0.0f, EditorTheme::Px(4.0f)));
    EditorTheme::PushHeading();
    const float lineH = ImGui::GetFontSize();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = pos.x;
    if (icon) {
        EditorTheme::PopFont();
        EditorTheme::PushSmall();
        const ImVec2 isz = ImGui::CalcTextSize(icon);
        dl->AddText(ImVec2(x, pos.y + (lineH - isz.y) * 0.5f), EditorTheme::U32(EditorTheme::Accent), icon);
        x += isz.x + EditorTheme::Px(7.0f);
        EditorTheme::PopFont();
        EditorTheme::PushHeading();
    }
    const float tracking = EditorTheme::HeadingTracking();
    const ImVec2 tsz = EditorTheme::CalcTrackedSize(text, tracking);
    EditorTheme::DrawTracked(dl, ImVec2(x, pos.y), EditorTheme::U32(EditorTheme::Secondary), text, tracking);
    x += tsz.x + EditorTheme::Px(10.0f);
    const float right = ImGui::GetContentRegionMax().x + ImGui::GetWindowPos().x;
    if (right > x) {
        const float y = std::floor(pos.y + lineH * 0.5f) + 0.5f;
        dl->AddLine(ImVec2(x, y), ImVec2(right, y), EditorTheme::U32(EditorTheme::Hairline), 1.0f);
    }
    ImGui::Dummy(ImVec2(std::max(1.0f, right - pos.x), lineH));
    EditorTheme::PopFont();
    ImGui::Dummy(ImVec2(0.0f, EditorTheme::Px(1.0f)));
}

// The one search box: a magnifier inside the field, the hint in the dim colour, and a clear (x)
// button once there is text. Same height as every other frame widget. `width` <= 0 fills the
// remaining row (minus -width). Returns true when the text changed (typing or clearing).
inline bool SearchField(const char* id, char* buf, size_t bufSize, const char* hint, float width = 0.0f) {
    ImGui::PushID(id);
    const float iconW = EditorTheme::Px(22.0f);
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    ImGui::SetNextItemWidth(width > 0.0f ? width : ImGui::GetContentRegionAvail().x + width);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad.x + iconW, pad.y));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, EditorTheme::Dim);
    bool changed = ImGui::InputTextWithHint("##search", hint, buf, bufSize);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    const bool focused = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    EditorTheme::PushSmall();
    const ImVec2 gsz = ImGui::CalcTextSize(ICON_FA_MAGNIFYING_GLASS);
    dl->AddText(ImVec2(mn.x + pad.x + (iconW - gsz.x) * 0.35f, (mn.y + mx.y - gsz.y) * 0.5f),
                EditorTheme::U32(focused || buf[0] ? EditorTheme::Accent : EditorTheme::Dim), ICON_FA_MAGNIFYING_GLASS);
    EditorTheme::PopFont();
    if (focused) dl->AddRect(mn, mx, EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Accent, 0.55f)),
                             ImGui::GetStyle().FrameRounding, 0, 1.0f);
    if (buf[0]) {
        // The clear button, inside the field's right end.
        const float h = mx.y - mn.y;
        const ImVec2 bmin(mx.x - h, mn.y), bmax(mx.x, mx.y);
        const bool hov = ImGui::IsMouseHoveringRect(bmin, bmax) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        EditorTheme::PushSmall();
        const ImVec2 xsz = ImGui::CalcTextSize(ICON_FA_XMARK);
        dl->AddText(ImVec2((bmin.x + bmax.x - xsz.x) * 0.5f, (bmin.y + bmax.y - xsz.y) * 0.5f),
                    EditorTheme::U32(hov ? EditorTheme::Text : EditorTheme::Dim), ICON_FA_XMARK);
        EditorTheme::PopFont();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            buf[0] = '\0';
            changed = true;
            ImGui::ClearActiveID();
        }
    }
    ImGui::PopID();
    return changed;
}

// A small rounded tag: a count, a state, a type. `tint` colours the text and a faint body.
inline void Chip(const char* text, ImVec4 tint) {
    EditorTheme::PushSmall();
    const ImVec2 tsz = ImGui::CalcTextSize(text);
    const ImVec2 pad(EditorTheme::Px(6.0f), EditorTheme::Px(1.0f));
    const float h = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 mn(pos.x, pos.y + (h - tsz.y - pad.y * 2.0f) * 0.5f);
    const ImVec2 mx(mn.x + tsz.x + pad.x * 2.0f, mn.y + tsz.y + pad.y * 2.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(mn, mx, EditorTheme::U32(EditorTheme::WithAlpha(tint, 0.14f)), EditorTheme::Px(3.0f));
    dl->AddText(ImVec2(mn.x + pad.x, mn.y + pad.y), EditorTheme::U32(tint), text);
    ImGui::Dummy(ImVec2(mx.x - mn.x, h));
    EditorTheme::PopFont();
}

// What a panel shows when it has nothing to show: a large dim icon, a line of text and a hint,
// centred in the remaining space.
inline void EmptyState(const char* icon, const char* text, const char* hint = nullptr) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(EditorTheme::UiFont(), EditorTheme::Px(28.0f));
    const ImVec2 isz = ImGui::CalcTextSize(icon);
    ImGui::PopFont();
    EditorTheme::PushBody();
    const ImVec2 tsz = ImGui::CalcTextSize(text);
    EditorTheme::PopFont();
    EditorTheme::PushSmall();
    const ImVec2 hsz = hint ? ImGui::CalcTextSize(hint, nullptr, false, avail.x - EditorTheme::Px(24.0f)) : ImVec2(0, 0);
    EditorTheme::PopFont();
    const float gap = EditorTheme::Px(10.0f);
    const float total = isz.y + gap + tsz.y + (hint ? EditorTheme::Px(4.0f) + hsz.y : 0.0f);
    float y = origin.y + std::max(EditorTheme::Px(12.0f), (avail.y - total) * 0.42f);
    const float cx = origin.x + avail.x * 0.5f;
    dl->AddText(EditorTheme::UiFont(), EditorTheme::Px(28.0f), ImVec2(cx - isz.x * 0.5f, y),
                EditorTheme::U32(EditorTheme::Strong), icon);
    y += isz.y + gap;
    dl->AddText(EditorTheme::UiFont(), EditorTheme::BodySize(), ImVec2(cx - tsz.x * 0.5f, y),
                EditorTheme::U32(EditorTheme::Secondary), text);
    y += tsz.y;
    if (hint) {
        y += EditorTheme::Px(4.0f);
        dl->AddText(EditorTheme::UiFont(), EditorTheme::SmallSize(), ImVec2(cx - hsz.x * 0.5f, y),
                    EditorTheme::U32(EditorTheme::Dim), hint, nullptr, avail.x - EditorTheme::Px(24.0f));
        y += hsz.y;
    }
    ImGui::Dummy(ImVec2(avail.x, std::max(0.0f, y - origin.y)));
}

// A panel's top toolbar strip: a Raised band, full width, with a hairline under it. Everything
// between Begin and End lays out on one row as usual (SameLine); End closes the band.
inline void BeginPanelToolbar(const char* id) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight() + EditorTheme::Px(8.0f);
    const ImVec2 wpos = ImGui::GetWindowPos();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float left = wpos.x, right = wpos.x + ImGui::GetWindowWidth();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float top = cur.y - st.WindowPadding.y;
    dl->AddRectFilled(ImVec2(left, top), ImVec2(right, cur.y + h - st.WindowPadding.y + EditorTheme::Px(4.0f)),
                      EditorTheme::U32(EditorTheme::Raised));
    dl->AddLine(ImVec2(left, cur.y + h - st.WindowPadding.y + EditorTheme::Px(4.0f) - 0.5f),
                ImVec2(right, cur.y + h - st.WindowPadding.y + EditorTheme::Px(4.0f) - 0.5f),
                EditorTheme::U32(EditorTheme::Hairline));
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y - st.WindowPadding.y + EditorTheme::Px(4.0f)));
    ImGui::BeginGroup();
}
inline void EndPanelToolbar() {
    ImGui::EndGroup();
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, EditorTheme::Px(6.0f)));
}


// A row highlight for custom lists (Hierarchy, Console, Asset list): the selection is an accent wash
// with a 2px accent bar at the left edge; hover is a faint white wash. Call before drawing the row's
// content, with the row's full-width rect.
inline void DrawRowState(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool selected, bool hovered) {
    if (selected) {
        dl->AddRectFilled(mn, mx, EditorTheme::U32(EditorTheme::AccentWash));
        dl->AddRectFilled(mn, ImVec2(mn.x + EditorTheme::Px(2.0f), mx.y), EditorTheme::U32(EditorTheme::Accent));
    } else if (hovered) {
        dl->AddRectFilled(mn, mx, EditorTheme::U32(FlatHover()));
    }
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
    Chip(ICON_FA_FILM " SCENE", InfoColor());
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
inline constexpr ImU32 kHudTextColor         = IM_COL32(0xE6, 0xE8, 0xE6, 255); // EditorTheme::Text
inline constexpr ImU32 kHudTextDisabledColor = IM_COL32(0xE6, 0xE8, 0xE6, 150);
inline constexpr ImU32 kHudPlateColor        = IM_COL32(0x00, 0x00, 0x00, 219); // EditorTheme::HudPlate

// Fills `mn`..`mx` with the standard HUD plate colour. Pass the tight bounding box of the
// content that sits on top (e.g. from ImGui::CalcTextSize / ImFont::CalcTextSizeA), already
// padded — this does not add its own margin, since callers pad differently (a text hint vs. a
// multi-line stats block).
inline void DrawHudPlate(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding = 4.0f) {
    dl->AddRectFilled(mn, mx, kHudPlateColor, rounding);
    dl->AddRect(mn, mx, EditorTheme::U32(EditorTheme::Hairline), rounding, 0, 1.0f);
}

// --- Depth, chips and rows (Editor Enhancers polish pass) ------------------------------------
// A soft drop shadow under a floating card or popover: rounded rings, fading out over `spread`,
// nudged down a little as if lit from above. Draw it before the card.
inline void DrawSoftShadow(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding, float spread, float alpha = 0.55f) {
    constexpr int kSteps = 6;
    const float drop = spread * 0.3f;
    for (int i = kSteps; i >= 1; --i) {
        const float t = (float)i / (float)kSteps; // outermost ring first
        const float grow = spread * t;
        const float a = alpha * (1.0f - t) * (1.0f - t) * (2.0f / kSteps) + alpha * 0.02f;
        dl->AddRectFilled(ImVec2(mn.x - grow, mn.y - grow + drop), ImVec2(mx.x + grow, mx.y + grow + drop),
                          IM_COL32(0, 0, 0, (int)(std::min(1.0f, a) * 255.0f)), rounding + grow);
    }
}

// Text cut with an ellipsis at `maxX`; `pos` is its top-left.
inline void TextEllipsis(ImDrawList* dl, ImVec2 pos, float maxX, ImU32 col, const char* text) {
    const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, true);
    if (pos.x + ts.x <= maxX) { dl->AddText(pos, col, text, ImGui::FindRenderedTextEnd(text)); return; }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::RenderTextEllipsis(dl, pos, ImVec2(maxX, pos.y + ts.y), maxX, text, nullptr, &ts);
    ImGui::PopStyleColor();
}

// A dashed rectangle outline (axis-aligned dashes, pixel-snapped): a drop target waiting for a drop.
inline void DrawDashedRect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float dash = 0.0f, float gap = 0.0f) {
    dash = dash > 0.0f ? dash : EditorTheme::Px(5.0f);
    gap = gap > 0.0f ? gap : EditorTheme::Px(3.0f);
    const float t = std::max(1.0f, std::floor(EditorTheme::Px(1.0f)));
    mn = ImVec2(std::floor(mn.x), std::floor(mn.y));
    mx = ImVec2(std::floor(mx.x), std::floor(mx.y));
    for (float x = mn.x; x < mx.x; x += dash + gap) {
        const float x1 = std::min(x + dash, mx.x);
        dl->AddRectFilled(ImVec2(x, mn.y), ImVec2(x1, mn.y + t), col);
        dl->AddRectFilled(ImVec2(x, mx.y - t), ImVec2(x1, mx.y), col);
    }
    for (float y = mn.y; y < mx.y; y += dash + gap) {
        const float y1 = std::min(y + dash, mx.y);
        dl->AddRectFilled(ImVec2(mn.x, y), ImVec2(mn.x + t, y1), col);
        dl->AddRectFilled(ImVec2(mx.x - t, y), ImVec2(mx.x, y1), col);
    }
}

// A disclosure chevron drawn as a stroke so it can turn: `angle` 0 points right, pi/2 points down.
// `size` is its height when pointing right.
inline void DrawChevron(ImDrawList* dl, ImVec2 c, float size, float angle, ImU32 col, float thickness = 0.0f) {
    const float s = size * 0.5f, cs = std::cos(angle), sn = std::sin(angle);
    const ImVec2 base[3] = {ImVec2(-s * 0.5f, -s), ImVec2(s * 0.5f, 0.0f), ImVec2(-s * 0.5f, s)};
    ImVec2 pts[3];
    for (int i = 0; i < 3; ++i)
        pts[i] = ImVec2(c.x + base[i].x * cs - base[i].y * sn, c.y + base[i].x * sn + base[i].y * cs);
    dl->AddPolyline(pts, 3, col, ImDrawFlags_None, thickness > 0.0f ? thickness : EditorTheme::Px(1.5f));
}

// A framed foldout (a TreeNode that pushes, so the caller TreePops while it is open): the raised
// strip with a hairline, a stroke chevron that turns as it opens, and the label. ImGui's own filled
// triangle and label are hidden; the item keeps its label, so its ID and test lookups don't change.
inline bool FramedFoldout(const char* label, ImGuiTreeNodeFlags flags = 0) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float x0 = ImGui::GetCursorScreenPos().x;
    ImGui::PushStyleColor(ImGuiCol_Header,        EditorTheme::Raised);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::Hover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  EditorTheme::Pressed);
    ImGui::PushStyleColor(ImGuiCol_Border,        EditorTheme::Hairline);
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool open = ImGui::TreeNodeEx(label, flags | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth);
    ImGui::PopStyleColor(5);
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetFontSize();
    const float cy = std::floor((mn.y + mx.y) * 0.5f);
    const bool hov = ImGui::IsItemHovered();
    const float t = EditorTheme::AnimT(ImGui::GetItemID() ^ 0x5F01D0E7u, open, EditorTheme::MotionNormal);
    DrawChevron(dl, ImVec2(std::floor(x0 + style.FramePadding.x + fs * 0.5f) + 0.5f, cy + 0.5f), EditorTheme::Px(8.0f),
                t * 1.5707963f, EditorTheme::U32(hov ? EditorTheme::Secondary : EditorTheme::Dim));
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    dl->AddText(ImVec2(x0 + fs + style.FramePadding.x * 2.0f, std::floor(cy - ts.y * 0.5f)), EditorTheme::U32(EditorTheme::Text), label, end);
    return open;
}
// A collapsible section inside a panel (ImGui::CollapsingHeader semantics: no TreePop) in the same look.
inline bool Foldout(const char* label, ImGuiTreeNodeFlags flags = 0) {
    return FramedFoldout(label, flags | ImGuiTreeNodeFlags_CollapsingHeader);
}

// A small status pill drawn at `mn` (top-left): mono capitals in `col` on a faint wash of it with
// a soft edge ("KEEP", "OFF"). Returns its width.
inline float DrawStatusPill(ImDrawList* dl, ImVec2 mn, const char* text, ImVec4 col, float alpha = 1.0f) {
    EditorTheme::PushMonoSmall();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 pad(EditorTheme::Px(5.0f), EditorTheme::Px(1.5f));
    const ImVec2 mx(mn.x + ts.x + pad.x * 2.0f, mn.y + ts.y + pad.y * 2.0f);
    const float r = (mx.y - mn.y) * 0.5f;
    dl->AddRectFilled(mn, mx, EditorTheme::U32(EditorTheme::WithAlpha(col, 0.14f * alpha)), r);
    dl->AddRect(mn, mx, EditorTheme::U32(EditorTheme::WithAlpha(col, 0.45f * alpha)), r, 0, 1.0f);
    dl->AddText(ImVec2(mn.x + pad.x, mn.y + pad.y), EditorTheme::U32(EditorTheme::WithAlpha(col, col.w * alpha)), text);
    EditorTheme::PopFont();
    return mx.x - mn.x;
}
inline ImVec2 StatusPillSize(const char* text) {
    EditorTheme::PushMonoSmall();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    EditorTheme::PopFont();
    return ImVec2(ts.x + EditorTheme::Px(10.0f), ts.y + EditorTheme::Px(3.0f));
}

// A user colour's mark at a row's or tile's left edge: a slim rounded bar.
inline void DrawColorStripe(ImDrawList* dl, float x, float y0, float y1, ImU32 col) {
    const float w = EditorTheme::UserStripeW();
    dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + w, y1), col | IM_COL32_A_MASK, w * 0.5f);
}

// A clickable pill: page chips, variant chips, "+N". `selected` reads as an accent wash with bright
// text; hover eases in. `tint` (a packed user colour, or 0) adds a small dot before the label.
// The label is cut with an ellipsis past `maxW` (0 = no limit). Returns true when clicked.
inline bool Pill(const char* id, const char* icon, const char* label, bool selected, float maxW = 0.0f,
                 unsigned tint = 0, bool dim = false, bool* hoveredOut = nullptr) {
    EditorTheme::PushSmall();
    const float padX = EditorTheme::Px(9.0f);
    const float dot = tint ? EditorTheme::Px(12.0f) : 0.0f;
    const ImVec2 is = icon ? ImGui::CalcTextSize(icon) : ImVec2(0, 0);
    const float iconGap = icon ? EditorTheme::Px(6.0f) : 0.0f;
    const ImVec2 ls = ImGui::CalcTextSize(label, nullptr, true);
    const float h = ImGui::GetFrameHeight() - EditorTheme::Px(4.0f);
    float w = padX * 2.0f + dot + is.x + iconGap + ls.x;
    if (maxW > 0.0f && w > maxW) w = maxW;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 mn(p.x, p.y + EditorTheme::Px(2.0f));
    ImGui::SetCursorScreenPos(mn);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    if (hoveredOut) *hoveredOut = hov;
    const float ht = EditorTheme::AnimT(ImGui::GetItemID(), hov || held);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mx(mn.x + w, mn.y + h);
    ImVec4 bg = selected ? EditorTheme::AccentWash : EditorTheme::Raised;
    bg = EditorTheme::Mix(bg, selected ? EditorTheme::WithAlpha(EditorTheme::Accent, 0.20f) : EditorTheme::Hover, ht);
    if (held) bg = selected ? EditorTheme::WithAlpha(EditorTheme::Accent, 0.28f) : EditorTheme::Pressed;
    dl->AddRectFilled(mn, mx, EditorTheme::U32(bg), h * 0.5f);
    if (selected) dl->AddRect(mn, mx, EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Accent, 0.35f)), h * 0.5f, 0, 1.0f);
    const ImVec4 tc = dim ? EditorTheme::Dim
                    : selected ? EditorTheme::AccentBright : EditorTheme::Mix(EditorTheme::Secondary, EditorTheme::Text, ht);
    float x = mn.x + padX;
    const float cy = (mn.y + mx.y) * 0.5f;
    if (tint) {
        const float r = EditorTheme::Px(3.0f);
        dl->AddCircleFilled(ImVec2(x + r, cy), r, EditorTheme::U32(EditorTheme::UserGlyphColor(tint)), 12);
        x += dot;
    }
    if (icon) { dl->AddText(ImVec2(x, cy - is.y * 0.5f), EditorTheme::U32(tc), icon); x += is.x + iconGap; }
    TextEllipsis(dl, ImVec2(x, cy - ls.y * 0.5f), mx.x - padX * 0.6f, EditorTheme::U32(tc), label);
    EditorTheme::PopFont();
    return clicked;
}

// A keyboard hint drawn as a key: a small raised cap with a hairline edge and the key's name in the
// mono face. An item, so it lays out with SameLine like text.
inline void KeyCap(const char* key) {
    EditorTheme::PushMonoSmall();
    const ImVec2 ts = ImGui::CalcTextSize(key);
    const ImVec2 pad(EditorTheme::Px(5.0f), EditorTheme::Px(1.5f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 mx(p.x + ts.x + pad.x * 2.0f, p.y + ts.y + pad.y * 2.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = EditorTheme::Px(3.0f);
    dl->AddRectFilled(p, mx, EditorTheme::U32(EditorTheme::Raised), r);
    dl->AddRect(p, mx, EditorTheme::U32(EditorTheme::Hairline), r, 0, 1.0f);
    dl->AddLine(ImVec2(p.x + r, mx.y - 0.5f), ImVec2(mx.x - r, mx.y - 0.5f), EditorTheme::U32(EditorTheme::Strong), 1.0f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), EditorTheme::U32(EditorTheme::Secondary), key);
    ImGui::Dummy(ImVec2(mx.x - p.x, mx.y - p.y));
    EditorTheme::PopFont();
}

// The background of a styled tree row (Hierarchy, folder tree): the user colour as a stripe at the
// row's indent and a faint wash, then hover and selection over it, so a selected row always reads
// as selected whatever its colour. `fill`: 0 = icon only (no stripe, no wash), 1 = flat, 2 = gradient.
// `selT` / `hovT` are 0..1 fades (EditorTheme::AnimT); the wash eases back as the row is selected.
inline void DrawStyledRowBackground(ImDrawList* dl, ImVec2 rowMin, ImVec2 rowMax, float colorX, unsigned color,
                                    int fill, float selT, float hovT) {
    if (color && fill != 0) {
        const ImU32 rgb = color & 0x00FFFFFFu;
        const float washK = 1.0f - 0.65f * selT;
        if (fill == 1) {
            dl->AddRectFilled(ImVec2(colorX, rowMin.y), rowMax, rgb | ((ImU32)(EditorTheme::UserFlatAlpha * washK * 255.0f) << 24));
        } else {
            const float x1 = colorX + (rowMax.x - colorX) * 0.6f;
            const ImU32 a = rgb | ((ImU32)(EditorTheme::UserWashAlpha * washK * 255.0f) << 24);
            dl->AddRectFilledMultiColor(ImVec2(colorX, rowMin.y), ImVec2(x1, rowMax.y), a, rgb, rgb, a);
        }
        DrawColorStripe(dl, colorX, rowMin.y + EditorTheme::Px(3.0f), rowMax.y - EditorTheme::Px(3.0f), color);
    }
    if (hovT > 0.0f && selT < 1.0f)
        dl->AddRectFilled(rowMin, rowMax, EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Hover, hovT * (1.0f - selT) * 0.9f)));
    if (selT > 0.0f) {
        dl->AddRectFilled(rowMin, rowMax, EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Accent, 0.14f * selT)));
        dl->AddRectFilled(rowMin, ImVec2(rowMin.x + EditorTheme::Px(2.0f), rowMax.y),
                          EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Accent, selT)));
    }
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
