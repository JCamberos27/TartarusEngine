#include "LightSwitch.h"
#include "Components.h"
#include "MaterialAsset.h"
#include "Model.h"
#include "Openable.h"
#include "ReflectionProbeCapture.h"
#include "World.h"

#include <glm/gtc/quaternion.hpp>
#include <unordered_map>

namespace LightSwitch {
namespace {

// A switch plate is a few centimetres across: its box grows by this much each way, so a glance at it is enough.
constexpr float kGrab = 0.04f;

std::shared_ptr<MaterialAsset> WithGlow(const std::shared_ptr<MaterialAsset>& m, float glow) {
    if (!m || glow == 1.0f) return m;
    auto c = std::make_shared<MaterialAsset>(*m);
    c->Mat.EmissiveStrength *= glow;
    return c;
}

bool Glows(const std::shared_ptr<MaterialAsset>& m) {
    return m && !m->Missing && m->Mat.EmissiveStrength > 0.0f &&
           (m->Mat.EmissiveMap || glm::dot(m->Mat.EmissiveColor, m->Mat.EmissiveColor) > 0.0f);
}

void StartCircuitPiece(World& world, entt::entity e, LightCircuitComponent& c) {
    c.Started = true;
    auto* r = world.Registry.try_get<RenderableComponent>(e);
    if (!r || c.Glow <= 0.0f) return;
    c.OnMaterials.clear();
    c.OffMaterials.clear();
    for (const auto& m : r->Materials) {
        c.OnMaterials.push_back(WithGlow(m, c.Glow));
        if (Glows(m)) {
            auto off = std::make_shared<MaterialAsset>(*m);
            off->Mat.EmissiveStrength = 0.0f;
            c.OffMaterials.push_back(off);
        } else {
            c.OffMaterials.push_back(m);
        }
    }
}

void StartSwitch(World& world, entt::entity e, LightSwitchComponent& s) {
    s.Started = true;
    s.On = s.StartOn;
    s.RockerEntity = entt::null;
    if (s.Rocker.empty()) return;
    // The rocker: the first descendant by that name.
    std::vector<entt::entity> todo{e};
    while (!todo.empty() && s.RockerEntity == entt::null) {
        const entt::entity cur = todo.back();
        todo.pop_back();
        const auto* h = world.Registry.try_get<HierarchyComponent>(cur);
        if (!h) continue;
        for (entt::entity c : h->Children) {
            const auto* n = world.Registry.try_get<NameComponent>(c);
            if (n && n->Name == s.Rocker) { s.RockerEntity = c; break; }
            todo.push_back(c);
        }
    }
    if (s.RockerEntity != entt::null)
        if (const auto* t = world.Registry.try_get<TransformComponent>(s.RockerEntity)) s.RockerBase = t->Rotation;
}

void PoseRocker(World& world, LightSwitchComponent& s) {
    if (s.RockerEntity == entt::null || !world.Registry.valid(s.RockerEntity)) return;
    auto* t = world.Registry.try_get<TransformComponent>(s.RockerEntity);
    if (!t) return;
    t->Rotation = s.RockerBase * glm::angleAxis(glm::radians(s.On ? -s.RockerAngle : s.RockerAngle), glm::vec3(0, 0, 1));
}

// Starts whatever hasn't started; a circuit's first-found switch decides the state it starts in.
void StartAll(World& world) {
    std::unordered_map<std::string, bool> start;
    bool any = false;
    {
        auto v = world.Registry.view<LightSwitchComponent>();
        for (auto e : v) {
            auto& s = v.get<LightSwitchComponent>(e);
            if (s.Started) continue;
            StartSwitch(world, e, s);
            start.emplace(s.Circuit, s.StartOn);
            any = true;
        }
    }
    {
        auto v = world.Registry.view<LightCircuitComponent>();
        for (auto e : v) {
            auto& c = v.get<LightCircuitComponent>(e);
            if (c.Started) continue;
            StartCircuitPiece(world, e, c);
            start.emplace(c.Circuit, true); // a circuit nobody switches stays as authored: on
            any = true;
        }
    }
    if (!any) return;
    for (const auto& [name, on] : start) Set(world, name, on);
    // Every switch takes its circuit's state (several switches on one circuit flip together).
    auto v = world.Registry.view<LightSwitchComponent>();
    for (auto e : v) {
        auto& s = v.get<LightSwitchComponent>(e);
        s.On = IsOn(world, s.Circuit);
        PoseRocker(world, s);
    }
}

} // namespace

bool Set(World& world, const std::string& circuit, bool on) {
    bool found = false, changed = false;
    auto cv = world.Registry.view<LightCircuitComponent>();
    for (auto e : cv) {
        auto& c = cv.get<LightCircuitComponent>(e);
        if (!c.Started || c.Circuit != circuit) continue;
        found = true;
        if (auto* r = world.Registry.try_get<RenderableComponent>(e); r && !c.OnMaterials.empty()) {
            r->Materials = on ? c.OnMaterials : c.OffMaterials;
        } else if (world.Registry.all_of<LightComponent>(e)) {
            const bool off = world.Registry.all_of<DeactivatedTag>(e);
            if (on && off) { world.Registry.remove<DeactivatedTag>(e); changed = true; }
            if (!on && !off) { world.Registry.emplace<DeactivatedTag>(e); changed = true; }
        }
    }
    auto sv = world.Registry.view<LightSwitchComponent>();
    for (auto e : sv) {
        auto& s = sv.get<LightSwitchComponent>(e);
        if (!s.Started || s.Circuit != circuit) continue;
        found = true;
        s.On = on;
        PoseRocker(world, s);
    }
    if (changed) world.SyncActiveInHierarchy();
    return found;
}

bool IsOn(World& world, const std::string& circuit) {
    auto sv = world.Registry.view<LightSwitchComponent>();
    for (auto e : sv) {
        const auto& s = sv.get<LightSwitchComponent>(e);
        if (s.Started && s.Circuit == circuit) return s.On;
    }
    auto cv = world.Registry.view<LightCircuitComponent>();
    for (auto e : cv) {
        const auto& c = cv.get<LightCircuitComponent>(e);
        if (!c.Started || c.Circuit != circuit) continue;
        if (world.Registry.all_of<LightComponent>(e) && !world.Registry.all_of<RenderableComponent>(e))
            return !world.Registry.all_of<DeactivatedTag>(e);
    }
    return false;
}

entt::entity Pick(World& world, const glm::vec3& eye, const glm::vec3& forward, float* outDist) {
    auto view = world.Registry.view<LightSwitchComponent, RenderableComponent>(entt::exclude<InactiveTag>);
    const glm::vec3 dir = glm::normalize(forward);
    entt::entity best = entt::null;
    float bestDist = Openable::kReach;
    for (auto e : view) {
        const auto& r = view.get<RenderableComponent>(e);
        if (!r.ModelRef || !view.get<LightSwitchComponent>(e).Started) continue;
        const float d = Openable::RayBox(world.GetCachedWorldTransform(e), r.ModelRef->BoundsMin() - glm::vec3(kGrab),
                                         r.ModelRef->BoundsMax() + glm::vec3(kGrab), eye, dir);
        if (d >= 0.0f && d < bestDist) {
            best = e;
            bestDist = d;
        }
    }
    if (best == entt::null || !Openable::Unblocked(world, eye, dir, best, bestDist)) return entt::null;
    if (outDist) *outDist = bestDist;
    return best;
}

void Update(World& world, const glm::vec3& eye, const glm::vec3& forward, bool interact) {
    StartAll(world);
    if (!interact) return;
    const entt::entity e = Pick(world, eye, forward);
    if (e == entt::null) return;
    const auto& s = world.Registry.get<LightSwitchComponent>(e);
    const bool on = !s.On;
    const std::string circuit = s.Circuit, sound = on ? s.OnSound : s.OffSound;
    Set(world, circuit, on);
    Openable::PlaySound(sound, glm::vec3(world.GetCachedWorldTransform(e)[3]));
    // The rooms' reflections were captured with the old lighting: retake them (spread over a few frames, ~6 a probe).
    ReflectionProbeCapture::RequestRecapture();
}

void SetAll(World& world, bool on) {
    StartAll(world);
    std::unordered_map<std::string, bool> names;
    for (auto e : world.Registry.view<LightCircuitComponent>()) names[world.Registry.get<LightCircuitComponent>(e).Circuit] = true;
    for (auto e : world.Registry.view<LightSwitchComponent>()) names[world.Registry.get<LightSwitchComponent>(e).Circuit] = true;
    for (const auto& kv : names) Set(world, kv.first, on);
}

} // namespace LightSwitch
