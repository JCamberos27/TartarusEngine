#include "ComponentRegistry.h"
#include "ComponentRegistryInternal.h"

#include "Components.h"
#include "ReflectAttributes.h" // vInspector attributes (Attr)

#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <random>

// ---------------------------------------------------------------------------------------------
// House rule: ship components and controls fully wired, or not visible (#215)
//
//  1. A component or import setting is not shown in the editor until its runtime behaviour is
//     implemented. If the field must exist for serialization, keep it in the struct and the
//     serializer but leave it out of the Inspector.
//  2. A component that ships with runtime behaviour ships with an Inspector section and an Add
//     Component entry in the same change, not a follow-up.
//  3. If something must be visible before it works, show it disabled, labelled
//     "(not implemented)", with a tooltip naming the tracking issue.
//
// Registering a component (here: RegisterEngineComponents's per-area files, ComponentRegistry<Area>.cpp) generates its serialization, Inspector section and Add Component
// entry together, so both failure modes are impossible by construction. Prefer it. Every struct
// in Components.h must be registered here or listed in tools/component_registration_allowlist.txt
// with a reason; tools/check_component_registration.py enforces that in CI.
//
// Opt-outs: ReflectComponent::GenericSerialize / GenericInspector (both default true) let a
// registered component keep a hand-written JSON block and/or Inspector section for state that
// isn't plain reflected fields. Use sparingly. Current users: Mesh Renderer (both — see its
// registration in ComponentRegistryScene.cpp), Collider and Joint (GenericSerialize only; their bespoke fields are
// EditorHidden and drawn by DrawReflectedComponentExtra).
// ---------------------------------------------------------------------------------------------

namespace ComponentRegistry {

std::vector<RegisteredComponent>& Storage() {
    static std::vector<RegisteredComponent> registry;
    return registry;
}

const std::vector<RegisteredComponent>& All() { return Storage(); }

void Add(RegisteredComponent entry) { Storage().push_back(std::move(entry)); }

void RegisterEngineComponents() {
    RegisterGameplayComponents();
    RegisterCharacterComponents();
    RegisterSceneComponents();
    RegisterPhysicsComponents();
    RegisterAudioComponents();

    // #132 - String fields that hold asset paths: tracked by GUID so renaming or moving the file
    // outside the editor keeps the reference (see ReflectField::AssetPath).
    const std::pair<const char*, const char*> kAssetPathFields[] = {
        {"Animation", "Clip"},
        {"Animator Controller", "Controller"},
        {"Transform Controller", "Script Path"},
        {"First Person Controller", "Animation Set"},
        {"First Person Controller", "Secondary Animation Set"},
        {"First Person Controller", "Primary Weapon Prefab"},
        {"First Person Controller", "Secondary Weapon Prefab"},
        {"First Person Controller", "Sidearm Animation Set"},
        {"First Person Controller", "Sidearm Weapon Prefab"},
        {"Weapon Audio", "Data File"},
    };
    for (const auto& [component, field] : kAssetPathFields)
        for (RegisteredComponent& rc : Storage())
            if (std::strcmp(rc.Meta.Name, component) == 0)
                for (ReflectField& f : rc.Meta.Fields)
                    if (std::strcmp(f.Name, field) == 0) f.AssetPath = true;
}

// Runs once, before main(). It only appends to All()'s function-local static, so there is no
// static-init-order dependency.
namespace {
struct AutoRegister {
    AutoRegister() { RegisterEngineComponents(); }
} g_autoRegister;
} // namespace

} // namespace ComponentRegistry

// --- vInspector: per-field Reset to Default ----------------------------------------------------
namespace {
template <class V>
bool CopyOrCompare(void* dst, const void* src, bool copy) {
    if (copy) { *static_cast<V*>(dst) = *static_cast<const V*>(src); return true; }
    return *static_cast<const V*>(dst) == *static_cast<const V*>(src);
}

bool FieldCopyOrCompare(const RegisteredComponent& rc, const ReflectField& f, void* comp, bool copy) {
    if (!rc.DefaultInstance || !comp || !f.Address) return false;
    // Address() only computes a member address; the default instance is never written through it.
    void* def = f.Address(const_cast<void*>(rc.DefaultInstance()));
    void* cur = f.Address(comp);
    switch (f.Type) {
        case ReflectFieldType::Bool:     return CopyOrCompare<bool>(cur, def, copy);
        case ReflectFieldType::Int:
        case ReflectFieldType::Enum:     return CopyOrCompare<int>(cur, def, copy);
        case ReflectFieldType::Float:    return CopyOrCompare<float>(cur, def, copy);
        case ReflectFieldType::Vec3:
        case ReflectFieldType::Color:    return CopyOrCompare<glm::vec3>(cur, def, copy);
        case ReflectFieldType::String:
        case ReflectFieldType::AssetRef: return CopyOrCompare<std::string>(cur, def, copy);
    }
    return false;
}
} // namespace

bool ResetReflectFieldToDefault(const RegisteredComponent& rc, const ReflectField& f, void* component) {
    return FieldCopyOrCompare(rc, f, component, true);
}

bool ReflectFieldIsDefault(const RegisteredComponent& rc, const ReflectField& f, const void* component) {
    return FieldCopyOrCompare(rc, f, const_cast<void*>(component), false);
}
