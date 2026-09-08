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
    // vec3 storage, an RGB swatch widget, same "[r,g,b]" JSON as Vec3 (#302 Wave 2a).
    Color,
    // int storage, a Combo widget, JSON round-trips the label TEXT (stable if EnumLabels is
    // reordered); an unknown / out-of-range label loads as index 0. See EnumLabels / EnumCount.
    Enum,
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

    // --- Float widget hints (#302 Wave 2a; ignored by every other type) --------------------
    bool Slider = false;             // draw an EditorUI::SliderFloat instead of DragFloat (needs Min < Max)
    bool Logarithmic = false;        // ImGuiSliderFlags_Logarithmic on the slider
    const char* Format = nullptr;    // printf format e.g. "%.2f deg"; nullptr -> "%.3f"

    // --- Enum (ReflectFieldType::Enum) ---------------------------------------------------
    const char* EnumLabels = nullptr; // ImGui packed list: "Point\0Spot\0Directional\0"
    int EnumCount = 0;                 // number of labels in EnumLabels

    // --- Conditional visibility (Inspector only; the field is always serialized) -----------
    // Shown only when the sibling field named VisibleIfField (an Int or Enum field of the same
    // component) equals VisibleIfValue — or, with VisibleIfNot, does NOT equal it. In multi-edit
    // the sibling's shared value is used; a mixed sibling shows the field.
    const char* VisibleIfField = nullptr;
    int VisibleIfValue = 0;
    bool VisibleIfNot = false;

    // --- Grouping (Inspector single-select only; multi-select renders flat) ---------------
    // A run of consecutive fields with the same non-null Group renders inside one collapsible
    // TreeNode titled Group. A null Group ends the run.
    const char* Group = nullptr;

    // --- Inspector suppression -----------------------------------------------------------
    // Serialized like any field, but NOT drawn by the generic Inspector — a
    // DrawReflectedComponentExtra() handler draws it with a custom widget (e.g. Light's Color,
    // which sits behind the Kelvin/RGB toggle).
    bool EditorHidden = false;
};

// Walks EnumLabels (an ImGui "a\0b\0c\0" packed list) to the idx-th label. Returns "" for an
// out-of-range index or a null list. Shared by the serializer and the Inspector.
inline const char* ReflectEnumLabel(const ReflectField& f, int idx) {
    if (!f.EnumLabels || idx < 0 || idx >= f.EnumCount) return "";
    const char* p = f.EnumLabels;
    for (int i = 0; i < idx; ++i) {
        while (*p) ++p;   // advance to this label's NUL
        ++p;              // step past it onto the next label
    }
    return p;
}

// Maps a label back to its index in EnumLabels; returns 0 for no match (the safe default).
inline int ReflectEnumIndex(const ReflectField& f, const char* label) {
    if (!f.EnumLabels || !label) return 0;
    const char* p = f.EnumLabels;
    for (int i = 0; i < f.EnumCount && *p; ++i) {
        const char* q = label;
        const char* r = p;
        while (*q && *r && *q == *r) { ++q; ++r; }
        if (*q == '\0' && *r == '\0') return i;
        while (*p) ++p;
        ++p;
    }
    return 0;
}

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
