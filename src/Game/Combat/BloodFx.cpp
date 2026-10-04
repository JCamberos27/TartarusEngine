#include "BloodFx.h"

#include "BloodRenderer.h"
#include "FxSprites.h"
#include "GameModuleAPI.h"
#include "PhysicsWorld.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
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

glm::vec3 BloodFx::PrefabAxis(const BloodPresetDef& preset,
                              const std::function<const BloodFxImport::VatFrame*(const char* sim, glm::vec3& origin)>& lastFrame) {
    glm::vec3 sum(0.0f);
    for (const BloodSprayDef& def : preset.Sprays) {
        glm::vec3 origin;
        const BloodFxImport::VatFrame* f = lastFrame ? lastFrame(def.Sim, origin) : nullptr;
        if (!f) continue;
        // Where the fluid went, sim space -> the prefab's frame.
        const glm::vec3 travel(FromRows(def.M) * glm::vec4(glm::vec3(f->Centroid[0], f->Centroid[1], f->Centroid[2]) - origin, 0.0f));
        sum += glm::vec3(travel.x, 0.0f, travel.z);
    }
    return glm::length(sum) > 0.05f ? glm::normalize(sum) : glm::vec3(1.0f, 0.0f, 0.0f);
}

glm::mat4 BloodFx::PrefabToWorld(const glm::vec3& exitPoint, const glm::vec3& dir, float size, float yawJitterRad,
                                 const glm::vec3& prefabAxis, float stretch) {
    const glm::vec3 f = Flat(dir);
    // The prefab's axis onto f (right-handed, about +Y).
    const float yaw = std::atan2(-f.z, f.x) - std::atan2(-prefabAxis.z, prefabAxis.x) + yawJitterRad;
    glm::mat4 m = glm::translate(glm::mat4(1.0f), exitPoint);
    if (stretch != 1.0f) { // longer along the (jittered) line of flight: I + (k - 1) a a^T
        const glm::vec3 a = glm::vec3(glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::vec4(prefabAxis, 0.0f));
        glm::mat4 st(1.0f);
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) st[c][r] += (stretch - 1.0f) * a[c] * a[r];
        m = m * st;
    }
    m = glm::rotate(m, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    return glm::scale(m, glm::vec3(size));
}

float BloodFx::Energy(const Hit& hit) {
    // Damage per round: ~35-45 for a rifle. A shotgun's pellets arrive as one load (most of them land up close).
    float e = hit.Damage / 38.0f;
    if (hit.Pellets > 1) e *= std::sqrt((float)std::min(hit.Pellets, 12)) * 0.9f;
    if (hit.Origin != glm::vec3(0.0f)) { // a round slows over distance
        const float d = glm::length(hit.Point - hit.Origin);
        e *= std::clamp(1.25f - d / 80.0f, 0.65f, 1.25f);
    }
    if (hit.Head) e *= 1.25f;
    return std::clamp(e, 0.25f, 3.0f);
}

bool BloodFx::FindExit(const glm::vec3& entry, const glm::vec3& dir, unsigned entity, bool head, const BodyRayFn& bodyRay,
                       glm::vec3& exit) {
    exit = entry + dir * (head ? 0.07f : 0.13f);
    if (!bodyRay) return false;
    // From past the far side, back toward the entry: the first part of the same body is where the round came out.
    // Someone standing right behind is looked past.
    const float reach = head ? 0.45f : 0.7f;
    glm::vec3 from = entry + dir * reach;
    for (int tries = 0; tries < 3; ++tries) {
        const float left = glm::dot(from - entry, dir) - 0.01f;
        unsigned who = 0xFFFFFFFFu;
        int part = -1;
        glm::vec3 p;
        if (left <= 0.0f || !bodyRay(from, -dir, left, who, part, p)) return false;
        if (who != entity) { from = p - dir * 0.005f; continue; }
        if (glm::dot(p - entry, dir) < 0.03f) return false; // only the entry itself: no far side
        exit = p;
        return true;
    }
    return false;
}

float BloodFx::Random01() {
    m_Rng ^= m_Rng << 13;
    m_Rng ^= m_Rng >> 17;
    m_Rng ^= m_Rng << 5;
    return (float)(m_Rng & 0xFFFFFFu) / 16777216.0f;
}

void BloodFx::Clear() {
    m_Sprays.clear();
    m_Decals.clear();
    m_Splats.clear();
    m_Pools.clear();
    m_Recent.clear();
    m_Now = 0.0f;
}

