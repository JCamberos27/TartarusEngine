#include "WeaponReverb.h"

#include "Components.h"
#include "WeaponAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

void WeaponReverb::Reset() {
    m_ListenerProbe = EnvironmentProbe{};
    m_ReverbValid = false;
    m_ReverbSent = m_RemoteSent = AudioEngine::ReverbSpec{};
    m_ListenerMix = ReverbZoneMix{};
    m_Beds.clear();
    m_AmbienceDebug.clear();
    m_IrWarned = 0;
    m_RemoteHoldUntil = -1e9;
    m_RemoteActive = false;
    for (ClassIrEntry& e : m_ClassIr) e = ClassIrEntry{};
}

void WeaponReverb::PreloadIrs() {
    SoundBackend& be = m_Owner.Player().Backend();
    for (int c = 0; c < kSpaceClassCount; ++c)
        if (const std::string ir = ClassIr(c); !ir.empty()) be.PreloadIr(ir);
    for (const ReverbZoneVolume& z : m_Owner.Zones().Zones())
        if (!z.Reverb.Ir.empty()) be.PreloadIr(z.Reverb.Ir);
}

void WeaponReverb::Update(float dt, const glm::vec3& listener) {
    m_ListenerMix = m_Owner.Zones().Mix(listener); // (once a frame: the reverb below and the ambience read it)
    // The remote room's reverb (a voice heard through a portal brings its own room's): runs while such voices play, then rings out.
    if (m_Owner.Bus().Enabled) UpdateRemoteRoom();
    // The reverb follows the listener's space: told when it changes enough to matter (the DSP glides to what it is told).
    if (m_Owner.Bus().Enabled) UpdateListenerReverb(listener);
    UpdateAmbience(dt);
}

void WeaponReverb::UpdateRemoteRoom() {
    SoundPlayer& player = m_Owner.Player();
    if (player.PortalVoices() > 0) m_RemoteHoldUntil = player.Now() + (double)m_Owner.Bus().RemoteReverbHold;
    const bool active = player.Now() < m_RemoteHoldUntil;
    if (active) {
        AudioEngine::ReverbSpec rs;
        AudioEngine::ReverbLayerSpec layer;
        if (BuildLayer(player.RemoteRoom(), 1.0f, layer)) {
            rs.Count = 1;
            rs.Layers[0] = layer;
        }
        if (SpecDiffers(rs, m_RemoteSent)) {
            player.Backend().SetReverb(rs, 1);
            m_RemoteSent = rs;
        }
    }
    if (active != m_RemoteActive) {
        player.Backend().SetReverbBusActive(1, active);
        m_RemoteActive = active;
    }
}

void WeaponReverb::UpdateListenerReverb(const glm::vec3& listener) {
    SoundPlayer& player = m_Owner.Player();
    std::FILE* log = m_Owner.LogFile();
    const AudioEngine::ReverbSpec spec = ReverbAt(listener, m_ListenerMix);
    if (!m_ReverbValid || SpecDiffers(spec, m_ReverbSent)) {
        player.Backend().SetReverb(spec, 0);
        m_ReverbSent = spec;
        m_ReverbValid = true;
        if (log && player.Now() - m_LastReverbLog >= 0.1) {
            m_LastReverbLog = player.Now();
            std::fprintf(log, "R %.4f %d", player.Now(), spec.Count);
            for (int i = 0; i < spec.Count; ++i) std::fprintf(log, " %s %.3f %.2f %.1f", spec.Layers[i].Ir.c_str(), spec.Layers[i].Weight, spec.Layers[i].WetDb, spec.Layers[i].PreDelayMs);
            std::fprintf(log, "\n");
        }
    }
}

void WeaponReverb::CacheClassIrs(const SoundManifest& manifest) {
    for (int c = 0; c < kSpaceClassCount; ++c) {
        const SoundManifestEntry* e = manifest.FirstFor(std::string("ir.") + SpaceClassName((SpaceClass)c));
        m_ClassIr[c] = e ? ClassIrEntry{e->File, e->PreDelayMs} : ClassIrEntry{};
    }
}

std::string WeaponReverb::ClassIr(int tailClass) const { return m_ClassIr[std::clamp(tailClass, 0, kSpaceClassCount - 1)].File; }

float WeaponReverb::ClassPreDelayMs(int tailClass) const { return m_ClassIr[std::clamp(tailClass, 0, kSpaceClassCount - 1)].PreDelayMs; }

