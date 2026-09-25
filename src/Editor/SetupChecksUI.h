#pragma once

#include "EditorUIPrimitives.h"
#include "FirstPersonBodyContract.h"

#include <IconsFontAwesome6.h>
#include <imgui.h>

#include <vector>

// The "Setup" box the First Person Body component and the weapon Inspector share: a header line (a
// green tick when nothing needs attention, else how many problems), and a collapsible list of every
// check with what to do about it. Info notes stay quiet (they never make the header alarming).
namespace SetupChecksUI {

inline void Draw(const std::vector<FPBody::Check>& checks, const char* okText, const char* id) {
    const FPBody::Severity worst = FPBody::Worst(checks);
    int problems = 0;
    for (const FPBody::Check& c : checks)
        if ((int)c.Level >= (int)FPBody::Severity::Warning) ++problems;
    ImVec4 col = EditorUIPrimitives::SuccessColor();
    const char* icon = ICON_FA_CIRCLE_CHECK;
    if (worst == FPBody::Severity::Error) { col = EditorUIPrimitives::DangerColor(); icon = ICON_FA_CIRCLE_XMARK; }
    else if (problems > 0) { col = EditorUIPrimitives::WarningColor(); icon = ICON_FA_TRIANGLE_EXCLAMATION; }
    ImGui::TextColored(col, "%s", icon);
    ImGui::SameLine();
    ImGui::TextUnformatted(problems == 0 ? okText : (problems == 1 ? "Setup: 1 problem" : "Setup: problems"));
    if (problems > 1) { ImGui::SameLine(); ImGui::TextDisabled("(%d)", problems); }
    if (checks.empty()) return;
    if (ImGui::TreeNodeEx(id, (problems > 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0) | ImGuiTreeNodeFlags_SpanAvailWidth,
                          "Details (%d)", (int)checks.size())) {
        for (const FPBody::Check& c : checks) {
            ImVec4 cc = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            const char* ci = ICON_FA_CIRCLE_INFO;
            if (c.Level == FPBody::Severity::Error) { cc = EditorUIPrimitives::DangerColor(); ci = ICON_FA_CIRCLE_XMARK; }
            else if (c.Level == FPBody::Severity::Warning) { cc = EditorUIPrimitives::WarningColor(); ci = ICON_FA_TRIANGLE_EXCLAMATION; }
            ImGui::TextColored(cc, "%s", ci);
            ImGui::SameLine();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(c.Message.c_str());
            if (!c.Hint.empty()) ImGui::TextDisabled("    %s", c.Hint.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TreePop();
    }
}

} // namespace SetupChecksUI
