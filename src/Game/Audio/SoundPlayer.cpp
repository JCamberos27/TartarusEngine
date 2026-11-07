#include "SoundPlayer.h"

#include "Core/Paths.h"

#include <algorithm>
#include <cmath>
#include <iterator>

// --- backend -----------------------------------------------------------------------------------

namespace {
struct EngineBackend : SoundBackend {
    AudioEngine::SoundHandle Start(const SoundVoice& v) override {
        if (!AudioEngine::IsInitialized()) return AudioEngine::InvalidHandle;
        AudioEngine::VoiceFx fx;
        fx.ReverbSend = v.ReverbSend;
        fx.Occlusion = v.Occlusion && v.Spatial;
        fx.Cutoff = v.OcclusionHz;
        fx.Gain = v.PortalGain;
        fx.ReverbBus = v.ReverbBus;
        fx.Pitch = v.Pitch;
        fx.Duck = v.Duck;
        fx.Spatial = v.Spatial;
        fx.Position = v.Position;
        fx.RolloffMode = v.Rolloff;
        fx.MinDistance = v.MinDistance;
        fx.MaxDistance = v.MaxDistance;
        fx.RolloffFactor = v.RolloffFactor;
        return AudioEngine::Play(ProjectPaths::Resolve(v.File), std::clamp(v.Volume, 0.0f, 8.0f), v.Loop, v.Bus, v.StartOffset, &fx);
    }
    void Stop(AudioEngine::SoundHandle h) override { AudioEngine::Stop(h); }
    void SetVolume(AudioEngine::SoundHandle h, float volume) override { AudioEngine::SetVolume(h, volume); }
    void SetDuck(AudioEngine::SoundHandle h, float gain, float seconds) override { AudioEngine::SetDuck(h, gain, seconds); }
    bool IsPlaying(AudioEngine::SoundHandle h) override { return AudioEngine::IsPlaying(h); }
    void SetOcclusion(AudioEngine::SoundHandle h, float cutoffHz, float gain) override { AudioEngine::SetOcclusion(h, cutoffHz, gain); }
    void SetVoicePosition(AudioEngine::SoundHandle h, const glm::vec3& p) override { AudioEngine::SetPosition(h, p); }
    void SetReverbSendBus(AudioEngine::SoundHandle h, int bus) override { AudioEngine::SetReverbSendBus(h, bus); }
    void SetReverb(const AudioEngine::ReverbSpec& spec, int bus) override {
        AudioEngine::ReverbSpec s = spec;
        for (int i = 0; i < s.Count && i < 2; ++i) s.Layers[i].Ir = ProjectPaths::Resolve(s.Layers[i].Ir);
        AudioEngine::SetReverb(s, bus);
    }
    void PreloadIr(const std::string& file) override {
        if (AudioEngine::IsInitialized()) AudioEngine::PreloadIr(ProjectPaths::Resolve(file));
    }
    void ConfigureLimiter(const LimiterSettings& l) override { AudioEngine::SetMasterLimiter(l); }
    void SetReverbBusActive(int bus, bool active) override { AudioEngine::SetReverbEnabled(active, bus); }
    void ConfigureReverb(bool enabled, float returnLevel, float glideSeconds) override {
        AudioEngine::SetReverbEnabled(enabled);
        AudioEngine::SetReverbReturn(returnLevel);
        AudioEngine::SetReverbGlide(glideSeconds);
    }
    void Preload(const std::string& file) override {
        if (AudioEngine::IsInitialized()) AudioEngine::Load(ProjectPaths::Resolve(file));
    }
};
} // namespace

SoundBackend& SoundBackend::Engine() {
    static EngineBackend b;
    return b;
}

// --- SoundPlayer -------------------------------------------------------------------------------

float SoundPlayer::Rand01() {
    m_Rng ^= m_Rng << 13;
    m_Rng ^= m_Rng >> 17;
    m_Rng ^= m_Rng << 5;
    return (float)(m_Rng & 0xFFFFFFu) / (float)0x1000000u;
}

void SoundPlayer::Reap(Pool& p) {
    SoundBackend& be = Backend();
    p.Voices.erase(std::remove_if(p.Voices.begin(), p.Voices.end(),
                                  [&](const Voice& v) { return v.Handle == AudioEngine::InvalidHandle || !be.IsPlaying(v.Handle); }),
                   p.Voices.end());
}

