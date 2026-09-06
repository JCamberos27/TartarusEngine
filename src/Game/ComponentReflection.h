#pragma once

#include <cstddef>
#include <vector>

// Minimal native reflection for gameplay components (#184, thin slice).
//
// Declaring a component here + registering it in ComponentRegistry.cpp gives it, with no
// hand-written code in the serializer / Inspector / Add Component menu:
//   - JSON save + load (SceneSerializer's generic pass)
//   - an Inspector section with a widget per field
//   - an Add Component menu entry
// The per-frame behaviour still lives in a system (in TartarusGame.dll) — reflection describes
// the DATA, not the logic.
//
// v1 field types are deliberately just the four the first real component needs. Add more
// (String, Int-as-enum, asset refs, entity refs) when a component actually wants them; the
// generic serializer + Inspector switch on this enum in one place each.

enum class ReflectFieldType {
    Bool,
    Int,
    Float,
    Vec3,
};

struct ReflectField {
    const char* Name = "";          // Inspector label + JSON key
    ReflectFieldType Type = ReflectFieldType::Float;
    std::size_t Offset = 0;         // offsetof(Component, member) — components are standard-layout
    float DragSpeed = 0.1f;         // per-pixel step for the Inspector's Drag* widget
    const char* Tooltip = nullptr;  // optional
};

struct ReflectComponent {
    const char* Name = "";          // stable key: JSON object key, section title, menu label
    const char* Icon = "";          // ICON_FA_* string
    const char* Tooltip = nullptr;  // section / menu tooltip
    std::vector<ReflectField> Fields;
};
