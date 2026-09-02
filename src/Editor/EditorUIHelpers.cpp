#include "EditorUIHelpers.h"
#include "EditorSettings.h"

#include <IconsFontAwesome6.h>
#include <cstdarg>

void EditorUI::SetTooltip(const char* fmt, ...) {
    // Checked first, before touching the hover state or the format string — a disabled
    // preference must bypass string formatting and the ImGui tooltip path entirely, not just
    // suppress the visible result.
    if (!EditorSettings::Get().ShowTooltips) return;
    if (!ImGui::IsItemHovered()) return;

    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(fmt, args);
    va_end(args);
}

void EditorUI::VSeparator(float gapScale) {
    const float gap = ImGui::GetStyle().ItemSpacing.x * gapScale;
    ImGui::SameLine(0.0f, gap);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(p.x, p.y), ImVec2(p.x, p.y + h),
        ImGui::GetColorU32(ImGuiCol_Separator));

    // Reserve a real 1px-wide item so the layout cursor advances past the rule and the caller's
    // following SameLine() spaces off it correctly (a bare SameLine here would measure from the
    // item *before* the rule).
    ImGui::Dummy(ImVec2(1.0f, h));
    ImGui::SameLine(0.0f, gap);
}

void EditorUI::HelpMarker(const char* desc) {
    if (!EditorSettings::Get().ShowTooltips) return;

    ImGui::SameLine();
    ImGui::TextDisabled(ICON_FA_CIRCLE_INFO);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
