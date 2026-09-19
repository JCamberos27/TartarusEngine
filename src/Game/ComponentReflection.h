#pragma once

#include <cstddef>
#include <cstring>
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
    // std::string storage (a project-relative asset path), same plain-string JSON as String.
    // The Inspector shows a Combo of the AssetLibrary's assets of the field's AssetKind; on
    // load the path is registered with the library so it lists even if nothing else imported it.
    AssetRef,
};

// Which AssetLibrary list an AssetRef field draws from / registers into.
enum class ReflectAssetKind { Sound, Model, Texture, Material, Script };

struct ReflectField {
    const char* Name = "";          // Inspector label
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

    // --- AssetRef (ReflectFieldType::AssetRef) -----------------------------------------
    ReflectAssetKind AssetKind = ReflectAssetKind::Sound;

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

    // --- Serialization key vs. display label (#43) — positioned last so the existing positional
    // brace-inits at every registration site are unaffected -------------------------------------
    // Stable JSON round-trip key. Defaults to Name (via ReflectFieldKey() below) when left null,
    // so every existing registration keeps serializing exactly as before with zero changes at the
    // call site. Set this explicitly the moment Name is ever renamed for display (the audit's
    // §4.8/Q8 nomenclature pass, e.g. `Location` -> `Position`) so old scene/prefab JSON — which
    // was written with the OLD Name as its key — still finds the field, instead of the rename
    // silently reading back the field's default (Name used to double as the JSON key, so an
    // Inspector-label rename was also, invisibly, a save-format break).
    const char* Key = nullptr;

    // Prior Key/Name values to also try, in order, if the primary key isn't present in the JSON
    // object being loaded — covers a scene/prefab saved before this field's Key was last changed.
    // Only consulted on load; never written.
    const char* const* LegacyNames = nullptr;
    int LegacyNameCount = 0;

    // #132 - a String field holding a project-relative asset path (optionally "path#name", like
    // an animation clip reference). Saved as {"path", "pathGuid"} when the file has a GUID, and
    // on load a path that no longer exists follows the GUID to the file's new location. A plain
    // string (older scenes, or a value that isn't a file, e.g. an own clip name) still loads.
    bool AssetPath = false;
};

// The field's stable JSON key — Key when set, else Name (see ReflectField::Key). Every
// serializer/prefab-override site should key off this, never off Name directly, so an
// Inspector-label-only rename can never silently change what a save file reads/writes.
inline const char* ReflectFieldKey(const ReflectField& f) {
    return (f.Key && f.Key[0]) ? f.Key : f.Name;
}

// True if `key` names this field — its current key, or any of its LegacyNames. Prefer this to a
// direct string compare against Name/Key wherever an incoming key string (from a scene file, a
// prefab override entry, a menu action) is matched back to the ReflectField that owns it, so a
// file saved under a field's old key still resolves after a rename.
inline bool ReflectFieldMatchesKey(const ReflectField& f, const char* key) {
    if (!key) return false;
    if (std::strcmp(ReflectFieldKey(f), key) == 0) return true;
    for (int i = 0; i < f.LegacyNameCount; ++i)
        if (f.LegacyNames[i] && std::strcmp(f.LegacyNames[i], key) == 0) return true;
    return false;
}

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
    const char* Name = "";          // display: section title, menu label, Copy/Paste clipboard tag
    const char* Icon = "";          // ICON_FA_* string
    const char* Tooltip = nullptr;  // section / menu tooltip
    // Add Component menu grouping — matches the hand-coded menu's SeparatorText headings
    // ("Rendering", "Physics", "Audio", "Scripts"). Defaults to "Scripts": the first three
    // reflected components (Spin, Transform Controller, Animator) are all script-like behaviours,
    // and a non-script component (Camera) sets this explicitly.
    const char* Category = "Scripts";
    std::vector<ReflectField> Fields;

    // When false, the generic SceneSerializer pass skips this component entirely — its JSON is
    // written/read by a dedicated hand-coded path. For components whose state isn't plain
    // reflected fields (RenderableComponent holds a shared_ptr<Model>). Positioned after Fields
    // so the existing positional brace-inits are unaffected.
    bool GenericSerialize = true;

    // When false, the generic single-/multi-select Inspector loops and the generic slice of the
    // Add Component menu skip this component — a hand-coded Inspector section draws it instead.
    // Pairs with GenericSerialize for a component that IS registered (so the component-
    // registration guard rail counts it, and its Icon/Category/Tooltip live in one place) but
    // keeps custom editor code because its widgets or add/remove side effects (Mesh Renderer's
    // DetachedMeshComponent stash needs the editor-only AssetLibrary) don't fit the generic path.
    bool GenericInspector = true;

    // Stable prefab/scene JSON object key for this component — mirrors ReflectField::Key's exact
    // rationale one level up. Defaults to Name (via ReflectComponentKey() below), which is correct
    // for every GenericSerialize component (the generic pass writes/reads under Name verbatim).
    // Set this explicitly for a GenericSerialize = false component whose hand-written JSON path
    // predates this reflection layer under a DIFFERENT key than its display Name (Collider/Joint:
    // "collider"/"joint", lowercase, #185) — SceneSerializer's prefab-diff/override machinery
    // (IsPrefabFieldOverridden, IsPrefabComponentAdded, ApplyPrefabField/Component) looks the
    // component up as a JSON object key, so leaving this at Name for such a component would silent-
    // ly desync: e.g. IsPrefabComponentAdded would find no "Collider" key in the pristine JSON
    // (the file has "collider") and misreport every prefab instance's real, unmodified Collider as
    // user-added — including offering a "Revert to Prefab" that deletes it.
    const char* Key = nullptr;
};

// The component's stable JSON key — Key when set, else Name (see ReflectComponent::Key).
// SceneSerializer's prefab-diff/override lookups (and any other JSON-keyed use of a component's
// identity) must key off this, never off Name directly, so a GenericSerialize=false component
// with a legacy JSON key spelling doesn't silently desync — see ReflectComponent::Key.
inline const char* ReflectComponentKey(const ReflectComponent& c) {
    return (c.Key && c.Key[0]) ? c.Key : c.Name;
}
