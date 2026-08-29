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
