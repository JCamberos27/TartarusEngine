#include "ScoringSystem.h"
#include "Components.h"
#include "GameModuleAPI.h"
#include "MaterialAsset.h"
#include "World.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;

entt::entity ToEntity(const World& world, std::uint32_t id) {
    const entt::entity e = static_cast<entt::entity>(id);
    return (id != kNone && id != kPlayerEntity && world.Registry.valid(e)) ? e : entt::null;
}

// Effects in flight. Keyed by raw entity id; every entry is re-validated before use, and all of
// them are finished and dropped when a new Play session starts (see BeginFrame).
struct Effects {
    std::unordered_map<std::uint32_t, float> Bursts;                       // particle entity -> seconds left
    std::unordered_map<std::uint32_t, std::pair<float, float>> Flashes;    // light -> (authored intensity, boost 0..1)
    std::unordered_map<std::uint32_t, float> FlashPeak;                    // light -> intensity at boost 1
    std::unordered_map<std::uint32_t, glm::vec3> ReleasePoints;            // body -> where the gravity gun let go
    std::unordered_map<std::uint64_t, double> GoalCooldown;                // (trigger, body) -> time it last scored
    std::unordered_map<std::uint32_t, double> ImpactCooldown;              // body -> time it last made a sound
    std::uint32_t Held = kNone;
    std::uint64_t LastFrame = 0;
    double Now = 0.0;
};
Effects g_Fx;
std::mt19937 g_Rng{1234u};

// Update only runs while playing; a jump in the frame counter means Play was stopped (the scene
// was restored from its snapshot) or paused, so whatever was mid-effect is finished off.
void BeginFrame(const GameModuleHostAPI& host, World& world) {
    GameTime t;
    if (host.GetTime) host.GetTime(t);
    g_Fx.Now = t.Time;
    const bool newSession = g_Fx.LastFrame == 0 || t.FrameCount > g_Fx.LastFrame + 5 || t.FrameCount < g_Fx.LastFrame;
    g_Fx.LastFrame = t.FrameCount;
    if (!newSession) return;
    for (const auto& [id, left] : g_Fx.Bursts) {
        (void)left;
        const entt::entity e = ToEntity(world, id);
        if (e != entt::null)
            if (auto* ps = world.Registry.try_get<ParticleSystemComponent>(e)) ps->Emitting = false;
    }
    for (const auto& [id, f] : g_Fx.Flashes) {
        const entt::entity e = ToEntity(world, id);
        if (e != entt::null)
            if (auto* l = world.Registry.try_get<LightComponent>(e)) l->Intensity = f.first;
    }
    g_Fx.Bursts.clear();
    g_Fx.Flashes.clear();
    g_Fx.FlashPeak.clear();
    g_Fx.ReleasePoints.clear();
    g_Fx.GoalCooldown.clear();
    g_Fx.ImpactCooldown.clear();
    g_Fx.Held = kNone;
}

// Segment masks for 0-9, bit 0 = A .. bit 6 = G.
constexpr std::uint8_t kDigitSegments[10] = {
    0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
    0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111,
};

void ShowDigits(World& world, const ScoreboardComponent& board) {
    for (auto [e, digit] : world.Registry.view<ScoreDigitComponent>().each()) {
        const int score = std::clamp(digit.Team == 0 ? board.Home : board.Away, 0, 99);
        const int place = std::clamp(digit.Place, 0, 1);
        const bool blank = place == 1 && score < 10;
        const std::uint8_t mask = blank ? 0 : kDigitSegments[place == 0 ? score % 10 : score / 10];
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        if (!h) continue;
        for (entt::entity c : h->Children) {
            const auto* name = world.Registry.try_get<NameComponent>(c);
            auto* rc = world.Registry.try_get<RenderableComponent>(c);
            if (!name || !rc || rc->Materials.empty() || !rc->Materials[0]) continue;
            const std::string& n = name->Name;
            if (n.size() < 5 || n.compare(0, 4, "Seg ") != 0) continue;
            const int seg = n[4] - 'A';
            if (seg < 0 || seg > 6) continue;
            const bool on = (mask >> seg) & 1u;
            rc->Materials[0]->Mat.EmissiveStrength = on ? digit.OnStrength : digit.OffStrength;
        }
    }
}