float WeaponReverb::ClassWetDb(int tailClass) const {
    float db = m_Owner.Bus().WetIndoorSmallDb;
    switch (tailClass) {
    case 0: db = m_Owner.Bus().WetOutdoorOpenDb; break;
    case 1: db = m_Owner.Bus().WetOutdoorUrbanDb; break;
    case 3: db = m_Owner.Bus().WetIndoorLargeDb; break;
    default: break;
    }
    return db + m_Owner.Bus().WetTrimDb + 20.0f * std::log10(std::max(m_Owner.Bus().WetScale, 1e-4f));
}

bool WeaponReverb::BuildLayer(const ReverbPreset& p, float weight, AudioEngine::ReverbLayerSpec& out) {
    const int cls = std::clamp(p.Class, 0, kSpaceClassCount - 1);
    std::string ir = p.Ir.empty() ? ClassIr(cls) : p.Ir;
    if (ir.empty()) { // no recorded impulse response for this space: the reverb is silently off for it (said once per class)
        if (!(m_IrWarned & (1u << cls))) {
            m_IrWarned |= 1u << cls;
            std::printf("[WeaponAudio] no impulse response for %s (manifest key ir.%s): the reverb is off there\n", SpaceClassName((SpaceClass)cls), SpaceClassName((SpaceClass)cls));
        }
        return false;
    }
    out.Ir = std::move(ir);
    out.Weight = weight;
    out.WetDb = ClassWetDb(cls) + p.WetDb;
    out.PreDelayMs = p.PreDelayMs >= 0.0f ? p.PreDelayMs : ClassPreDelayMs(cls);
    out.HfDampDb = p.HfDampDb;
    out.LowCutHz = p.LowCutHz >= 0.0f ? p.LowCutHz : m_Owner.Bus().LowCutHz;
    return true;
}

bool WeaponReverb::SpecDiffers(const AudioEngine::ReverbSpec& a, const AudioEngine::ReverbSpec& b) {
    if (a.Count != b.Count) return true;
    for (int i = 0; i < a.Count; ++i) {
        const AudioEngine::ReverbLayerSpec &x = a.Layers[i], &y = b.Layers[i];
        if (x.Ir != y.Ir || std::fabs(x.Weight - y.Weight) > 0.002f || std::fabs(x.WetDb - y.WetDb) > 0.05f || std::fabs(x.PreDelayMs - y.PreDelayMs) > 0.2f ||
            std::fabs(x.HfDampDb - y.HfDampDb) > 0.05f || std::fabs(x.LowCutHz - y.LowCutHz) > 0.5f)
            return true;
    }
    return false;
}

AudioEngine::ReverbSpec WeaponReverb::ReverbAt(const glm::vec3& pos) { return ReverbAt(pos, m_Owner.Zones().Mix(pos)); }

AudioEngine::ReverbSpec WeaponReverb::ReverbAt(const glm::vec3& pos, const ReverbZoneMix& z) {
    AudioEngine::ReverbLayerSpec cands[ReverbZoneMix::kMaxClaims + kSpaceClassCount];
    int n = 0;
    auto add = [&](const ReverbPreset& p, float w) {
        if (w <= 1e-4f) return;
        AudioEngine::ReverbLayerSpec L;
        if (!BuildLayer(p, w, L)) return;
        for (int i = 0; i < n; ++i) // the same impulse response twice (two zones of a class, a zone and the probe): one layer
            if (cands[i].Ir == L.Ir) {
                const float t = cands[i].Weight + L.Weight;
                const float a = cands[i].Weight / t, b = L.Weight / t;
                cands[i].WetDb = cands[i].WetDb * a + L.WetDb * b;
                cands[i].PreDelayMs = cands[i].PreDelayMs * a + L.PreDelayMs * b;
                cands[i].HfDampDb = cands[i].HfDampDb * a + L.HfDampDb * b;
                cands[i].LowCutHz = cands[i].LowCutHz * a + L.LowCutHz * b;
                cands[i].Weight = t;
                return;
            }
        cands[n++] = std::move(L);
    };
    for (int i = 0; i < z.ClaimCount; ++i) add(m_Owner.Zones().Zones()[(size_t)z.Claims[i].Zone].Reverb, z.Claims[i].Weight);
    if (z.ProbeShare > 1e-3f) {
        constexpr std::uint32_t kListenerProbe = 0xFFFFFFFDu;
        const EnvironmentSettings env = m_Owner.ReverbEnvironment();
        const EnvironmentReading r = m_ListenerProbe.Query(kListenerProbe, pos, m_Owner.Player().Now(), env);
        for (int i = 0; i < kSpaceClassCount; ++i) add(ReverbPresetFor(i), z.ProbeShare * r.Weights[i]);
    }
    // the two heaviest, weights renormalised; a second layer under 3 % is not worth a convolver
    AudioEngine::ReverbSpec spec;
    int first = -1, second = -1;
    for (int i = 0; i < n; ++i) {
        if (first < 0 || cands[i].Weight > cands[first].Weight) { second = first; first = i; }
        else if (second < 0 || cands[i].Weight > cands[second].Weight) second = i;
    }
    if (first < 0) return spec;
    float total = cands[first].Weight;
    if (second >= 0 && cands[second].Weight >= WeaponAudioProfile::kMinClassWeight * (total + cands[second].Weight)) total += cands[second].Weight;
    else second = -1;
    spec.Layers[spec.Count] = std::move(cands[first]);
    spec.Layers[spec.Count++].Weight /= total;
    if (second >= 0) {
        spec.Layers[spec.Count] = std::move(cands[second]);
        spec.Layers[spec.Count++].Weight /= total;
    }
    return spec;
}

