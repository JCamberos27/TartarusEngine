#include "ShellCasings.h"

#include "AssetLibrary.h"
#include "Camera.h"
#include "Components.h"
#include "GameModuleAPI.h"
#include "Log.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "ProjectPaths.h"
#include "World.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {
// The shortest rotation taking unit vector `from` onto unit vector `to`.
glm::quat Between(const glm::vec3& from, const glm::vec3& to) {
    const float d = glm::dot(from, to);
    if (d > 0.99999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.99999f) {
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), from);
        if (glm::dot(axis, axis) < 1e-6f) axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), from);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 c = glm::cross(from, to);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

// `rot` turned the least it takes to lay `longAxis` (model space) flat: a case at rest on its side.
glm::quat LyingFlat(const glm::quat& rot, const glm::vec3& longAxis) {
    const glm::vec3 a = rot * longAxis;
    glm::vec3 h(a.x, 0.0f, a.z);
    if (glm::dot(h, h) < 1e-6f) h = rot * glm::vec3(1.0f, 0.0f, 0.0f), h.y = 0.0f; // stood on end
    if (glm::dot(h, h) < 1e-6f) h = glm::vec3(1.0f, 0.0f, 0.0f);
    return glm::normalize(Between(a, glm::normalize(h)) * rot);
}
} // namespace

bool ShellCasings::InView(const glm::vec3& camPos, const glm::vec3& front, const glm::vec3& right, const glm::vec3& up,
                          float fovDegrees, float aspect, const glm::vec3& point, float radius) {
    const glm::vec3 d = point - camPos;
    const float z = glm::dot(d, front);
    if (z < -radius) return false; // behind the camera
    const float tanV = std::tan(glm::radians(std::clamp(fovDegrees, 1.0f, 179.0f)) * 0.5f);
    const float tanH = tanV * std::max(aspect, 0.1f);
    // Each side plane, padded by the radius measured perpendicular to that plane.
    const float x = std::fabs(glm::dot(d, right)), y = std::fabs(glm::dot(d, up));
    const float zc = std::max(z, 0.0f);
    if (x - radius * std::sqrt(1.0f + tanH * tanH) > zc * tanH) return false;
    if (y - radius * std::sqrt(1.0f + tanV * tanV) > zc * tanV) return false;
    return true;
}

std::vector<int> ShellCasings::PickRemovals(const std::vector<LifeInput>& cases, const glm::vec3& player,
                                            const Settings& s) {
    std::vector<int> out;
    std::vector<char> gone(cases.size(), 0);
    // Left behind and out of sight.
    const float far2 = s.DespawnDistance * s.DespawnDistance;
    for (int i = 0; i < (int)cases.size(); ++i) {
        const glm::vec3 d = cases[i].Position - player;
        if (!cases[i].Seen && glm::dot(d, d) > far2) gone[i] = 1;
    }
    int left = (int)cases.size() - (int)std::count(gone.begin(), gone.end(), 1);
    // Too many: the oldest the camera can't see, then (all in view, past the hard cap) the oldest.
    if (left > s.SoftCap || left > s.HardCap) {
        std::vector<int> byAge((int)cases.size());
        std::iota(byAge.begin(), byAge.end(), 0);
        std::sort(byAge.begin(), byAge.end(), [&](int a, int b) { return cases[a].Order < cases[b].Order; });
        for (int i : byAge) {
            if (left <= s.SoftCap) break;
            if (!gone[i] && !cases[i].Seen) gone[i] = 1, --left;
        }
        for (int i : byAge) {
            if (left <= s.HardCap) break;
            if (!gone[i]) gone[i] = 1, --left;
        }
    }
    for (int i = 0; i < (int)cases.size(); ++i)
        if (gone[i]) out.push_back(i);
    return out;
}

