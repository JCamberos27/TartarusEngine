#pragma once

#include <functional>
#include <string>
#include <vector>

#include <json.hpp>

// #145 - Unity's Input Manager: named actions ("Horizontal", "Jump", ...) bound to keys, mouse
// buttons and gamepad controls, edited in Project Settings > Input and saved with the project.
// Gameplay reads actions by name (GetAxis / GetButton*), so rebinding never touches code. The
// built-in Player and the game module (GameModuleHostAPI v12) both go through it.
//
// Every action is an axis: the positive side (Positive / Alt Positive / Gamepad Button) pushes
// toward +1, the negative side toward -1, and a gamepad axis adds its analog value. A "button"
// is simply an action whose positive side is held. Values are raw (no Unity gravity/smoothing).
namespace InputMap {

constexpr int kNone = -1;
// Binding codes: GLFW_KEY_* for keys; kMouseBase + N for mouse button N.
constexpr int kMouseBase = 1000;
constexpr int kMouseButtons = 8;

struct Action {
    std::string Name;
    int  Positive = kNone, Negative = kNone;
    int  AltPositive = kNone, AltNegative = kNone;
    int  GamepadButton = kNone;    // GLFW_GAMEPAD_BUTTON_*, counts as the positive side
    int  GamepadAxis = kNone;      // GLFW_GAMEPAD_AXIS_*
    bool InvertGamepadAxis = false; // e.g. Vertical: stick up reads -1 in GLFW
};

// What the evaluators read. Live queries use Input's per-frame snapshot; tests pass fakes.
struct Source {
    std::function<bool(int binding)> BindingDown;
    std::function<bool(int button)>  PadButtonDown;
    std::function<float(int axis)>   PadAxis;
};
float Evaluate(const Action& a, const Source& src);       // -1..1
bool  EvaluateButton(const Action& a, const Source& src); // positive side held

std::vector<Action> Defaults();
std::vector<Action>& Actions(); // the project's map (starts as Defaults())
const Action* Find(const std::string& name);

// Gameplay queries against the live map. An unknown name reads 0 / false (and warns once).
// All read 0 / false while disabled - the host disables input whenever the Game view doesn't
// have it (not playing, or the cursor is released).
void  SetEnabled(bool enabled);
bool  Enabled();
float GetAxis(const std::string& name);
bool  GetButton(const std::string& name);
bool  GetButtonDown(const std::string& name); // the frame the positive side went down
bool  GetButtonUp(const std::string& name);   // the frame it was released
// Raw binding codes (a GLFW key or kMouseBase + button), same gating.
bool  GetBinding(int code);
bool  GetBindingDown(int code);
bool  GetBindingUp(int code);

// Display names ("Space", "W", "Mouse 0", "A (South)", "Left Stick X").
std::string BindingName(int code);
const char* GamepadButtonName(int button);
const char* GamepadAxisName(int axis);
constexpr int kGamepadButtonCount = 15;
constexpr int kGamepadAxisCount = 6;

// Project Settings persistence. FromJson skips malformed entries (wrong types never throw).
nlohmann::json ToJson(const std::vector<Action>& actions);
std::vector<Action> FromJson(const nlohmann::json& j);

// A saved action list wins over its defaults, but it must never *lose* one. The file only holds
// the actions that existed the last time it was written, so anything added since then would be
// missing from the live map - and an unknown name only warns once, which reads as "the key does
// nothing". Appends every default whose name the list does not already have, leaving the file's
// own entries (including its bindings) untouched.
void MergeDefaults(std::vector<Action>& actions);

} // namespace InputMap
