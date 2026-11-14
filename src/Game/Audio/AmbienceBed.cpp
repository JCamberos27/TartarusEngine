#include "AmbienceBed.h"

#include "ReverbZones.h"
#include "SoundManifest.h"
#include "SoundPlayer.h"

#include <algorithm>
#include <cmath>

glm::vec3 AmbienceBed::SpeakerOffset(int i, float radius, float height) {
    static const glm::vec2 kDir[kSpeakers] = {{0.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {-1.0f, 0.0f}}; // (x, z): N E S W
    const glm::vec2 d = kDir[((i % kSpeakers) + kSpeakers) % kSpeakers] * radius;
    return {d.x, height, d.y};
}

float AmbienceBed::IndoorShare(const ReverbZoneMix& mix, const ReverbZones& zones) {
    float share = 0.0f;
    for (int i = 0; i < mix.ClaimCount; ++i) {
        const int z = mix.Claims[i].Zone;
        if (z < 0 || (size_t)z >= zones.Zones().size()) continue;
        const SpaceClass c = zones.Zones()[(size_t)z].Class;
        if (c == SpaceClass::IndoorSmall || c == SpaceClass::IndoorLarge) share += mix.Claims[i].Weight;
    }
    return std::clamp(share, 0.0f, 1.0f);
}

float AmbienceBed::IndoorGain(const AmbienceBedComponent& c, float share) {
    return std::pow(10.0f, std::clamp(share, 0.0f, 1.0f) * std::min(c.IndoorGainDb, 0.0f) / 20.0f);
}

float AmbienceBed::IndoorCutoff(const AmbienceBedComponent& c, float share) {
    const float lo = std::clamp(c.IndoorCutoffHz, 20.0f, 20000.0f);
    return 20000.0f * std::pow(lo / 20000.0f, std::clamp(share, 0.0f, 1.0f));
}

void AmbienceBed::Start(const AmbienceBedComponent& c) {
    m_C = c;
    m_On = c.Enabled && !c.Sound.empty();
    m_Level = c.FadeIn > 0.0f ? 0.0f : 1.0f;
    m_SentCutoff = 20000.0f;
    for (int i = 0; i < kSpeakers; ++i) {
        m_Handle[i] = AudioEngine::InvalidHandle;
        m_Base[i] = 0.0f;
    }
}

void AmbienceBed::Update(float dt, const glm::vec3& listener, float indoorShare, SoundPlayer& player, SoundSet* set) {
    if (!m_On || !set || set->Files.empty()) return;
    set->Distance = DistanceModel{}; // the speakers keep one distance: the manifest's distance model must not roll them off
    SoundBackend& be = player.Backend();
    m_Level = m_C.FadeIn > 0.0f ? std::min(1.0f, m_Level + std::max(dt, 0.0f) / m_C.FadeIn) : 1.0f;
    const float gain = m_Level * IndoorGain(m_C, indoorShare);
    const float cutoff = IndoorCutoff(m_C, indoorShare);
    const bool sendCut = std::fabs(std::log(cutoff / m_SentCutoff)) > 0.02f || (cutoff >= 19999.0f) != (m_SentCutoff >= 19999.0f);
    for (int i = 0; i < kSpeakers; ++i) {
        const glm::vec3 pos = listener + SpeakerOffset(i, m_C.Radius, m_C.Height);
        if (m_Handle[i] == AudioEngine::InvalidHandle || !be.IsPlaying(m_Handle[i])) {
            SoundPlayer::Request r;
            r.At2D = false;
            r.Position = pos;
            r.Gain = std::max(m_C.Volume, 0.0f);
            r.Variant = i;
            r.ManualFilter = true;
            r.OverrideDistance = true;          // the speakers sit at a fixed distance: no rolloff
            r.OverrideMinDistance = m_C.Radius * 2.0f + 1.0f;
            r.OverrideMaxDistance = r.OverrideMinDistance + 1.0f;
            const SoundPlayer::Played p = player.Play(*set, r);
            m_Handle[i] = p.Started ? p.Handle : AudioEngine::InvalidHandle;
            m_Base[i] = p.Volume;
            if (!p.Started) continue;
            be.SetOcclusion(m_Handle[i], cutoff, 1.0f);
        }
        be.SetVoicePosition(m_Handle[i], pos);
        be.SetVolume(m_Handle[i], m_Base[i] * gain);
        if (sendCut) be.SetOcclusion(m_Handle[i], cutoff, 1.0f);
    }
    if (sendCut) m_SentCutoff = cutoff;
}

void AmbienceBed::Stop(SoundPlayer& player) {
    for (int i = 0; i < kSpeakers; ++i) {
        if (m_Handle[i] != AudioEngine::InvalidHandle) player.Backend().Stop(m_Handle[i]);
        m_Handle[i] = AudioEngine::InvalidHandle;
    }
    m_On = false;
}
