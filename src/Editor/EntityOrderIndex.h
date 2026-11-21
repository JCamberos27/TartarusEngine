#pragma once
#include "ComponentsCore.h" // OrderComponent

#include <entt/entt.hpp>

#include <cstdint>
#include <unordered_map>

// Order -> entity lookup for the editor's bookmark chips, tabs and pins, which resolve their entity every frame.
// The map is rebuilt only when the OrderComponent storage changed: once per frame (frameToken) a fingerprint of
// every (entity, order) pair is taken, a straight pass over the packed storage with no allocation, and compared
// with the one the map was built from. That replaced a full map rebuild once a second (a hash-map insert per
// entity), and it catches a renumber at once instead of up to a second later. Main thread only.
class EntityOrderIndex {
public:
    // frameToken: the same value for every call within one frame (ImGui's frame count); a negative one means
    // "no frame clock", and every call then re-checks the fingerprint.
    entt::entity Find(const entt::registry& reg, int order, std::int64_t frameToken) {
        if (order < 0) return entt::null;
        if (frameToken < 0 || frameToken != m_CheckedFrame || &reg != m_Registry) {
            const std::uint64_t fp = Fingerprint(reg);
            ++m_Fingerprints;
            if (fp != m_Fingerprint || &reg != m_Registry || m_Stale) Rebuild(reg, fp);
            m_CheckedFrame = frameToken;
        }
        const auto it = m_ByOrder.find(order);
        if (it == m_ByOrder.end()) return entt::null;
        // Renumbered since this frame's check: verify before trusting it, and re-check on the next call.
        const OrderComponent* o = reg.valid(it->second) ? reg.try_get<OrderComponent>(it->second) : nullptr;
        if (o && o->Value == order) return it->second;
        m_Stale = true;
        m_CheckedFrame = -1;
        for (auto [e, oc] : reg.view<const OrderComponent>().each())
            if (oc.Value == order) return e;
        return entt::null;
    }

    std::uint64_t Rebuilds() const { return m_Rebuilds; }
    std::uint64_t Fingerprints() const { return m_Fingerprints; }

    static std::uint64_t Fingerprint(const entt::registry& reg) {
        const auto* orders = reg.storage<OrderComponent>(); // const registry: null until any entity has one
        std::uint64_t h = 1469598103934665603ull;
        if (!orders) return h;
        for (auto [e, o] : orders->each()) {
            const std::uint64_t v = ((std::uint64_t)entt::to_integral(e) << 32) ^ (std::uint32_t)o.Value;
            h = (h ^ v) * 1099511628211ull;
            h ^= h >> 29;
        }
        return h ^ (std::uint64_t)orders->size();
    }

private:
    void Rebuild(const entt::registry& reg, std::uint64_t fp) {
        m_ByOrder.clear();
        for (auto [e, o] : reg.view<const OrderComponent>().each()) m_ByOrder.emplace(o.Value, e);
        m_Registry = &reg;
        m_Fingerprint = fp;
        m_Stale = false;
        ++m_Rebuilds;
    }

    std::unordered_map<int, entt::entity> m_ByOrder;
    const entt::registry* m_Registry = nullptr;
    std::uint64_t m_Fingerprint = 0;
    std::int64_t m_CheckedFrame = -1;
    bool m_Stale = false;
    std::uint64_t m_Rebuilds = 0, m_Fingerprints = 0;
};
