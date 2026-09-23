#include "CurveEditor.h"
#include "EditorUIHelpers.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace CurveEditor {

namespace {

// Per-widget state kept in ImGui's storage, keyed off the widget id.
struct State {
    ImGuiID Id;
    int Selected() const { return ImGui::GetStateStorage()->GetInt(Id ^ 0x51u, -1); }
    void SetSelected(int i) { ImGui::GetStateStorage()->SetInt(Id ^ 0x51u, i); }
    // 0 none, 1 key, 2 in-handle, 3 out-handle
    int Drag() const { return ImGui::GetStateStorage()->GetInt(Id ^ 0x52u, 0); }
    void SetDrag(int d) { ImGui::GetStateStorage()->SetInt(Id ^ 0x52u, d); }
    float Lo() const { return ImGui::GetStateStorage()->GetFloat(Id ^ 0x53u, 0.0f); }
    float Hi() const { return ImGui::GetStateStorage()->GetFloat(Id ^ 0x54u, 1.0f); }
    void SetRange(float lo, float hi) {
        ImGui::GetStateStorage()->SetFloat(Id ^ 0x53u, lo);
        ImGui::GetStateStorage()->SetFloat(Id ^ 0x54u, hi);
    }
};

void FitRange(const Curve& c, const Options& o, float& lo, float& hi) {
    lo = 0.0f;
    hi = 0.0f;
    for (int i = 0; i <= 64; ++i) {
        const float v = c.Evaluate(o.TimeMin + (o.TimeMax - o.TimeMin) * (float)i / 64.0f);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    for (const CurveKey& k : c.Keys) { lo = std::min(lo, k.Value); hi = std::max(hi, k.Value); }
    const float span = hi - lo;
    if (span < 1e-6f) {
        const float pad = std::max(std::fabs(hi), 1e-3f);
        lo -= pad;
        hi += pad;
    } else {
        lo -= span * 0.12f;
        hi += span * 0.12f;
    }
}

float PeakOf(const Curve& c, float fallback) {
    float peak = 0.0f;
    for (const CurveKey& k : c.Keys) peak = std::max(peak, std::fabs(k.Value));
    return peak > 1e-6f ? peak : fallback;
}

} // namespace

bool Draw(const char* id, Curve& curve, const ImVec2& sizeIn, const Options& o) {
    ImGui::PushID(id);
    State st{ImGui::GetID("##curve")};
    bool committed = false;

    const float t0 = o.TimeMin, t1 = std::max(o.TimeMax, o.TimeMin + 1e-4f);
    ImVec2 size = sizeIn;
    if (size.x <= 0.0f) size.x = std::max(ImGui::GetContentRegionAvail().x, 50.0f);
    if (size.y <= 0.0f) size.y = ImGui::GetFontSize() * 6.0f;

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##curve", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);

    // Value range: refit unless a drag is in progress (so the key doesn't run from the mouse).
    float lo, hi;
    if (st.Drag() == 0) {
        FitRange(curve, o, lo, hi);
        st.SetRange(lo, hi);
    } else {
        lo = st.Lo();
        hi = st.Hi();
    }
    const float pad = ImGui::GetFontSize() * 0.4f;
    const auto toScreen = [&](float t, float v) {
        return ImVec2(p0.x + pad + (t - t0) / (t1 - t0) * (size.x - 2.0f * pad),
                      p1.y - pad - (v - lo) / (hi - lo) * (size.y - 2.0f * pad));
    };
    const auto toCurve = [&](const ImVec2& s, float& t, float& v) {
        t = t0 + (s.x - p0.x - pad) / (size.x - 2.0f * pad) * (t1 - t0);
        v = lo + (p1.y - pad - s.y) / (size.y - 2.0f * pad) * (hi - lo);
    };
    // Slope in curve units -> a handle direction in screen space.
    const float sx = (size.x - 2.0f * pad) / (t1 - t0);
    const float sy = (size.y - 2.0f * pad) / (hi - lo);
    const float handleLen = ImGui::GetFontSize() * 2.2f;
    const auto handle = [&](const ImVec2& at, float slope, float dir) {
        ImVec2 d(sx, -slope * sy);
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        return ImVec2(at.x + dir * d.x / len * handleLen, at.y + dir * d.y / len * handleLen);
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_Border, 0.6f);
    const ImU32 axis = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.8f);
    const ImU32 line = o.Color ? o.Color : ImGui::GetColorU32(ImGuiCol_PlotLines);
    const ImU32 keyCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 selCol = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    dl->AddRectFilled(p0, p1, bg, style.FrameRounding);
    dl->PushClipRect(p0, p1, true);
    for (int i = 1; i < 4; ++i) {
        const float x = toScreen(t0 + (t1 - t0) * (float)i / 4.0f, 0.0f).x;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), grid);
    }
    if (lo < 0.0f && hi > 0.0f) {
        const float y = toScreen(t0, 0.0f).y;
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), axis);
    }
    // Range labels.
    char buf[64];
    std::snprintf(buf, sizeof buf, o.ValueFormat, hi);
    dl->AddText(ImVec2(p0.x + 3.0f, p0.y + 1.0f), axis, buf);
    std::snprintf(buf, sizeof buf, o.ValueFormat, lo);
    dl->AddText(ImVec2(p0.x + 3.0f, p1.y - ImGui::GetFontSize() - 1.0f), axis, buf);

    // The curve.
    constexpr int kSamples = 96;
    ImVec2 pts[kSamples + 1];
    for (int i = 0; i <= kSamples; ++i) {
        const float t = t0 + (t1 - t0) * (float)i / kSamples;
        pts[i] = toScreen(t, curve.Evaluate(t));
    }
    dl->AddPolyline(pts, kSamples + 1, line, 0, 2.0f);

    // Keys and handles.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float grab = ImGui::GetFontSize() * 0.45f;
    int sel = st.Selected();
    if (sel >= (int)curve.Keys.size()) sel = -1;
    int hoverKey = -1, hoverHandle = 0;
    const auto near = [&](const ImVec2& a) { return std::fabs(a.x - mouse.x) <= grab && std::fabs(a.y - mouse.y) <= grab; };
    if (sel >= 0) {
        const CurveKey& k = curve.Keys[sel];
        const ImVec2 at = toScreen(k.Time, k.Value);
        const ImVec2 hin = handle(at, k.InTangent, -1.0f), hout = handle(at, k.OutTangent, 1.0f);
        dl->AddLine(hin, at, selCol);
        dl->AddLine(at, hout, selCol);
        dl->AddCircleFilled(hin, 3.0f, selCol);
        dl->AddCircleFilled(hout, 3.0f, selCol);
        if (hovered && near(hin)) hoverHandle = 2;
        else if (hovered && near(hout)) hoverHandle = 3;
    }
    for (int i = 0; i < (int)curve.Keys.size(); ++i) {
        const ImVec2 at = toScreen(curve.Keys[i].Time, curve.Keys[i].Value);
        if (hovered && hoverHandle == 0 && hoverKey < 0 && near(at)) hoverKey = i;
        const float r = i == sel || i == hoverKey ? 5.0f : 4.0f;
        dl->AddCircleFilled(at, r, i == sel ? selCol : keyCol);
    }
    dl->PopClipRect();

    // Interaction.
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoverHandle) {
            st.SetDrag(hoverHandle);
        } else if (hoverKey >= 0) {
            sel = hoverKey;
            st.SetDrag(1);
        } else {
            sel = -1;
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hoverKey < 0 && hoverHandle == 0) {
        float t, v;
        toCurve(mouse, t, v);
        sel = curve.AddKey(std::clamp(t, t0, t1));
        st.SetDrag(0);
        committed = true;
    }
    if (st.Drag() != 0 && sel >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        CurveKey& k = curve.Keys[sel];
        float t, v;
        toCurve(mouse, t, v);
        if (st.Drag() == 1) {
            const float minT = sel > 0 ? curve.Keys[sel - 1].Time + 1e-4f : t0;
            const float maxT = sel + 1 < (int)curve.Keys.size() ? curve.Keys[sel + 1].Time - 1e-4f : t1;
            k.Time = std::clamp(t, minT, std::max(minT, maxT));
            k.Value = v;
        } else {
            const ImVec2 at = toScreen(k.Time, k.Value);
            float dx = (mouse.x - at.x) / sx, dy = -(mouse.y - at.y) / sy;
            if (st.Drag() == 2) { dx = -dx; dy = -dy; }
            const float slope = std::fabs(dx) > 1e-6f ? dy / dx : (dy > 0.0f ? 1e4f : -1e4f);
            // Linked handles: a smooth key, the usual case for procedural curves.
            k.InTangent = k.OutTangent = std::clamp(slope, -1e4f, 1e4f);
        }
    }
    if (st.Drag() != 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        st.SetDrag(0);
        committed = true;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hoverKey >= 0) {
            if (curve.Keys.size() > 1) {
                curve.Keys.erase(curve.Keys.begin() + hoverKey);
                sel = -1;
                committed = true;
            }
        } else {
            ImGui::OpenPopup("##curvepresets");
        }
    }
    if (hovered && hoverKey < 0 && hoverHandle == 0 && st.Drag() == 0)
        EditorUI::SetTooltip("Double-click: add key   Right-click: presets / delete key");

    if (ImGui::BeginPopup("##curvepresets")) {
        const float amp = PeakOf(curve, o.PresetAmplitude);
        const auto preset = [&](const char* label, Curve c) {
            if (ImGui::MenuItem(label)) {
                curve = std::move(c);
                sel = -1;
                committed = true;
            }
        };
        preset("Flat (0)", Curve::Constant(0.0f));
        preset("Line 0 -> peak", Curve::Line(t0, 0.0f, t1, amp));
        {
            Curve ease = Curve::EaseInOut();
            for (auto& k : ease.Keys) { k.Time = t0 + k.Time * (t1 - t0); k.Value *= amp; }
            preset("Ease In-Out", std::move(ease));
        }
        {
            Curve kick = Curve::Kick(0.1f);
            for (auto& k : kick.Keys) {
                k.Time = t0 + k.Time * (t1 - t0);
                k.Value *= amp;
                k.InTangent *= amp / (t1 - t0);
                k.OutTangent *= amp / (t1 - t0);
            }
            preset("Kick (rise + settle)", std::move(kick));
        }
        {
            Curve sine;
            for (int i = 0; i <= 8; ++i) {
                const float p = (float)i / 8.0f;
                const float w = glm::two_pi<float>() / (t1 - t0);
                const float slope = amp * w * std::cos(glm::two_pi<float>() * p);
                sine.Keys.push_back({t0 + p * (t1 - t0), amp * std::sin(glm::two_pi<float>() * p), slope, slope});
            }
            preset("Sine (one cycle)", std::move(sine));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Smooth all tangents")) {
            curve.AutoTangents();
            committed = true;
        }
        ImGui::EndPopup();
    }

    // The selected key, as numbers.
    if (sel >= 0 && sel < (int)curve.Keys.size()) {
        CurveKey& k = curve.Keys[sel];
        float f[4] = {k.Time, k.Value, k.InTangent, k.OutTangent};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragFloat4("##key", f, 0.001f, 0.0f, 0.0f, o.ValueFormat)) {
            const float minT = sel > 0 ? curve.Keys[sel - 1].Time + 1e-4f : t0;
            const float maxT = sel + 1 < (int)curve.Keys.size() ? curve.Keys[sel + 1].Time - 1e-4f : t1;
            k.Time = std::clamp(f[0], minT, std::max(minT, maxT));
            k.Value = f[1];
            k.InTangent = f[2];
            k.OutTangent = f[3];
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Selected key: time, value, in slope, out slope");
        if (ImGui::IsItemDeactivatedAfterEdit()) committed = true;
    }
    st.SetSelected(sel);
    ImGui::PopID();
    return committed;
}

} // namespace CurveEditor
