#include "CombatFx.h"

#include "AudioEngine.h"
#include "Components.h"
#include "ProjectPaths.h"
#include "World.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int kFlashLights = 6;
constexpr float kFlashTime = 0.055f;
const char* kDir = "assets/Audio/Combat/";

std::string Path(const std::string& file) { return ProjectPaths::Resolve(std::string(kDir) + file); }

const char* kAllSounds[] = {"ak_shot.wav", "ak_shot_b.wav", "ak_shot_far.wav", "shotgun_shot.wav", "shotgun_shot_far.wav",
                            "shotgun_pump.wav", "reload.wav", "dry_fire.wav", "whizz_1.wav", "whizz_2.wav", "whizz_3.wav",
                            "flesh_hit.wav", "hitmarker.wav", "hitmarker_kill.wav", "body_fall.wav"};

entt::entity MakeParticles(World& world, const char* name, float life, float startSize, float endSize, const glm::vec3& c0,
                           const glm::vec3& c1, float a0, float a1, float intensity, int blend) {
    const entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), name);
    auto& ps = world.Registry.emplace<ParticleSystemComponent>(e);
    ps.Emitting = false; // CombatFx puts every particle in by hand
    ps.MaxParticles = 2000;
    ps.Lifetime = life;
    ps.StartSize = startSize;
    ps.EndSize = endSize;
    ps.StartColor = c0;
    ps.EndColor = c1;
    ps.StartAlpha = a0;
    ps.EndAlpha = a1;
    ps.Intensity = intensity;
    ps.BlendMode = blend;
    return e;
}

glm::vec3 AnyPerpendicular(const glm::vec3& d) {
    const glm::vec3 a = std::abs(d.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    return glm::normalize(glm::cross(d, a));
}

} // namespace

float CombatFx::Rand01() {
    m_Rng ^= m_Rng << 13;
    m_Rng ^= m_Rng >> 17;
    m_Rng ^= m_Rng << 5;
    return (float)(m_Rng & 0xFFFFFFu) / (float)0x1000000u;
}

