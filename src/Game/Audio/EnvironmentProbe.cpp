#include "EnvironmentProbe.h"

#include "GameModuleAPI.h" // RaycastHit, QueryFilter
#include "PhysicsWorld.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

const char* SpaceClassName(SpaceClass c) {
    switch (c) {
    case SpaceClass::OutdoorOpen: return "outdoor_open";
    case SpaceClass::OutdoorUrban: return "outdoor_urban";
    case SpaceClass::IndoorSmall: return "indoor_small";
    case SpaceClass::IndoorLarge: return "indoor_large";
    }
    return "outdoor_open";
}

bool ParseSpaceClass(const char* name, SpaceClass& out) {
    for (int i = 0; i < kSpaceClassCount; ++i)
        if (std::strcmp(name, SpaceClassName((SpaceClass)i)) == 0) {
            out = (SpaceClass)i;
            return true;
        }
    return false;
}

namespace {

// 0 below lo, 1 above hi, a smoothstep between (a step at lo when the band has no width).
float Band(float x, float lo, float hi) {
    if (hi - lo < 1e-5f) return x >= 0.5f * (lo + hi) ? 1.0f : 0.0f;
    const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

EnvironmentProbe::RayFn EnvironmentProbe::PhysicsRays() {
    return [](const glm::vec3& origin, const glm::vec3& dir, float maxDistance, float& hitDistance) {
        const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z};
        QueryFilter f;
        f.HitTriggers = 0;
        RaycastHit hit;
        if (!PhysicsWorld::RaycastSolid(o, d, maxDistance, f, hit) || !hit.Hit) return false;
        hitDistance = hit.Distance;
        return true;
    };
}

std::vector<EnvironmentRay> EnvironmentProbe::Rays(int rayCount) {
    const int n = std::clamp(rayCount, 6, 64);
    const int rest = n - 1;
    const int horizontal = std::max(3, (int)std::ceil(rest * 0.6));
    const int diagonal = rest - horizontal;
    const float kTwoPi = 6.28318530718f;
    std::vector<EnvironmentRay> rays;
    rays.reserve((size_t)n);
    EnvironmentRay up;
    up.Dir = glm::vec3(0.0f, 1.0f, 0.0f);
    up.Type = EnvironmentRay::Kind::Up;
    rays.push_back(up);
    const float c45 = 0.70710678f;
    for (int i = 0; i < diagonal; ++i) { // staggered half a step against the horizontal ring so they don't line up
        const float a = kTwoPi * ((float)i + 0.5f) / (float)diagonal;
        EnvironmentRay r;
        r.Dir = glm::vec3(std::cos(a) * c45, c45, std::sin(a) * c45);
        r.Type = EnvironmentRay::Kind::Diagonal;
        rays.push_back(r);
    }
    for (int i = 0; i < horizontal; ++i) {
        const float a = kTwoPi * (float)i / (float)horizontal;
        EnvironmentRay r;
        r.Dir = glm::vec3(std::cos(a), 0.0f, std::sin(a));
        r.Type = EnvironmentRay::Kind::Horizontal;
        rays.push_back(r);
    }
    return rays;
}

void EnvironmentProbe::Weights(float cover, float wall, float meanWallDistance, const EnvironmentSettings& s, float out[kSpaceClassCount]) {
    const float bf = std::max(0.0f, s.BlendFraction);
    const float large = std::max(0.01f, s.LargeRoomDistance);
    const float bd = std::clamp(s.BlendDistance, 0.0f, 0.95f) * large;
    const float indoor = Band(cover, s.IndoorCover - bf, s.IndoorCover + bf);
    const float isLarge = Band(meanWallDistance, large - bd, large + bd);
    const float urban = Band(wall, s.UrbanWall - bf, s.UrbanWall + bf);
    out[(int)SpaceClass::IndoorSmall] = indoor * (1.0f - isLarge);
    out[(int)SpaceClass::IndoorLarge] = indoor * isLarge;
    out[(int)SpaceClass::OutdoorUrban] = (1.0f - indoor) * urban;
    out[(int)SpaceClass::OutdoorOpen] = (1.0f - indoor) * (1.0f - urban);
}

EnvironmentReading EnvironmentProbe::Classify(const std::vector<EnvironmentRay>& rays, const EnvironmentSettings& s) {
    EnvironmentReading r;
    const float maxD = std::max(0.1f, s.MaxDistance);
    int nUp = 0, nDiag = 0, nHor = 0, hitUp = 0, hitDiag = 0, wallHor = 0, hitAll = 0;
    float sumHor = 0.0f, sumAll = 0.0f;
    for (const EnvironmentRay& ray : rays) {
        const bool hit = ray.Hit && ray.Distance <= maxD;
        const float d = hit ? ray.Distance : maxD;
        sumAll += d;
        hitAll += hit ? 1 : 0;
        switch (ray.Type) {
        case EnvironmentRay::Kind::Up:
            ++nUp;
            hitUp += hit ? 1 : 0;
            break;
        case EnvironmentRay::Kind::Diagonal:
            ++nDiag;
            hitDiag += hit ? 1 : 0;
            break;
        case EnvironmentRay::Kind::Horizontal:
            ++nHor;
            sumHor += d;
            wallHor += (hit && ray.Distance <= s.UrbanDistance) ? 1 : 0;
            break;
        }
    }
    if (rays.empty()) return r;
    const float up = nUp > 0 ? (float)hitUp / (float)nUp : 0.0f;
    const float diag = nDiag > 0 ? (float)hitDiag / (float)nDiag : up;
    r.Cover = 0.5f * up + 0.5f * diag;
    r.CeilingHit = hitUp > 0;
    r.Wall = nHor > 0 ? (float)wallHor / (float)nHor : 0.0f;
    r.MeanWallDistance = nHor > 0 ? sumHor / (float)nHor : maxD;
    r.Enclosure = (float)hitAll / (float)rays.size();
    r.MeanDistance = sumAll / (float)rays.size();
    Weights(r.Cover, r.Wall, r.MeanWallDistance, s, r.Weights);
    int best = 0;
    for (int i = 1; i < kSpaceClassCount; ++i)
        if (r.Weights[i] > r.Weights[best]) best = i;
    r.Dominant = (SpaceClass)best;
    r.Valid = true;
    return r;
}

EnvironmentReading EnvironmentProbe::Probe(const glm::vec3& origin, const EnvironmentSettings& s, std::vector<EnvironmentRay>* raysOut) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!m_Ray) m_Ray = PhysicsRays();
    std::vector<EnvironmentRay> rays = Rays(s.RayCount);
    const float maxD = std::max(0.1f, s.MaxDistance);
    for (EnvironmentRay& ray : rays) {
        float d = 0.0f;
        ray.Hit = m_Ray(origin, ray.Dir, maxD, d);
        ray.Distance = ray.Hit ? d : maxD;
    }
    EnvironmentReading r = Classify(rays, s);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    ++m_Stats.Refreshes;
    m_Stats.Rays += (int)rays.size();
    m_Stats.TotalMicros += us;
    m_Stats.MaxMicros = std::max(m_Stats.MaxMicros, us);
    if (raysOut) *raysOut = std::move(rays);
    return r;
}