// --- zone ambience beds -------------------------------------------------------------------------------------------------
// Each Reverb Zone with an Ambience key adds a looping 2D bed. A bed's level is sqrt(the weight the listener's zones with that key
// claim) x the zones' Ambience Volume (equal-power, so two zones crossfading do not dip), and it moves by at most 1 per second:
// a hard zone edge or a teleport is a 1 s fade, never a step. A bed starts when it becomes audible and stops once faded out.

void WeaponReverb::StopAmbience() {
    for (Bed& b : m_Beds)
        if (b.Handle != AudioEngine::InvalidHandle) m_Owner.Player().Backend().Stop(b.Handle);
    m_Beds.clear();
    m_AmbienceDebug.clear();
}

void WeaponReverb::UpdateAmbience(float dt) {
    const std::string* keys[ReverbZoneMix::kMaxClaims];
    float take[ReverbZoneMix::kMaxClaims], vol[ReverbZoneMix::kMaxClaims];
    int n = 0;
    for (int i = 0; i < m_ListenerMix.ClaimCount; ++i) {
        const ReverbZoneVolume& z = m_Owner.Zones().Zones()[(size_t)m_ListenerMix.Claims[i].Zone];
        if (z.Ambience.empty()) continue;
        int k = 0;
        while (k < n && *keys[k] != z.Ambience) ++k;
        if (k == n) {
            keys[n] = &z.Ambience;
            take[n] = vol[n] = 0.0f;
            ++n;
        }
        take[k] += m_ListenerMix.Claims[i].Weight;
        vol[k] += m_ListenerMix.Claims[i].Weight * z.AmbienceVolume;
    }
    for (Bed& b : m_Beds) b.Target = 0.0f;
    for (int k = 0; k < n; ++k) {
        Bed* bed = nullptr;
        for (Bed& b : m_Beds)
            if (b.Key == *keys[k]) bed = &b;
        if (!bed) {
            m_Beds.emplace_back();
            m_Beds.back().Key = *keys[k];
            bed = &m_Beds.back();
        }
        bed->Target = take[k] > 1e-5f ? std::sqrt(take[k]) * (vol[k] / take[k]) : 0.0f;
    }
    m_AmbienceDebug.clear();
    for (Bed& b : m_Beds) {
        const float step = std::max(dt, 0.0f) / 1.0f;
        b.Level += std::clamp(b.Target - b.Level, -step, step);
        if (b.Level < 1e-4f && b.Target <= 0.0f) {
            b.Level = 0.0f;
            if (b.Handle != AudioEngine::InvalidHandle) {
                m_Owner.Player().Backend().Stop(b.Handle);
                b.Handle = AudioEngine::InvalidHandle;
            }
        } else if (b.Handle == AudioEngine::InvalidHandle || !m_Owner.Player().Backend().IsPlaying(b.Handle)) {
            b.Handle = AudioEngine::InvalidHandle;
            SoundSet* set = m_Owner.KeySet(b.Key, [](SoundSet& s) {
                s.Loop = true;
                s.Bus = AudioEngine::Bus::Ambient;
                s.MaxVoices = 2;
                s.StealFadeTime = 0.0f;
                s.ReverbSend = 0.0f;  // the bed already holds its room
                s.Occlusion = 0;
            });
            if (!set->Files.empty() && b.Level > 1e-4f) {
                SoundPlayer::Request r;
                r.At2D = true;
                r.Gain = b.Level;
                const SoundPlayer::Played pl = m_Owner.Player().Play(*set, r);
                if (pl.Started) {
                    b.Handle = pl.Handle;
                    b.BaseVolume = pl.Volume / b.Level;
                }
            }
        } else {
            m_Owner.Player().Backend().SetVolume(b.Handle, b.BaseVolume * b.Level);
        }
        AmbienceDebug d;
        d.Key = b.Key;
        d.Target = b.Target;
        d.Level = b.Level;
        d.Playing = b.Handle != AudioEngine::InvalidHandle;
        m_AmbienceDebug.push_back(d);
    }
}
