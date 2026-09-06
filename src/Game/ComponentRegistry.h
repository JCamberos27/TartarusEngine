#pragma once

#include <entt/entt.hpp>
#include <utility>
#include <vector>

#include "ComponentReflection.h"

// Type-erased handle for one registered component: its reflected field layout plus the four
// registry operations the generic serializer / Inspector / Add Component menu need. Filled in
// by ComponentRegistry::Register<T>() so no call site ever names the concrete type.
struct RegisteredComponent {
    ReflectComponent Meta;
    bool  (*Has)(const entt::registry&, entt::entity) = nullptr;
    void* (*Get)(entt::registry&, entt::entity) = nullptr;        // nullptr if absent
    const void* (*GetConst)(const entt::registry&, entt::entity) = nullptr;
    void  (*Add)(entt::registry&, entt::entity) = nullptr;
    void  (*Remove)(entt::registry&, entt::entity) = nullptr;
};

namespace ComponentRegistry {

// Every component registered via RegisterEngineComponents(), in registration order.
const std::vector<RegisteredComponent>& All();

void Add(RegisteredComponent entry);

template <class T>
void Register(ReflectComponent meta) {
    // Reflected fields are addressed through a pointer-to-member accessor (TARTARUS_REFLECT_FIELD
    // in ComponentReflection.h), not offsetof, so there is no standard-layout requirement on T —
    // components with std::string members (Transform Controller's ScriptPath) or non-standard-
    // layout glm::vec3 members register fine.
    Add(RegisteredComponent{
        std::move(meta),
        [](const entt::registry& r, entt::entity e) { return r.all_of<T>(e); },
        [](entt::registry& r, entt::entity e) -> void* { return r.try_get<T>(e); },
        [](const entt::registry& r, entt::entity e) -> const void* { return r.try_get<T>(e); },
        [](entt::registry& r, entt::entity e) { r.emplace_or_replace<T>(e); },
        [](entt::registry& r, entt::entity e) { r.remove<T>(e); },
    });
}

// The one place a new reflected component gets listed. Runs once at static init.
void RegisterEngineComponents();

} // namespace ComponentRegistry
