#include "ImpactAudio.h"

#include "FoleyAudio.h"
#include "World.h"

#include <algorithm>
#include <cmath>

ImpactAudio& ImpactAudio::Get() {
    static ImpactAudio instance;
    return instance;
}

void ImpactAudio::Start(World& world) {
    Stop();
    m_T = ImpactAudioComponent{};
    for (const entt::entity e : world.Registry.view<ImpactAudioComponent>()) {
        m_T = world.Registry.get<ImpactAudioComponent>(e);
        break;
    }
    m_LastImpact.clear();
    m_LastFlyby = -1e9;
    m_Played = Counts{};
    m_Active = true;
}

void ImpactAudio::Stop() {
    m_Active = false;
}

bool ImpactAudio::CasingContactAudible(const ImpactAudioComponent& t, int contactIndex, float speed) {
    return t.Enabled && t.CasingsEnabled && contactIndex >= 0 && contactIndex < t.CasingMaxContacts && speed >= t.CasingMinSpeed;
}

float ImpactAudio::CasingGain(const ImpactAudioComponent& t, float speed) {
    const float k = std::clamp((speed - t.CasingMinSpeed) / std::max(t.CasingFullSpeed - t.CasingMinSpeed, 1e-3f), 0.0f, 1.0f);
    return t.CasingVolume * (t.CasingGainMin + (1.0f - t.CasingGainMin) * k);
}

std::string ImpactAudio::ResolveSurface(const std::string& surface, const std::vector<std::string>& available, const std::string& fallback) {
    if (std::find(available.begin(), available.end(), surface) != available.end()) return surface;
    return fallback;
}

float ImpactAudio::FlybyGain(const ImpactAudioComponent& t, float miss) {
    if (miss > t.FlybyRadius || t.FlybyRadius <= 1e-4f) return 0.0f;
    const float k = std::clamp(miss / t.FlybyRadius, 0.0f, 1.0f);
    return t.FlybyVolume * (1.0f + (t.FlybyFarGain - 1.0f) * k);
}

std::string ImpactAudio::SurfaceOf(const World& world, std::uint32_t entity) const {
    const auto e = static_cast<entt::entity>(entity);
    if (entity == 0xFFFFFFFFu || !world.Registry.valid(e)) return m_T.DefaultSurface;
    std::string name;
    if (const auto* c = world.Registry.try_get<ColliderComponent>(e)) name += c->Material + " ";
    if (const auto* t = world.Registry.try_get<TagComponent>(e)) name += t->Tag + " ";
    if (const auto* n = world.Registry.try_get<NameComponent>(e)) name += n->Name;
    return FoleyAudio::SurfaceFromName(m_T.SurfaceTable, name, m_T.DefaultSurface);
}

SoundSet* ImpactAudio::ImpactSet(const std::string& surface) {
    WeaponAudio& wa = WeaponAudio::Get();
    return wa.KeySet("snd.impact." + surface, [&](SoundSet& s) {
        s.MinDistance = m_T.ImpactMinDistance;
        s.MaxDistance = m_T.ImpactMaxDistance;
        s.MaxVoices = std::max(1, m_T.ImpactMaxVoices);
        s.StealFadeTime = 0.03f;
        s.PitchMin = 0.97f;
        s.PitchMax = 1.03f;
        s.VolumeJitterDb = 1.5f;
    });
}

SoundSet* ImpactAudio::CasingSet(bool shell, const std::string& surface) {
    WeaponAudio& wa = WeaponAudio::Get();
    return wa.KeySet(std::string("snd.casing.") + (shell ? "shell." : "rifle.") + surface, [&](SoundSet& s) {
        s.MinDistance = m_T.CasingMinDistance;
        s.MaxDistance = m_T.CasingMaxDistance;
        s.MaxVoices = std::max(1, m_T.CasingMaxVoices);
        s.StealFadeTime = 0.05f;
        s.PitchMin = 0.96f;
        s.PitchMax = 1.04f;
        s.VolumeJitterDb = 2.0f;
    });
}

bool ImpactAudio::CasingContact(const World& world, const glm::vec3& pos, float speed, std::uint32_t groundEntity, bool shell, int contactIndex) {
    if (!m_Active || !CasingContactAudible(m_T, contactIndex, speed)) return false;
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return false;
    SoundSet* set = CasingSet(shell, SurfaceOf(world, groundEntity));
    if (set->Files.empty()) set = CasingSet(shell, m_T.DefaultSurface); // a surface with no takes: the default's
    if (set->Files.empty()) return false;
    wa.PlayKeyed(*set, pos, false, CasingGain(m_T, speed));
    ++m_Played.Casings;
    return true;
}

bool ImpactAudio::Impact(const World& world, std::uint32_t entity, const glm::vec3& pos) {
    if (!m_Active || !m_T.Enabled || !m_T.ImpactsEnabled) return false;
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return false;
    const std::string surface = SurfaceOf(world, entity);
    SoundSet* set = ImpactSet(surface);
    std::string used = surface;
    if (set->Files.empty()) { // tile / carpet / ... have no impact takes: the default surface's
        used = m_T.DefaultSurface;
        set = ImpactSet(used);
    }
    if (set->Files.empty()) return false;
    const double now = wa.Player().Now();
    double& last = m_LastImpact.try_emplace(used, -1e9).first->second;
    if (now - last < m_T.ImpactMinInterval) return false;
    last = now;
    wa.PlayKeyed(*set, pos, false, m_T.ImpactVolume);
    ++m_Played.Impacts;
    return true;
}

bool ImpactAudio::Flesh(const glm::vec3& pos, bool at2D, float gain) {
    if (!m_Active || !m_T.Enabled || !m_T.FleshUsesRecordings) return false;
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return false;
    SoundSet* set = ImpactSet("flesh");
    if (set->Files.empty()) return false;
    wa.PlayKeyed(*set, pos, at2D, m_T.FleshVolume * gain);
    ++m_Played.Flesh;
    return true;
}

bool ImpactAudio::Flyby(const glm::vec3& point, float miss) {
    if (!m_Active || !m_T.Enabled || !m_T.FlybyEnabled) return false;
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return false;
    SoundSet* set = wa.KeySet("snd.flyby", [&](SoundSet& s) {
        s.MinDistance = m_T.FlybyMinDistance;
        s.MaxDistance = m_T.FlybyMaxDistance;
        s.MaxVoices = 4;
        s.StealFadeTime = 0.03f;
        s.PitchMin = 0.95f;
        s.PitchMax = 1.05f;
        s.VolumeJitterDb = 1.5f;
    });
    if (set->Files.empty()) return false;
    const float g = FlybyGain(m_T, miss);
    const double now = wa.Player().Now();
    if (g <= 0.0f) return false;
    if (now - m_LastFlyby < m_T.FlybyMinInterval) return true; // a burst reads as a few cracks: this one is swallowed, not replaced by the placeholder
    m_LastFlyby = now;
    wa.PlayKeyed(*set, point, false, g);
    ++m_Played.Flybys;
    return true;
}