void StartEffects(World& world, entt::entity goal, const GoalTriggerComponent& g) {
    const auto* h = world.Registry.try_get<HierarchyComponent>(goal);
    if (!h) return;
    for (entt::entity c : h->Children) {
        const std::uint32_t id = static_cast<std::uint32_t>(c);
        if (auto* ps = world.Registry.try_get<ParticleSystemComponent>(c)) {
            ps->Emitting = true;
            g_Fx.Bursts[id] = 0.3f;
        }
        if (auto* l = world.Registry.try_get<LightComponent>(c)) {
            auto it = g_Fx.Flashes.find(id);
            if (it == g_Fx.Flashes.end()) it = g_Fx.Flashes.emplace(id, std::make_pair(l->Intensity, 0.0f)).first;
            it->second.second = 1.0f;
            g_Fx.FlashPeak[id] = std::max(g.FlashIntensity, it->second.first);
        }
    }
}

void TickEffects(World& world, float dt) {
    for (auto it = g_Fx.Bursts.begin(); it != g_Fx.Bursts.end();) {
        it->second -= dt;
        const entt::entity e = ToEntity(world, it->first);
        auto* ps = e != entt::null ? world.Registry.try_get<ParticleSystemComponent>(e) : nullptr;
        if (!ps) { it = g_Fx.Bursts.erase(it); continue; }
        if (it->second <= 0.0f) { ps->Emitting = false; it = g_Fx.Bursts.erase(it); continue; }
        ++it;
    }
    for (auto it = g_Fx.Flashes.begin(); it != g_Fx.Flashes.end();) {
        const entt::entity e = ToEntity(world, it->first);
        auto* l = e != entt::null ? world.Registry.try_get<LightComponent>(e) : nullptr;
        if (!l) { it = g_Fx.Flashes.erase(it); continue; }
        auto& [base, boost] = it->second;
        boost = std::max(0.0f, boost - dt / 1.2f);
        const float peak = g_Fx.FlashPeak.count(it->first) ? g_Fx.FlashPeak[it->first] : base;
        l->Intensity = base + (peak - base) * boost * boost; // quick flash, soft tail
        if (boost <= 0.0f) { l->Intensity = base; it = g_Fx.Flashes.erase(it); continue; }
        ++it;
    }
}

// Where the gravity gun let go of each body, for three-pointers.
void TrackReleases(const GameModuleHostAPI& host) {
    if (!host.GetGrabbedEntity) return;
    const std::uint32_t held = host.GetGrabbedEntity();
    if (g_Fx.Held != kNone && held != g_Fx.Held && host.GetActorPosition) {
        float p[3];
        if (host.GetActorPosition(g_Fx.Held, p)) g_Fx.ReleasePoints[g_Fx.Held] = glm::vec3(p[0], p[1], p[2]);
    }
    if (held != kNone) g_Fx.ReleasePoints.erase(held); // picked up again: the old throw no longer counts
    g_Fx.Held = held;
}

float RandomPitch(float variation) {
    std::uniform_real_distribution<float> d(-variation, variation);
    return 1.0f + d(g_Rng);
}

} // namespace