int SoundPlayer::Voices(const std::string& key) const {
    const auto it = m_Pools.find(key);
    if (it == m_Pools.end()) return 0;
    int n = 0;
    for (const Voice& v : it->second.Voices)
        if (v.FadeLeft < 0.0f) ++n;
    return n;
}

int SoundPlayer::AudibleVoices(const std::string& key) const {
    const auto it = m_Pools.find(key);
    return it == m_Pools.end() ? 0 : (int)it->second.Voices.size();
}

SoundPlayer::Played SoundPlayer::Play(const SoundSet& set, const Request& req) {
    Played out;
    Pool& pool = m_Pools[set.Key];
    Reap(pool);
    if (set.Files.empty()) {
        out.Voices = Voices(set.Key);
        if (m_Log) m_Log(m_Now, set.Key, std::string(), out.Voices, req, 0.0f, 1.0f, nullptr);
        return out;
    }
    // Round robin: a random variant, never the one that just played.
    const int n = (int)set.Files.size();
    int idx = 0;
    if (n > 1) {
        if (set.NoImmediateRepeat && pool.LastIndex >= 0 && pool.LastIndex < n) {
            idx = (int)(Rand01() * (float)(n - 1)) % (n - 1);
            if (idx >= pool.LastIndex) ++idx;
        } else {
            idx = std::min(n - 1, (int)(Rand01() * (float)n));
        }
    }
    pool.LastIndex = idx;
    float seek = 0.0f;
    if (req.LeadMs >= 0.0f) {
        // This variant's own lead-in decides: start it (LeadMs - anchor) from now, or skip into it when the frame is nearer.
        const float delay = (req.LeadMs - set.AnchorMs((size_t)idx)) * 0.001f;
        if (delay > 1e-3f) {
            m_Pending.push_back({set, idx, req, m_Now + (double)delay});
            out.File = set.Files[idx];
            out.Pending = true;
            out.Voices = Voices(set.Key);
            return out;
        }
        seek = std::max(0.0f, -delay);
    }
    return Start(set, idx, req, seek);
}

