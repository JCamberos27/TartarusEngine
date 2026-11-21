#pragma once
// A member of a placed prefab instance (not its root), as its instance root's order and its
// prefab-local index. Members are rebuilt from the .prefab on every scene load and get new orders
// (only the root's is saved), so their own order can't find them again after Stop / Undo / Redo /
// Rebuild & Restart. Used by the editor's selection capture/restore and RestartState.
#include "Components.h"
#include "World.h"

#include <algorithm>

// True (with rootOrder and local filled) when `e` is a member of a placed prefab instance.
inline bool PrefabMemberRef(const World& world, entt::entity e, int& rootOrder, int& local) {
    for (entt::entity cur = e; cur != entt::null && world.Registry.valid(cur);) {
        if (const auto* pi = world.Registry.try_get<PrefabInstanceComponent>(cur)) {
            const auto at = std::find(pi->InstanceEntities.begin(), pi->InstanceEntities.end(), e);
            if (cur == e || at == pi->InstanceEntities.end() || !world.Registry.all_of<OrderComponent>(cur)) return false;
            rootOrder = world.Registry.get<OrderComponent>(cur).Value;
            local = (int)(at - pi->InstanceEntities.begin());
            return true;
        }
        const auto* h = world.Registry.try_get<HierarchyComponent>(cur);
        cur = h ? h->Parent : entt::null;
    }
    return false;
}

// The member at `local` of the instance whose root is `root`; entt::null when there is none.
inline entt::entity PrefabMemberAt(const World& world, entt::entity root, int local) {
    const auto* pi = world.Registry.try_get<PrefabInstanceComponent>(root);
    const entt::entity e = pi && local >= 0 && local < (int)pi->InstanceEntities.size() ? pi->InstanceEntities[local] : entt::null;
    return e != entt::null && world.Registry.valid(e) ? e : entt::null;
}
