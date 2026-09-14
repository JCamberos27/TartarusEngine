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

namespace EditorUIPrimitives {

using TooltipFn = void (*)(const char*);

// The accent colour every "on" toggle state reads in — the same ImGui role (ImGuiCol_SliderGrab)
// the styled sliders use — kept as one named accessor rather than a raw ImGuiCol_* index
// sprinkled at every call site. Matches docs/CONVENTIONS.md's accent-discipline table: cyan =
// selection / "you are here".
inline ImVec4 AccentColor() { return ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab); }

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
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.20f, 0.20f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.82f, 0.24f, 0.24f, 1.00f));
    ImGui::PushID(tooltip);
    bool clicked = ImGui::Button(icon, size);
    ImGui::PopID();
    ImGui::PopStyleColor(3);
    if (tooltipFn && ImGui::IsItemHovered()) tooltipFn(tooltip);
    return clicked;
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

} // namespace EditorUIPrimitives