SoundPlayer::Played SoundPlayer::Start(const SoundSet& set, int idx, const Request& req, float seek) {
    Played out;
    Pool& pool = m_Pools[set.Key];
    Reap(pool);
    // At the cap the oldest live voice goes (faded out over StealFadeTime, or cut).
    if (set.MaxVoices > 0) {
        while (Voices(set.Key) >= set.MaxVoices) {
            Voice* oldest = nullptr;
            for (Voice& v : pool.Voices)
                if (v.FadeLeft < 0.0f && (!oldest || v.Started < oldest->Started)) oldest = &v;
            if (!oldest) break;
            if (set.StealFadeTime > 1e-4f) {
                oldest->FadeLeft = oldest->FadeTotal = set.StealFadeTime;
            } else {
                Backend().Stop(oldest->Handle);
                oldest->Handle = AudioEngine::InvalidHandle;
                Reap(pool);
            }
        }
    }
    const float jitterDb = req.Override ? req.OverrideJitterDb : set.VolumeJitterDb;
    const float pitchMin = req.Override ? req.OverridePitchMin : set.PitchMin, pitchMax = req.Override ? req.OverridePitchMax : set.PitchMax;
    const float jitter = jitterDb > 0.0f ? std::pow(10.0f, (Rand01() * 2.0f - 1.0f) * jitterDb / 20.0f) : 1.0f;
    SoundVoice v;
    v.File = set.Files[idx];
    v.Volume = set.Volume * req.Gain * jitter * set.GainLin((size_t)idx);
    v.Pitch = (pitchMin + (pitchMax - pitchMin) * Rand01()) * req.PitchScale;
    v.Loop = set.Loop;
    v.Bus = set.Bus;
    v.Spatial = !req.At2D;
    v.Position = req.Position;
    v.MinDistance = std::max(req.OverrideDistance ? req.OverrideMinDistance : set.MinDistance, 0.01f); // (Max >= Min > 0: the clamp and the duck's ratio below need it)
    v.MaxDistance = std::max(req.OverrideDistance ? req.OverrideMaxDistance : set.MaxDistance, v.MinDistance);
    v.Rolloff = set.Rolloff;
    if (v.Spatial && set.Distance.Valid()) { // the mix spec's distance model: the level holds at its reference distance, capped close up
        v.Rolloff = AudioEngine::Rolloff::Exponential;
        v.RolloffFactor = set.Distance.Factor();
        v.MinDistance = set.Distance.MinDistance();
        v.MaxDistance = std::max(set.Distance.MaxM, v.MinDistance + 0.01f);
        v.Volume *= std::pow(10.0f, set.Distance.StartGainDb() / 20.0f);
    }
    v.Group = MixGroupForKey(set.Key, req.At2D);
    v.Duck = m_Ducker ? m_Ducker->Gain(v.Group) : 1.0f;
    v.StartOffset = seek;
    // The reverb send and the occlusion low-pass (a set's own value, else its category's).
    Routing rt;
    if (m_Routing) rt = m_Routing(set.Key);
    v.ReverbSend = set.ReverbSend >= 0.0f ? set.ReverbSend : rt.Send;
    const bool occ = v.Spatial && m_Occ.Enabled && !set.Loop && (set.Occlusion < 0 ? rt.Occlusion : set.Occlusion > 0);
    v.Occlusion = occ || (v.Spatial && m_Occ.AirEnabled); // the low-pass carries the air absorption too
    float occAmount = 0.0f;
    VoicePath vp;
    if (occ && m_Path) {
        vp = m_Path(m_ListenerPos, req.Position);
    }
    const float occDistance = glm::length(req.Position - m_ListenerPos);
    if (vp.ViaPortal) { // another room: heard from the portal, through its loss, with the source room's reverb
        v.Position = vp.Virtual;
        v.PortalGain = vp.Gain;
        v.ReverbBus = 1;
    } else if (occ && m_Blocked && occDistance >= m_Occ.MinDistance && m_Blocked(m_ListenerPos, req.Position)) {
        occAmount = 1.0f; // behind something already: it starts muffled
    }
    v.OcclusionHz = vp.ViaPortal ? std::min(vp.CutoffHz, 20000.0f) : OcclusionCutoff(m_Occ, occAmount);
    if (v.Spatial) v.OcclusionHz = std::min(v.OcclusionHz, AirCutoff(m_Occ, glm::length(v.Position - m_ListenerPos)));
    out.File = v.File;
    out.Volume = v.Volume;
    out.Pitch = v.Pitch;
    out.Handle = Backend().Start(v);
    out.Started = out.Handle != AudioEngine::InvalidHandle;
    // The duck's key: a gunshot or a round cracking past, as loud as it is where the listener is.
    if (out.Started && m_Ducker && m_HasRef && (v.Group == MixGroup::Weapon || v.Group == MixGroup::Threat) && (size_t)idx < set.FileLufs.size() &&
        set.FileLufs[(size_t)idx] > -150.0f) {
        float db = set.FileLufs[(size_t)idx] - m_RefLufs + 20.0f * std::log10(std::max(v.Volume * v.PortalGain, 1e-6f));
        if (v.Spatial) {
            const float d = std::max(glm::length(v.Position - m_ListenerPos), 0.01f);
            if (v.Rolloff == AudioEngine::Rolloff::Exponential) db -= 6.0206f * v.RolloffFactor * std::log2(std::clamp(d, v.MinDistance, v.MaxDistance) / v.MinDistance);
            else if (v.Rolloff == AudioEngine::Rolloff::Logarithmic) db -= 20.0f * std::log10(std::clamp(d, v.MinDistance, v.MaxDistance) / v.MinDistance);
        }
        m_Ducker->Key(db);
    }
    if (out.Started && v.Occlusion) {
        Tracked t;
        t.Handle = out.Handle;
        t.Pos = req.Position;
        t.Amount = t.Target = occAmount;
        t.LastCutoff = v.OcclusionHz;
        t.CurPos = t.SentPos = vp.ViaPortal ? vp.Virtual : req.Position;
        t.TargetPos = t.CurPos;
        if (vp.ViaPortal) {
            t.Portal = true;
            t.Gain = t.TargetGain = t.SentGain = vp.Gain;
            t.PortalCut = t.TargetPortalCut = vp.CutoffHz;
            m_RemoteRoom = vp.Remote;
        }
        t.NextCheck = m_Now + (double)m_Occ.Interval;
        t.MinDist = m_Occ.MinDistance;
        t.Raycast = occ;
        m_Occluded.push_back(t);
    }
    if (out.Started) {
        Voice vo;
        vo.Handle = out.Handle;
        vo.Started = m_Now;
        vo.Volume = v.Volume;
        vo.Group = v.Group;
        vo.SentDuck = v.Duck;
        pool.Voices.push_back(vo);
    }
    out.Voices = Voices(set.Key);
    if (m_Log) m_Log(m_Now, set.Key, out.File, out.Voices, req, out.Volume, out.Pitch, &v);
    return out;
}