void BloodFx::EnsureHooks() {
    if (!m_SimLookup) m_SimLookup = [](const char* sim) { return BloodRenderer::Get().SimIndex(sim); };
    if (!m_SetLookup) m_SetLookup = [](const char* set) { return BloodRenderer::Get().DecalSet(set); };
    if (!m_Ray) m_Ray = PhysicsRay;
    if (!m_BodyRay)
        m_BodyRay = [](const glm::vec3& o, const glm::vec3& d, float maxD, unsigned& entity, int& part, glm::vec3& point) {
            const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
            PhysicsWorld::BodyPartHit bp;
            if (!PhysicsWorld::RaycastBodyParts(oo, dd, maxD, bp)) return false;
            entity = bp.Entity;
            part = bp.Part;
            point = glm::vec3(bp.Point[0], bp.Point[1], bp.Point[2]);
            return true;
        };
}

void BloodFx::AddSplat(unsigned entity, int part, bool corpse, const glm::vec3& point, const glm::vec3& normal, const glm::vec3& along,
                       const char* set, float radius, float depth, float delay, float grow) {
    if (!Config.BodySplats || (entity == kPlayerEntity && !Config.GearSpatter)) return;
    EnsureHooks();
    SplatSpace space;
    if (!m_SplatSpace || !m_SplatSpace(entity, part, corpse, point, space)) return;
    const int s = m_SetLookup(set);
    if (s < 0 || space.Members.empty()) return;
    const float dry = Config.DrySeconds * 1.5f; // soaked into cloth, it stays wet longer
    for (const auto& [member, worldToBind] : space.Members) {
        const glm::mat3 lin(worldToBind);
        const float scale = glm::length(lin * glm::vec3(0.57735f)); // world metres -> bind units
        Splat sp;
        sp.Group = space.Group;
        sp.Member = member;
        sp.Center = glm::vec3(worldToBind * glm::vec4(point, 1.0f));
        sp.Normal = glm::normalize(lin * normal);
        glm::vec3 t = lin * along;
        t -= sp.Normal * glm::dot(t, sp.Normal);
        sp.Tangent = glm::length(t) > 1e-6f ? glm::normalize(t) : glm::normalize(glm::cross(sp.Normal, glm::vec3(0.3f, 0.9f, 0.1f)));
        sp.Radius = radius * scale;
        sp.Depth = depth * scale;
        sp.Set = s;
        sp.Age = -std::max(delay, 0.0f);
        sp.Grow = grow;
        sp.DrySeconds = dry;
        // A mesh holds 24 (the shader loops them); past that its oldest goes.
        int onMember = 0;
        for (const Splat& o : m_Splats) onMember += o.Member == member;
        if (onMember >= 24)
            for (auto it = m_Splats.begin(); it != m_Splats.end(); ++it)
                if (it->Member == member) { m_Splats.erase(it); break; }
        m_Splats.push_back(sp);
        ++m_SplatsSpawned;
    }
    ++m_SplatHits;
}

