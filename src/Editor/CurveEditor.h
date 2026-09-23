#pragma once

#include "Curve.h"

#include <imgui.h>

// An ImGui curve editor for Curve (src/Core/Curve.h), drawn with the window's draw list.
//
//   - Drag a key to move it (it can't pass its neighbours); drag the handles of the selected key
//     to set its slope.
//   - Double-click empty space to add a key on the curve.
//   - Right-click a key for its menu (delete, flatten, smooth); Delete removes the selected key
//     while the graph is hovered. Hovering a key shows its time and value.
//   - Right-click empty space for presets (flat, line, ease, kick, sine, auto tangents, and
//     "Reset to default" when the caller supplies one).
//   - The selected key's time / value / slopes are editable as numbers under the graph.
//
// The value axis fits the curve (and 0) automatically, and holds still while you drag.
namespace CurveEditor {

struct Options {
    float TimeMin = 0.0f;
    float TimeMax = 1.0f;
    const char* ValueFormat = "%.3f";
    // Amplitude the presets use when the curve is flat (otherwise its current peak).
    float PresetAmplitude = 1.0f;
    ImU32 Color = 0; // 0 = the style's plot colour
    const Curve* Default = nullptr; // offered as "Reset to default" in the presets menu
};

// Returns true when an edit is committed: a drag released, a key added or removed, a preset
// applied, or a number field changed. `curve` is edited live while dragging.
bool Draw(const char* id, Curve& curve, const ImVec2& size, const Options& options = {});

} // namespace CurveEditor
