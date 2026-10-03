#include "ReverbZones.h"

#include "World.h"

#include <algorithm>
#include <cmath>

float ReverbZoneVolume::Depth(const glm::vec3& p) const {
    const glm::vec3 l = ToLocal * (p - Center);
    if (Shape == 1) return Radius - glm::length(l);
    const glm::vec3 d = Extents - glm::abs(l); // per axis, how far inside that pair of faces
    return std::min({d.x, d.y, d.z});
}

float ReverbZoneVolume::Weight(const glm::vec3& p) const {
    const float depth = Depth(p);
    if (depth <= 0.0f) return 0.0f;
    if (FadeDistance <= 1e-4f) return 1.0f;
    const float t = std::clamp(depth / FadeDistance, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void ReverbZones::Set(std::vector<ReverbZoneVolume> zones) {
    m_Zones = std::move(zones);
    std::stable_sort(m_Zones.begin(), m_Zones.end(), [](const ReverbZoneVolume& a, const ReverbZoneVolume& b) {
        if (a.Priority != b.Priority) return a.Priority > b.Priority;
        return a.Entity < b.Entity;
    });
}

void ReverbZones::Build(const World& world) {
    std::vector<ReverbZoneVolume> zones;
    for (const entt::entity e : world.Registry.view<const ReverbZoneComponent>()) {
        const ReverbZoneComponent& c = world.Registry.get<const ReverbZoneComponent>(e);
        if (!c.Enabled) continue;
        const glm::mat4 m = world.ComposeWorldTransform(e);
        glm::mat3 rot(m);
        for (int i = 0; i < 3; ++i) {
            const float len = glm::length(rot[i]);
            if (len > 1e-6f) rot[i] /= len; // the scale is not the zone's
        }
        ReverbZoneVolume v;
        v.Center = glm::vec3(m[3]);
        v.ToLocal = glm::transpose(rot);
        v.Shape = c.Shape == 1 ? 1 : 0;
        v.Extents = glm::max(c.Extents, glm::vec3(0.01f));
        v.Radius = std::max(c.Radius, 0.01f);
        v.Priority = c.Priority;
        v.FadeDistance = std::max(c.FadeDistance, 0.0f);
        v.Class = (SpaceClass)std::clamp(c.TailClass, 0, kSpaceClassCount - 1);
        v.TailGain = c.TailGain;
        v.Reverb = c.Resolved();
        v.Entity = (unsigned)e;
        zones.push_back(v);
    }
    Set(std::move(zones));
}

ReverbZoneMix ReverbZones::Mix(const glm::vec3& p) const {
    ReverbZoneMix mix;
    float remaining = 1.0f, gain = 0.0f;
    for (const ReverbZoneVolume& z : m_Zones) { // highest priority first
        if (remaining <= 1e-5f) break;
        const float w = z.Weight(p);
        if (w <= 0.0f) continue;
        const float take = remaining * w;
        mix.Weights[(int)z.Class] += take;
        gain += take * z.TailGain;
        mix.Reverb.RoomSize += take * z.Reverb.RoomSize;
        mix.Reverb.DecayTime += take * z.Reverb.DecayTime;
        mix.Reverb.HfDamping += take * z.Reverb.HfDamping;
        mix.Reverb.PreDelayMs += take * z.Reverb.PreDelayMs;
        mix.Reverb.WetLevel += take * z.Reverb.WetLevel;
        mix.Reverb.EarlyLateMix += take * z.Reverb.EarlyLateMix;
        remaining -= take;
        ++mix.Zones;
    }
    mix.ProbeShare = std::max(remaining, 0.0f);
    mix.Gain = gain + mix.ProbeShare;
    return mix;
}

void ReverbZones::DebugLines(std::vector<float>& out) const {
    static const glm::vec4 kColors[kSpaceClassCount] = {{0.3f, 0.8f, 1.0f, 0.9f}, {0.9f, 0.8f, 0.3f, 0.9f}, {1.0f, 0.5f, 0.3f, 0.9f}, {0.8f, 0.4f, 1.0f, 0.9f}};
    auto line = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec4& c) {
        out.insert(out.end(), {a.x, a.y, a.z, c.r, c.g, c.b, c.a, b.x, b.y, b.z, c.r, c.g, c.b, c.a});
    };
    for (const ReverbZoneVolume& z : m_Zones) {
        const glm::vec4 col = kColors[(int)z.Class];
        const glm::mat3 toWorld = glm::transpose(z.ToLocal);
        auto w = [&](const glm::vec3& l) { return z.Center + toWorld * l; };
        if (z.Shape == 1) {
            for (int axis = 0; axis < 3; ++axis) {
                const int n = 32;
                for (int i = 0; i < n; ++i) {
                    const float a0 = 6.2831853f * (float)i / n, a1 = 6.2831853f * (float)(i + 1) / n;
                    glm::vec3 p0(0.0f), p1(0.0f);
                    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                    p0[u] = std::cos(a0) * z.Radius; p0[v] = std::sin(a0) * z.Radius;
                    p1[u] = std::cos(a1) * z.Radius; p1[v] = std::sin(a1) * z.Radius;
                    line(w(p0), w(p1), col);
                }
            }
        } else {
            const glm::vec3 e = z.Extents;
            for (int i = 0; i < 8; ++i)
                for (int axis = 0; axis < 3; ++axis) {
                    if (i & (1 << axis)) continue; // each edge once: from the corner with that bit clear
                    glm::vec3 a((i & 1) ? e.x : -e.x, (i & 2) ? e.y : -e.y, (i & 4) ? e.z : -e.z), b = a;
                    b[axis] = e[axis];
                    a[axis] = -e[axis];
                    line(w(a), w(b), col);
                }
        }
    }
}
