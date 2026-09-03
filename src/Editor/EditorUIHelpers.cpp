#include "EditorUIHelpers.h"
#include "EditorSettings.h"

#include <IconsFontAwesome6.h>
#include <imgui_internal.h>
#include <cstdarg>
#include <cstdio>

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

namespace {

// Shared body for EditorUI::SliderFloat / SliderInt. Draws a thin rounded track + a filled
// portion + a circular grab, then an editable numeric box on the right. Drag math and the grab
// position come from ImGui::SliderBehavior so keyboard, gamepad, Ctrl+click and range clamping
// all behave exactly like a stock slider; only the paint and the trailing box are custom.
bool SliderStyled(const char* label, ImGuiDataType dt, void* p_v,
                  const void* p_min, const void* p_max, const char* format, ImGuiSliderFlags flags) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);

    const float w_full = ImGui::CalcItemWidth();
    const float h = ImGui::GetFrameHeight();
    const bool show_box = (format && format[0] != '\0');

    // [ track .................. ]  gap  [ number box ]
    const float box_w = show_box
        ? ImMin(w_full * 0.42f, ImGui::CalcTextSize("-00000.00").x + style.FramePadding.x * 2.0f)
        : 0.0f;
    const float gap = show_box ? style.ItemInnerSpacing.x : 0.0f;
    const float track_w = ImMax(w_full - box_w - gap, h * 1.5f);

    const ImVec2 p0 = window->DC.CursorPos;
    ImRect frame_bb(p0, ImVec2(p0.x + track_w, p0.y + h));
    ImGui::ItemSize(frame_bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(frame_bb, id, &frame_bb))
        return false;

    const bool hovered = ImGui::ItemHoverable(frame_bb, id, g.LastItemData.ItemFlags);
    const bool clicked = hovered && g.IO.MouseClicked[0];
    if (clicked || g.NavActivateId == id) {
        ImGui::SetActiveID(id, window);
        ImGui::SetFocusID(id, window);
        ImGui::FocusWindow(window);
        g.ActiveIdUsingNavDirMask |= (1u << ImGuiDir_Left) | (1u << ImGuiDir_Right);
    }

    ImRect grab_bb;
    bool changed = ImGui::SliderBehavior(frame_bb, id, dt, p_v, p_min, p_max, format,
                                         flags | ImGuiSliderFlags_AlwaysClamp, &grab_bb);
    if (changed) ImGui::MarkItemEdited(id);

    // --- paint: thin track, filled portion, circular grab ---
    ImDrawList* dl = window->DrawList;
    const bool active = (g.ActiveId == id);
    const float cy = IM_ROUND(p0.y + h * 0.5f);
    const float track_th = ImMax(3.0f, IM_ROUND(h * 0.26f));
    const float knob_r = IM_ROUND(h * 0.36f);
    const float x0 = p0.x + knob_r;
    const float x1 = p0.x + track_w - knob_r;
    const float knob_cx = ImClamp(IM_ROUND((grab_bb.Min.x + grab_bb.Max.x) * 0.5f), x0, x1);

    const ImU32 col_track = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 col_fill  = ImGui::GetColorU32(active ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab);
    const ImU32 col_knob  = ImGui::GetColorU32((hovered || active) ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab);

    dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), col_track, track_th);
    if (knob_cx > x0 + 0.5f)
        dl->AddLine(ImVec2(x0, cy), ImVec2(knob_cx, cy), col_fill, track_th);
    dl->AddCircleFilled(ImVec2(knob_cx, cy), knob_r, col_knob, 0);
    if (hovered || active)
        dl->AddCircle(ImVec2(knob_cx, cy), knob_r, ImGui::GetColorU32(ImGuiCol_Text, 0.25f), 0, 1.5f);

    // --- editable numeric box ---
    if (show_box) {
        ImGui::SameLine(0.0f, gap);
        ImGui::SetNextItemWidth(box_w);
        char buf[64];
        ImGui::DataTypeFormatString(buf, IM_ARRAYSIZE(buf), dt, p_v, format);
        ImGui::PushID((int)id);
        const ImGuiInputTextFlags itf = ImGuiInputTextFlags_AutoSelectAll |
            ImGuiInputTextFlags_EnterReturnsTrue |
            ((dt == ImGuiDataType_Float || dt == ImGuiDataType_Double)
                ? ImGuiInputTextFlags_CharsScientific : ImGuiInputTextFlags_CharsDecimal);
        const bool entered = ImGui::InputText("##box", buf, IM_ARRAYSIZE(buf), itf);
        if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
            // Tolerate unit prefixes/suffixes baked into `format` ("x%.2f", "%.0f m", ...):
            // skip to the first character that can start a number, then parse.
            const char* s = buf;
            while (*s && !(*s == '-' || *s == '+' || *s == '.' || (*s >= '0' && *s <= '9'))) ++s;
            if (dt == ImGuiDataType_S32) {
                int nv = *(const int*)p_v;
                if (sscanf(s, "%d", &nv) == 1) {
                    nv = ImClamp(nv, *(const int*)p_min, *(const int*)p_max);
                    if (nv != *(int*)p_v) { *(int*)p_v = nv; changed = true; ImGui::MarkItemEdited(id); }
                }
            } else {
                float nv = *(const float*)p_v;
                if (sscanf(s, "%f", &nv) == 1) {
                    nv = ImClamp(nv, *(const float*)p_min, *(const float*)p_max);
                    if (nv != *(float*)p_v) { *(float*)p_v = nv; changed = true; ImGui::MarkItemEdited(id); }
                }
            }
        }
        ImGui::PopID();
    }

    // --- visible label (anything before a "##"), same as a stock slider ---
    const char* label_end = ImGui::FindRenderedTextEnd(label);
    if (label != label_end) {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::TextEx(label, label_end);
    }
    return changed;
}

} // namespace

bool EditorUI::SliderFloat(const char* label, float* v, float v_min, float v_max,
                           const char* format, ImGuiSliderFlags flags) {
    if (!format) format = "%.3f";
    return SliderStyled(label, ImGuiDataType_Float, v, &v_min, &v_max, format, flags);
}

bool EditorUI::SliderInt(const char* label, int* v, int v_min, int v_max,
                         const char* format, ImGuiSliderFlags flags) {
    if (!format) format = "%d";
    return SliderStyled(label, ImGuiDataType_S32, v, &v_min, &v_max, format, flags);
}
