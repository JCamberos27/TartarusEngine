#include "BloodFx.h"

#include "BloodRenderer.h"
#include "FxSprites.h"
#include "BloodPalette.h"
#include "KnifeFxLibrary.h"
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
    m_Walkers.clear();
    m_Bleeds.clear();
    m_Now = 0.0f;
}

void BloodFx::EnsureHooks() {
    if (!m_SimLookup) m_SimLookup = [](const char* sim) { return BloodRenderer::Get().SimIndex(sim); };
    if (!m_SetLookup) m_SetLookup = [](const char* set) { return BloodRenderer::Get().DecalSet(set); };
    if (!m_KnifeLookup)
        m_KnifeLookup = [](const char* entry, int& cells) {
            const KnifeFxLibrary& lib = KnifeFxLibrary::Get();
            const KnifeFxLibrary::Entry* e = lib.At(lib.Find(entry));
            if (!e || e->Lib == KnifeFxImport::Library::Sprite) return -1;
            cells = e->Frames;
            return lib.Find(entry);
        };
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
    // Stains don't shrink away: one leaves whole, out of view (Update).
    if (d.Spread) { // the pool grows by its size (PoolScale): only its edge firms up as it goes
        const float grow = std::clamp(t / std::max(d.PoolGrow, 1e-3f), 0.0f, 1.0f);
        return 0.3f * (1.0f - grow) * (1.0f - grow);
    }
    if (!d.Reveal) {
        const float grow = std::clamp(t / std::max(d.PoolGrow, 1e-3f), 0.0f, 1.0f);
        return 0.9f * (1.0f - grow) * (1.0f - grow) * (1.0f - grow); // fast at first, slowing as it thins
    }
    const float g = std::max(d.RevealSeconds, 1e-3f);
    return std::clamp(d.Reveal->Eval(std::min(t / g, 0.3f)), 0.0f, 1.0f);
}

float BloodFx::PoolScale(const Decal& d) {
    if (!d.Spread) return 1.0f;
    const float g = std::clamp(std::max(d.Age, 0.0f) / std::max(d.PoolGrow, 1e-3f), 0.0f, 1.0f);
    // Ease-out: blood pouring onto the floor spreads quickly at first, then creeps as the film thins.
    const float k = 1.0f - std::pow(1.0f - g, 2.2f);
    return kPoolStartScale + (1.0f - kPoolStartScale) * k;
}

int BloodFx::DuplicateOf(int set, int knife, const glm::mat4& model) const {
    const glm::vec3 c(model[3]), up = glm::normalize(glm::vec3(model[1]));
    const float size = std::max(glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[2])));
    for (int i = (int)m_Decals.size() - 1; i >= 0; --i) {
        const Decal& d = m_Decals[(size_t)i];
        if (d.Set != set || d.Knife != knife || d.Age > 5.0f) continue;
        const float other = std::max(glm::length(glm::vec3(d.Model[0])), glm::length(glm::vec3(d.Model[2])));
        if (std::abs(other - size) > 0.25f * size) continue;
        if (glm::length(glm::vec3(d.Model[3]) - c) > 0.15f * size) continue;
        if (glm::dot(glm::normalize(glm::vec3(d.Model[1])), up) < 0.9f) continue;
        return i;
    }
    return -1;
}

BloodFx::Decal* BloodFx::AddKnifeDecal(const char* entry, const glm::vec3& centre, const glm::vec3& up, const glm::vec3& along,
                                       const glm::vec3& extent, float delay, int cell) {
    EnsureHooks();
    int cells = 1;
    const int id = m_KnifeLookup(entry, cells);
    if (id < 0) return nullptr;
    MakeRoom(std::string(entry) == "footprint");
    const glm::vec3 y = glm::normalize(up);
    glm::vec3 x = along - y * glm::dot(along, y);
    x = glm::length(x) > 1e-4f ? glm::normalize(x) : RandomTangent(y);
    const glm::vec3 z = glm::cross(x, y);
    Decal d;
    d.Knife = id;
    d.Cell = cell >= 0 ? std::min(cell, std::max(cells - 1, 0)) : std::min(cells - 1, (int)(Random01() * (float)cells));
    d.Frames = std::max(cells, 1);
    d.Model = glm::mat4(glm::vec4(x * extent.x, 0.0f), glm::vec4(y * extent.y, 0.0f), glm::vec4(z * extent.z, 0.0f), glm::vec4(centre, 1.0f));
    d.Age = -std::max(delay, 0.0f);
    d.Life = Config.DecalLifetime > 0.0f ? Config.DecalLifetime * (0.9f + 0.2f * Random01()) : 1e9f;
    d.DrySeconds = Config.DrySeconds * (0.8f + 0.4f * Random01());
    if (const int dup = DuplicateOf(d.Set, d.Knife, d.Model); dup >= 0 && d.FrameSeconds <= 0.0f) {
        ++m_StainsMerged;
        return &m_Decals[(size_t)dup];
    }
    m_Decals.push_back(d);
    ++m_DecalsSpawned;
    return &m_Decals.back();
}