void SoundPlayer::Update(float dt) {
    m_Now += dt;
    SoundBackend& be = Backend();
    // Delayed starts (animation events that fire ahead of their contact) that have come due; a start that lands a frame late
    // skips that much of the file so the contact is still on time.
    for (size_t i = 0; i < m_Pending.size();) {
        if (m_Pending[i].Due <= m_Now) {
            Pending p = std::move(m_Pending[i]);
            m_Pending.erase(m_Pending.begin() + (std::ptrdiff_t)i);
            Start(p.Set, p.Index, p.Req, (float)(m_Now - p.Due));
        } else {
            ++i;
        }
    }
    UpdateOcclusion(dt);
    for (auto& [key, pool] : m_Pools) {
        for (Voice& v : pool.Voices) {
            // The mix: each voice follows its group's duck gain (going down over the duck's attack, up over the frame it moved in).
            const float duck = m_Ducker ? m_Ducker->Gain(v.Group) : 1.0f;
            if (v.Handle != AudioEngine::InvalidHandle && std::fabs(duck - v.SentDuck) > 0.005f * std::max(duck, v.SentDuck)) {
                be.SetDuck(v.Handle, duck, duck < v.SentDuck ? std::max(m_Ducker->AttackSeconds(), 0.005f) : std::max(dt, 0.005f));
                v.SentDuck = duck;
            }
            if (v.FadeLeft < 0.0f) continue;
            v.FadeLeft -= dt;
            if (v.FadeLeft <= 0.0f) {
                be.Stop(v.Handle);
                v.Handle = AudioEngine::InvalidHandle;
            } else {
                be.SetVolume(v.Handle, v.Volume * (v.FadeLeft / std::max(v.FadeTotal, 1e-4f)));
            }
        }
        Reap(pool);
    }
}

float SoundPlayer::AirCutoff(const OcclusionSettings& s, float distance) {
    if (!s.AirEnabled || s.AirStartDistance <= 0.0f || distance <= s.AirStartDistance) return 20000.0f;
    return std::max(20000.0f * std::pow(s.AirStartDistance / distance, s.AirExponent), s.AirMinHz);
}

void SoundPlayer::GroupVoices(int (&out)[(int)MixGroup::Count]) const {
    for (int& n : out) n = 0;
    for (const auto& [key, pool] : m_Pools)
        for (const Voice& v : pool.Voices)
            if (v.FadeLeft < 0.0f) ++out[std::clamp((int)v.Group, 0, (int)MixGroup::Count - 1)];
}

float SoundPlayer::OcclusionCutoff(const OcclusionSettings& s, float amount) {
    const float a = std::clamp(amount, 0.0f, 1.0f);
    if (a <= 0.0f) return 20000.0f;
    return 20000.0f * std::pow(std::max(s.CutoffHz, 80.0f) / 20000.0f, a);
}

