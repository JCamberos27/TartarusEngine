#include "ReverbZones.h"

#include "World.h"

#include <algorithm>
#include <cmath>

namespace {

bool SamePreset(const ReverbPreset& a, const ReverbPreset& b) {
    return a.RoomSize == b.RoomSize && a.DecayTime == b.DecayTime && a.HfDamping == b.HfDamping && a.PreDelayMs == b.PreDelayMs &&
           a.WetLevel == b.WetLevel && a.EarlyLateMix == b.EarlyLateMix;
}
bool Same(const ReverbZoneComponent& a, const ReverbZoneComponent& b) {
    return a.Enabled == b.Enabled && a.Shape == b.Shape && a.Extents == b.Extents && a.Radius == b.Radius && a.Priority == b.Priority &&
           a.FadeDistance == b.FadeDistance && a.TailClass == b.TailClass && a.TailGain == b.TailGain && a.ReverbMode == b.ReverbMode &&
           SamePreset(a.Reverb, b.Reverb);
}
bool Same(const ReverbPortalComponent& a, const ReverbPortalComponent& b) {
    return a.Enabled == b.Enabled && a.Extents == b.Extents && a.OpenAmount == b.OpenAmount && a.RoomA == b.RoomA && a.RoomB == b.RoomB &&
           a.ProbeDistance == b.ProbeDistance && a.ClosedGainDb == b.ClosedGainDb && a.ClosedCutoff == b.ClosedCutoff &&
           a.DiffractionCutoff == b.DiffractionCutoff && a.DiffractionMaxAngle == b.DiffractionMaxAngle && a.DiffractionGainDb == b.DiffractionGainDb;
}

void ApplyZone(ReverbZoneVolume& v, const ReverbZoneComponent& c, const glm::mat4& m, entt::entity e) {
    glm::mat3 rot(m);
    for (int i = 0; i < 3; ++i) {
        const float len = glm::length(rot[i]);
        if (len > 1e-6f) rot[i] /= len; // the scale is not the zone's
    }
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
    v.Placed = m;
    v.Src = c;
}

void ApplyPortalParams(ReverbPortalVolume& v, const ReverbPortalComponent& c) {
    v.Extents = glm::max(c.Extents, glm::vec3(0.01f));
    v.Open = std::clamp(c.OpenAmount, 0.0f, 1.0f);
    v.ProbeDistance = std::max(c.ProbeDistance, 0.01f);
    v.ClosedGainDb = c.ClosedGainDb;
    v.ClosedCutoff = std::clamp(c.ClosedCutoff, 50.0f, 20000.0f);
    v.DiffractionCutoff = std::clamp(c.DiffractionCutoff, 50.0f, 20000.0f);
    v.DiffractionMaxAngle = std::max(c.DiffractionMaxAngle, 1.0f);
    v.DiffractionGainDb = c.DiffractionGainDb;
    v.Src = c;
}

void PlacePortal(ReverbPortalVolume& v, const glm::mat4& m, entt::entity e) {
    glm::vec3 r(m[0]), u(m[1]), n(m[2]);
    const auto unit = [](const glm::vec3& a, const glm::vec3& fallback) { return glm::dot(a, a) > 1e-12f ? glm::normalize(a) : fallback; };
    v.Right = unit(r, glm::vec3(1, 0, 0));
    v.Up = unit(u, glm::vec3(0, 1, 0));
    v.Normal = unit(n, glm::vec3(0, 0, 1));
    v.Center = glm::vec3(m[3]);
    v.Entity = (unsigned)e;
    v.Placed = m;
}

unsigned ZoneNamed(const World& world, const std::string& name) {
    if (name.empty()) return kNoRoom;
    for (const entt::entity e : world.Registry.view<const ReverbZoneComponent, const NameComponent>())
        if (world.Registry.get<const NameComponent>(e).Name == name) return (unsigned)e;
    return kNoRoom;
}

} // namespace

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

void ReverbPortalVolume::Loss(const glm::vec3& from, const glm::vec3& to, float& gain, float& cutoffHz, float* bendDegrees) const {
    const glm::vec3 in = Center - from, out = to - Center;
    const float li = glm::length(in), lo = glm::length(out);
    float bend = 0.0f;
    if (li > 1e-4f && lo > 1e-4f) bend = glm::degrees(std::acos(std::clamp(glm::dot(in / li, out / lo), -1.0f, 1.0f)));
    if (bendDegrees) *bendDegrees = bend;
    const float k = std::clamp(bend / std::max(DiffractionMaxAngle, 1.0f), 0.0f, 1.0f);
    const float db = (1.0f - Open) * ClosedGainDb + k * DiffractionGainDb;
    gain = std::pow(10.0f, db / 20.0f);
    const float closedHz = ClosedCutoff * std::pow(20000.0f / ClosedCutoff, Open);   // log-spaced: shut = ClosedCutoff, open = unfiltered
    const float bendHz = 20000.0f * std::pow(DiffractionCutoff / 20000.0f, k);
    cutoffHz = std::min(closedHz, bendHz);
}

