#pragma once
#include "World.h"
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace Scripting {
// Relative references survive prefab instantiation without retaining source-world entity IDs.
inline entt::entity ResolveSceneReference(const World& world, entt::entity owner, const std::string& path) {
    if (path.empty()) return entt::null;
    size_t begin = 0;
    while (begin <= path.size() && world.Registry.valid(owner)) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
        const auto* h = world.Registry.try_get<HierarchyComponent>(owner);
        if (part == "..") owner = h ? h->Parent : entt::null;
        else if (!part.empty() && part != ".") {
            entt::entity next = entt::null;
            if (h) for (auto child : h->Children) {
                const auto* name = world.Registry.try_get<NameComponent>(child);
                if (name && name->Name == part) { next = child; break; }
            }
            owner = next;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return world.Registry.valid(owner) ? owner : entt::null;
}

inline std::optional<std::string> SceneReferencePath(const World& world, entt::entity owner, entt::entity target) {
    const auto parent = [&](entt::entity e) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        return h ? h->Parent : entt::null;
    };
    std::vector<entt::entity> ancestors;
    for (auto e = owner; world.Registry.valid(e); e = parent(e)) {
        if (std::find(ancestors.begin(), ancestors.end(), e) != ancestors.end()) return {};
        ancestors.push_back(e);
    }
    std::vector<entt::entity> descendants;
    auto common = target;
    while (world.Registry.valid(common) && std::find(ancestors.begin(), ancestors.end(), common) == ancestors.end()) {
        if (std::find(descendants.begin(), descendants.end(), common) != descendants.end()) return {};
        descendants.push_back(common);
        common = parent(common);
    }
    if (!world.Registry.valid(common)) return {};
    std::string path;
    const auto append = [&](const std::string& part) { if (!path.empty()) path += '/'; path += part; };
    for (auto e : ancestors) { if (e == common) break; append(".."); }
    for (auto it = descendants.rbegin(); it != descendants.rend(); ++it) {
        const auto* name = world.Registry.try_get<NameComponent>(*it);
        if (!name || name->Name.empty()) return {};
        append(name->Name);
    }
    if (path.empty()) path = ".";
    // Reject ambiguous names or names containing path separators rather than bind another object.
    return ResolveSceneReference(world, owner, path) == target ? std::optional<std::string>(path) : std::nullopt;
}

inline bool AcceptsSceneReference(const World& world, entt::entity owner, entt::entity target,
                                  const std::string& component, bool childrenOnly) {
    if (!world.Registry.valid(target)) return false;
    if (component == "Transform") { if (!world.Registry.all_of<TransformComponent>(target)) return false; }
    else if (component == "Particle System") { if (!world.Registry.all_of<ParticleSystemComponent>(target)) return false; }
    else return false;
    const auto path = SceneReferencePath(world, owner, target);
    return path && (!childrenOnly || (*path != ".." && path->compare(0, 3, "../") != 0));
}
}
