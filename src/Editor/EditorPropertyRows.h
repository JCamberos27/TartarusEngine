#pragma once

// Labelled property rows for asset inspectors that edit a plain struct and save it on commit
// (the weapon definition, for one). Every row is "label | widget" with the label in a fixed
// column; hovering either shows the tooltip. Edits are reported through `Changed` only when they
// COMMIT (a drag released, a checkbox clicked, a typed value entered), so a caller that saves on
// Changed writes once per edit, not once per frame of a drag.
//
//     PropertyRows r(150.0f * uiScale, changed);
//     if (r.Section(ICON_FA_GUN, "Recoil", "0.28 s, hip x1.3")) {
//         r.Float("Duration", recoil.Duration, 0.005f, 0.02f, 3.0f, "%.3f s", "Seconds one shot spans.");
//         ...
//     }

#include <glm/glm.hpp>
#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

class PropertyRows {
public:
    PropertyRows(float labelWidth, bool& changed) : m_LabelWidth(labelWidth), m_Changed(changed) {}

    float LabelWidth() const { return m_LabelWidth; }
    void MarkChanged() { m_Changed = true; }

    // The label, aligned to the widget column, with its tooltip; the next widget fills the rest.
    void Label(const char* label, const char* tip = nullptr);

    bool Float(const char* label, float& v, float speed, float lo, float hi, const char* fmt, const char* tip = nullptr);
    bool Int(const char* label, int& v, int lo, int hi, const char* tip = nullptr);
    bool Check(const char* label, bool& v, const char* tip = nullptr);
    bool Vec2(const char* label, glm::vec2& v, float speed, const char* fmt, const char* tip = nullptr);
    // A random min..max range, kept in order.
    bool Range(const char* label, glm::vec2& v, float speed, const char* fmt, const char* tip = nullptr);
    // Three fields with red / green / blue axis marks, like the Transform rows.
    bool Vec3(const char* label, glm::vec3& v, float speed, const char* fmt, const char* tip = nullptr,
              const char* axes = "XYZ");
    // Spring: frequency (Hz) and damping ratio.
    bool Spring(const char* label, float& frequency, float& damping, const char* tip = nullptr);
    bool Text(const char* label, std::string& v, const char* tip = nullptr);
    // A name typed in or picked from `items`. With `validate`, a name not in `items` shows in the
    // warning colour and `unknownTip` (printf, %s = the name) explains why.
    bool Name(const char* label, std::string& v, const std::vector<std::string>& items, bool validate,
              const char* tip = nullptr, const char* unknownTip = nullptr);
    // A read-only value in the widget column.
    void Value(const char* label, const char* text, const char* tip = nullptr);

    // A collapsing section: icon + title, with a short dimmed summary on the right of the header.
    bool Section(const char* icon, const char* title, const char* summary = nullptr, bool defaultOpen = false);
    // A subheading inside a section.
    void Heading(const char* text);
    // A wrapped, dimmed explanation.
    void Note(const char* text);
    // "Reset to Defaults" with a confirmation popup; `apply` then runs and Changed is set.
    void ResetButton(const char* what, const std::function<void()>& apply);

    enum class Status { Ok, Info, Warning, Error };
    // A small coloured status chip ("icon  text"), inline.
    static void Badge(Status status, const char* text, const char* tip = nullptr);
    static ImVec4 StatusColor(Status status);

private:
    bool Commit(bool committed);
    float m_LabelWidth;
    bool& m_Changed;
};