void ShellCasings::Spawn(World& world, AssetLibrary& assets, const CasingSpawn& spawn) {
    if (spawn.Model.empty()) return;
    Kind& kind = m_Kinds[spawn.Model + "|" + spawn.Material];
    if (!kind.Mesh) {
        kind.Mesh = assets.LoadModel(ProjectPaths::Resolve(spawn.Model));
        if (!kind.Mesh) {
            Log::Warn("Shell casings: could not load '" + spawn.Model + "'.");
            m_Kinds.erase(spawn.Model + "|" + spawn.Material);
            return;
        }
        if (!spawn.Material.empty()) {
            kind.Material = assets.LoadMaterial(ProjectPaths::Resolve(spawn.Material));
            if (kind.Material && kind.Material->Missing) kind.Material = nullptr;
            if (!kind.Material) Log::Warn("Shell casings: could not load the material '" + spawn.Material + "'.");
        }
    }
    Case c;
    // Its long axis is the mesh's longest extent; the sweep is a sphere as wide as the case.
    const glm::vec3 size = glm::max(kind.Mesh->BoundsMax() - kind.Mesh->BoundsMin(), glm::vec3(1e-4f));
    const int axis = size.x >= size.y && size.x >= size.z ? 0 : (size.y >= size.z ? 1 : 2);
    c.LongAxis = glm::vec3(0.0f);
    c.LongAxis[axis] = 1.0f;
    c.Length = size[axis];
    c.Radius = 0.5f * std::min({size[(axis + 1) % 3], size[(axis + 2) % 3]});
    c.Position = spawn.Position;
    c.Velocity = spawn.Velocity;
    c.Rotation = glm::normalize(spawn.Rotation);
    c.AngularVelocity = spawn.AngularVelocity;
    c.Order = m_Order++;
    c.Entity = world.CreateModelEntity(kind.Mesh->CreateInstance(), c.Position, glm::vec3(0.0f), glm::vec3(1.0f),
                                       "[Runtime] Casing");
    auto& renderable = world.Registry.get<RenderableComponent>(c.Entity);
    // A few centimetres of brass: its shadow is a speck that would cost a shadow draw per cascade.
    renderable.CastShadows = RenderableComponent::ShadowCasting::Off;
    if (kind.Material) renderable.Materials.assign((std::size_t)std::max(1, renderable.ModelRef->MeshCount()), kind.Material);
    world.SetWorldPose(c.Entity, c.Position, c.Rotation);
    m_Cases.push_back(c);
}

void ShellCasings::Step(Case& c, float dt) {
    c.Age += dt;
    c.Velocity.y -= m_Settings.Gravity * dt;
    glm::vec3 move = c.Velocity * dt;
    bool landed = false;
    // Up to two contacts a step: a bounce off a wall into the floor, say.
    for (int pass = 0; pass < 2; ++pass) {
        const float len = glm::length(move);
        if (len < 1e-7f) break;
        const glm::vec3 dir = move / len;
        const float o[3] = {c.Position.x, c.Position.y, c.Position.z}, d[3] = {dir.x, dir.y, dir.z};
        RaycastHit hit;
        QueryFilter filter;
        filter.HitTriggers = 0;
        if (!PhysicsWorld::SphereCastSolid(o, d, c.Radius, len, filter, hit) || !hit.Hit) {
            c.Position += move;
            break;
        }
        glm::vec3 n(hit.Normal[0], hit.Normal[1], hit.Normal[2]);
        if (glm::dot(n, n) < 1e-6f || glm::dot(c.Velocity, n) >= 0.0f) { // started touching, moving away
            c.Position += move;
            break;
        }
        n = glm::normalize(n);
        const float travel = std::max(hit.Distance - 1e-4f, 0.0f);
        c.Position += dir * travel;
        const glm::vec3 vn = glm::dot(c.Velocity, n) * n, vt = c.Velocity - vn;
        c.Velocity = vt * (1.0f - m_Settings.Friction) - vn * m_Settings.Restitution;
        // A hard knock sets it tumbling a new way; a soft one just slows the spin.
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        const float knock = glm::length(vn);
        c.AngularVelocity = c.AngularVelocity * 0.5f + glm::vec3(u(m_Rng), u(m_Rng), u(m_Rng)) * knock * 8.0f;
        if (n.y > 0.5f) landed = true;
        move = c.Velocity * dt * std::max(0.0f, 1.0f - travel / len);
    }
    const float w = glm::length(c.AngularVelocity);
    if (w > 1e-5f) c.Rotation = glm::normalize(glm::angleAxis(w * dt, c.AngularVelocity / w) * c.Rotation);

    // Slow on the ground: it rolls onto its side and stops.
    if (landed && glm::length(c.Velocity) < 0.6f) c.GroundTime += dt;
    else if (!landed && c.GroundTime > 0.0f && c.Velocity.y < -0.5f) c.GroundTime = 0.0f; // off an edge
    if (c.GroundTime > 0.0f) {
        const float k = 1.0f - std::exp(-dt * 18.0f);
        c.Rotation = glm::normalize(glm::slerp(c.Rotation, LyingFlat(c.Rotation, c.LongAxis), k));
        c.Velocity.x *= 1.0f - k * 0.5f;
        c.Velocity.z *= 1.0f - k * 0.5f;
        c.AngularVelocity *= 1.0f - k;
        if (c.GroundTime > 0.3f) {
            c.Rotation = LyingFlat(c.Rotation, c.LongAxis);
            c.Velocity = c.AngularVelocity = glm::vec3(0.0f);
            c.Sleeping = true;
        }
    }
}

