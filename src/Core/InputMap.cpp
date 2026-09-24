#include "InputMap.h"

#include "Input.h"
#include "Log.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <unordered_set>

namespace InputMap {

namespace {
std::vector<Action> g_Actions = Defaults();
bool g_Enabled = false;

bool LiveBindingDown(int code) {
    if (code >= kMouseBase) return Input::IsMouseButtonDown(code - kMouseBase);
    return Input::IsKeyDown(code);
}
bool LiveBindingPressed(int code) {
    if (code >= kMouseBase) return Input::IsMouseButtonPressed(code - kMouseBase);
    return Input::IsKeyPressed(code);
}
bool LiveBindingReleased(int code) {
    if (code >= kMouseBase) return Input::IsMouseButtonReleased(code - kMouseBase);
    return Input::IsKeyReleased(code);
}

const Source& LiveSource() {
    static const Source s{
        [](int code) { return LiveBindingDown(code); },
        [](int button) { return Input::IsGamepadButtonDown(button); },
        [](int axis) { return Input::GetGamepadAxis(axis); },
    };
    return s;
}

const Action* Lookup(const std::string& name) {
    if (const Action* a = Find(name)) return a;
    static std::unordered_set<std::string> warned;
    if (warned.insert(name).second)
        Log::Warn("Input: no action named '" + name + "' (Project Settings > Input).");
    return nullptr;
}

bool Down(const Source& src, int code) { return code != kNone && src.BindingDown && src.BindingDown(code); }
} // namespace

float Evaluate(const Action& a, const Source& src) {
    float v = 0.0f;
    if (EvaluateButton(a, src)) v += 1.0f;
    if (Down(src, a.Negative) || Down(src, a.AltNegative)) v -= 1.0f;
    if (a.GamepadAxis != kNone && src.PadAxis) {
        const float pad = src.PadAxis(a.GamepadAxis);
        v += a.InvertGamepadAxis ? -pad : pad;
    }
    return std::clamp(v, -1.0f, 1.0f);
}

bool EvaluateButton(const Action& a, const Source& src) {
    return Down(src, a.Positive) || Down(src, a.AltPositive) ||
           (a.GamepadButton != kNone && src.PadButtonDown && src.PadButtonDown(a.GamepadButton));
}

std::vector<Action> Defaults() {
    auto act = [](const char* name, int pos, int neg, int altPos, int altNeg, int padButton, int padAxis, bool invert) {
        Action a;
        a.Name = name;
        a.Positive = pos; a.Negative = neg; a.AltPositive = altPos; a.AltNegative = altNeg;
        a.GamepadButton = padButton; a.GamepadAxis = padAxis; a.InvertGamepadAxis = invert;
        return a;
    };
    return {
        act("Horizontal", GLFW_KEY_D, GLFW_KEY_A, GLFW_KEY_RIGHT, GLFW_KEY_LEFT, kNone, GLFW_GAMEPAD_AXIS_LEFT_X, false),
        act("Vertical",   GLFW_KEY_W, GLFW_KEY_S, GLFW_KEY_UP,    GLFW_KEY_DOWN, kNone, GLFW_GAMEPAD_AXIS_LEFT_Y, true),
        act("Jump",   GLFW_KEY_SPACE,      kNone, kNone, kNone, GLFW_GAMEPAD_BUTTON_A,          kNone, false),
        act("Sprint", GLFW_KEY_LEFT_SHIFT, kNone, kNone, kNone, GLFW_GAMEPAD_BUTTON_LEFT_THUMB, kNone, false),
        act("Fire1",  kMouseBase + 0, kNone, GLFW_KEY_LEFT_CONTROL, kNone, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER, kNone, false),
        act("Fire2",  kMouseBase + 1, kNone, GLFW_KEY_LEFT_ALT,     kNone, GLFW_GAMEPAD_BUTTON_LEFT_BUMPER,  kNone, false),
        act("Interact", GLFW_KEY_E, kNone, kNone, kNone, GLFW_GAMEPAD_BUTTON_X, kNone, false),
        act("Cancel", GLFW_KEY_ESCAPE, kNone, kNone, kNone, GLFW_GAMEPAD_BUTTON_START, kNone, false),
        // First-person weapon actions (#165 FirstPersonPresentation). Fire/Aim reuse Fire1/Fire2
        // rather than adding new mouse-bound actions - the two only ever apply to a controller
        // with its gravity gun turned off, so the bindings never actually collide in one scene.
        // Reload is tap/hold: a tap reloads (tactical or empty, from the magazine count), a hold
        // checks the magazine - so neither EmptyReload nor MagCheck needs a key of its own.
        act("Reload",   GLFW_KEY_R, kNone, kNone, kNone, kNone, kNone, false),
        act("FireMode", GLFW_KEY_B, kNone, kNone, kNone, kNone, kNone, false), // semi <-> full auto
        act("Inspect",  GLFW_KEY_I, kNone, kNone, kNone, kNone, kNone, false),
        act("Melee",    GLFW_KEY_Q, kNone, kNone, kNone, kNone, kNone, false),
        act("Holster",  GLFW_KEY_H, kNone, kNone, kNone, kNone, kNone, false),
        // Weapon slots: 1 = the AK, 2 = unarmed (the scroll wheel toggles between them too).
        act("Weapon1",  GLFW_KEY_1, kNone, kNone, kNone, kNone, kNone, false),
        act("Weapon2",  GLFW_KEY_2, kNone, kNone, kNone, kNone, kNone, false),
    };
}

std::vector<Action>& Actions() { return g_Actions; }

const Action* Find(const std::string& name) {
    for (const Action& a : g_Actions)
        if (a.Name == name) return &a;
    return nullptr;
}

void SetEnabled(bool enabled) { g_Enabled = enabled; }
bool Enabled() { return g_Enabled; }

float GetAxis(const std::string& name) {
    if (!g_Enabled) return 0.0f;
    const Action* a = Lookup(name);
    return a ? Evaluate(*a, LiveSource()) : 0.0f;
}

bool GetButton(const std::string& name) {
    if (!g_Enabled) return false;
    const Action* a = Lookup(name);
    return a && EvaluateButton(*a, LiveSource());
}

bool GetButtonDown(const std::string& name) {
    if (!g_Enabled) return false;
    const Action* a = Lookup(name);
    if (!a) return false;
    return (a->Positive != kNone && LiveBindingPressed(a->Positive)) ||
           (a->AltPositive != kNone && LiveBindingPressed(a->AltPositive)) ||
           (a->GamepadButton != kNone && Input::IsGamepadButtonPressed(a->GamepadButton));
}

bool GetButtonUp(const std::string& name) {
    if (!g_Enabled) return false;
    const Action* a = Lookup(name);
    if (!a) return false;
    return (a->Positive != kNone && LiveBindingReleased(a->Positive)) ||
           (a->AltPositive != kNone && LiveBindingReleased(a->AltPositive)) ||
           (a->GamepadButton != kNone && Input::IsGamepadButtonReleased(a->GamepadButton));
}

bool GetBinding(int code)     { return g_Enabled && code != kNone && LiveBindingDown(code); }
bool GetBindingDown(int code) { return g_Enabled && code != kNone && LiveBindingPressed(code); }
bool GetBindingUp(int code)   { return g_Enabled && code != kNone && LiveBindingReleased(code); }

std::string BindingName(int code) {
    if (code == kNone) return "None";
    if (code >= kMouseBase) return "Mouse " + std::to_string(code - kMouseBase);
    if ((code >= GLFW_KEY_0 && code <= GLFW_KEY_9) || (code >= GLFW_KEY_A && code <= GLFW_KEY_Z))
        return std::string(1, (char)code);
    if (code >= GLFW_KEY_F1 && code <= GLFW_KEY_F25) return "F" + std::to_string(code - GLFW_KEY_F1 + 1);
    if (code >= GLFW_KEY_KP_0 && code <= GLFW_KEY_KP_9) return "Keypad " + std::to_string(code - GLFW_KEY_KP_0);
    switch (code) {
    case GLFW_KEY_SPACE: return "Space";
    case GLFW_KEY_APOSTROPHE: return "'";
    case GLFW_KEY_COMMA: return ",";
    case GLFW_KEY_MINUS: return "-";
    case GLFW_KEY_PERIOD: return ".";
    case GLFW_KEY_SLASH: return "/";
    case GLFW_KEY_SEMICOLON: return ";";
    case GLFW_KEY_EQUAL: return "=";
    case GLFW_KEY_LEFT_BRACKET: return "[";
    case GLFW_KEY_BACKSLASH: return "\\";
    case GLFW_KEY_RIGHT_BRACKET: return "]";
    case GLFW_KEY_GRAVE_ACCENT: return "`";
    case GLFW_KEY_ESCAPE: return "Escape";
    case GLFW_KEY_ENTER: return "Enter";
    case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_BACKSPACE: return "Backspace";
    case GLFW_KEY_INSERT: return "Insert";
    case GLFW_KEY_DELETE: return "Delete";
    case GLFW_KEY_RIGHT: return "Right Arrow";
    case GLFW_KEY_LEFT: return "Left Arrow";
    case GLFW_KEY_DOWN: return "Down Arrow";
    case GLFW_KEY_UP: return "Up Arrow";
    case GLFW_KEY_PAGE_UP: return "Page Up";
    case GLFW_KEY_PAGE_DOWN: return "Page Down";
    case GLFW_KEY_HOME: return "Home";
    case GLFW_KEY_END: return "End";
    case GLFW_KEY_CAPS_LOCK: return "Caps Lock";
    case GLFW_KEY_KP_ENTER: return "Keypad Enter";
    case GLFW_KEY_LEFT_SHIFT: return "Left Shift";
    case GLFW_KEY_LEFT_CONTROL: return "Left Ctrl";
    case GLFW_KEY_LEFT_ALT: return "Left Alt";
    case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
    case GLFW_KEY_RIGHT_CONTROL: return "Right Ctrl";
    case GLFW_KEY_RIGHT_ALT: return "Right Alt";
    default: return "Key " + std::to_string(code);
    }
}

const char* GamepadButtonName(int button) {
    static const char* kNames[kGamepadButtonCount] = {
        "A (South)", "B (East)", "X (West)", "Y (North)", "Left Bumper", "Right Bumper", "Back", "Start",
        "Guide", "Left Stick Click", "Right Stick Click", "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left"};
    return button >= 0 && button < kGamepadButtonCount ? kNames[button] : "None";
}

const char* GamepadAxisName(int axis) {
    static const char* kNames[kGamepadAxisCount] = {
        "Left Stick X", "Left Stick Y", "Right Stick X", "Right Stick Y", "Left Trigger", "Right Trigger"};
    return axis >= 0 && axis < kGamepadAxisCount ? kNames[axis] : "None";
}

nlohmann::json ToJson(const std::vector<Action>& actions) {
    nlohmann::json arr = nlohmann::json::array();
    for (const Action& a : actions) {
        arr.push_back({
            {"name", a.Name},
            {"positive", a.Positive}, {"negative", a.Negative},
            {"altPositive", a.AltPositive}, {"altNegative", a.AltNegative},
            {"gamepadButton", a.GamepadButton}, {"gamepadAxis", a.GamepadAxis},
            {"invertGamepadAxis", a.InvertGamepadAxis},
        });
    }
    return arr;
}

std::vector<Action> FromJson(const nlohmann::json& j) {
    std::vector<Action> out;
    if (!j.is_array()) return out;
    auto code = [](const nlohmann::json& o, const char* key, int lo, int hi) {
        const auto it = o.find(key);
        if (it == o.end() || !it->is_number_integer()) return kNone;
        const int v = it->get<int>();
        return v >= lo && v <= hi ? v : kNone;
    };
    for (const auto& o : j) {
        if (!o.is_object()) continue;
        const auto n = o.find("name");
        if (n == o.end() || !n->is_string() || n->get<std::string>().empty()) continue;
        Action a;
        a.Name = n->get<std::string>();
        a.Positive    = code(o, "positive", 0, kMouseBase + kMouseButtons - 1);
        a.Negative    = code(o, "negative", 0, kMouseBase + kMouseButtons - 1);
        a.AltPositive = code(o, "altPositive", 0, kMouseBase + kMouseButtons - 1);
        a.AltNegative = code(o, "altNegative", 0, kMouseBase + kMouseButtons - 1);
        a.GamepadButton = code(o, "gamepadButton", 0, kGamepadButtonCount - 1);
        a.GamepadAxis   = code(o, "gamepadAxis", 0, kGamepadAxisCount - 1);
        const auto inv = o.find("invertGamepadAxis");
        a.InvertGamepadAxis = inv != o.end() && inv->is_boolean() && inv->get<bool>();
        out.push_back(std::move(a));
    }
    return out;
}

void MergeDefaults(std::vector<Action>& actions) {
    for (Action& d : Defaults()) {
        bool have = false;
        for (const Action& a : actions)
            if (a.Name == d.Name) { have = true; break; }
        if (!have) actions.push_back(std::move(d));
    }
}

} // namespace InputMap
