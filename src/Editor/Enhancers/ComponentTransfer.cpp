#include "ComponentTransfer.h"

#include "ComponentRegistry.h"
#include "SceneSerializer.h"
#include "World.h"

#include <unordered_map>

namespace Enhancers {

namespace {
const RegisteredComponent* FindSerialisable(const std::string& name) {
    if (name.empty()) return nullptr;
    for (const auto& rc : ComponentRegistry::All())
        if (rc.Meta.GenericSerialize && name == rc.Meta.Name) return &rc;
    return nullptr;
}

std::unordered_map<int, entt::entity> EntitiesByOrder(const World& world) {
    std::unordered_map<int, entt::entity> m;
    for (auto [e, o] : world.Registry.view<const OrderComponent>().each()) m[o.Value] = e;
    return m;
}
} // namespace

std::vector<std::string> CopyAllComponents(const World& world, entt::entity e) {
    std::vector<std::string> out;
    if (!world.Registry.valid(e)) return out;
    for (const auto& rc : ComponentRegistry::All()) {
        if (!rc.Meta.GenericSerialize || !rc.Has(world.Registry, e)) continue;
        std::string j = SceneSerializer::ComponentToPresetJson(world, e, rc.Meta.Name);
        if (!j.empty()) out.push_back(std::move(j));
    }
    return out;
}

bool CanPastePreset(const World& world, entt::entity e, const std::string& presetJson, PasteMode mode) {
    if (!world.Registry.valid(e)) return false;
    const RegisteredComponent* rc = FindSerialisable(SceneSerializer::PresetComponentName(presetJson));
    if (!rc) return false;
    const bool has = rc->Has(world.Registry, e);
    return mode == PasteMode::AsNew ? !has : has;
}

int CountPastable(const World& world, entt::entity e, const std::vector<std::string>& clip, PasteMode mode) {
    int n = 0;
    for (const auto& j : clip) n += CanPastePreset(world, e, j, mode) ? 1 : 0;
    return n;
}

PasteReport PasteComponents(World& world, AssetLibrary& assets, entt::entity e,
                            const std::vector<std::string>& clip, PasteMode mode) {
    PasteReport r;
    for (const auto& j : clip) {
        if (CanPastePreset(world, e, j, mode) && SceneSerializer::ApplyComponentPresetJson(world, assets, e, j)) {
            ++r.Applied;
        } else {
            const std::string name = SceneSerializer::PresetComponentName(j);
            r.Skipped.push_back(name.empty() ? std::string("(unknown)") : name);
        }
    }
    return r;
}

std::vector<KeptValue> CaptureKept(const World& playWorld, const std::vector<PlayKeep>& keeps) {
    std::vector<KeptValue> out;
    if (keeps.empty()) return out;
    const auto byOrder = EntitiesByOrder(playWorld);
    for (const PlayKeep& k : keeps) {
        const auto it = byOrder.find(k.Order);
        if (it == byOrder.end()) continue;
        KeptValue v;
        v.Order = k.Order;
        v.Component = k.Component;
        if (k.Component == kKeepTransform) {
            const auto* t = playWorld.Registry.try_get<TransformComponent>(it->second);
            if (!t) continue;
            v.Transform = *t;
        } else {
            v.Preset = SceneSerializer::ComponentToPresetJson(playWorld, it->second, k.Component.c_str());
            if (v.Preset.empty()) continue;
        }
        out.push_back(std::move(v));
    }
    return out;
}

int ApplyKept(World& world, AssetLibrary& assets, const std::vector<KeptValue>& kept) {
    if (kept.empty()) return 0;
    const auto byOrder = EntitiesByOrder(world);
    int n = 0;
    for (const KeptValue& v : kept) {
        const auto it = byOrder.find(v.Order);
        if (it == byOrder.end()) continue;
        if (v.Component == kKeepTransform) {
            auto* t = world.Registry.try_get<TransformComponent>(it->second);
            if (!t) continue;
            *t = v.Transform;
            ++n;
        } else if (SceneSerializer::ApplyComponentPresetJson(world, assets, it->second, v.Preset)) {
            ++n;
        }
    }
    return n;
}

} // namespace Enhancers