const EnvironmentReading& EnvironmentProbe::Query(std::uint32_t shooter, const glm::vec3& pos, double now, const EnvironmentSettings& s, bool* refreshed) {
    if (refreshed) *refreshed = false;
    constexpr size_t kMaxEntries = 64;
    Entry* e = nullptr;
    if (shooter != 0) {
        for (Entry& c : m_Entries)
            if (c.Id == shooter) {
                e = &c;
                break;
            }
    } else {
        float best = s.MatchRadius * s.MatchRadius;
        for (Entry& c : m_Entries) {
            if (c.Id != 0) continue;
            const glm::vec3 d = c.LastSeen - pos;
            const float d2 = glm::dot(d, d);
            if (d2 <= best) {
                best = d2;
                e = &c;
            }
        }
    }
    if (!e) {
        if (m_Entries.size() >= kMaxEntries) { // forget the shooter heard longest ago
            size_t oldest = 0;
            for (size_t i = 1; i < m_Entries.size(); ++i)
                if (m_Entries[i].LastUsed < m_Entries[oldest].LastUsed) oldest = i;
            m_Entries.erase(m_Entries.begin() + (std::ptrdiff_t)oldest);
        }
        m_Entries.emplace_back();
        e = &m_Entries.back();
        e->Id = shooter;
    }
    e->LastSeen = pos;
    e->LastUsed = now;
    const glm::vec3 moved = pos - e->ProbedAt;
    const bool stale = !e->Reading.Valid || now - e->ProbeTime >= s.RefreshInterval ||
                       glm::dot(moved, moved) > s.RefreshMoveDistance * s.RefreshMoveDistance;
    if (stale) {
        e->Reading = Probe(pos, s, s.DebugDraw ? &e->Rays : nullptr);
        if (!s.DebugDraw) e->Rays.clear();
        e->ProbeTime = now;
        e->ProbedAt = pos;
        e->Debug = s.DebugDraw;
        e->MaxDistance = s.MaxDistance;
        if (refreshed) *refreshed = true;
    }
    return e->Reading;
}

void EnvironmentProbe::Clear() {
    m_Entries.clear();
    m_Stats = Stats{};
}

void EnvironmentProbe::DebugLines(std::vector<float>& out) const {
    for (const Entry& e : m_Entries) {
        if (!e.Debug) continue;
        for (const EnvironmentRay& r : e.Rays) {
            const glm::vec3 a = e.ProbedAt, b = e.ProbedAt + r.Dir * (r.Hit ? r.Distance : e.MaxDistance);
            const glm::vec4 c = r.Hit ? glm::vec4(1.0f, 0.55f, 0.1f, 0.9f) : glm::vec4(0.2f, 0.9f, 0.9f, 0.3f);
            for (const glm::vec3& p : {a, b}) {
                out.insert(out.end(), {p.x, p.y, p.z, c.r, c.g, c.b, c.a});
            }
        }
    }
}
