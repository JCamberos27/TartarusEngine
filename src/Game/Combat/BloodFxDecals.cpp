// BloodFx's stains: decals added (merged with a fresh one just like it), room made at the cap, pools and wall spatter put
// down directly, wall drips, and the footprints a walker leaves after stepping in fresh blood. The rest of BloodFx (the
// hit, the spray, splatter, puffs, Update and Submit) is in BloodFx.cpp.
#include "BloodFx.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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
        if (d.Set != set || d.Knife != knife || d.Age > 5.0f || d.Palette != m_Palette) continue;
        const float other = std::max(glm::length(glm::vec3(d.Model[0])), glm::length(glm::vec3(d.Model[2])));
        if (std::abs(other - size) > 0.25f * size) continue;
        if (glm::length(glm::vec3(d.Model[3]) - c) > 0.15f * size) continue;
        if (glm::dot(glm::normalize(glm::vec3(d.Model[1])), up) < 0.9f) continue;
        return i;
    }
    return -1;
}

BloodFx::Decal* BloodFx::AddKnifeDecal(const char* entry, const glm::vec3& centre, const glm::vec3& up, const glm::vec3& along,
                                       const glm::vec3& extent, float delay, int cell, bool flipbook) {
    EnsureHooks();
    int cells = 1;
    const int id = m_KnifeLookup(entry, cells);
    if (id < 0) return nullptr;
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
    // (A flipbook - a wall drip running down - is its own stain: merged, the caller would re-time the one it landed on.)
    if (const int dup = flipbook ? -1 : DuplicateOf(d.Set, d.Knife, d.Model); dup >= 0) {
        ++m_StainsMerged;
        return &m_Decals[(size_t)dup];
    }
    MakeRoom(std::string(entry) == "footprint"); // (only for a stain that's added: a merge adds none)
    d.Palette = m_Palette;
    m_Decals.push_back(d);
    ++m_DecalsSpawned;
    return &m_Decals.back();
}

bool BloodFx::InView(const Decal& d) const {
    return InViewPoint(glm::vec3(d.Model[3]), 0.5f * std::max(glm::length(glm::vec3(d.Model[0])), glm::length(glm::vec3(d.Model[2]))));
}

bool BloodFx::InViewPoint(const glm::vec3& c, float r) const {
    if (!m_HasViewer) return true;
    const glm::vec3 to = c - m_Eye;
    const float dist = glm::length(to);
    if (dist - r > kVisibleReach) return false;
    if (dist <= r + 2.0f) return true; // right here: under your feet counts as seen
    // A 75 degree half-angle cone (wider than any view and its edges), grown by the thing's size. Inside the bare cone it's
    // in view whatever its size (most stains, every frame): no asin / cos for those.
    const float facing = glm::dot(to / dist, m_Forward);
    constexpr float kCosCone = 0.25882f; // cos(1.309 rad = 75 degrees)
    if (facing > kCosCone) return true;
    return facing > std::cos(std::min(1.309f + std::asin(std::min(r / dist, 1.0f)), 3.1416f));
}

bool BloodFx::Pooled(unsigned entity) const {
    if (std::find(m_Pooled.begin(), m_Pooled.end(), entity) != m_Pooled.end()) return true;
    for (const PendingPool& p : m_Pools)
        if (p.Entity == entity) return true;
    return false;
}

void BloodFx::MakeRoom(bool print) {
    if (print) {
        // Prints have their own budget: past it the oldest starts a short fade (it never pops out).
        int live = 0, oldest = -1;
        for (int i = 0; i < (int)m_Decals.size(); ++i) {
            const Decal& d = m_Decals[(size_t)i];
            if (!d.Print || d.Age >= d.FadeAt) continue;
            ++live;
            if (oldest < 0) oldest = i;
        }
        if (live >= kMaxPrints && oldest >= 0) {
            Decal& d = m_Decals[(size_t)oldest];
            d.FadeAt = std::max(d.Age, 0.0f);
            d.FadeLen = 20.0f;
        }
        return;
    }
    int count = 0;
    for (const Decal& d : m_Decals) count += !d.Print;
    const int cap = std::max(1, Config.MaxDecals);
    if (count < cap) return;
    // Out of view first: the small marks (drops, specks) before the stains, the farthest first. Failing that (it's
    // all in view), the oldest small mark, then the oldest stain - never a pool or a print while anything else can go.
    // Ranked in one pass and the `count - cap + 1` first ones dropped together, plus a thirty-second of the cap as
    // headroom: a hit lays 15-30 stains, and ranking the whole list for each one was most of a hit's cost at the cap.
    auto size = [](const Decal& d) { return std::max(glm::length(glm::vec3(d.Model[0])), glm::length(glm::vec3(d.Model[2]))); };
    struct Candidate {
        int Index, Tier; // 0 out of view, 1 a small mark in view, 2 a stain in view, 3 anything else (pools)
        float Score;     // out of view: farther (and small) goes first
    };
    static std::vector<Candidate> order;
    order.clear();
    for (int i = 0; i < (int)m_Decals.size(); ++i) {
        const Decal& d = m_Decals[(size_t)i];
        if (d.Print) continue;
        const bool small = size(d) < 0.2f;
        if (!InView(d)) order.push_back({i, 0, glm::length(glm::vec3(d.Model[3]) - m_Eye) + (small ? 1000.0f : 0.0f)});
        else order.push_back({i, small ? 1 : !d.Spread ? 2 : 3, 0.0f});
    }
    std::stable_sort(order.begin(), order.end(), [](const Candidate& a, const Candidate& b) {
        return a.Tier != b.Tier ? a.Tier < b.Tier : a.Score > b.Score;
    });
    const size_t drop = std::min(order.size(), (size_t)(count - cap + 1 + cap / 32));
    if (drop == 0) return;
    static std::vector<unsigned char> gone;
    gone.assign(m_Decals.size(), 0);
    for (size_t i = 0; i < drop; ++i) gone[(size_t)order[i].Index] = 1;
    size_t w = 0;
    for (size_t i = 0; i < m_Decals.size(); ++i)
        if (!gone[i]) {
            if (w != i) m_Decals[w] = std::move(m_Decals[i]);
            ++w;
        }
    m_Decals.resize(w);
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
    if (m_PrintId < 0 && m_KnifeLookup) { // (found once; a miss is asked again: the library may not have loaded yet)
        int cells = 0;
        m_PrintId = m_KnifeLookup("footprint", cells);
        m_PrintCells = cells;
    }
    const int printId = m_PrintId >= 0 ? m_PrintId : -2;
    for (const Decal& d : m_Decals) {
        if (d.Age < 0.5f || d.Age > kFreshSeconds || d.Print || (d.Knife >= 0 && d.Knife == printId)) continue;
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
            const int cells = std::max(m_PrintCells, 1); // (InFreshBlood looked the footprint entry up)
            w->Shoe = std::min(cells - 1, (int)(Random01() * (float)cells));
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
        d->Print = true;
        d->FadeAt = kPrintLife;
        d->FadeLen = kPrintFade;
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
                                 land + 0.05f + 0.15f * Random01(), 0, /*flipbook=*/true);
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
    if (const int dup = DuplicateOf(d.Set, d.Knife, d.Model); dup >= 0) {
        ++m_StainsMerged;
        return &m_Decals[(size_t)dup];
    }
    MakeRoom(false);
    d.Palette = m_Palette;
    m_Decals.push_back(d);
    ++m_DecalsSpawned;
    return &m_Decals.back();
}
