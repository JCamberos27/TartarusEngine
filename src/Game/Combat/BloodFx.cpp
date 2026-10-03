#include "BloodFx.h"

#include "BloodRenderer.h"
#include "GameModuleAPI.h"
#include "PhysicsWorld.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace {
glm::mat4 FromRows(const float m[12]) {
    glm::mat4 r(1.0f);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col) r[col][row] = m[row * 4 + col];
    return r;
}

bool PhysicsRay(const glm::vec3& origin, const glm::vec3& dir, float maxDistance, glm::vec3& point, glm::vec3& normal) {
    const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z};
    QueryFilter f;
    f.HitTriggers = 0;
    RaycastHit h;
    if (!PhysicsWorld::RaycastSolid(o, d, maxDistance, f, h) || !h.Hit) return false;
    point = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
    normal = glm::vec3(h.Normal[0], h.Normal[1], h.Normal[2]);
    return true;
}

// A horizontal unit vector along `dir` (the sims only yaw); straight up / down falls back to +X.
glm::vec3 Flat(const glm::vec3& dir) {
    glm::vec3 f(dir.x, 0.0f, dir.z);
    const float l = glm::length(f);
    return l > 1e-4f ? f / l : glm::vec3(1.0f, 0.0f, 0.0f);
}
} // namespace

int BloodFx::FrameAt(float t01, float framesCount) {
    const float t = std::clamp(t01, 0.0f, 1.0f);
    return std::clamp((int)std::floor(t * framesCount + 0.1f), 0, std::max(0, (int)framesCount));
}

float BloodFx::PlaybackSeconds(const BloodSprayDef& def, float animationSpeed, float size) {
    return def.TimeLimit / std::max(animationSpeed, 1e-3f) * std::sqrt(std::max(size, 0.05f));
}

BloodFx::Choice BloodFx::Choose(const Hit& hit, float roll) {
    // Sizes are x the prefab's authored splash, which is a cinematic few metres: the game's are
    // smaller - bloody, not a fire hose - and grow with how bad the wound is.
    if (hit.Player) return {"blood3", 0.45f};
    if (hit.Corpse) return {roll < 0.5f ? "blood5" : "blood4", 0.5f};
    if (hit.Direction.y < -0.8f) return {"blood2", 0.6f}; // straight down into someone on the ground
    if (hit.Head && hit.Killed) return {roll < 0.5f ? "blood7" : "blood8", roll < 0.5f ? 0.8f : 0.65f};
    if (hit.Killed) {
        static const Choice kKill[] = {{"blood1", 0.75f}, {"blood9", 0.55f}, {"blood2_left", 0.75f}, {"blood2_right", 0.75f}};
        return kKill[std::min(3, (int)(roll * 4.0f))];
    }
    if (hit.Head) return {"blood5", 0.9f};
    static const Choice kHit[] = {{"blood3", 0.6f}, {"blood4", 0.85f}, {"blood6", 0.8f}, {"blood5", 0.95f}};
    return kHit[std::min(3, (int)(roll * 4.0f))];
}

