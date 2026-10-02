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

// The accent every "on" state, selection and focus reads in: the launch screen's gold
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
//                   low-frequency icon actions. `active` = a toggle that is on: gold glyph on a
//                   gold wash with a gold keyline.
//   SecondaryButton a raised body with a hairline: ordinary text buttons (Cancel, Import...,
//                   Add Component).
//   PrimaryButton   a gold body with dark text: the one confirming action of a dialog.
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

inline bool DangerIconButton(const char* icon, const char* tooltip, TooltipFn tooltipFn,
                              ImVec2 size = ImVec2(0, 0)) {
    const ImVec4 danger = DangerColor();
    ImGui::PushStyleColor(ImGuiCol_Text,          EditorTheme::Secondary);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(danger.x, danger.y, danger.z, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(danger.x, danger.y, danger.z, 1.00f));
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

// The one filled treatment: a gold body with dark text, for a dialog's confirming action (Save,
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
// gold. Labels may be icons or short words. Returns true when `*current` changed.
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
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? ImVec4(0.894f, 0.722f, 0.408f, 0.24f) : EditorTheme::Hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  on ? ImVec4(0.894f, 0.722f, 0.408f, 0.32f) : EditorTheme::Pressed);
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
// colour, an optional icon in gold, and a hairline running out to the right edge. Replaces
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

// A collapsible section inside a panel (ImGui::CollapsingHeader): the raised surface with a hairline
// instead of the gold selection fill CollapsingHeader would otherwise borrow from ImGuiCol_Header.
inline bool Foldout(const char* label, ImGuiTreeNodeFlags flags = 0) {
    ImGui::PushStyleColor(ImGuiCol_Header,        EditorTheme::Raised);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::Hover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  EditorTheme::Pressed);
    ImGui::PushStyleColor(ImGuiCol_Border,        EditorTheme::Hairline);
    const bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopStyleColor(4);
    return open;
}

// A row highlight for custom lists (Hierarchy, Console, Asset list): the selection is a gold wash
// with a 2px gold bar at the left edge; hover is a faint white wash. Call before drawing the row's
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
inline constexpr ImU32 kHudTextColor         = IM_COL32(0xEC, 0xEC, 0xF0, 255); // EditorTheme::Text
inline constexpr ImU32 kHudTextDisabledColor = IM_COL32(0xEC, 0xEC, 0xF0, 150);
inline constexpr ImU32 kHudPlateColor        = IM_COL32(0x0E, 0x0E, 0x11, 219); // EditorTheme::HudPlate

// Fills `mn`..`mx` with the standard HUD plate colour. Pass the tight bounding box of the
// content that sits on top (e.g. from ImGui::CalcTextSize / ImFont::CalcTextSizeA), already
// padded — this does not add its own margin, since callers pad differently (a text hint vs. a
// multi-line stats block).
inline void DrawHudPlate(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding = 4.0f) {
    dl->AddRectFilled(mn, mx, kHudPlateColor, rounding);
    dl->AddRect(mn, mx, EditorTheme::U32(EditorTheme::Hairline), rounding, 0, 1.0f);
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