void ShellCasings::Update(World& world, float dt, const Camera& camera, const glm::vec3& player) {
    // Entities a scene change took with it.
    m_Cases.erase(std::remove_if(m_Cases.begin(), m_Cases.end(),
                                 [&](const Case& c) { return !world.Registry.valid(c.Entity); }),
                  m_Cases.end());
    if (m_Cases.empty()) return;

    if (dt > 0.0f) {
        const bool recording = PhysicsWorld::GetQueryRecording();
        PhysicsWorld::SetQueryRecording(false); // plumbing: kept off the physics debug overlay
        const float h = std::min(dt, 0.05f);
        for (Case& c : m_Cases) {
            if (c.Sleeping) continue;
            // Small steps while it's fast, so a quick case doesn't skip through a thin rail.
            const int sub = std::clamp((int)std::ceil(glm::length(c.Velocity) * h / std::max(c.Radius, 0.002f)), 1, 4);
            for (int i = 0; i < sub && !c.Sleeping; ++i) Step(c, h / (float)sub);
            world.SetWorldPose(c.Entity, c.Position, c.Rotation);
        }
        PhysicsWorld::SetQueryRecording(recording);
    }

    // Who may go. Only cases a rule would remove need the (costly) line-of-sight check; the rest
    // keep Seen = in view. Checked oldest first, a few a frame - an unchecked one counts as seen.
    const glm::vec3 front = camera.Front(), right = camera.Right(), up = camera.Up();
    std::vector<LifeInput> life(m_Cases.size());
    const float far2 = m_Settings.DespawnDistance * m_Settings.DespawnDistance;
    const bool crowded = (int)m_Cases.size() > m_Settings.SoftCap;
    int rays = m_Settings.OcclusionRaysPerFrame;
    const bool recording = PhysicsWorld::GetQueryRecording();
    PhysicsWorld::SetQueryRecording(false);
    for (std::size_t i = 0; i < m_Cases.size(); ++i) { // m_Cases is in spawn order: oldest first
        const Case& c = m_Cases[i];
        LifeInput& in = life[i];
        in.Position = c.Position;
        in.Order = c.Order;
        const float reach = 0.5f * c.Length + c.Radius;
        in.Seen = InView(camera.Position, front, right, up, camera.Fov, m_Settings.ViewAspect, c.Position, reach);
        if (c.Position.y < -1000.0f) in.Seen = false; // fell out of the world: nobody is watching it there
        if (!in.Seen || rays <= 0) continue;
        const glm::vec3 d = c.Position - player;
        if (!crowded && glm::dot(d, d) <= far2) continue;
        // Behind something solid from the camera: as good as out of view.
        const glm::vec3 to = c.Position - camera.Position;
        const float dist = glm::length(to);
        if (dist < 1e-3f) continue;
        --rays;
        const glm::vec3 dir = to / dist;
        const float o[3] = {camera.Position.x, camera.Position.y, camera.Position.z}, dd[3] = {dir.x, dir.y, dir.z};
        RaycastHit hit;
        QueryFilter filter;
        filter.HitTriggers = 0;
        if (PhysicsWorld::RaycastSolid(o, dd, dist, filter, hit) && hit.Hit && hit.Distance < dist - reach - 0.02f)
            in.Seen = false;
    }
    PhysicsWorld::SetQueryRecording(recording);

    const std::vector<int> removals = PickRemovals(life, player, m_Settings);
    if (removals.empty()) return;
    std::vector<char> gone(m_Cases.size(), 0);
    for (int i : removals) {
        gone[i] = 1;
        if (world.Registry.valid(m_Cases[i].Entity)) world.DestroyEntityAndChildren(m_Cases[i].Entity);
    }
    std::size_t w = 0;
    for (std::size_t i = 0; i < m_Cases.size(); ++i)
        if (!gone[i]) m_Cases[w++] = m_Cases[i];
    m_Cases.resize(w);
}

void ShellCasings::Clear(World& world) {
    for (const Case& c : m_Cases)
        if (world.Registry.valid(c.Entity)) world.DestroyEntityAndChildren(c.Entity);
    m_Cases.clear();
    m_Kinds.clear();
}

int ShellCasings::SleepingCount() const {
    return (int)std::count_if(m_Cases.begin(), m_Cases.end(), [](const Case& c) { return c.Sleeping; });
}