glm::mat4 BloodFx::PrefabToWorld(const glm::vec3& exitPoint, const glm::vec3& dir, float size, float yawJitterRad) {
    const glm::vec3 f = Flat(dir);
    const float yaw = std::atan2(-f.z, f.x) + yawJitterRad; // prefab +X onto f (right-handed, about +Y)
    glm::mat4 m = glm::translate(glm::mat4(1.0f), exitPoint);
    m = glm::rotate(m, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    return glm::scale(m, glm::vec3(size));
}

float BloodFx::Random01() {
    m_Rng ^= m_Rng << 13;
    m_Rng ^= m_Rng >> 17;
    m_Rng ^= m_Rng << 5;
    return (float)(m_Rng & 0xFFFFFFu) / 16777216.0f;
}

void BloodFx::Clear() {
    m_Sprays.clear();
    m_Recent.clear();
    m_Now = 0.0f;
}

void BloodFx::OnFleshHit(const Hit& hit) {
    if (!Config.Enabled) return;
    // One spray per body per moment: a shotgun's pellets arrive together.
    for (const Recent& r : m_Recent)
        if (r.Entity == hit.Entity && m_Now - r.Time < 0.08f) return;
    m_Recent.push_back({hit.Entity, m_Now});

    Choice c = Choose(hit, Random01());
    const BloodPresetDef* preset = FindBloodPreset(c.Preset);
    if (!preset) return;
    float size = c.Size * Config.Size * (0.88f + 0.24f * Random01());
    size *= std::clamp(0.85f + hit.Damage / 220.0f, 0.85f, 1.2f);
    if (hit.Pellets > 1) size *= 1.15f;

    const glm::vec3 dir = glm::length(hit.Direction) > 1e-5f ? glm::normalize(hit.Direction) : glm::vec3(0.0f, 0.0f, -1.0f);
    // Out of the far side: the round's exit, still inside the body's silhouette.
    const glm::vec3 exitPoint = hit.Point + dir * (hit.Head ? 0.07f : 0.13f);
    const float jitter = (Random01() - 0.5f) * 0.45f;
    const glm::mat4 toWorld = PrefabToWorld(exitPoint, dir, size, jitter);
    SpawnSprays(*preset, toWorld, size, exitPoint, Flat(dir), hit.Entity, 0.85f + 0.25f * Random01());
}

void BloodFx::SpawnSprays(const BloodPresetDef& preset, const glm::mat4& prefabToWorld, float size, const glm::vec3& wound,
                          const glm::vec3& flatDir, unsigned entity, float tint) {
    if (!m_SimLookup) m_SimLookup = [](const char* sim) { return BloodRenderer::Get().SimIndex(sim); };
    if (!m_Ray) m_Ray = PhysicsRay;
    // An obstacle along the spray's path: the fluid stops at it rather than passing through.
    glm::vec4 clip(0.0f, 0.0f, 0.0f, 1.0f);
    m_LastClipped = false;
    {
        glm::vec3 p, n;
        const float reach = 3.2f * size / 0.6f;
        if (m_Ray(wound, flatDir, reach, p, n) && glm::dot(n, flatDir) < -0.2f) {
            n = glm::normalize(n);
            clip = glm::vec4(n, -glm::dot(n, p) + 0.004f);
            m_LastClipped = true;
        }
    }
    for (const BloodSprayDef& def : preset.Sprays) {
        const int sim = m_SimLookup(def.Sim);
        if (sim < 0) continue;
        if ((int)m_Sprays.size() >= std::max(1, Config.MaxSprays)) m_Sprays.erase(m_Sprays.begin());
        Spray s;
        s.Sim = sim;
        s.Model = prefabToWorld * FromRows(def.M);
        s.Duration = PlaybackSeconds(def, preset.AnimationSpeed, size);
        s.FramesCount = def.FramesCount;
        s.ClipPlane = clip;
        s.Tint = glm::vec3(tint);
        s.Entity = entity;
        m_Sprays.push_back(s);
        ++m_SpraysSpawned;
    }
}

void BloodFx::Update(float dt) {
    m_Now += dt;
    for (Spray& s : m_Sprays) s.Age += dt;
    m_Sprays.erase(std::remove_if(m_Sprays.begin(), m_Sprays.end(), [](const Spray& s) { return s.Age >= s.Duration; }),
                   m_Sprays.end());
    m_Recent.erase(std::remove_if(m_Recent.begin(), m_Recent.end(), [&](const Recent& r) { return m_Now - r.Time > 0.25f; }),
                   m_Recent.end());
}

void BloodFx::Submit(BloodRenderer& renderer) const {
    for (const Spray& s : m_Sprays) {
        BloodRenderer::Spray d;
        d.Sim = s.Sim;
        d.Model = s.Model;
        d.Frame = FrameAt(s.Age / std::max(s.Duration, 1e-3f), s.FramesCount);
        d.Tint = s.Tint;
        d.ClipPlane = s.ClipPlane;
        renderer.AddSpray(d);
    }
}