glm::vec3 BloodFx::RandomTangent(const glm::vec3& n) {
    const glm::vec3 a = glm::normalize(glm::cross(n, std::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
    const glm::vec3 b = glm::cross(n, a);
    const float t = Random01() * 6.2831853f;
    return a * std::cos(t) + b * std::sin(t);
}

float BloodFx::DecalCutout(const Decal& d) {
    const float t = std::max(d.Age, 0.0f);
    // The last stretch of its life: what's left shrinks back along the reveal order.
    const float tail = d.Reveal ? 0.7f * d.RevealSeconds : 8.0f;
    const float fade = std::clamp((t - (d.Life - tail)) / tail, 0.0f, 1.0f);
    if (!d.Reveal) {
        const float grow = std::clamp(t / std::max(d.PoolGrow, 1e-3f), 0.0f, 1.0f);
        const float spread = 0.9f * (1.0f - grow) * (1.0f - grow) * (1.0f - grow); // fast at first, slowing as it thins
        return std::max(spread, fade);
    }
    const float g = std::max(d.RevealSeconds, 1e-3f);
    if (fade > 0.0f) return std::clamp(d.Reveal->Eval(0.3f + 0.7f * fade), 0.0f, 1.0f);
    return std::clamp(d.Reveal->Eval(std::min(t / g, 0.3f)), 0.0f, 1.0f);
}

BloodFx::Decal* BloodFx::AddDecal(const char* set, const glm::vec3& centre, const glm::vec3& up, const glm::vec3& along,
                                  const glm::vec3& extent, float delay) {
    EnsureHooks();
    const int s = m_SetLookup(set);
    if (s < 0) return nullptr;
    if ((int)m_Decals.size() >= std::max(1, Config.MaxDecals)) m_Decals.erase(m_Decals.begin());
    const glm::vec3 y = glm::normalize(up);
    glm::vec3 x = along - y * glm::dot(along, y);
    x = glm::length(x) > 1e-4f ? glm::normalize(x) : RandomTangent(y);
    const glm::vec3 z = glm::cross(x, y);
    Decal d;
    d.Set = s;
    d.Model = glm::mat4(glm::vec4(x * extent.x, 0.0f), glm::vec4(y * extent.y, 0.0f), glm::vec4(z * extent.z, 0.0f), glm::vec4(centre, 1.0f));
    d.Age = -std::max(delay, 0.0f);
    d.Life = Config.DecalLifetime * (0.9f + 0.2f * Random01());
    d.DrySeconds = Config.DrySeconds * (0.8f + 0.4f * Random01());
    m_Decals.push_back(d);
    ++m_DecalsSpawned;
    return &m_Decals.back();
}

void BloodFx::SpawnFloorDecals(const BloodPresetDef& preset, const glm::mat4& prefabToWorld, float size, const glm::vec3& wound,
                               const glm::vec3& flatDir) {
    // BFX_DecalSettings: the ground a step along the spray, how far the blood falls to it, and from
    // that the splat's spread, slide and when it lands.
    glm::vec3 ground, n;
    const glm::vec3 probe = wound + flatDir * size + glm::vec3(0.0f, 0.2f, 0.0f);
    if (!m_Ray(probe, glm::vec3(0.0f, -1.0f, 0.0f), 6.0f, ground, n) || n.y < 0.5f) return;
    const float height = wound.y - ground.y;
    for (const BloodDecalDef& def : preset.Decals) {
        // Not shrunk with our smaller sprays: a small splash from chest height still reaches the floor.
        const float hiH = def.HeightMax * preset.RootScale, loH = def.HeightMin * preset.RootScale;
        if (height >= hiH || height <= loH) continue;
        const float k = std::abs(height / hiH);
        const glm::vec3 sMul = glm::mix(glm::vec3(def.ScaleMin[0], def.ScaleMin[1], def.ScaleMin[2]),
                                        glm::vec3(def.ScaleMax[0], def.ScaleMax[1], def.ScaleMax[2]), k);
        const glm::vec3 off = glm::mix(glm::vec3(def.OffsetMin[0], def.OffsetMin[1], def.OffsetMin[2]),
                                       glm::vec3(def.OffsetMax[0], def.OffsetMax[1], def.OffsetMax[2]), k);
        glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3(def.Pos[0], def.Pos[1], def.Pos[2]) + off);
        local *= glm::mat4_cast(glm::quat(def.Rot[3], def.Rot[0], def.Rot[1], def.Rot[2]));
        local = glm::scale(local, glm::vec3(def.Scale[0] * sMul.x, def.Scale[1], def.Scale[2] * sMul.z));
        const glm::mat4 box = prefabToWorld * FromRows(def.Parent) * local;
        glm::vec3 x(box[0]), z(box[2]);
        // Lying on the ground; projected through 40 cm, so steps and kerbs take it but walls don't.
        const glm::vec3 centre(box[3].x, ground.y + 0.01f, box[3].z);
        const float delay = def.DelayByHeight.Eval(k) / std::max(preset.AnimationSpeed, 1e-3f) * std::sqrt(size);
        Decal* d = AddDecal(def.Set, centre, glm::vec3(0, 1, 0), x, glm::vec3(glm::length(x), 0.4f, glm::length(z)), delay);
        if (!d) continue;
        d->Reveal = &def.Reveal;
        d->RevealSeconds = def.RevealSeconds;
    }
}

void BloodFx::OnFleshHit(const Hit& hit) {
    if (!Config.Enabled || Config.Gore <= 0) return;
    EnsureHooks();
    const glm::vec3 dir = glm::length(hit.Direction) > 1e-5f ? glm::normalize(hit.Direction) : glm::vec3(0.0f, 0.0f, -1.0f);
    // One spray per body per moment: a shotgun's pellets arrive together (the first carries the load's energy);
    // the rest each still puff where they go in.
    for (const Recent& r : m_Recent)
        if (r.Entity == hit.Entity && m_Now - r.Time < 0.08f) {
            if (!hit.Player && Config.ImpactPuffs) SpawnPuffs(hit, dir, Energy(hit) * 0.5f * Config.EnergyScale, false, hit.Point, true);
            return;
        }
    m_Recent.push_back({hit.Entity, m_Now});

    const float energy = Energy(hit) * Config.EnergyScale;
    m_LastEnergy = energy;
    // Out of the far side: where the round really leaves the body (its hitbox / ragdoll part along the line),
    // when it has the energy to. Otherwise it stays in, and the blood comes back out of the entry.
    glm::vec3 exitPoint = hit.Point + dir * (hit.Head ? 0.07f : 0.13f);
    const bool canExit = energy >= kExitEnergy && !hit.Player;
    m_LastExitFound = canExit && FindExit(hit.Point, dir, hit.Entity, hit.Head, m_BodyRay, exitPoint);

    m_LastExited = canExit;
    m_LastExitPoint = exitPoint;
    if (Config.ImpactPuffs && !hit.Player) SpawnPuffs(hit, dir, energy, canExit, exitPoint, false);

    const bool forward = canExit || hit.Player || hit.Corpse;
    Choice c = forward ? Choose(hit, Random01()) : Choice{Random01() < 0.5f ? "blood5" : "blood3", 0.5f};
    const BloodPresetDef* preset = FindBloodPreset(c.Preset);
    if (!preset) return;
    float size = c.Size * Config.Size * (0.88f + 0.24f * Random01());
    size *= std::clamp(0.8f + 0.22f * energy, 0.85f, 1.3f);
    // Faster with energy: the same fall, carried further along the line.
    const float stretch = std::clamp(0.85f + 0.25f * energy, 0.9f, 1.5f);
    // A round that stayed in: a smaller spurt back out of the entry, toward the shooter.
    const glm::vec3 sprayDir = forward ? dir : -dir;
    const glm::vec3 sprayFrom = forward ? exitPoint : hit.Point - dir * 0.03f;
    if (!forward) size *= 0.75f;
    const float jitter = (Random01() - 0.5f) * 0.45f;
    const glm::vec3 axis = PrefabAxis(*preset, [](const char* sim, glm::vec3& origin) -> const BloodFxImport::VatFrame* {
        const BloodRenderer::SimInfo* info = BloodRenderer::Get().Sim(BloodRenderer::Get().SimIndex(sim));
        if (!info || info->Frames.size() < 2) return nullptr;
        origin = glm::vec3(info->Header.Origin[0], info->Header.Origin[1], info->Header.Origin[2]);
        // A quarter of the way in: the burst's own heading, before most of it has fallen.
        return &info->Frames[info->Frames.size() / 4];
    });
    const glm::mat4 toWorld = PrefabToWorld(sprayFrom, sprayDir, size, jitter, axis, stretch);
    SpawnSprays(*preset, toWorld, size, sprayFrom, Flat(sprayDir), hit.Entity, 0.85f + 0.25f * Random01());
    SpawnFloorDecals(*preset, toWorld, size, sprayFrom, Flat(sprayDir));

    // On the body: the entry wound (a tight blot that seeps out over a few seconds) and, out of the far side,
    // the exit's wider mess; on the living, a run of it down from the wound. The hit point is on the hit part's
    // capsule, not the cloth, so each projects 30 cm deep; only surfaces facing its way take it.
    if (!hit.Player) {
        const float k = std::clamp(0.8f + hit.Damage / 150.0f, 0.8f, 1.4f);
        AddSplat(hit.Entity, hit.Part, hit.Corpse, hit.Point, -dir, RandomTangent(-dir), "attached", 0.08f * k, 0.3f, 0.0f, 4.0f);
        if (!hit.Corpse) {
            if (canExit) // the exit's wider mess, on the far side where the round came out (or about where it must have)
                AddSplat(hit.Entity, hit.Part, false, m_LastExitFound ? exitPoint + dir * 0.03f : hit.Point + dir * (hit.Head ? 0.16f : 0.24f), dir, RandomTangent(dir), Random01() < 0.5f ? "blood7" : "attached",
                         0.14f * k, 0.3f, 0.05f, 1.5f);
            AddSplat(hit.Entity, hit.Part, false, hit.Point - glm::vec3(0.0f, 0.12f * k, 0.0f), -dir, glm::vec3(0.0f, -1.0f, 0.0f),
                     "char", 0.15f * k, 0.3f, 0.3f, 6.0f);
        }
    }
    // The player's own: a wound where they were hit, and some of it on their hands and gun.
    if (hit.Player) {
        AddSplat(kPlayerEntity, -1, false, hit.Point, -dir, RandomTangent(-dir), "attached", 0.08f, 0.3f, 0.0f, 3.0f);
        AddSplat(kPlayerEntity, -1, false, hit.Point - glm::vec3(0.0f, 0.1f, 0.0f), -dir, glm::vec3(0.0f, -1.0f, 0.0f), "char", 0.13f,
                 0.3f, 0.2f, 5.0f);
        if (Random01() < 0.5f) SpatterGear(1 + (int)(Random01() * 2.0f), 0.8f, 0.1f);
    }
    // Point blank, what comes out of the entry comes back at the shooter: specks on the gun and hands.
    if (hit.ByPlayer && !hit.Corpse) {
        const float d = glm::length(hit.Point - hit.Origin);
        if (d < 2.5f && Random01() < 1.2f - d / 2.5f) SpatterGear(d < 1.2f ? 3 : 1 + (int)(Random01() * 2.0f), 1.3f - d / 2.5f, d / 8.0f);
    }
    // Whoever stands in the spray's path gets it on them: a few rays through the cone behind the body.
    if (!hit.Corpse && canExit) {
        const glm::vec3 from = exitPoint + dir * 0.25f;
        for (int r = 0; r < 3; ++r) {
            const glm::vec3 jitterDir = glm::normalize(dir + RandomTangent(dir) * 0.25f + glm::vec3(0.0f, -0.08f, 0.0f));
            unsigned other = 0xFFFFFFFFu;
            int part = -1;
            glm::vec3 at;
            if (!m_BodyRay(from, jitterDir, 2.5f * size / 0.6f, other, part, at) || other == hit.Entity) continue;
            const float d = glm::length(at - from);
            AddSplat(other, part, false, at, -jitterDir, RandomTangent(-jitterDir), r == 0 ? "blood9" : "blood3",
                     (0.12f + 0.08f * Random01()) * std::clamp(1.3f - d / 2.5f, 0.5f, 1.0f), 0.12f, d / 4.0f, 0.3f);
        }
        // A loose prop in the way (a crate, a dropped gun): it's spattered too.
        glm::vec3 p, n;
        unsigned prop = 0xFFFFFFFFu;
        if (m_PropRay && m_PropRay(from, Flat(dir), 2.0f * size / 0.6f, p, n, prop))
            AddSplat(prop, -1, false, p, n, RandomTangent(n), Random01() < 0.5f ? "blood1" : "blood6", 0.3f * size, 0.15f,
                     glm::length(p - from) / 4.0f, 0.3f);
    }
    // A head shot throws some of it up: the ceiling above, if there's one within reach.
    if (hit.Head && !hit.Player) {
        glm::vec3 p, n;
        if (m_Ray(exitPoint, glm::vec3(0.0f, 1.0f, 0.0f), 2.2f, p, n) && n.y < -0.5f) {
            const float s = size * (0.7f + 0.3f * Random01());
            if (Decal* d = AddDecal("blood7", p, n, RandomTangent(n), glm::vec3(1.1f * s, 0.3f, 1.1f * s), 0.12f + glm::length(p - exitPoint) / 6.0f))
                d->Opacity = 0.85f;
        }
    }
    // A body that died here bleeds out where it comes to rest.
    if (hit.Killed && !hit.Player && Config.Pools) m_Pools.push_back({hit.Entity, m_Now + 2.0f + Random01(), size});
}

void BloodFx::SpawnPuffs(const Hit& hit, const glm::vec3& dir, float energy, bool exited, const glm::vec3& exitPoint, bool extraPellet) {
    if (!m_Sprites) return;
    FxSprites& fx = *m_Sprites;
    // Knife "Real Blood" BloodHit / BloodExplosion, re-timed for a round: everything here is over in well under a
    // second - the frame the round goes in has to read as a hit before the volumetric spray has moved.
    static const glm::vec3 kBlood(0.30f, 0.012f, 0.010f);  // the liquid in a thin sheet (brighter than a pool)
    static const glm::vec3 kMist(0.17f, 0.014f, 0.012f);   // a fine dark-red haze (Knife's BloodCloud, darkened for our sun)
    const int hitSheet = fx.Entry("blood_hit"), burst = fx.Entry("blood_burst"), jet = fx.Entry("blood_jet");
    const int cloud = fx.Entry("blood_cloud"), drop = fx.Entry("blood_drop"), fan = fx.Entry("blood_fan");
    const float k = std::sqrt(std::max(energy, 0.1f));
    const glm::vec3 back = -dir; // out of the entry, toward the shooter
    auto jittered = [&](const glm::vec3& d, float spread) {
        return glm::normalize(d + RandomTangent(d) * (spread * Random01()) + glm::vec3(0.0f, 0.15f, 0.0f));
    };
    ++m_PuffsSpawned;

    // The entry: a small burst of the liquid back out, a puff of mist around it.
    if (hitSheet >= 0) {
        FxSprites::Emit e;
        e.Entry = hitSheet;
        e.Mode = FxSprites::Shade::Blood;
        e.Pos = hit.Point + back * 0.04f;
        e.Vel = back * 0.6f;
        e.Life = 0.32f + 0.08f * Random01();
        e.Size0 = 0.45f * k;
        e.Size1 = 0.55f * k;
        e.Rot = Random01() * 6.2832f;
        e.Color = kBlood;
        e.Erosion0 = 0.05f;
        e.Erosion1 = 0.75f;
        e.Softness = 0.18f;
        e.FadeOut = 0.2f;
        fx.Spawn(e);
    }
    if (cloud >= 0) {
        const int n = hit.Head ? 3 : extraPellet ? 1 : 2;
        for (int i = 0; i < n; ++i) {
            FxSprites::Emit e;
            e.Entry = cloud;
            e.Mode = FxSprites::Shade::Lit;
            e.Pos = hit.Point + back * 0.05f;
            e.Vel = jittered(back, 0.6f) * (0.5f + 0.5f * Random01()) * k;
            e.Drag = 3.0f;
            e.Gravity = 0.02f;
            e.Life = (hit.Head ? 0.9f : 0.55f) + 0.3f * Random01();
            e.Size0 = (hit.Head ? 0.3f : 0.22f) * k;
            e.Size1 = (hit.Head ? 0.75f : 0.5f) * k;
            e.Rot = Random01() * 6.2832f;
            e.Spin = (Random01() - 0.5f) * 1.5f;
            e.Color = hit.Head ? kMist * 1.15f : kMist;
            e.Alpha = (hit.Head ? 0.6f : 0.45f) * std::min(1.0f, 0.6f + 0.4f * energy);
            e.FadeIn = 0.05f;
            e.FadeOut = 0.8f;
            fx.Spawn(e);
        }
    }
    if (extraPellet) return;

    // The exit: the liquid bursting out of the far side along the line - the sheet's spray, the jet, droplets.
    if (exited) {
        const glm::vec3 out = glm::normalize(dir + glm::vec3(0.0f, 0.1f, 0.0f));
        if (burst >= 0) {
            FxSprites::Emit e;
            e.Entry = burst;
            e.Mode = FxSprites::Shade::Blood;
            e.Pos = exitPoint + dir * 0.12f * k;
            e.Vel = out * 1.2f * k;
            e.Drag = 2.0f;
            e.Life = 0.42f + 0.1f * Random01();
            e.Size0 = 0.75f * k;
            e.Size1 = 0.95f * k;
            e.Rot = Random01() * 6.2832f;
            e.Color = kBlood;
            e.Erosion0 = 0.0f;
            e.Erosion1 = 0.6f;
            e.Softness = 0.2f;
            e.FadeOut = 0.15f;
            fx.Spawn(e);
        }
        if (jet >= 0) {
            FxSprites::Emit e;
            e.Entry = jet;
            e.Mode = FxSprites::Shade::Blood;
            e.Pos = exitPoint + out * 0.18f * k;
            e.Axis = out * 2.0f; // the sheet's jet runs along u
            e.Vel = out * 0.8f;
            e.Life = 0.3f;
            e.Size0 = 0.3f * k;
            e.Size1 = 0.4f * k;
            e.Color = kBlood;
            e.Erosion1 = 0.5f;
            e.Softness = 0.2f;
            fx.Spawn(e);
        }
        if (cloud >= 0) {
            FxSprites::Emit e;
            e.Entry = cloud;
            e.Mode = FxSprites::Shade::Lit;
            e.Pos = exitPoint + dir * 0.1f;
            e.Vel = out * 1.5f * k;
            e.Drag = 3.5f;
            e.Life = (hit.Head ? 1.1f : 0.7f) + 0.3f * Random01();
            e.Size0 = 0.25f * k;
            e.Size1 = (hit.Head ? 0.9f : 0.6f) * k;
            e.Rot = Random01() * 6.2832f;
            e.Spin = (Random01() - 0.5f) * 1.2f;
            e.Color = hit.Head ? kMist * 1.15f : kMist;
            e.Alpha = hit.Head ? 0.6f : 0.4f;
            e.FadeIn = 0.04f;
            e.FadeOut = 0.8f;
            fx.Spawn(e);
        }
    } else if (fan >= 0 && !hit.Corpse) {
        // No exit: what comes back out of the entry fans toward the shooter.
        FxSprites::Emit e;
        e.Entry = fan;
        e.Mode = FxSprites::Shade::Blood;
        e.Pos = hit.Point + back * 0.1f;
        e.Vel = back * 0.5f;
        e.Life = 0.35f;
        e.Size0 = 0.45f * k;
        e.Size1 = 0.55f * k;
        e.Rot = std::atan2(back.y, 1.0f) + (Random01() - 0.5f) * 0.6f;
        e.Color = kBlood;
        e.Erosion1 = 0.7f;
        e.Softness = 0.2f;
        fx.Spawn(e);
    }
    // Droplets thrown on: streaks along their flight, falling.
    if (drop >= 0) {
        const glm::vec3 from = exited ? exitPoint : hit.Point;
        const glm::vec3 way = exited ? dir : back;
        const int n = (int)((hit.Head ? 16.0f : 8.0f) * std::min(energy, 2.0f) + 3.0f * Random01());
        for (int i = 0; i < n; ++i) {
            FxSprites::Emit e;
            e.Entry = drop;
            e.Mode = FxSprites::Shade::Blood;
            e.Pos = from + way * 0.05f;
            e.Vel = jittered(way, 0.9f) * ((2.0f + 4.5f * Random01()) * k);
            e.Gravity = 1.0f;
            e.Drag = 0.6f;
            e.Life = 0.35f + 0.35f * Random01();
            e.Size0 = e.Size1 = 0.03f + 0.035f * Random01();
            e.Stretch = 0.09f;
            e.Color = kBlood;
            e.Erosion0 = 0.15f;
            e.Erosion1 = 0.55f;
            e.Softness = 0.25f;
            e.FadeOut = 0.25f;
            fx.Spawn(e);
        }
    }
}

void BloodFx::SpawnSprays(const BloodPresetDef& preset, const glm::mat4& prefabToWorld, float size, const glm::vec3& wound,
                          const glm::vec3& flatDir, unsigned entity, float tint) {
    EnsureHooks();
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
            // The spatter where it strikes: streaks fanning across the wall, a little below the line (it's
            // already falling), landing when the fluid gets there (about 4 m/s out of a wound).
            static const char* const kStreaks[] = {"blood1", "blood3", "blood9", "blood2_right"};
            const float dist = glm::length(p - wound);
            const float s = size * (0.85f + 0.3f * Random01()) * std::clamp(1.4f - dist / reach, 0.6f, 1.2f);
            const glm::vec3 at = p - glm::vec3(0.0f, 0.04f + 0.08f * dist, 0.0f);
            glm::vec3 along = RandomTangent(n);
            if (along.y > 0.0f) along = -along; // streaks run out and down, not up
            // The splash's body (a blot with its spokes) and the streaks thrown out of it.
            const float land = dist / 4.0f;
            const char* blot = Random01() < 0.5f ? "blood7" : "attached";
            if (Decal* body = AddDecal(blot, at, n, RandomTangent(n), glm::vec3(0.75f * s, 0.25f, 0.75f * s), land)) body->PoolGrow = 0.35f;
            const char* streaks = kStreaks[std::min(3, (int)(Random01() * 4.0f))];
            if (Decal* streak = AddDecal(streaks, at, n, along, glm::vec3(1.3f * s, 0.25f, 0.9f * s), land)) streak->PoolGrow = 0.25f;
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

void BloodFx::SpatterGear(int drops, float sizeScale, float delay) {
    if (!m_Gear || drops <= 0) return;
    glm::vec3 points[8], normals[8];
    const int n = std::min(m_Gear(points, normals, 8), 8);
    for (int i = 0; i < drops && n > 0; ++i) {
        const int k = std::min(n - 1, (int)(Random01() * (float)n));
        const glm::vec3 nrm = glm::normalize(normals[k]);
        const glm::vec3 at = points[k] + RandomTangent(nrm) * (0.03f * Random01());
        AddSplat(kPlayerEntity, -1, false, at, nrm, RandomTangent(nrm), Random01() < 0.5f ? "blood3" : "blood9",
                 (0.025f + 0.03f * Random01()) * sizeScale, 0.05f, delay, 0.15f);
    }
}

void BloodFx::Update(float dt) {
    m_Now += dt;
    for (Spray& s : m_Sprays) s.Age += dt;
    for (Decal& d : m_Decals) d.Age += dt;
    m_Decals.erase(std::remove_if(m_Decals.begin(), m_Decals.end(), [](const Decal& d) { return d.Age >= d.Life; }), m_Decals.end());
    // Corpses that have come to rest: the pool under the pelvis.
    for (size_t i = 0; i < m_Pools.size();) {
        if (m_Now < m_Pools[i].At) { ++i; continue; }
        const PendingPool pp = m_Pools[i];
        m_Pools.erase(m_Pools.begin() + (std::ptrdiff_t)i);
        glm::vec3 centre, p, n;
        if (!m_Body || !m_Body(pp.Entity, centre)) continue;
        EnsureHooks();
        if (!m_Ray(centre + glm::vec3(0.0f, 0.3f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 1.8f, p, n) || n.y < 0.6f) continue;
        const float s = std::clamp(pp.Size, 0.5f, 1.0f) * (1.3f + 0.4f * Random01());
        if (Decal* d = AddDecal("attached", p + glm::vec3(0.0f, 0.01f, 0.0f), n, RandomTangent(n), glm::vec3(s, 0.3f, s), 0.0f)) {
            d->PoolGrow = 20.0f + 10.0f * Random01();
            ++m_PoolsSpawned;
            d->DrySeconds *= 2.5f; // a pool stays wet far longer than a spatter
        }
    }
    m_Sprays.erase(std::remove_if(m_Sprays.begin(), m_Sprays.end(), [](const Spray& s) { return s.Age >= s.Duration; }),
                   m_Sprays.end());
    m_Recent.erase(std::remove_if(m_Recent.begin(), m_Recent.end(), [&](const Recent& r) { return m_Now - r.Time > 0.25f; }),
                   m_Recent.end());
    // Splats live as long as the body they're on.
    for (Splat& s : m_Splats) s.Age += dt;
    static std::vector<unsigned> alive, gone, scratch;
    alive.clear();
    gone.clear();
    for (const Splat& s : m_Splats) {
        if (std::find(alive.begin(), alive.end(), s.Group) != alive.end() || std::find(gone.begin(), gone.end(), s.Group) != gone.end())
            continue;
        scratch.clear();
        (m_Members && m_Members(s.Group, scratch) ? alive : gone).push_back(s.Group);
    }
    if (!gone.empty())
        m_Splats.erase(std::remove_if(m_Splats.begin(), m_Splats.end(),
                                      [&](const Splat& s) { return std::find(gone.begin(), gone.end(), s.Group) != gone.end(); }),
                       m_Splats.end());
}

void BloodFx::Submit(BloodRenderer& renderer) const {
    {   // Splats, by the mesh that draws them: each entity's run of the list.
        static std::vector<BloodRenderer::Splat> splats;
        static std::vector<std::pair<unsigned, glm::ivec2>> ranges;
        static std::vector<unsigned> members;
        splats.clear();
        ranges.clear();
        members.clear();
        for (const Splat& s : m_Splats)
            if (std::find(members.begin(), members.end(), s.Member) == members.end()) members.push_back(s.Member);
        for (unsigned member : members) {
            const int first = (int)splats.size();
            for (const Splat& s : m_Splats) {
                if (s.Member != member || s.Age < 0.0f) continue;
                BloodRenderer::Splat r;
                r.Center = s.Center;
                r.Radius = s.Radius;
                r.Normal = s.Normal;
                r.Depth = s.Depth;
                r.Tangent = s.Tangent;
                r.Set = s.Set;
                const float grow = std::clamp(s.Age / std::max(s.Grow, 1e-3f), 0.0f, 1.0f);
                r.Cutout = 0.85f * (1.0f - grow) * (1.0f - grow);
                const float dry = std::clamp(s.Age / std::max(s.DrySeconds, 1.0f), 0.0f, 1.0f);
                r.Dry = dry * dry * (3.0f - 2.0f * dry);
                r.Opacity = s.Opacity;
                splats.push_back(r);
            }
            const int count = (int)splats.size() - first;
            if (count > 0) ranges.emplace_back(member, glm::ivec2(first, count));
        }
        renderer.SetSplats(splats, ranges);
    }
    for (const Decal& d : m_Decals) {
        if (d.Age < 0.0f) continue;
        BloodRenderer::Decal r;
        r.Model = d.Model;
        r.Set = d.Set;
        r.Cutout = DecalCutout(d);
        const float dry = std::clamp(d.Age / std::max(d.DrySeconds, 1.0f), 0.0f, 1.0f);
        r.Dry = dry * dry * (3.0f - 2.0f * dry);
        r.Opacity = d.Opacity;
        renderer.AddDecal(r);
    }
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
