#include "EditorPropertyRows.h"

#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"

#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>

namespace {

bool ContainsNoCase(const std::string& text, const char* needle) {
    const auto lower = [](unsigned char c) { return (char)std::tolower(c); };
    std::string a(text), b(needle);
    std::transform(a.begin(), a.end(), a.begin(), lower);
    std::transform(b.begin(), b.end(), b.begin(), lower);
    return a.find(b) != std::string::npos;
}

void Tip(const char* tip) {
    if (tip && *tip && ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", tip);
}

// The Transform rows' axis tints.
ImVec4 AxisTint(int i) {
    static const ImVec4 kTints[3] = {ImVec4(0.86f, 0.36f, 0.36f, 1.0f), ImVec4(0.45f, 0.78f, 0.42f, 1.0f),
                                     ImVec4(0.42f, 0.62f, 0.95f, 1.0f)};
    return kTints[std::clamp(i, 0, 2)];
}

} // namespace

bool PropertyRows::Commit(bool committed) {
    if (committed) m_Changed = true;
    return committed;
}

void PropertyRows::Label(const char* label, const char* tip) {
    const float x = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    Tip(tip);
    ImGui::SameLine(x + m_LabelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool PropertyRows::Float(const char* label, float& v, float speed, float lo, float hi, const char* fmt, const char* tip) {
    ImGui::PushID(label);
    Label(label, tip);
    ImGui::DragFloat("##v", &v, speed, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
    Tip(tip);
    const bool c = Commit(ImGui::IsItemDeactivatedAfterEdit());
    ImGui::PopID();
    return c;
}

bool PropertyRows::Int(const char* label, int& v, int lo, int hi, const char* tip) {
    ImGui::PushID(label);
    Label(label, tip);
    ImGui::DragInt("##v", &v, 0.2f, lo, hi, "%d", ImGuiSliderFlags_AlwaysClamp);
    Tip(tip);
    const bool c = Commit(ImGui::IsItemDeactivatedAfterEdit());
    ImGui::PopID();
    return c;
}

bool PropertyRows::Check(const char* label, bool& v, const char* tip) {
    ImGui::PushID(label);
    Label(label, tip);
    const bool c = Commit(EditorUIPrimitives::Checkbox("##v", &v));
    Tip(tip);
    ImGui::PopID();
    return c;
}

bool PropertyRows::Vec2(const char* label, glm::vec2& v, float speed, const char* fmt, const char* tip) {
    ImGui::PushID(label);
    Label(label, tip);
    float f[2] = {v.x, v.y};
    if (ImGui::DragFloat2("##v", f, speed, 0.0f, 0.0f, fmt)) v = {f[0], f[1]};
    Tip(tip);
    const bool c = Commit(ImGui::IsItemDeactivatedAfterEdit());
    ImGui::PopID();
    return c;
}

bool PropertyRows::Range(const char* label, glm::vec2& v, float speed, const char* fmt, const char* tip) {
    ImGui::PushID(label);
    Label(label, tip);
    float f[2] = {v.x, v.y};
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(" to ").x - ImGui::GetStyle().ItemSpacing.x * 2.0f) * 0.5f;
    ImGui::SetNextItemWidth(w);
    ImGui::DragFloat("##min", &f[0], speed, 0.0f, 0.0f, fmt);
    Tip(tip);
    bool c = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    ImGui::TextDisabled("to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##max", &f[1], speed, 0.0f, 0.0f, fmt);
    Tip(tip);
    c |= ImGui::IsItemDeactivatedAfterEdit();
    // Ordered only when the edit ends: swapping every frame handed the dragged field the other's role mid-drag.
    v = c ? glm::vec2(std::min(f[0], f[1]), std::max(f[0], f[1])) : glm::vec2(f[0], f[1]);
    ImGui::PopID();
    return Commit(c);
}

bool PropertyRows::Vec3(const char* label, glm::vec3& v, float speed, const char* fmt, const char* tip, const char* axes) {
    ImGui::PushID(label);
    Label(label, tip);
    const ImGuiStyle& st = ImGui::GetStyle();
    // The fields share what the three letters leave, so the row ends flush with the others.
    float marks = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const char letter[2] = {axes && axes[i] ? axes[i] : "XYZ"[i], '\0'};
        marks += ImGui::CalcTextSize(letter).x;
    }
    const float w = (ImGui::GetContentRegionAvail().x - marks - 2.0f * st.ItemSpacing.x - 3.0f * st.ItemInnerSpacing.x) / 3.0f;
    bool c = false;
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        if (i) ImGui::SameLine();
        const char letter[2] = {axes && axes[i] ? axes[i] : "XYZ"[i], '\0'};
        ImGui::PushStyleColor(ImGuiCol_Text, AxisTint(i));
        ImGui::TextUnformatted(letter);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("##a", &v[i], speed, 0.0f, 0.0f, fmt);
        Tip(tip);
        c |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopID();
    }
    ImGui::PopID();
    return Commit(c);
}

bool PropertyRows::Spring(const char* label, float& frequency, float& damping, const char* tip) {
    ImGui::PushID(label);
    Label(label, tip);
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::SetNextItemWidth(w);
    ImGui::DragFloat("##f", &frequency, 0.05f, 0.1f, 100.0f, "%.1f Hz", ImGuiSliderFlags_AlwaysClamp);
    Tip(tip);
    bool c = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##d", &damping, 0.01f, 0.0f, 5.0f, "damping %.2f", ImGuiSliderFlags_AlwaysClamp);
    Tip(tip);
    c |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();
    return Commit(c);
}

bool PropertyRows::Text(const char* label, std::string& v, const char* tip) {
    ImGui::PushID(label);
    Label(label, tip);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", v.c_str());
    ImGui::InputText("##v", buf, sizeof buf);
    Tip(tip);
    bool c = false;
    if (ImGui::IsItemDeactivatedAfterEdit() && v != buf) {
        v = buf;
        c = true;
    }
    ImGui::PopID();
    return Commit(c);
}

bool PropertyRows::Name(const char* label, std::string& v, const std::vector<std::string>& items, bool validate,
                        const char* tip, const char* unknownTip) {
    ImGui::PushID(label);
    Label(label, tip);
    const bool unknown = validate && !v.empty() && std::find(items.begin(), items.end(), v) == items.end();
    const float pickW = ImGui::GetFrameHeight();
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", v.c_str());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - pickW - ImGui::GetStyle().ItemSpacing.x);
    if (unknown) ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::WarningColor());
    ImGui::InputText("##v", buf, sizeof buf);
    if (unknown) ImGui::PopStyleColor();
    bool c = false;
    if (ImGui::IsItemDeactivatedAfterEdit() && v != buf) {
        v = buf;
        c = true;
    }
    if (ImGui::IsItemHovered()) {
        if (unknown && unknownTip) EditorUI::SetTooltip(unknownTip, v.c_str());
        else if (tip) EditorUI::SetTooltip("%s", tip);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(pickW);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 14.0f, 0.0f), ImVec2(FLT_MAX, ImGui::GetFontSize() * 24.0f));
    if (ImGui::BeginCombo("##pick", nullptr, ImGuiComboFlags_NoPreview)) {
        static char filter[64] = "";
        if (ImGui::IsWindowAppearing()) {
            filter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "Type to filter", filter, sizeof filter);
        if (items.empty()) ImGui::TextDisabled("(nothing to pick from)");
        for (const std::string& item : items) {
            if (filter[0] && !ContainsNoCase(item, filter)) continue;
            if (ImGui::Selectable(item.c_str(), item == v) && item != v) {
                v = item;
                c = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Pick from the list");
    ImGui::PopID();
    return Commit(c);
}

void PropertyRows::Value(const char* label, const char* text, const char* tip) {
    Label(label, tip);
    ImGui::TextDisabled("%s", text);
    Tip(tip);
}

bool PropertyRows::Section(const char* icon, const char* title, const char* summary, bool defaultOpen) {
    char header[160];
    std::snprintf(header, sizeof header, "%s  %s###sec_%s", icon ? icon : "", title, title);
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const bool open = ImGui::CollapsingHeader(header, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    if (summary && *summary) {
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const ImVec2 ts = ImGui::CalcTextSize(summary);
        const float titleEnd = min.x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(header, nullptr, true).x + ts.y;
        const float x = std::max(titleEnd, right - ts.x - ImGui::GetStyle().FramePadding.x);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(ImVec2(titleEnd, min.y), max, true);
        dl->AddText(ImVec2(x, min.y + (max.y - min.y - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), summary);
        dl->PopClipRect();
    }
    if (open) ImGui::Spacing();
    return open;
}

void PropertyRows::Heading(const char* text) {
    ImGui::Spacing();
    ImGui::SeparatorText(text);
}

void PropertyRows::Note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void PropertyRows::ResetButton(const char* what, const std::function<void()>& apply) {
    ImGui::PushID(what);
    char tip[128];
    std::snprintf(tip, sizeof tip, "Put every %s setting back to the engine default", what);
    ImGui::Spacing();
    if (EditorInternal::ActionButton(ICON_FA_ROTATE_LEFT "  Reset to Defaults", tip, false, ImVec2(-FLT_MIN, 0.0f)))
        ImGui::OpenPopup("##reset");
    if (ImGui::BeginPopup("##reset")) {
        ImGui::Text("Reset all %s settings to the defaults?", what);
        ImGui::TextDisabled("Undo in the header brings them back.");
        if (EditorInternal::PrimaryButton("Reset")) {
            apply();
            m_Changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorInternal::PrimaryButton("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

ImVec4 PropertyRows::StatusColor(Status status) {
    switch (status) {
        case Status::Ok: return EditorUIPrimitives::SuccessColor();
        case Status::Info: return EditorUIPrimitives::InfoColor();
        case Status::Warning: return EditorUIPrimitives::WarningColor();
        default: return EditorUIPrimitives::DangerColor();
    }
}

void PropertyRows::Badge(Status status, const char* text, const char* tip) {
    const char* icon = status == Status::Ok        ? ICON_FA_CIRCLE_CHECK
                       : status == Status::Info    ? ICON_FA_CIRCLE_INFO
                       : status == Status::Warning ? ICON_FA_TRIANGLE_EXCLAMATION
                                                   : ICON_FA_CIRCLE_XMARK;
    const ImVec4 col = StatusColor(status);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s  %s", icon, text);
    const ImVec2 pad(ImGui::GetStyle().FramePadding.x * 0.8f, ImGui::GetStyle().FramePadding.y * 0.5f);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + ts.x + pad.x * 2.0f, p.y + ts.y + pad.y * 2.0f);
    dl->AddRectFilled(p, q, ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.14f)), ts.y * 0.5f);
    dl->AddRect(p, q, ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.45f)), ts.y * 0.5f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::GetColorU32(col), buf);
    ImGui::Dummy(ImVec2(q.x - p.x, q.y - p.y));
    Tip(tip);
}