void ReverbZones::Set(std::vector<ReverbZoneVolume> zones) {
    m_Zones = std::move(zones);
    std::stable_sort(m_Zones.begin(), m_Zones.end(), [](const ReverbZoneVolume& a, const ReverbZoneVolume& b) {
        if (a.Priority != b.Priority) return a.Priority > b.Priority;
        return a.Entity < b.Entity;
    });
}

void ReverbZones::ResolveRooms(ReverbPortalVolume& p, const World* world) const {
    const unsigned named[2] = {world ? ZoneNamed(*world, p.Src.RoomA) : kNoRoom, world ? ZoneNamed(*world, p.Src.RoomB) : kNoRoom};
    const float out = p.Extents.z + p.ProbeDistance;
    p.RoomA = named[0] != kNoRoom ? named[0] : RoomAt(p.Center - p.Normal * out);
    p.RoomB = named[1] != kNoRoom ? named[1] : RoomAt(p.Center + p.Normal * out);
}

void ReverbZones::Build(const World& world) {
    std::vector<ReverbZoneVolume> zones;
    for (const entt::entity e : world.Registry.view<const ReverbZoneComponent>()) {
        const ReverbZoneComponent& c = world.Registry.get<const ReverbZoneComponent>(e);
        if (!c.Enabled) continue;
        ReverbZoneVolume v;
        ApplyZone(v, c, world.GetCachedWorldTransform(e), e);
        zones.push_back(v);
    }
    Set(std::move(zones));
    m_Portals.clear();
    for (const entt::entity e : world.Registry.view<const ReverbPortalComponent>()) {
        const ReverbPortalComponent& c = world.Registry.get<const ReverbPortalComponent>(e);
        if (!c.Enabled) continue;
        ReverbPortalVolume v;
        ApplyPortalParams(v, c);
        PlacePortal(v, world.GetCachedWorldTransform(e), e);
        ResolveRooms(v, &world);
        m_Portals.push_back(v);
    }
}