bool BloodFx::InView(const Decal& d) const {
    if (!m_HasViewer) return true;
    const glm::vec3 c(d.Model[3]);
    const float r = 0.5f * std::max(glm::length(glm::vec3(d.Model[0])), glm::length(glm::vec3(d.Model[2])));
    const glm::vec3 to = c - m_Eye;
    const float dist = glm::length(to);
    if (dist - r > kVisibleReach) return false;
    if (dist <= r + 2.0f) return true; // right here: under your feet counts as seen
    // A 75 degree half-angle cone (wider than any view and its edges), grown by the stain's size.
    return glm::dot(to / dist, m_Forward) > std::cos(std::min(1.309f + std::asin(std::min(r / dist, 1.0f)), 3.1416f));
}

void BloodFx::MakeRoom(bool print) {
    if (m_PrintId == -2 && m_KnifeLookup) {
        int cells = 0;
        m_PrintId = m_KnifeLookup("footprint", cells);
    }
    auto isPrint = [&](const Decal& d) { return d.Knife >= 0 && d.Knife == m_PrintId; };
    int count = 0, cap = std::max(1, Config.MaxDecals);
    if (print) {
        for (const Decal& d : m_Decals) count += isPrint(d);
        cap = kMaxPrints;
    } else {
        count = (int)m_Decals.size();
    }
    for (; count >= cap; --count) { // (a lowered cap trims down to it)
        // The farthest one nobody can see; failing that (everything in view), the oldest.
        int pick = -1, oldest = -1;
        float far = -1.0f;
        for (int i = 0; i < (int)m_Decals.size(); ++i) {
            const Decal& d = m_Decals[(size_t)i];
            if (print && !isPrint(d)) continue;
            if (oldest < 0) oldest = i;
            if (InView(d)) continue;
            const float dist = glm::length(glm::vec3(d.Model[3]) - m_Eye);
            if (dist > far) { far = dist; pick = i; }
        }
        if (pick < 0) pick = oldest;
        if (pick < 0) return;
        m_Decals.erase(m_Decals.begin() + pick);
    }
}

void BloodFx::SpawnPoolAt(const glm::vec3& at, const glm::vec3& normal, float size) {
    EnsureHooks();
    const glm::vec3 n = glm::normalize(normal);
    Decal* d = AddKnifeDecal("pool_smooth", at + n * 0.01f, n, RandomTangent(n), glm::vec3(size * 1.25f, 0.3f, size * 1.25f), 0.0f);
    if (!d) d = AddDecal("attached", at + n * 0.01f, n, RandomTangent(n), glm::vec3(size, 0.3f, size), 0.0f);
    if (d) {
        d->PoolGrow = 22.0f;
        d->Spread = true;
        d->DrySeconds *= 2.5f;
        ++m_PoolsSpawned;
    }
}

void BloodFx::SpatterWallAt(const glm::vec3& at, const glm::vec3& normal, float size) {
    EnsureHooks();
    const glm::vec3 n = glm::normalize(normal);
    glm::vec3 along = RandomTangent(n);
    if (along.y > 0.0f) along = -along;
    if (Decal* body = AddDecal(Random01() < 0.5f ? "blood7" : "attached", at, n, RandomTangent(n), glm::vec3(0.75f * size, 0.25f, 0.75f * size), 0.0f))
        body->PoolGrow = 0.35f;
    if (Decal* streak = AddDecal("blood1", at, n, along, glm::vec3(1.3f * size, 0.25f, 0.9f * size), 0.0f)) streak->PoolGrow = 0.25f;
    SpawnWallDrips(at, n, size, 0.0f);
}

int BloodFx::BloodySteps(int walker) const {
    for (const Walker& w : m_Walkers)
        if (w.Id == walker) return w.Steps;
    return 0;
}

bool BloodFx::InFreshBlood(const glm::vec3& feet) const {
    int printId = -2, cells = 0;
    if (m_KnifeLookup) printId = m_KnifeLookup("footprint", cells);
    for (const Decal& d : m_Decals) {
        if (d.Age < 0.5f || d.Age > kFreshSeconds || (d.Knife >= 0 && d.Knife == printId)) continue;
        const glm::vec3 up = glm::normalize(glm::vec3(d.Model[1]));
        if (up.y < 0.7f) continue; // the ground only
        const glm::vec3 c(d.Model[3]);
        if (std::abs(feet.y - c.y) > 0.35f) continue;
        // Within the stain's middle (its box is bigger than the blood in it), grown in far enough by now.
        const float half = 0.3f * PoolScale(d) * std::min(glm::length(glm::vec3(d.Model[0])), glm::length(glm::vec3(d.Model[2])));
        if (DecalCutout(d) > 0.5f) continue;
        if (glm::length(glm::vec2(feet.x - c.x, feet.z - c.z)) < half) return true;
    }
    return false;
}

