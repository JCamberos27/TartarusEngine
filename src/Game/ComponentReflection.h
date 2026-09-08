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
// v1 field types were deliberately just the four the first real component needed; String is the
// second addition (TransformControllerComponent's ScriptPath). Add more (Int-as-enum, asset
// refs, entity refs) when a component actually wants them; the generic serializer + Inspector
// switch on this enum in one place each.

enum class ReflectFieldType {
    Bool,
    Int,
    Float,
    Vec3,
    String,
};

struct ReflectField {
    const char* Name = "";          // Inspector label + JSON key
    ReflectFieldType Type = ReflectFieldType::Float;
    // Returns the address of this field within a component instance. Built from a pointer-to-
    // member at the registration site (TARTARUS_REFLECT_FIELD below) rather than an offsetof, so
    // the component need NOT be standard-layout — a std::string or a non-standard-layout
    // glm::vec3 member is fine. `component` is the void* from RegisteredComponent::Get/GetConst.
    void* (*Address)(void* component) = nullptr;
    float DragSpeed = 0.1f;          // per-pixel step for the Inspector's Drag* widget
    const char* Tooltip = nullptr;   // optional
    // Optional Int/Float/Vec3 clamp, forwarded straight to the Inspector's Drag* widgets. Leave
    // both at 0 (the default) for "unclamped" — ImGui's own convention for DragFloat/DragInt/
    // DragFloat3 (v_min == v_max == 0 disables clamping), so a field that doesn't set these
    // behaves exactly as it did before Min/Max existed. Ignored by Bool/String.
    float Min = 0.0f;
    float Max = 0.0f;
};

// Builds a ReflectField::Address accessor from a pointer-to-member. The unary + forces the
// captureless lambda to decay to a plain function pointer.
#define TARTARUS_REFLECT_FIELD(ComponentType, member) \
    (+[](void* c) -> void* { return &static_cast<ComponentType*>(c)->member; })

struct ReflectComponent {
    const char* Name = "";          // stable key: JSON object key, section title, menu label
    const char* Icon = "";          // ICON_FA_* string
    const char* Tooltip = nullptr;  // section / menu tooltip
    // Add Component menu grouping — matches the hand-coded menu's SeparatorText headings
    // ("Rendering", "Physics", "Audio", "Scripts"). Defaults to "Scripts": the first three
    // reflected components (Spin, Transform Controller, Animator) are all script-like behaviours,
    // and a non-script component (Camera) sets this explicitly.
    const char* Category = "Scripts";
    std::vector<ReflectField> Fields;
};