void CombatFx::Start(World& world) {
    Stop(world);
    m_Active = true;
    m_Now = 0.0f;
    m_ShotsHeard = m_Whizzes = 0;
    if (AudioEngine::IsInitialized())
        for (const char* s : kAllSounds) AudioEngine::Load(Path(s)); // decoded up front: no hitch on the first shot
    for (int i = 0; i < kFlashLights; ++i) {
        Flash f;
        f.Light = world.CreateEmptyEntity(glm::vec3(0.0f, -100.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "[Runtime] Muzzle Light");
        auto& l = world.Registry.emplace<LightComponent>(f.Light);
        l.Kind = LightComponent::Type::Point;
        l.Color = glm::vec3(1.0f, 0.72f, 0.38f);
        l.Intensity = 0.0f;
        l.Range = 0.01f;
        m_Flashes.push_back(f);
    }
    // Sparks: the flash itself, a few hot additive specks thrown forward that die in a frame or three.
    m_Sparks = MakeParticles(world, "[Runtime] Muzzle Flash", 0.05f, 0.075f, 0.015f, glm::vec3(1.0f, 0.85f, 0.5f),
                             glm::vec3(1.0f, 0.4f, 0.1f), 1.0f, 0.0f, 9.0f, 1);
    m_Smoke = MakeParticles(world, "[Runtime] Muzzle Smoke", 0.9f, 0.05f, 0.3f, glm::vec3(0.45f, 0.44f, 0.43f),
                            glm::vec3(0.55f, 0.54f, 0.53f), 0.09f, 0.0f, 0.6f, 0);
    m_Tracers = MakeParticles(world, "[Runtime] Tracers", 0.06f, 0.035f, 0.025f, glm::vec3(1.0f, 0.75f, 0.35f),
                              glm::vec3(1.0f, 0.55f, 0.2f), 1.0f, 0.6f, 14.0f, 1);
}

void CombatFx::Stop(World& world) {
    for (Flash& f : m_Flashes)
        if (world.Registry.valid(f.Light)) world.DestroyEntityAndChildren(f.Light);
    m_Flashes.clear();
    for (entt::entity* e : {&m_Sparks, &m_Smoke, &m_Tracers}) {
        if (*e != entt::null && world.Registry.valid(*e)) world.DestroyEntityAndChildren(*e);
        *e = entt::null;
    }
    m_Active = false;
}

void CombatFx::PlaySound(const std::string& file, const glm::vec3& pos, bool at2D, float volume, float pitch, float minDist,
                         float maxDist) {
    if (!AudioEngine::IsInitialized()) return;
    const AudioEngine::SoundHandle h = AudioEngine::Play(Path(file), std::clamp(volume, 0.0f, 1.0f), false, AudioEngine::Bus::SFX);
    if (h == AudioEngine::InvalidHandle) return;
    AudioEngine::SetPitch(h, pitch);
    if (!at2D) {
        AudioEngine::SetPosition(h, pos);
        AudioEngine::SetAttenuation(h, minDist, maxDist, 1.0f);
    }
}

void CombatFx::Shot(World& world, Gun gun, const glm::vec3& origin, const glm::vec3& end, bool fromPlayer, bool tracer) {
    if (!m_Active) return;
    ++m_ShotsHeard;
    const bool shotgun = gun == Gun::Shotgun;
    const float pitch = 0.96f + 0.08f * Rand01();
    if (fromPlayer) {
        PlaySound(shotgun ? "shotgun_shot.wav" : (Rand01() < 0.5f ? "ak_shot.wav" : "ak_shot_b.wav"), origin, true, 0.75f, pitch, 1, 1);
    } else {
        // Up close it cracks; across the arena the crack is gone and the field's echo carries it.
        const float d = glm::length(origin - m_Listener);
        const float nearW = std::clamp(1.0f - (d - 18.0f) / 25.0f, 0.0f, 1.0f);
        if (nearW > 0.01f)
            PlaySound(shotgun ? "shotgun_shot.wav" : (Rand01() < 0.5f ? "ak_shot.wav" : "ak_shot_b.wav"), origin, false, nearW,
                      pitch, 3.0f, 90.0f);
        if (nearW < 0.99f)
            PlaySound(shotgun ? "shotgun_shot_far.wav" : "ak_shot_far.wav", origin, false, 0.9f * (1.0f - nearW) + 0.1f, pitch, 6.0f, 160.0f);
    }

    glm::vec3 dir = end - origin;
    const float len = glm::length(dir);
    dir = len > 1e-4f ? dir / len : glm::vec3(0.0f, 0.0f, -1.0f);

    // The light: off the muzzle a hand's width, so the gun and the shooter's hands catch it.
    if (!fromPlayer && !m_Flashes.empty()) {
        Flash& f = m_Flashes[m_NextFlash++ % m_Flashes.size()];
        if (world.Registry.valid(f.Light)) {
            world.Registry.get<TransformComponent>(f.Light).Position = origin + dir * 0.12f;
            f.Peak = shotgun ? 26.0f : 18.0f;
            f.Left = kFlashTime;
        }
    }
    // The flash: a hot core plus a fan of sparks along the bore, then a puff of smoke drifting up.
    const glm::vec3 side = AnyPerpendicular(dir), up = glm::cross(side, dir);
    if (auto* ps = world.Registry.try_get<ParticleSystemComponent>(m_Sparks); ps && !fromPlayer) {
        const int n = shotgun ? 14 : 9;
        for (int i = 0; i < n; ++i) {
            ParticleSystemComponent::Particle p;
            const float a = Rand01() * 6.2831853f, r = Rand01() * (i == 0 ? 0.0f : 0.35f);
            const glm::vec3 v = glm::normalize(dir + (side * std::cos(a) + up * std::sin(a)) * r);
            p.Pos = origin + dir * 0.03f;
            p.Vel = v * (i == 0 ? 0.5f : 6.0f + 10.0f * Rand01());
            p.Life = (i == 0 ? 0.035f : 0.03f + 0.03f * Rand01()) * (shotgun ? 1.4f : 1.0f);
            ps->Live.push_back(p);
        }
    }
    if (auto* ps = world.Registry.try_get<ParticleSystemComponent>(m_Smoke)) {
        const int n = shotgun ? 3 : 1;
        for (int i = 0; i < n && ps->Live.size() < 1500; ++i) {
            ParticleSystemComponent::Particle p;
            p.Pos = origin + dir * (0.05f + 0.1f * Rand01());
            p.Vel = dir * (0.6f + 0.8f * Rand01()) + glm::vec3(0.0f, 0.25f + 0.2f * Rand01(), 0.0f) +
                    (side * (Rand01() - 0.5f) + up * (Rand01() - 0.5f)) * 0.3f;
            p.Life = 0.6f + 0.6f * Rand01();
            ps->Live.push_back(p);
        }
    }
    // A tracer: a short dashed streak flying down the line at a few hundred metres a second.
    if (tracer && len > 2.0f) {
        if (auto* ps = world.Registry.try_get<ParticleSystemComponent>(m_Tracers)) {
            const float speed = 320.0f, life = std::min(0.5f, (len - 1.5f) / speed);
            for (int i = 0; i < 7 && ps->Live.size() < 1900; ++i) {
                ParticleSystemComponent::Particle p;
                p.Pos = origin + dir * (1.2f + 0.22f * (float)i);
                p.Vel = dir * speed;
                p.Life = std::max(0.02f, life - 0.22f * (float)i / speed);
                ps->Live.push_back(p);
            }
        }
    }
}

void CombatFx::Whizz(const glm::vec3& point) {
    if (!m_Active || m_Now - m_LastWhizz < 0.07f) return; // a burst reads as a few cracks, not a smear
    m_LastWhizz = m_Now;
    ++m_Whizzes;
    static const char* kWhizz[] = {"whizz_1.wav", "whizz_2.wav", "whizz_3.wav"};
    PlaySound(kWhizz[(int)(Rand01() * 2.999f)], point, false, 0.9f, 0.92f + 0.16f * Rand01(), 0.4f, 6.0f);
}

void CombatFx::Play(Cue cue, const glm::vec3& pos, bool at2D, float volume) {
    if (!m_Active) return;
    const float pitch = 0.95f + 0.1f * Rand01();
    switch (cue) {
    case Cue::Pump: PlaySound("shotgun_pump.wav", pos, at2D, 0.7f * volume, pitch, 1.5f, 25.0f); break;
    case Cue::Reload: PlaySound("reload.wav", pos, at2D, 0.7f * volume, pitch, 1.5f, 18.0f); break;
    case Cue::DryFire: PlaySound("dry_fire.wav", pos, at2D, 0.6f * volume, pitch, 1.0f, 10.0f); break;
    case Cue::BodyFall: PlaySound("body_fall.wav", pos, at2D, 0.9f * volume, pitch, 2.0f, 30.0f); break;
    case Cue::FleshHit: PlaySound("flesh_hit.wav", pos, at2D, 0.8f * volume, pitch, 1.0f, 25.0f); break;
    case Cue::Hitmarker: PlaySound("hitmarker.wav", pos, true, 0.45f * volume, 1.0f, 1, 1); break;
    case Cue::HitmarkerKill: PlaySound("hitmarker_kill.wav", pos, true, 0.6f * volume, 1.0f, 1, 1); break;
    }
}

void CombatFx::Update(World& world, float dt) {
    if (!m_Active) return;
    m_Now += dt;
    for (Flash& f : m_Flashes) {
        if (!world.Registry.valid(f.Light)) continue;
        auto& l = world.Registry.get<LightComponent>(f.Light);
        if (f.Left <= 0.0f) {
            l.Intensity = 0.0f;
            l.Range = 0.01f;
            continue;
        }
        // Full on the frame it fires, gone two or three frames later.
        const float k = std::clamp(f.Left / kFlashTime, 0.0f, 1.0f);
        l.Intensity = f.Peak * k * k;
        l.Range = 7.0f;
        f.Left -= dt;
    }
}