bool ReverbZones::Refresh(const World& world) {
    m_Rebuilt = false;
    bool changed = false, structural = false, zonesMoved = false;
    int zoneCount = 0, portalCount = 0;
    for (const entt::entity e : world.Registry.view<const ReverbZoneComponent>()) {
        const ReverbZoneComponent& c = world.Registry.get<const ReverbZoneComponent>(e);
        if (!c.Enabled) continue;
        ++zoneCount;
        ReverbZoneVolume* v = nullptr;
        for (ReverbZoneVolume& z : m_Zones)
            if (z.Entity == (unsigned)e) { v = &z; break; }
        if (!v || c.Priority != v->Priority) { structural = true; break; }
        const glm::mat4 m = world.GetCachedWorldTransform(e);
        if (m != v->Placed || !Same(c, v->Src)) {
            ApplyZone(*v, c, m, e);
            changed = zonesMoved = true;
        }
    }
    if (!structural && zoneCount != (int)m_Zones.size()) structural = true;
    if (!structural) {
        for (const entt::entity e : world.Registry.view<const ReverbPortalComponent>()) {
            const ReverbPortalComponent& c = world.Registry.get<const ReverbPortalComponent>(e);
            if (!c.Enabled) continue;
            ++portalCount;
            ReverbPortalVolume* v = nullptr;
            for (ReverbPortalVolume& p : m_Portals)
                if (p.Entity == (unsigned)e) { v = &p; break; }
            if (!v) { structural = true; break; }
            const glm::mat4 m = world.GetCachedWorldTransform(e);
            const bool moved = m != v->Placed, edited = !Same(c, v->Src);
            if (moved || edited || zonesMoved) {
                const bool rooms = moved || zonesMoved || c.RoomA != v->Src.RoomA || c.RoomB != v->Src.RoomB || c.ProbeDistance != v->Src.ProbeDistance ||
                                   c.Extents != v->Src.Extents;
                ApplyPortalParams(*v, c);
                if (moved) PlacePortal(*v, m, e);
                if (rooms) ResolveRooms(*v, &world);
                changed = true;
            }
        }
        if (!structural && portalCount != (int)m_Portals.size()) structural = true;
    }
    if (structural) {
        Build(world);
        m_Rebuilt = true;
        return true;
    }
    return changed;
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

unsigned ReverbZones::RoomAt(const glm::vec3& p) const {
    for (const ReverbZoneVolume& z : m_Zones)
        if (z.Depth(p) > 0.0f) return z.Entity;
    return kNoRoom;
}

const ReverbZoneVolume* ReverbZones::ZoneOfRoom(unsigned room) const {
    if (room == kNoRoom) return nullptr;
    for (const ReverbZoneVolume& z : m_Zones)
        if (z.Entity == room) return &z;
    return nullptr;
}

bool ReverbZones::FindPath(const glm::vec3& source, const glm::vec3& listener, PortalPath& out) const {
    out = PortalPath{};
    if (m_Portals.empty()) return false;
    const unsigned from = RoomAt(source), to = RoomAt(listener);
    out.SourceRoom = from;
    out.ListenerRoom = to;
    if (from == to) return false;
    // Depth-first over the portals, at most three, never back into a room already passed: the shortest way wins.
    struct Frame { int Portals[3]; unsigned Rooms[4]; int Count; };
    float bestLen = 1e30f;
    int bestPortals[3] = {-1, -1, -1};
    int bestCount = 0;
    Frame stack[64];
    int top = 0;
    stack[top++] = {{-1, -1, -1}, {from, kNoRoom, kNoRoom, kNoRoom}, 0};
    while (top > 0) {
        const Frame f = stack[--top];
        const unsigned here = f.Rooms[f.Count];
        for (int i = 0; i < (int)m_Portals.size(); ++i) {
            const ReverbPortalVolume& p = m_Portals[(size_t)i];
            unsigned next;
            if (p.RoomA == here) next = p.RoomB;
            else if (p.RoomB == here) next = p.RoomA;
            else continue;
            if (next == here) continue;
            bool seen = false;
            for (int k = 0; k <= f.Count; ++k) seen |= f.Rooms[k] == next;
            if (seen) continue;
            Frame g = f;
            g.Portals[g.Count] = i;
            g.Rooms[g.Count + 1] = next;
            g.Count++;
            if (next == to) { // a complete path: its length
                float len = 0.0f;
                glm::vec3 prev = source;
                for (int k = 0; k < g.Count; ++k) {
                    const glm::vec3& c = m_Portals[(size_t)g.Portals[k]].Center;
                    len += glm::length(c - prev);
                    prev = c;
                }
                len += glm::length(listener - prev);
                if (len < bestLen) {
                    bestLen = len;
                    bestCount = g.Count;
                    for (int k = 0; k < 3; ++k) bestPortals[k] = g.Portals[k];
                }
            } else if (g.Count < 3 && top < 64) {
                stack[top++] = g;
            }
        }
    }
    if (bestCount == 0) return false;
    out.Valid = true;
    out.Count = bestCount;
    out.Length = bestLen;
    for (int k = 0; k < 3; ++k) out.Portals[k] = bestPortals[k];
    float gain = 1.0f, cutoff = 20000.0f;
    glm::vec3 prev = source;
    for (int k = 0; k < bestCount; ++k) {
        const ReverbPortalVolume& p = m_Portals[(size_t)bestPortals[k]];
        const glm::vec3 next = k + 1 < bestCount ? m_Portals[(size_t)bestPortals[k + 1]].Center : listener;
        float g, c;
        p.Loss(prev, next, g, c);
        gain *= g;
        cutoff = std::min(cutoff, c);
        prev = p.Center;
    }
    out.Gain = gain;
    out.CutoffHz = cutoff;
    const glm::vec3 last = m_Portals[(size_t)bestPortals[bestCount - 1]].Center;
    glm::vec3 dir = last - listener;
    if (glm::dot(dir, dir) < 1e-4f) dir = last - source;
    out.Virtual = listener + (glm::dot(dir, dir) > 1e-8f ? glm::normalize(dir) : glm::vec3(0.0f, 0.0f, -1.0f)) * bestLen;
    return true;
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
    // Portals: the opening's rectangle (green open .. red shut), the way it faces (room B), a cross when it is shut.
    for (const ReverbPortalVolume& p : m_Portals) {
        const glm::vec4 col(1.0f - p.Open, 0.3f + 0.7f * p.Open, 0.2f, 0.95f);
        const glm::vec3 r = p.Right * p.Extents.x, u = p.Up * p.Extents.y;
        const glm::vec3 c[4] = {p.Center - r - u, p.Center + r - u, p.Center + r + u, p.Center - r + u};
        for (int i = 0; i < 4; ++i) line(c[i], c[(i + 1) % 4], col);
        line(p.Center, p.Center + p.Normal * 0.6f, col);
        if (p.Open < 0.05f) {
            line(c[0], c[2], col);
            line(c[1], c[3], col);
        }
    }
}