void UpdateScoring(const GameModuleHostAPI& host, World& world, float dt) {
    BeginFrame(host, world);
    TrackReleases(host);

    // The first scoreboard in the scene (front() rather than a break out of a loop: MSVC flags
    // the loop's unreachable increment as C4702 in Debug, and warnings are errors, #173).
    auto boards = world.Registry.view<ScoreboardComponent>();
    ScoreboardComponent* board = boards.empty() ? nullptr : &boards.get<ScoreboardComponent>(boards.front());

    if (host.GetTriggerEvents) {
        TriggerEvent events[64];
        const int n = std::min(host.GetTriggerEvents(events, 64), 64);
        for (int i = 0; i < n; ++i) {
            const TriggerEvent& ev = events[i];
            if (ev.Kind != TriggerEvent::Enter) continue;
            const entt::entity goal = ToEntity(world, ev.Trigger), ball = ToEntity(world, ev.Other);
            if (goal == entt::null || ball == entt::null) continue;
            const auto* g = world.Registry.try_get<GoalTriggerComponent>(goal);
            if (!g || world.Registry.all_of<InactiveTag>(goal)) continue;
            const auto* tag = world.Registry.try_get<TagComponent>(ball);
            if (!tag || tag->Tag != g->Tag) continue;
            if (g->RequireDownward && host.GetBodyState) {
                BodyState bs;
                if (!host.GetBodyState(ev.Other, bs) || bs.Velocity[1] > -0.2f) continue;
            }
            // One goal per ball per hoop per second: a ball rattling around inside can re-enter.
            const std::uint64_t key = (std::uint64_t)ev.Trigger << 32 | ev.Other;
            if (auto it = g_Fx.GoalCooldown.find(key); it != g_Fx.GoalCooldown.end() && g_Fx.Now - it->second < 1.0) continue;
            g_Fx.GoalCooldown[key] = g_Fx.Now;

            int points = g->Points;
            float goalPos[3] = {0.0f, 0.0f, 0.0f};
            const bool haveGoalPos = host.GetActorPosition && host.GetActorPosition(ev.Trigger, goalPos);
            if (g->ThreePointDistance > 0.0f && haveGoalPos) {
                if (auto it = g_Fx.ReleasePoints.find(ev.Other); it != g_Fx.ReleasePoints.end()) {
                    const glm::vec2 d(it->second.x - goalPos[0], it->second.z - goalPos[2]);
                    if (glm::length(d) >= g->ThreePointDistance) points = g->ThreePoints;
                }
            }
            g_Fx.ReleasePoints.erase(ev.Other);
            if (board) (g->Team == 0 ? board->Home : board->Away) += points;
            StartEffects(world, goal, *g);
            if (!g->ScoreSound.empty() && host.PlaySoundAt && haveGoalPos)
                host.PlaySoundAt(g->ScoreSound.c_str(), goalPos, points >= g->ThreePoints ? 1.0f : 0.85f, 1.0f);
        }
    }

    TickEffects(world, dt);
    if (board) ShowDigits(world, *board);
}

void UpdateImpactSounds(const GameModuleHostAPI& host, World& world, float dt) {
    (void)dt;
    if (!host.GetContactEvents || !host.PlaySoundAt) return;
    ContactEvent events[128];
    const int n = std::min(host.GetContactEvents(events, 128), 128);
    int played = 0;
    for (int i = 0; i < n && played < 12; ++i) {
        const ContactEvent& ev = events[i];
        if (ev.Kind != ContactEvent::Enter || ev.NormalSpeed <= 0.0f) continue;
        for (const std::uint32_t id : { ev.A, ev.B }) {
            const entt::entity e = ToEntity(world, id);
            if (e == entt::null) continue;
            const auto* snd = world.Registry.try_get<ImpactSoundComponent>(e);
            if (!snd || snd->Clip.empty() || ev.NormalSpeed < snd->MinSpeed) continue;
            // A body settling can report a burst of tiny re-contacts; one sound per 60 ms each.
            if (auto it = g_Fx.ImpactCooldown.find(id); it != g_Fx.ImpactCooldown.end() && g_Fx.Now - it->second < 0.06)
                continue;
            g_Fx.ImpactCooldown[id] = g_Fx.Now;
            const float t = std::clamp((ev.NormalSpeed - snd->MinSpeed) / std::max(snd->MaxSpeed - snd->MinSpeed, 0.01f),
                                       0.0f, 1.0f);
            const float volume = snd->Volume * (0.12f + 0.88f * std::sqrt(t));
            host.PlaySoundAt(snd->Clip.c_str(), ev.Point, volume, RandomPitch(snd->PitchVariation));
            ++played;
        }
    }
}