void BloodFx::OnFootstep(int walker, const glm::vec3& feet, const glm::vec3& velocity, int foot, const glm::vec3& facing) {
    if (!Config.Enabled || Config.Gore <= 0) return;
    EnsureHooks();
    Walker* w = nullptr;
    for (Walker& x : m_Walkers)
        if (x.Id == walker) w = &x;
    if (!w) {
        Walker nw;
        nw.Id = walker;
        m_Walkers.push_back(nw);
        w = &m_Walkers.back();
    }
    // Which way the feet point: where they face when that's known, else the way they walk (the velocity, the last step).
    const glm::vec3 flatFace(facing.x, 0.0f, facing.z), flatVel(velocity.x, 0.0f, velocity.z);
    if (glm::length(flatFace) > 0.1f) w->Forward = glm::normalize(flatFace);
    else if (glm::length(flatVel) > 0.3f) w->Forward = glm::normalize(flatVel);
    else if (w->HasLast && glm::length(glm::vec2(feet.x - w->Last.x, feet.z - w->Last.z)) > 0.15f)
        w->Forward = glm::normalize(glm::vec3(feet.x - w->Last.x, 0.0f, feet.z - w->Last.z));
    w->Last = feet;
    w->HasLast = true;
    const bool left = foot == 0 ? true : foot == 1 ? false : !w->Left;
    w->Left = left;

    if (InFreshBlood(feet)) { // soles soaked: the next steps carry it out
        if (w->Steps == 0) {
            int cells = 1;
            m_KnifeLookup("footprint", cells);
            w->Shoe = std::min(cells - 1, (int)(Random01() * (float)std::max(cells, 1)));
        }
        w->Steps = kPrintSteps;
        return;
    }
    if (w->Steps <= 0) return;
    const float strength = (float)w->Steps / (float)kPrintSteps;
    --w->Steps;
    const glm::vec3 right = glm::normalize(glm::cross(w->Forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 at = feet + right * (left ? -0.11f : 0.11f);
    glm::vec3 p, n;
    if (!m_Ray(at + glm::vec3(0.0f, 0.4f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 1.0f, p, n) || n.y < 0.6f) return;
    // The print's toe (the image's top) forward: v runs heel-ward, so u = forward x up.
    const glm::vec3 along = glm::cross(w->Forward, n);
    if (Decal* d = AddKnifeDecal("footprint", p + n * 0.005f, n, along, glm::vec3(0.13f, 0.15f, 0.3f), 0.0f, w->Shoe)) {
        d->Opacity = 0.45f + 0.55f * strength; // each print a little fainter, the trail running out over kPrintSteps
        d->PoolGrow = 0.05f;
        ++m_PrintsSpawned;
    }
}

void BloodFx::SpawnWallDrips(const glm::vec3& at, const glm::vec3& n, float size, float land) {
    if (std::abs(n.y) > 0.5f) return; // walls only: on a floor it pools, a ceiling drips straight down
    static const char* const kLeaks[] = {"leak1", "leak2", "leak3"};
    // The image's top at the spatter, running down the wall: u across (horizontal), v down.
    const glm::vec3 down(0.0f, -1.0f, 0.0f);
    glm::vec3 across = glm::cross(n, down);
    if (glm::length(across) < 1e-3f) return;
    across = glm::normalize(across);
    const int count = 1 + (int)(Random01() * 2.5f * std::min(size / 0.6f, 1.3f));
    for (int i = 0; i < count; ++i) {
        const float h = (0.35f + 0.3f * Random01()) * std::clamp(size / 0.6f, 0.6f, 1.3f); // how far it runs
        const float w = h * 0.5f;
        const glm::vec3 top = at + across * ((Random01() - 0.5f) * 0.5f * size) + n * 0.005f;
        Decal* d = AddKnifeDecal(kLeaks[std::min(2, (int)(Random01() * 3.0f))], top + down * (0.5f * h), n, across, glm::vec3(w, 0.2f, h),
                                 land + 0.05f + 0.15f * Random01(), 0);
        if (!d) return;
        d->FrameSeconds = (2.0f + 3.0f * Random01()) / (float)std::max(d->Frames, 1); // runs down over 2-5 s, then stops
        d->PoolGrow = 0.05f;
        ++m_DripsSpawned;
    }
}

BloodFx::Decal* BloodFx::AddDecal(const char* set, const glm::vec3& centre, const glm::vec3& up, const glm::vec3& along,
                                  const glm::vec3& extent, float delay) {
    EnsureHooks();
    const int s = m_SetLookup(set);
    if (s < 0) return nullptr;
    MakeRoom(false);
    const glm::vec3 y = glm::normalize(up);
    glm::vec3 x = along - y * glm::dot(along, y);
    x = glm::length(x) > 1e-4f ? glm::normalize(x) : RandomTangent(y);
    const glm::vec3 z = glm::cross(x, y);
    Decal d;
    d.Set = s;
    d.Model = glm::mat4(glm::vec4(x * extent.x, 0.0f), glm::vec4(y * extent.y, 0.0f), glm::vec4(z * extent.z, 0.0f), glm::vec4(centre, 1.0f));
    d.Age = -std::max(delay, 0.0f);
    d.Life = Config.DecalLifetime > 0.0f ? Config.DecalLifetime * (0.9f + 0.2f * Random01()) : 1e9f;
    d.DrySeconds = Config.DrySeconds * (0.8f + 0.4f * Random01());
    if (const int dup = DuplicateOf(d.Set, d.Knife, d.Model); dup >= 0 && d.FrameSeconds <= 0.0f) {
        ++m_StainsMerged;
        return &m_Decals[(size_t)dup];
    }
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
        const float speed = std::max(Config.Speed, 0.1f);
        const float delay = def.DelayByHeight.Eval(k) / std::max(preset.AnimationSpeed, 1e-3f) * std::sqrt(size) / speed;
        Decal* d = AddDecal(def.Set, centre, glm::vec3(0, 1, 0), x, glm::vec3(glm::length(x), 0.4f, glm::length(z)), delay);
        if (!d) continue;
        d->Reveal = &def.Reveal;
        d->RevealSeconds = def.RevealSeconds / speed;
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
    SpawnSprays(*preset, toWorld, size, sprayFrom, Flat(sprayDir), hit.Entity, 0.92f + 0.13f * Random01());
    SpawnFloorDecals(*preset, toWorld, size, sprayFrom, Flat(sprayDir));

    // On the body: the entry wound (a tight blot that seeps out over a few seconds) and, out of the far side,
    // the exit's wider mess; on the living, a run of it down from the wound. The hit point is on the hit part's
    // capsule, not the cloth, so each projects 30 cm deep; only surfaces facing its way take it.
    if (!hit.Player) {
        // A torso wound is a tight hole in the cloth; a head wound bleeds wide (scalp and face), its exit wider still.
        const float k = std::clamp(0.8f + hit.Damage / 150.0f, 0.8f, 1.4f);
        const float entry = hit.Head ? 0.075f : 0.05f, exitR = hit.Head ? 0.16f : 0.095f, run = hit.Head ? 0.13f : 0.1f;
        AddSplat(hit.Entity, hit.Part, hit.Corpse, hit.Point, -dir, RandomTangent(-dir), "attached", entry * k, 0.3f, 0.0f, 0.15f);
        if (!hit.Corpse) {
            if (canExit) // the exit's wider mess, on the far side where the round came out (or about where it must have)
                AddSplat(hit.Entity, hit.Part, false, m_LastExitFound ? exitPoint + dir * 0.03f : hit.Point + dir * (hit.Head ? 0.16f : 0.24f), dir, RandomTangent(dir), Random01() < 0.5f ? "blood7" : "attached",
                         exitR * k, 0.3f, 0.0f, 0.2f);
            AddSplat(hit.Entity, hit.Part, false, hit.Point - glm::vec3(0.0f, 0.12f * k, 0.0f), -dir, glm::vec3(0.0f, -1.0f, 0.0f),
                     "char", run * k, 0.3f, 0.05f, 1.5f);
        }
    }
    // The player's own: a wound where they were hit, and some of it on their hands and gun.
    if (hit.Player) {
        AddSplat(kPlayerEntity, -1, false, hit.Point, -dir, RandomTangent(-dir), "attached", 0.08f, 0.3f, 0.0f, 0.15f);
        AddSplat(kPlayerEntity, -1, false, hit.Point - glm::vec3(0.0f, 0.1f, 0.0f), -dir, glm::vec3(0.0f, -1.0f, 0.0f), "char", 0.13f,
                 0.3f, 0.05f, 1.5f);
        if (Random01() < 0.5f) SpatterGear(1 + (int)(Random01() * 2.0f), 0.8f, 0.1f);
    }
    // Point blank, what comes out of the entry comes back at the shooter: specks on the gun and hands.
    if (hit.ByPlayer && !hit.Corpse) {
        const float d = glm::length(hit.Point - hit.Origin);
        if (d < 2.5f && (d < 1.6f || Random01() < 1.2f - d / 2.5f)) SpatterGear(d < 1.2f ? 3 : 1 + (int)(Random01() * 2.0f), 1.3f - d / 2.5f, d / 24.0f);
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
                     (0.12f + 0.08f * Random01()) * std::clamp(1.3f - d / 2.5f, 0.5f, 1.0f), 0.12f, d / 12.0f, 0.15f);
        }
        // A loose prop in the way (a crate, a dropped gun): it's spattered too.
        glm::vec3 p, n;
        unsigned prop = 0xFFFFFFFFu;
        if (m_PropRay && m_PropRay(from, Flat(dir), 2.0f * size / 0.6f, p, n, prop))
            AddSplat(prop, -1, false, p, n, RandomTangent(n), Random01() < 0.5f ? "blood1" : "blood6", 0.3f * size, 0.15f,
                     glm::length(p - from) / 12.0f, 0.15f);
    }
    if (hit.Corpse) SpawnCorpseSplash(hit, energy);
    else if (!hit.Player) SpawnGroundSplatter(hit, energy, sprayFrom, Flat(sprayDir), forward);
    // A living wound keeps bleeding: drops under him as he goes (a head or a hard hit bleeds faster).
    if (!hit.Player && !hit.Killed && !hit.Corpse) {
        Bleed* b = nullptr;
        for (Bleed& x : m_Bleeds)
            if (x.Entity == hit.Entity) b = &x;
        if (!b) {
            m_Bleeds.push_back({hit.Entity, 0.0f, m_Now + 0.4f, 0.0f});
            b = &m_Bleeds.back();
        }
        b->Until = m_Now + kBleedSeconds;
        b->Rate = std::max(b->Rate, std::clamp(0.3f / std::max(energy, 0.3f), 0.15f, 0.5f) * (hit.Head ? 0.7f : 1.0f)); // seconds between drops
    }
    // A head shot throws some of it up: the ceiling above, if there's one within reach.
    if (hit.Head && !hit.Player) {
        glm::vec3 p, n;
        if (m_Ray(exitPoint, glm::vec3(0.0f, 1.0f, 0.0f), 2.2f, p, n) && n.y < -0.5f) {
            const float s = size * (0.7f + 0.3f * Random01());
            if (Decal* d = AddDecal("blood7", p, n, RandomTangent(n), glm::vec3(1.1f * s, 0.3f, 1.1f * s), glm::length(p - exitPoint) / 12.0f))
                d->Opacity = 0.85f;
        }
    }
    // A body that died here bleeds out where it comes to rest.
    if (hit.Killed && !hit.Player && Config.Pools) m_Pools.push_back({hit.Entity, m_Now + 0.8f + 0.5f * Random01(), size});
}

float BloodFx::GroundSplatterSize(const Hit& hit, float energy) {
    float s = hit.Head ? 0.5f : 0.3f;
    if (hit.Head && hit.Killed) s = 0.7f;
    if (hit.Corpse) s = 0.22f;
    return s * std::clamp(0.75f + 0.25f * energy, 0.8f, 1.4f);
}

bool BloodFx::Bleeding(unsigned entity) const {
    for (const Bleed& b : m_Bleeds)
        if (b.Entity == entity && m_Now <= b.Until) return true;
    return false;
}

bool BloodFx::ThrowArc(const glm::vec3& from, const glm::vec3& velocity, glm::vec3& point, glm::vec3& normal, glm::vec3& vel,
                       float& seconds, float step) const {
    // Short straight steps along the fall (gravity only: the drops are heavy and quick), the first surface met.
    // ~40 short rays an arc at most (the specks take longer steps).
    const float kStep = std::max(step, 0.01f), kMax = 1.4f;
    glm::vec3 p = from, v = velocity;
    for (float t = 0.0f; t < kMax; t += kStep) {
        const glm::vec3 nv = v + glm::vec3(0.0f, -9.81f, 0.0f) * kStep;
        const glm::vec3 np = p + (v + nv) * (0.5f * kStep);
        const glm::vec3 seg = np - p;
        const float len = glm::length(seg);
        glm::vec3 hp, hn;
        if (len > 1e-5f && m_Ray(p, seg / len, len, hp, hn)) {
            point = hp;
            normal = glm::normalize(hn);
            vel = v;
            seconds = t + kStep * glm::length(hp - p) / len;
            return true;
        }
        p = np;
        v = nv;
    }
    return false;
}

BloodFx::Decal* BloodFx::AddSplash(Splash kind, const glm::vec3& centre, const glm::vec3& up, const glm::vec3& along,
                                   const glm::vec3& extent, float delay) {
    const bool wall = std::abs(up.y) < 0.5f;
    const float r = Random01();
    Decal* d = nullptr;
    auto pick = [&](const char* const* names, int n) { return names[std::min(n - 1, (int)(Random01() * (float)n))]; };
    switch (kind) {
    case Splash::Big: {
        static const char* const kKnife[] = {"splat_small", "splat_wide", "splat_medium"};
        static const char* const kSets[] = {"blood7", "attached", "blood6", "blood4"};
        if (r < 0.55f) d = AddKnifeDecal(pick(kKnife, 3), centre, up, along, extent * 1.2f, delay); // their splat fills ~80% of the cell
        if (!d) d = AddDecal(pick(kSets, 4), centre, up, along, extent, delay);
        break;
    }
    case Splash::Streak: {
        static const char* const kSets[] = {"blood1", "blood2_right", "blood9", "blood3", "blood2_left"};
        if (r < 0.3f) d = AddKnifeDecal("splat_small", centre, up, along, extent * 1.2f, delay);
        if (!d) d = AddDecal(pick(kSets, 5), centre, up, along, extent, delay);
        break;
    }
    case Splash::Drop: {
        // Real Blood's drop sheet: on a wall any cell (runs and drops); on a floor or ceiling only the round drops.
        static const int kRound[] = {3, 4, 6, 8, 9, 10, 11, 13, 15};
        const int cell = wall ? (int)(Random01() * 16.0f) % 16 : kRound[std::min(8, (int)(Random01() * 9.0f))];
        // A wall's runs hang down: the image's top up the wall.
        const glm::vec3 across = wall ? glm::normalize(glm::cross(up, glm::vec3(0.0f, -1.0f, 0.0f))) : along;
        if (r < 0.6f) d = AddKnifeDecal("drops", centre, up, across, extent * 1.6f, delay, cell); // a drop fills little of its cell
        if (!d) d = AddDecal(Random01() < 0.5f ? "blood3" : "blood9", centre, up, along, extent, delay);
        break;
    }
    }
    if (d && kind != Splash::Drop) d->Mirror = Random01() < 0.5f; // (a wall's drop keeps its run downward either way)
    return d;
}

void BloodFx::SpawnGroundSplatter(const Hit& hit, float energy, const glm::vec3& from, const glm::vec3& flatDir, bool forward) {
    EnsureHooks();
    // Thrown out of the wound along the round's line, faster the harder the hit (a round that stayed in only spills
    // back out), and splashed on whatever it comes down on: floor, wall, crate, stairs.
    const float speed = forward ? (2.2f + 1.6f * std::min(energy, 2.0f)) * (hit.Head ? 1.15f : 1.0f) : 1.2f;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 side = glm::normalize(glm::cross(flatDir, up));
    const float timeScale = 1.6f / std::max(Config.Speed, 0.1f); // the default Speed is real time; it scales like the sprays
    auto splash = [&](const glm::vec3& origin, const glm::vec3& v0, Splash kind, float size, float grow, float step) -> bool {
        glm::vec3 p, n, v;
        float t = 0.0f;
        if (!ThrowArc(origin, v0, p, n, v, t, step)) return false;
        // Stretched along the way it was going on that surface, longer the more it skidded in (a grazing landing).
        glm::vec3 along = v - n * glm::dot(v, n);
        const float skid = glm::length(along) / std::max(glm::length(v), 1e-3f);
        along = glm::length(along) > 1e-3f ? along : RandomTangent(n);
        const float stretch = kind == Splash::Drop ? 1.0f + 0.4f * skid : 1.0f + 1.2f * skid * std::clamp(glm::length(v) / 4.0f, 0.3f, 1.0f);
        Decal* d = AddSplash(kind, p + n * 0.01f, n, along, glm::vec3(size * stretch, 0.3f, size), t * timeScale);
        if (d) d->PoolGrow = grow;
        if (d && kind != Splash::Drop && std::abs(n.y) < 0.5f) SpawnWallDrips(p, n, size, t * timeScale); // a wall: it runs
        return d != nullptr;
    };
    const float s = GroundSplatterSize(hit, energy) * (0.85f + 0.3f * Random01());
    const float k = std::clamp(energy, 0.5f, 2.0f);
    bool landed = false;
    // The bulk (skipped when the spray already spattered a wall in its path - that's this blood).
    if (!m_LastClipped)
        landed = splash(from, flatDir * speed + up * 0.6f + side * ((Random01() - 0.5f) * 0.4f), Random01() < 0.5f ? Splash::Big : Splash::Streak, s,
                        0.25f, 0.035f);
    // Drops flung wider, each its own arc and landing: more the harder the hit, more again from a head.
    const int drops = (int)((hit.Head ? 7.0f : 4.0f) * k + Random01() * 3.0f);
    for (int i = 0; i < drops; ++i) {
        const float f = 0.5f + 0.8f * Random01();
        const glm::vec3 v0 = flatDir * (speed * f) + up * (0.3f + 1.2f * Random01()) + side * ((Random01() - 0.5f) * 1.8f);
        landed |= splash(from, v0, Random01() < 0.35f ? Splash::Streak : Splash::Drop, s * (0.15f + 0.22f * Random01()), 0.15f, 0.035f);
    }
    // Specks: the fine spray that makes it read wet and violent - small, many, cheap (longer arc steps).
    const int specks = (int)((hit.Head ? 8.0f : 5.0f) * k);
    for (int i = 0; i < specks; ++i) {
        const glm::vec3 v0 = flatDir * (speed * (0.6f + 0.9f * Random01())) + up * (0.5f + 1.5f * Random01()) + side * ((Random01() - 0.5f) * 2.4f);
        landed |= splash(from, v0, Splash::Drop, s * (0.06f + 0.08f * Random01()), 0.1f, 0.07f);
    }
    // And out of the entry, back toward the shooter: a few drops on the near side too.
    if (forward) {
        const int back = 1 + (int)(Random01() * 2.5f * k);
        for (int i = 0; i < back; ++i) {
            const glm::vec3 v0 = -flatDir * (0.8f + 1.4f * Random01()) + up * (0.3f + 0.8f * Random01()) + side * ((Random01() - 0.5f) * 1.2f);
            landed |= splash(hit.Point - flatDir * 0.05f, v0, Splash::Drop, s * (0.1f + 0.15f * Random01()), 0.12f, 0.05f);
        }
    }
    if (landed) ++m_GroundSplatters;
}

void BloodFx::SpawnCorpseSplash(const Hit& hit, float energy) {
    EnsureHooks();
    // A body lying there: the round goes on into what it lies on, so the blood comes back up out of the wound and
    // splashes round it - from just above the wound, outward, low.
    const glm::vec3 from = hit.Point + glm::vec3(0.0f, 0.06f, 0.0f) - glm::normalize(hit.Direction) * 0.04f;
    const float timeScale = 1.6f / std::max(Config.Speed, 0.1f);
    const float k = std::clamp(energy, 0.5f, 2.0f);
    const float s = GroundSplatterSize(hit, energy);
    const int n = (int)(5.0f * k + Random01() * 3.0f);
    bool landed = false;
    for (int i = 0; i < n; ++i) {
        const float a = Random01() * 6.2832f;
        const glm::vec3 out(std::cos(a), 0.0f, std::sin(a));
        const glm::vec3 v0 = out * (0.8f + 1.8f * Random01()) * k + glm::vec3(0.0f, 0.8f + 1.0f * Random01(), 0.0f);
        glm::vec3 p, nn, v;
        float t = 0.0f;
        if (!ThrowArc(from, v0, p, nn, v, t, 0.05f)) continue;
        glm::vec3 along = v - nn * glm::dot(v, nn);
        along = glm::length(along) > 1e-3f ? along : RandomTangent(nn);
        const float ds = s * (i == 0 ? 0.9f : 0.15f + 0.25f * Random01());
        if (Decal* d = AddSplash(i == 0 ? Splash::Big : Splash::Drop, p + nn * 0.01f, nn, along, glm::vec3(ds * 1.2f, 0.3f, ds), t * timeScale)) {
            d->PoolGrow = 0.2f;
            landed = true;
        }
    }
    // The new wound bleeds into what's under it: a small pool, spreading from a patch.
    glm::vec3 p, nn;
    if (Config.Pools && m_Ray(from + glm::vec3(0.0f, 0.3f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 1.2f, p, nn) && nn.y > 0.6f) {
        const float ps = 0.35f + 0.2f * Random01();
        Decal* d = AddKnifeDecal("pool_smooth", p + nn * 0.01f, nn, RandomTangent(nn), glm::vec3(ps * 1.25f, 0.3f, ps * 1.25f), 0.3f);
        if (!d) d = AddDecal("attached", p + nn * 0.01f, nn, RandomTangent(nn), glm::vec3(ps, 0.3f, ps), 0.3f);
        if (d) {
            d->PoolGrow = 8.0f + 4.0f * Random01();
            d->Spread = true;
            d->DrySeconds *= 2.5f;
            landed = true;
        }
    }
    if (landed) ++m_GroundSplatters;
}

void BloodFx::SpawnPuffs(const Hit& hit, const glm::vec3& dir, float energy, bool exited, const glm::vec3& exitPoint, bool extraPellet) {
    if (!m_Sprites) return;
    FxSprites& fx = *m_Sprites;
    // Knife "Real Blood" BloodHit / BloodExplosion, re-timed for a round: everything here is over in well under a
    // second - the frame the round goes in has to read as a hit before the volumetric spray has moved.
    const glm::vec3 kBlood = BloodPalette::Thin; // the liquid in a thin sheet (the palette: one material with the stains)
    const glm::vec3 kMist = BloodPalette::Mist;  // a fine dark-red haze
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
        e.Life = 0.26f + 0.06f * Random01();
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
            e.Life = (hit.Head ? 0.6f : 0.4f) + 0.2f * Random01();
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
            e.Life = 0.34f + 0.08f * Random01();
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
            e.Life = (hit.Head ? 0.75f : 0.5f) + 0.2f * Random01();
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
            // already falling), landing when the fluid gets there (~12 m/s out of a wound).
            static const char* const kStreaks[] = {"blood1", "blood3", "blood9", "blood2_right"};
            const float dist = glm::length(p - wound);
            const float s = size * (0.85f + 0.3f * Random01()) * std::clamp(1.4f - dist / reach, 0.6f, 1.2f);
            const glm::vec3 at = p - glm::vec3(0.0f, 0.04f + 0.08f * dist, 0.0f);
            glm::vec3 along = RandomTangent(n);
            if (along.y > 0.0f) along = -along; // streaks run out and down, not up
            // The splash's body (a blot with its spokes) and the streaks thrown out of it.
            const float land = dist / 12.0f;
            if (Decal* body = AddSplash(Splash::Big, at, n, RandomTangent(n), glm::vec3(0.75f * s, 0.25f, 0.75f * s), land)) body->PoolGrow = 0.35f;
            const char* streaks = kStreaks[std::min(3, (int)(Random01() * 4.0f))];
            if (Decal* streak = AddDecal(streaks, at, n, along, glm::vec3(1.3f * s, 0.25f, 0.9f * s), land)) streak->PoolGrow = 0.25f;
            SpawnWallDrips(at, n, s, land);
        }
    }
    for (const BloodSprayDef& def : preset.Sprays) {
        const int sim = m_SimLookup(def.Sim);
        if (sim < 0) continue;
        if ((int)m_Sprays.size() >= std::max(1, Config.MaxSprays)) m_Sprays.erase(m_Sprays.begin());
        Spray s;
        s.Sim = sim;
        s.Model = prefabToWorld * FromRows(def.M);
        s.Duration = PlaybackSeconds(def, preset.AnimationSpeed, size) / std::max(Config.Speed, 0.1f);
        s.Age = 0.06f * s.Duration; // out of the wound on the frame of the hit, not still inside it
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
    // Past its lifetime (when one is set) a stain still waits until it's out of view: nothing vanishes in front of you.
    m_Decals.erase(std::remove_if(m_Decals.begin(), m_Decals.end(), [&](const Decal& d) { return d.Age >= d.Life && !InView(d); }),
                   m_Decals.end());
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
        // Real Blood's PBR pools when the Knife library is there (bigger: the pool fills ~70% of its cell).
        Decal* d = AddKnifeDecal("pool_smooth", p + glm::vec3(0.0f, 0.01f, 0.0f), n, RandomTangent(n),
                                 glm::vec3(s * 1.25f, 0.3f, s * 1.25f), 0.0f);
        if (!d) d = AddDecal("attached", p + glm::vec3(0.0f, 0.01f, 0.0f), n, RandomTangent(n), glm::vec3(s, 0.3f, s), 0.0f);
        if (d) {
            d->PoolGrow = 20.0f + 8.0f * Random01(); // a slow spread from a small patch (PoolScale)
            d->Spread = true;
            ++m_PoolsSpawned;
            d->DrySeconds *= 2.5f; // a pool stays wet far longer than a spatter
        }
    }
    // The wounded drip as they go: a drop on the ground under him every so often, falling from about the wound.
    for (size_t i = 0; i < m_Bleeds.size();) {
        Bleed& b = m_Bleeds[i];
        glm::vec3 centre, p, n;
        if (m_Now > b.Until || !m_Body || !m_Body(b.Entity, centre)) { m_Bleeds.erase(m_Bleeds.begin() + (std::ptrdiff_t)i); continue; }
        if (m_Now >= b.Next) {
            b.Next = m_Now + b.Rate * (0.7f + 0.6f * Random01());
            EnsureHooks();
            const glm::vec3 at = centre + glm::vec3((Random01() - 0.5f) * 0.3f, 0.0f, (Random01() - 0.5f) * 0.3f);
            if (m_Ray(at, glm::vec3(0.0f, -1.0f, 0.0f), 2.5f, p, n) && n.y > 0.6f) {
                const float s = 0.05f + 0.06f * Random01();
                const float fall = std::sqrt(2.0f * std::max(at.y - p.y, 0.05f) / 9.81f);
                if (Decal* d = AddSplash(Splash::Drop, p + n * 0.01f, n, RandomTangent(n), glm::vec3(s, 0.3f, s), fall)) {
                    d->PoolGrow = 0.15f;
                    ++m_BleedDrops;
                }
            }
        }
        ++i;
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
                r.Dry = std::clamp(s.Age / std::max(s.DrySeconds, 1.0f), 0.0f, 1.0f);
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
        if (d.Spread) { // a pool widens (x, z); its projection depth stays
            const float k = PoolScale(d);
            r.Model[0] *= k;
            r.Model[2] *= k;
        }
        r.Set = d.Set;
        r.Knife = d.Knife;
        r.Mirror = d.Mirror;
        r.Cell = r.NextCell = d.Cell;
        if (d.FrameSeconds > 0.0f && d.Frames > 1) { // a flipbook: on to its last cell, then held
            const float f = std::min(d.Age / d.FrameSeconds, (float)(d.Frames - 1));
            r.Cell = std::min(d.Cell + (int)f, d.Frames - 1);
            r.NextCell = std::min(r.Cell + 1, d.Frames - 1);
            r.CellBlend = f - std::floor(f);
        }
        r.Cutout = DecalCutout(d);
        r.Dry = std::clamp(d.Age / std::max(d.DrySeconds, 1.0f), 0.0f, 1.0f);
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