void SoundPlayer::UpdateOcclusion(float dt) {
    if (m_Occluded.empty()) return;
    SoundBackend& be = Backend();
    m_Occluded.erase(std::remove_if(m_Occluded.begin(), m_Occluded.end(), [&](const Tracked& t) { return !be.IsPlaying(t.Handle); }), m_Occluded.end());
    int budget = m_Occ.RaysPerFrame;
    const size_t n = m_Occluded.size();
    m_PortalVoices = 0;
    for (size_t k = 0; k < n; ++k) { // a rolling start, so a busy frame does not always spend the budget on the same voices
        Tracked& t = m_Occluded[(m_OccCursor + k) % n];
        if (t.Raycast && m_Now >= t.NextCheck && (m_Path || (budget > 0 && m_Blocked))) {
            VoicePath vp;
            const bool pathDue = m_Path && m_Now >= t.NextPath;
            if (pathDue) {
                vp = m_Path(m_ListenerPos, t.Pos);
                t.NextPath = m_Now + (double)m_Occ.Interval;
            } else if (t.Portal) {
                vp.ViaPortal = true; // (between searches a portal voice keeps its last way round)
                vp.Virtual = t.TargetPos;
                vp.Gain = t.TargetGain;
                vp.CutoffHz = t.TargetPortalCut;
                vp.Remote = m_RemoteRoom;
            }
            if (vp.ViaPortal) {
                if (!t.Portal) {
                    t.Portal = true;
                    be.SetReverbSendBus(t.Handle, 1);
                }
                t.NextCheck = m_Now + (double)m_Occ.Interval;
                t.TargetPos = vp.Virtual;
                t.TargetGain = vp.Gain;
                t.TargetPortalCut = vp.CutoffHz;
                t.Target = 0.0f; // the direct line is not the way it comes: no occlusion on top of the portal's own
                m_RemoteRoom = vp.Remote;
            } else {
                if (t.Portal) {
                    t.Portal = false;
                    be.SetReverbSendBus(t.Handle, 0);
                    t.TargetPos = t.Pos;
                    t.TargetGain = 1.0f;
                    t.TargetPortalCut = 20000.0f;
                }
                if (budget > 0 && m_Blocked) {
                    --budget;
                    t.NextCheck = m_Now + (double)m_Occ.Interval;
                    t.Target = glm::length(t.Pos - m_ListenerPos) >= t.MinDist && m_Blocked(m_ListenerPos, t.Pos) ? 1.0f : 0.0f;
                } else if (!m_Blocked) {
                    t.NextCheck = m_Now + (double)m_Occ.Interval;
                }
            }
        }
        const float before = t.Amount;
        const float a = std::min(1.0f, m_Occ.GlideRate * dt);
        t.Amount += (t.Target - t.Amount) * a;
        if (std::fabs(t.Target - t.Amount) < 0.005f) t.Amount = t.Target;
        // The portal's state glides too (position, gain, cutoff in the log domain): a door swinging or a step through a doorway is not a click.
        t.CurPos += (t.TargetPos - t.CurPos) * a;
        t.Gain += (t.TargetGain - t.Gain) * a;
        t.PortalCut = std::exp(std::log(t.PortalCut) + (std::log(t.TargetPortalCut) - std::log(t.PortalCut)) * a);
        if (std::fabs(t.TargetGain - t.Gain) < 0.002f) t.Gain = t.TargetGain;
        if (std::fabs(std::log(t.TargetPortalCut / t.PortalCut)) < 0.01f) t.PortalCut = t.TargetPortalCut;
        if (t.Portal || glm::dot(t.CurPos - t.SentPos, t.CurPos - t.SentPos) > 1e-6f) {
            if (glm::dot(t.CurPos - t.SentPos, t.CurPos - t.SentPos) > 1e-6f) {
                be.SetVoicePosition(t.Handle, t.CurPos);
                t.SentPos = t.CurPos;
            }
        }
        if (t.Portal) ++m_PortalVoices;
        const float hz = std::min({OcclusionCutoff(m_Occ, t.Amount), std::min(t.PortalCut, 20000.0f), AirCutoff(m_Occ, glm::length(t.CurPos - m_ListenerPos))});
        if (t.Amount != before || std::fabs(std::log(hz / t.LastCutoff)) > 0.02f || std::fabs(t.Gain - t.SentGain) > 0.01f) {
            if (std::fabs(std::log(hz / t.LastCutoff)) > 0.02f || std::fabs(t.Gain - t.SentGain) > 0.01f || hz >= 20000.0f != (t.LastCutoff >= 20000.0f)) {
                be.SetOcclusion(t.Handle, hz, t.Gain);
                t.LastCutoff = hz;
                t.SentGain = t.Gain;
            }
        }
    }
    m_OccCursor = (m_OccCursor + 1) % std::max<size_t>(m_Occluded.size(), 1);
}

void SoundPlayer::StopAll() {
    SoundBackend& be = Backend();
    for (auto& [key, pool] : m_Pools)
        for (Voice& v : pool.Voices) be.Stop(v.Handle);
    m_Pools.clear();
    m_Pending.clear();
    m_Occluded.clear();
}
