#include "WeaponAudio.h"
#include "../Scripting/GameFrames.h"
#include "../Scripting/ScriptRuntime.h"
#include "../Scripting/NpcDefinitions.h"
#include <stdexcept>

#include "Components.h"
#include "GameModuleAPI.h"
#include "PhysicsWorld.h"
#include "ProjectPaths.h"
#include "World.h"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace {

constexpr size_t kMaxHistory = 4096;

float Num(const json& j, const char* key, float fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<float>() : fallback;
}
bool Flag(const json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
std::string Str(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

const char* BusName(AudioEngine::Bus b) {
    switch (b) {
    case AudioEngine::Bus::Music: return "music";
    case AudioEngine::Bus::Ambient: return "ambient";
    case AudioEngine::Bus::UI: return "ui";
    case AudioEngine::Bus::Voice: return "voice";
    default: return "sfx";
    }
}
AudioEngine::Bus BusFrom(const std::string& s, AudioEngine::Bus fallback) {
    if (s == "sfx") return AudioEngine::Bus::SFX;
    if (s == "music") return AudioEngine::Bus::Music;
    if (s == "ambient") return AudioEngine::Bus::Ambient;
    if (s == "ui") return AudioEngine::Bus::UI;
    if (s == "voice") return AudioEngine::Bus::Voice;
    return fallback;
}

SoundSet ApplySetJson(const json& j, SoundSet s) {
    if (const auto it = j.find("files"); it != j.end() && it->is_array()) {
        s.Files.clear();
        s.FileAnchorMs.clear(); // (the per-file values belonged to the files this replaces)
        s.FileGainDb.clear();
        s.FilePeakDb.clear();
        for (const auto& f : *it)
            if (f.is_string()) s.Files.push_back(f.get<std::string>());
        s.FilesExplicit = !s.Files.empty();
    }
    if (const auto it = j.find("mixDb"); it != j.end() && it->is_number()) {
        s.HasMixDb = true;
        s.MixDb = it->get<float>();
    }
    s.Volume = Num(j, "volume", s.Volume);
    s.VolumeJitterDb = Num(j, "volumeJitterDb", s.VolumeJitterDb);
    s.PitchMin = Num(j, "pitchMin", s.PitchMin);
    s.PitchMax = Num(j, "pitchMax", s.PitchMax);
    s.Bus = BusFrom(Str(j, "bus"), s.Bus);
    s.MinDistance = Num(j, "minDistance", s.MinDistance);
    s.MaxDistance = Num(j, "maxDistance", s.MaxDistance);
    if (const std::string r = Str(j, "rolloff"); !r.empty())
        s.Rolloff = r == "linear" ? AudioEngine::Rolloff::Linear : AudioEngine::Rolloff::Logarithmic;
    s.NoImmediateRepeat = Flag(j, "noImmediateRepeat", s.NoImmediateRepeat);
    s.MaxVoices = (int)Num(j, "maxVoices", (float)s.MaxVoices);
    s.StealFadeTime = Num(j, "stealFadeTime", s.StealFadeTime);
    s.Loop = Flag(j, "loop", s.Loop);
    s.ReverbSend = Num(j, "reverbSend", s.ReverbSend);
    if (const auto it = j.find("occlusion"); it != j.end() && it->is_boolean()) s.Occlusion = it->get<bool>() ? 1 : 0;
    return s;
}

json SetToJson(const SoundSet& s) {
    json j;
    j["files"] = s.Files;
    j["volume"] = s.Volume;
    j["volumeJitterDb"] = s.VolumeJitterDb;
    j["pitchMin"] = s.PitchMin;
    j["pitchMax"] = s.PitchMax;
    j["bus"] = BusName(s.Bus);
    j["minDistance"] = s.MinDistance;
    j["maxDistance"] = s.MaxDistance;
    j["rolloff"] = s.Rolloff == AudioEngine::Rolloff::Linear ? "linear" : "log";
    j["noImmediateRepeat"] = s.NoImmediateRepeat;
    j["maxVoices"] = s.MaxVoices;
    j["stealFadeTime"] = s.StealFadeTime;
    j["loop"] = s.Loop;
    j["reverbSend"] = s.ReverbSend;
    if (s.HasMixDb) j["mixDb"] = s.MixDb;
    if (s.Occlusion >= 0) j["occlusion"] = s.Occlusion > 0;
    return j;
}

// "assets/..." as written, or a name inside assets/Audio.
std::string ManifestPath(const std::string& file) {
    if (file.rfind("assets/", 0) == 0 || file.rfind("assets\\", 0) == 0) return file;
    return "assets/Audio/" + file;
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

} // namespace

// --- SoundSet / BlendCurve ---------------------------------------------------------------------

SoundSet SoundSet::FromJson(const std::string& key, const std::string& text, const SoundSet& base) {
    SoundSet s = base;
    s.Key = key;
    const json j = json::parse(text, nullptr, false);
    if (j.is_object()) s = ApplySetJson(j, s);
    s.Key = key;
    return s;
}

std::string SoundSet::ToJson() const { return SetToJson(*this).dump(2); }

float SoundSet::GainLin(size_t i) const {
    if (HasMixDb) return std::pow(10.0f, MixDb / 20.0f);
    return i < FileGainDb.size() ? std::pow(10.0f, FileGainDb[i] / 20.0f) : 1.0f;
}
float SoundSet::PeakLin(size_t i) const { return i < FilePeakDb.size() ? std::pow(10.0f, FilePeakDb[i] / 20.0f) : 0.7f; }

float BlendCurve::Weight(float distance) const {
    const float span = FarDistance - NearDistance;
    const float t = span > 1e-4f ? std::clamp((distance - NearDistance) / span, 0.0f, 1.0f) : (distance >= FarDistance ? 1.0f : 0.0f);
    return NearWeight + (FarWeight - NearWeight) * t;
}

// --- manifest ----------------------------------------------------------------------------------

bool SoundManifest::FromJson(const std::string& text, SoundManifest& out, std::string* error) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        if (error) *error = "audio manifest isn't valid json";
        return false;
    }
    out.Entries.clear();
    out.Distance.clear();
    out.ShotOverReferenceDb = 0.0f;
    if (j.is_object())
        if (const auto mix = j.find("mix"); mix != j.end() && mix->is_object()) {
            if (const auto d = mix->find("distance"); d != mix->end() && d->is_object())
                for (auto r = d->begin(); r != d->end(); ++r) {
                    if (!r.value().is_object()) continue;
                    DistanceModel m;
                    m.RefM = Num(r.value(), "ref_m", 0.0f);
                    m.SlopeDb = Num(r.value(), "slope_db", 6.0f);
                    m.NearDb = Num(r.value(), "near_db", 0.0f);
                    m.MaxM = Num(r.value(), "max_m", 40.0f);
                    m.OffsetDb = Num(r.value(), "offset_db", 0.0f);
                    if (m.Valid()) out.Distance[r.key()] = m;
                }
            out.ShotOverReferenceDb = Num(*mix, "shot_lufs_m", 0.0f) - Num(*mix, "reference_lufs_m", 0.0f);
            out.HasReference = mix->contains("reference_lufs_m") && (*mix)["reference_lufs_m"].is_number();
            out.ReferenceLufs = Num(*mix, "reference_lufs_m", 0.0f);
        }
    auto add = [&](const std::string& file, const json& e) {
        if (!e.is_object() || file.empty()) return;
        SoundManifestEntry m;
        m.File = ManifestPath(file);
        m.Key = Str(e, "key");
        m.Category = Str(e, "category");
        m.Layer = Str(e, "layer");
        m.AnchorMs = Num(e, "anchor_ms", 0.0f);
        m.MixDb = Num(e, "mix_db", 0.0f);
        m.PeakDb = Num(e, "true_peak_dbtp", -3.0f);
        m.HasLufs = e.contains("lufs_m_max") && e["lufs_m_max"].is_number();
        m.LufsM = Num(e, "lufs_m_max", 0.0f);
        m.Rt60S = Num(e, "rt60_s", 0.0f);
        m.PreDelayMs = Num(e, "predelay_ms", 0.0f);
        m.Loop = Flag(e, "loop", false);
        if (!m.Key.empty()) out.Entries.push_back(std::move(m));
    };
    const json* list = &j;
    if (j.is_object()) {
        if (const auto it = j.find("files"); it != j.end()) list = &*it;
        else if (const auto it2 = j.find("entries"); it2 != j.end()) list = &*it2;
    }
    if (list->is_array()) {
        for (const auto& e : *list) add(e.is_object() ? Str(e, "file") : std::string(), e);
    } else if (list->is_object()) {
        for (auto it = list->begin(); it != list->end(); ++it) add(it.key(), it.value()); // file -> {key, category, layer}
    }
    std::sort(out.Entries.begin(), out.Entries.end(), [](const SoundManifestEntry& a, const SoundManifestEntry& b) { return a.File < b.File; });
    return true;
}

const SoundManifestEntry* SoundManifest::FirstFor(const std::string& key) const {
    for (const SoundManifestEntry& e : Entries)
        if (e.Key == key) return &e;
    return nullptr;
}

std::vector<std::string> SoundManifest::FilesFor(const std::string& key) const {
    std::vector<std::string> out;
    for (const SoundManifestEntry& e : Entries)
        if (e.Key == key) out.push_back(e.File);
    return out;
}

std::vector<std::string> SoundManifest::Keys() const {
    std::set<std::string> k;
    for (const SoundManifestEntry& e : Entries) k.insert(e.Key);
    return {k.begin(), k.end()};
}

namespace {
json ProjectAudio(const char* operation,const std::string& value="") {
    static std::unordered_map<std::string,json> cache;static std::uint64_t generation=~std::uint64_t{};
    if(generation!=Scripting::CodeGeneration()){cache.clear();generation=Scripting::CodeGeneration();}
    const auto key=std::string(operation)+":"+value;if(auto it=cache.find(key);it!=cache.end())return it->second;
    std::string result;if(!Scripting::RequestProject(operation,json(value).dump(),result))throw std::runtime_error("Project sound policy unavailable");auto data=json::parse(result);cache[key]=data;return data;
}
}
std::string WeaponAudioGunFolder(const std::string& gunId) {return ProjectAudio("audio.folder",gunId).get<std::string>();}
std::string WeaponAudioGunId(const std::string& path) {return ProjectAudio("audio.gun",path).get<std::string>();}
std::string SoundKeyFilePrefix(const std::string& key) {return ProjectAudio("audio.prefix",key).get<std::string>();}

std::vector<std::string> SoundFilesByLayout(const std::string& root, const std::string& key) {
    std::vector<std::string> out;
    const std::string prefix = SoundKeyFilePrefix(key);
    if (prefix.empty()) return out;
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(root) / fs::path(prefix).parent_path();
    const std::string stem = fs::path(prefix).filename().string(); // "mag_out_"
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string name = e.path().filename().string();
        const std::string ext = Lower(e.path().extension().string());
        if (name.rfind(stem, 0) != 0 || (ext != ".wav" && ext != ".ogg" && ext != ".mp3" && ext != ".flac")) continue;
        // "mag_out_1.wav", not "mag_out_long_1.wav": the rest of the stem is the variant number.
        const std::string num = e.path().stem().string().substr(stem.size());
        if (num.empty() || !std::all_of(num.begin(), num.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; })) continue;
        out.push_back(std::string(fs::path(prefix).parent_path().generic_string()) + "/" + name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

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
    const float jitter = set.VolumeJitterDb > 0.0f ? std::pow(10.0f, (Rand01() * 2.0f - 1.0f) * set.VolumeJitterDb / 20.0f) : 1.0f;
    SoundVoice v;
    v.File = set.Files[idx];
    v.Volume = set.Volume * req.Gain * jitter * set.GainLin((size_t)idx);
    v.Pitch = (set.PitchMin + (set.PitchMax - set.PitchMin) * Rand01()) * req.PitchScale;
    v.Loop = set.Loop;
    v.Bus = set.Bus;
    v.Spatial = !req.At2D;
    v.Position = req.Position;
    v.MinDistance = set.MinDistance;
    v.MaxDistance = set.MaxDistance;
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
        ++m_PathChecks;
    }
    const float occDistance = glm::length(req.Position - m_ListenerPos);
    if (vp.ViaPortal) { // another room: heard from the portal, through its loss, with the source room's reverb
        v.Position = vp.Virtual;
        v.PortalGain = vp.Gain;
        v.ReverbBus = 1;
    } else if (occ && m_Blocked && occDistance >= m_Occ.MinDistance && m_Blocked(m_ListenerPos, req.Position)) {
        occAmount = 1.0f; // behind something already: it starts muffled
        ++m_OccChecks;
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
            if (m_Path) {
                vp = m_Path(m_ListenerPos, t.Pos);
                ++m_PathChecks;
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
                    ++m_OccChecks;
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

// --- profile -----------------------------------------------------------------------------------

WeaponAudioProfile::Layer* WeaponAudioProfile::LayerByName(const std::string& name) {
    if (name == "close") return &Close;
    if (name == "mech") return &Mech;
    if (name == "sub") return &Sub;
    if (name == "tail") return &Tail;
    if (name == "far") return &Far;
    SpaceClass c;
    if (name.rfind("tail_", 0) == 0 && ParseSpaceClass(name.c_str() + 5, c)) return &TailClass[(int)c];
    return nullptr;
}

WeaponAudioProfile WeaponAudioProfile::Default(const std::string& gun) {
    WeaponAudioProfile p;p.Gun=gun;p.ApplyComponent(Scripting::DefaultWeaponAudioDefinition());
    const auto data=ProjectAudio("audio.default-profile",gun);
    for(auto it=data.at("layers").begin();it!=data.at("layers").end();++it)if(auto* layer=p.LayerByName(it.key()))layer->Set.Key=it.value().at("key").get<std::string>();
    p.ApplyJson(data.dump());return p;
}

void WeaponAudioProfile::ApplyComponent(const WeaponAudioComponent& c) {
    Enabled = c.Enabled;
    Volume = c.Volume;
    PlayerGain = c.PlayerGain;
    ShotPitchMin = std::min(c.ShotPitchMin, c.ShotPitchMax);
    ShotPitchMax = std::max(c.ShotPitchMin, c.ShotPitchMax);
    for (Layer* l : {&Close, &Mech, &Sub, &Tail, &Far}) {
        l->Set.VolumeJitterDb = c.VolumeJitterDb;
        if (l != &Tail && l != &Far) l->Set.MaxVoices = std::max(1, c.ShotMaxVoices);
    }
    Close.Curve.NearDistance = c.CloseFullDistance;
    Close.Curve.FarDistance = std::max(c.CloseZeroDistance, c.CloseFullDistance + 0.01f);
    Far.Curve.NearDistance = Close.Curve.NearDistance;
    Far.Curve.FarDistance = Close.Curve.FarDistance;
    Far.Curve.NearWeight = c.FarMinWeight;
    Far.Curve.FarWeight = c.FarMaxWeight;
    Close.Set.MinDistance = Mech.Set.MinDistance = std::max(c.ShotMinDistance, 0.1f);
    Sub.Set.MinDistance = Tail.Set.MinDistance = Far.Set.MinDistance = std::max(c.BassMinDistance, 0.1f);
    EventMinDistance = std::max(c.EventMinDistance, 0.1f);
    EventMaxDistance = std::max(c.EventMaxDistance, EventMinDistance + 0.1f);
    Close.Set.MaxDistance = Mech.Set.MaxDistance = Sub.Set.MaxDistance = c.MaxDistance;
    Far.Set.MaxDistance = Tail.Set.MaxDistance = c.FarMaxDistance;
    Tail.Set.MaxVoices = std::max(1, c.TailMaxVoices);
    Tail.Set.StealFadeTime = c.TailFadeTime;
    TailMinInterval = c.TailMinInterval;
    TailDuckPerVoice = c.TailDuckPerVoice;
    Tail.Every = std::max(1, c.TailEvery);
    Far.Every = std::max(1, c.FarEvery);
    BurstGap = c.BurstGap;
    for (Layer& l : TailClass) { // the class tails follow the tail layer's voice policy
        l.Set.VolumeJitterDb = c.VolumeJitterDb;
        l.Set.MaxDistance = c.FarMaxDistance;
        l.Set.MinDistance = Tail.Set.MinDistance;
        l.Set.MaxVoices = std::max(1, c.TailMaxVoices);
        l.Set.StealFadeTime = c.TailFadeTime;
    }
    Env.Enabled = c.EnvEnabled;
    Env.RayCount = std::clamp(c.EnvRayCount, 6, 64);
    Env.MaxDistance = std::max(c.EnvMaxDistance, 1.0f);
    Env.IndoorCover = c.EnvIndoorCover;
    Env.UrbanWall = c.EnvUrbanWall;
    Env.UrbanDistance = c.EnvUrbanDistance;
    Env.LargeRoomDistance = std::max(c.EnvLargeRoomDistance, 0.1f);
    Env.BlendFraction = c.EnvBlendFraction;
    Env.BlendDistance = c.EnvBlendDistance;
    Env.RefreshInterval = std::max(c.EnvRefreshInterval, 0.0f);
    Env.RefreshMoveDistance = std::max(c.EnvRefreshMoveDistance, 0.0f);
    Env.MatchRadius = std::max(c.EnvMatchRadius, 0.1f);
    Env.TailGain[(int)SpaceClass::OutdoorOpen] = c.EnvTailGainOutdoorOpen;
    Env.TailGain[(int)SpaceClass::OutdoorUrban] = c.EnvTailGainOutdoorUrban;
    Env.TailGain[(int)SpaceClass::IndoorSmall] = c.EnvTailGainIndoorSmall;
    Env.TailGain[(int)SpaceClass::IndoorLarge] = c.EnvTailGainIndoorLarge;
    Env.DebugDraw = c.EnvDebugDraw;
}

void WeaponAudioProfile::ApplyJson(const std::string& text) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    if(j.contains("aliases") && j["aliases"].is_object())for(auto it=j["aliases"].begin();it!=j["aliases"].end();++it)if(it.value().is_string())Aliases[it.key()]=it.value().get<std::string>();
    TailMinInterval = Num(j, "tailMinInterval", TailMinInterval);
    TailDuckPerVoice = Num(j, "tailDuckPerVoice", TailDuckPerVoice);
    BurstGap = Num(j, "burstGap", BurstGap);
    if (const auto it = j.find("layers"); it != j.end() && it->is_object())
        for (auto l = it->begin(); l != it->end(); ++l) {
            Layer* layer = LayerByName(l.key());
            if (!layer || !l.value().is_object()) continue;
            const std::string key = layer->Set.Key;
            layer->Set = ApplySetJson(l.value(), layer->Set);
            layer->Set.Key = key;
            if (layer == &Tail)
                for (Layer& tc : TailClass) {
                    const std::string ck = tc.Set.Key;
                    const std::vector<std::string> cf = tc.Set.Files;
                    tc.Set = ApplySetJson(l.value(), tc.Set);
                    tc.Set.Key = ck;
                    tc.Set.Files = cf; // the generic tail's files are not the class tails'
                }
            layer->Player2D = Flag(l.value(), "player2d", layer->Player2D);
            layer->Every = std::max(1, (int)Num(l.value(), "every", (float)layer->Every));
            if (const auto c = l.value().find("curve"); c != l.value().end() && c->is_object()) {
                layer->Curve.NearDistance = Num(*c, "nearDistance", layer->Curve.NearDistance);
                layer->Curve.FarDistance = Num(*c, "farDistance", layer->Curve.FarDistance);
                layer->Curve.NearWeight = Num(*c, "nearWeight", layer->Curve.NearWeight);
                layer->Curve.FarWeight = Num(*c, "farWeight", layer->Curve.FarWeight);
            }
        }
    if (const auto it = j.find("events"); it != j.end() && it->is_object())
        for (auto e = it->begin(); e != it->end(); ++e) {
            if (!e.value().is_object()) continue;
            SoundSet base;
            if (const auto have = Events.find(e.key()); have != Events.end()) base = have->second;
            base.MinDistance = EventMinDistance;
            base.MaxDistance = EventMaxDistance;
            SoundSet s = ApplySetJson(e.value(), base);
            s.Key = "snd." + Gun + "." + e.key();
            Events[e.key()] = s;
        }
}

std::string WeaponAudioProfile::ToJson() const {
    json j;
    j["gun"] = Gun;
    j["aliases"] = Aliases;
    j["tailMinInterval"] = TailMinInterval;
    j["tailDuckPerVoice"] = TailDuckPerVoice;
    j["burstGap"] = BurstGap;
    for (const auto& [name, l] : {std::pair<const char*, const Layer*>{"close", &Close}, {"mech", &Mech}, {"sub", &Sub}, {"tail", &Tail}, {"far", &Far}}) {
        json lj = SetToJson(l->Set);
        lj["player2d"] = l->Player2D;
        lj["every"] = l->Every;
        lj["curve"] = {{"nearDistance", l->Curve.NearDistance}, {"farDistance", l->Curve.FarDistance},
                       {"nearWeight", l->Curve.NearWeight}, {"farWeight", l->Curve.FarWeight}};
        j["layers"][name] = lj;
    }
    for (int c = 0; c < kSpaceClassCount; ++c) j["layers"][std::string("tail_") + SpaceClassName((SpaceClass)c)] = SetToJson(TailClass[c].Set);
    for (const auto& [e, s] : Events) j["events"][e] = SetToJson(s);
    return j.dump(2);
}

// --- WeaponAudio -------------------------------------------------------------------------------

WeaponAudio& WeaponAudio::Get() {
    static WeaponAudio instance;
    return instance;
}

void WeaponAudio::InstallLog() {
    m_Player.SetLog([this](double t, const std::string& key, const std::string& file, int voices, const SoundPlayer::Request& r, float vol, float pitch, const SoundVoice* v) {
        if (!m_LogFile) return;
        // W <t> <key> <file|-> <voices> <vol> <pitch> <2d> <x y z> <min dist> <max dist> <start offset s>   (CombatFx's audio.txt, beside
        // its S / L lines)
        std::fprintf(m_LogFile, "W %.4f %s %s %d %.3f %.3f %d %.3f %.3f %.3f %.2f %.2f %.4f %.3f %.0f\n", t, key.c_str(), file.empty() ? "-" : file.c_str(), voices,
                     vol, pitch, r.At2D ? 1 : 0, r.Position.x, r.Position.y, r.Position.z, v ? v->MinDistance : 1.0f, v ? v->MaxDistance : 40.0f,
                     v ? v->StartOffset : 0.0f, v ? v->ReverbSend : 0.0f, v ? v->OcclusionHz : 20000.0f);
    });
}

namespace {
// Which manifest mix.distance model a set plays by when it is in the world, by key (null: the set's own Min / Max).
const char* DistanceNameForKey(const std::string& key) {
    if (key.rfind("snd.impact.", 0) == 0) return "impact";
    if (key == "snd.body_fall") return "body_fall";
    if (key == "snd.flyby") return "flyby";
    if (key.rfind("snd.casing.", 0) == 0) return "casing";
    if (key.rfind("snd.foley.step_", 0) == 0) return "npc_step";
    if (key.rfind("snd.foley.", 0) == 0) return "npc_gear";
    if (key.rfind("snd.amb.", 0) == 0 || key.rfind("snd.ui.", 0) == 0) return nullptr;
    const size_t dot = key.find('.', 4);
    if (key.rfind("snd.", 0) == 0 && dot != std::string::npos) return key.compare(dot + 1, 5, "fire_") == 0 ? "npc_shot" : "npc_gear";
    return nullptr;
}
} // namespace

void WeaponAudio::FillSet(SoundSet& set) {
    if (const char* name = DistanceNameForKey(set.Key)) set.Distance = m_Manifest.DistanceFor(name);
    if (!set.Files.empty()) return;
    set.FileAnchorMs.clear();
    set.FileGainDb.clear();
    set.FilePeakDb.clear();
    set.FileLufs.clear();
    for (const SoundManifestEntry& e : m_Manifest.Entries)
        if (e.Key == set.Key) {
            set.Files.push_back(e.File);
            set.FileAnchorMs.push_back(e.AnchorMs);
            set.FileGainDb.push_back(e.MixDb);
            set.FilePeakDb.push_back(e.PeakDb);
            set.FileLufs.push_back(e.HasLufs ? e.LufsM : -200.0f);
            if (e.Loop) set.Loop = true;
        }
    if (set.Files.empty() && !m_Root.empty()) set.Files = SoundFilesByLayout(m_Root, set.Key);
}

void WeaponAudio::StartForTest(const std::string& projectRoot, SoundBackend* backend) {
    m_Root = projectRoot;
    m_Player.SetBackend(backend);
    m_Player.SetBlockedFn(nullptr);
    m_Player.SetOcclusion(SoundPlayer::OcclusionSettings{});
    m_Player.StopAll();
    m_Player.Seed(12345u);
    m_Bursts.clear();
    m_Profiles.clear();
    m_Foley.clear();
    m_History.clear();
    m_Manifest = SoundManifest{};
    m_ShotVoices = 0;
    m_LastTail = -1e9;
    m_Probe = EnvironmentProbe{};
    m_ListenerProbe = EnvironmentProbe{};
    m_Zones.Set({});
    m_LastSpace = SpaceMix{};
    m_LastDominant.clear();
    m_SpaceLines = 0;
    m_Keyed.clear();
    m_Bus = ReverbBusComponent{};
    m_Mix = AudioMixComponent{};
    m_Mix.DuckEnabled = m_Mix.FocusEnabled = m_Mix.AirEnabled = false; // (tests turn them on themselves)
    m_Ducker.Reset();
    m_Player.SetDucker(&m_Ducker);
    m_Player.SetReferenceLufs(false, 0.0f);
    ApplyMix();
    m_ReverbValid = false;
    m_ReverbSent = m_RemoteSent = AudioEngine::ReverbSpec{};
    m_ListenerMix = ReverbZoneMix{};
    m_Beds.clear();
    m_AmbienceDebug.clear();
    m_IrWarned = 0;
    m_World = nullptr;
    m_RemoteHoldUntil = -1e9;
    m_RemoteActive = false;
    m_PathCalls = m_Refreshes = 0;
    m_PathMicros = m_RefreshMicros = 0.0;
    for (const auto& value : ProjectAudio("audio.profiles")) {const auto gun=value.get<std::string>();m_Profiles[gun]=WeaponAudioProfile::Default(gun);}
    InstallLog();
    InstallRouting();
    m_Active = true;
}

void WeaponAudio::Start(World& world, const std::string& projectRoot) {
    Stop();
    m_Root = projectRoot.empty() ? ProjectPaths::Resolve("") : projectRoot;
    m_Player.SetBackend(nullptr);
    m_Player.Seed(0xA5F00Du);
    m_Manifest = SoundManifest{};
    {
        std::ifstream in(std::filesystem::path(m_Root) / "assets/Audio/audio_manifest.json", std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            SoundManifest::FromJson(ss.str(), m_Manifest);
        }
    }
    for (const auto& value : ProjectAudio("audio.profiles")) {const auto gun=value.get<std::string>();m_Profiles[gun]=WeaponAudioProfile::Default(gun);}
    Scripting::SyncNpcDefinitions(world);
    for (const entt::entity e : world.Registry.view<WeaponAudioComponent>()) {
        const WeaponAudioComponent& c = world.Registry.get<WeaponAudioComponent>(e);
        if (c.Gun.empty()) continue;
        auto it = m_Profiles.find(c.Gun);
        if (it == m_Profiles.end()) it = m_Profiles.emplace(c.Gun, WeaponAudioProfile::Default(c.Gun)).first;
        it->second.ApplyComponent(c);
        if (!c.DataFile.empty()) {
            std::ifstream in(std::filesystem::path(m_Root) / c.DataFile, std::ios::binary);
            if (in) {
                std::stringstream ss;
                ss << in.rdbuf();
                it->second.ApplyJson(ss.str());
            }
        }
    }
    // Every layer from the manifest (keys snd.<gun>.fire_<layer>, each file at its mix_db); recorded takes replace the
    // placeholders (the old Combat/ files stay only where the manifest has none). Event sets likewise.
    for (auto& [gun, p] : m_Profiles) {
        for (const std::string name : {"close", "mech", "sub", "tail", "far", "tail_outdoor_open", "tail_outdoor_urban", "tail_indoor_small", "tail_indoor_large"}) {
            WeaponAudioProfile::Layer* l = p.LayerByName(name);
            SoundSet fresh;
            fresh.Key = l->Set.Key;
            FillSet(fresh);
            l->Set.Distance = fresh.Distance;
            if (!fresh.Files.empty() && !l->Set.FilesExplicit) { // (a Data File's own files win over the manifest's)
                l->Set.Files = fresh.Files;
                l->Set.FileAnchorMs = fresh.FileAnchorMs;
                l->Set.FileGainDb = fresh.FileGainDb;
                l->Set.FilePeakDb = fresh.FilePeakDb;
                l->Set.FileLufs = fresh.FileLufs;
                if (fresh.Loop) l->Set.Loop = true;
            }
        }
        for (auto& [element, es] : p.Events) {
            SoundSet fresh;
            fresh.Key = es.Key;
            FillSet(fresh);
            if (!fresh.Files.empty() && !es.FilesExplicit) {
                es.Files = fresh.Files;
                es.FileAnchorMs = fresh.FileAnchorMs;
                es.FileGainDb = fresh.FileGainDb;
                es.FilePeakDb = fresh.FilePeakDb;
                es.FileLufs = fresh.FileLufs;
            }
        }
        for (const std::string name : {"close", "mech", "sub", "tail", "far", "tail_outdoor_open", "tail_outdoor_urban", "tail_indoor_small", "tail_indoor_large"})
            for (const std::string& f : p.LayerByName(name)->Set.Files) m_Player.Backend().Preload(f);
    }
    // Every manifest key is a set: variants preloaded so the first play has no decode hitch.
    for (const std::string& key : m_Manifest.Keys())
        for (const std::string& f : m_Manifest.FilesFor(key)) m_Player.Backend().Preload(f);
    InstallLog();
    m_History.clear();
    m_Transcript.clear();
    m_ShotVoices = 0;
    m_LastTail = -1e9;
    m_Probe = EnvironmentProbe{};
    m_ListenerProbe = EnvironmentProbe{};
    m_Zones.Build(world);
    m_LastSpace = SpaceMix{};
    m_LastDominant.clear();
    m_SpaceLines = 0;
    m_Keyed.clear();
    m_Bus = ReverbBusComponent{};
    if (const auto first = world.Registry.view<ReverbBusComponent>(); first.begin() != first.end()) { // the first one counts
        const entt::entity e = *first.begin();
        m_Bus = world.Registry.get<ReverbBusComponent>(e);
    }
    m_Mix = AudioMixComponent{};
    if (const auto first = world.Registry.view<AudioMixComponent>(); first.begin() != first.end()) { // the first one counts
        const entt::entity e = *first.begin();
        m_Mix = world.Registry.get<AudioMixComponent>(e);
    }
    m_Ducker.Reset();
    m_Player.SetDucker(&m_Ducker);
    m_Player.SetReferenceLufs(m_Manifest.HasReference, m_Manifest.ReferenceLufs);
    m_ReverbValid = false;
    m_ReverbSent = m_RemoteSent = AudioEngine::ReverbSpec{};
    m_ListenerMix = ReverbZoneMix{};
    m_Beds.clear();
    m_AmbienceDebug.clear();
    m_IrWarned = 0;
    m_World = &world;
    m_RemoteHoldUntil = -1e9;
    m_RemoteActive = false;
    m_PathCalls = m_Refreshes = 0;
    m_PathMicros = m_RefreshMicros = 0.0;
    InstallRouting();
    m_Player.SetBlockedFn(EnvironmentBlockedFn());
    m_Player.Backend().ConfigureReverb(m_Bus.Enabled, m_Bus.ReturnLevel, m_Bus.GlideTime);
    ApplyMix(); // the ducking, the air, the master chain (trim, glue, limiter)
    // The impulse responses: the classes' (manifest ir.<class>) and the zones' own, decoded and prepared now, not on the first frame in a room.
    for (int c = 0; c < kSpaceClassCount; ++c)
        if (const std::string ir = ClassIr(c); !ir.empty()) m_Player.Backend().PreloadIr(ir);
    for (const ReverbZoneVolume& z : m_Zones.Zones())
        if (!z.Reverb.Ir.empty()) m_Player.Backend().PreloadIr(z.Reverb.Ir);
    m_Active = true;
}

void WeaponAudio::ApplyMix() {
    m_Ducker.Configure(m_Mix);
    SoundPlayer::OcclusionSettings occ = m_Player.GetOcclusion();
    occ.AirEnabled = m_Mix.AirEnabled;
    occ.AirStartDistance = m_Mix.AirStartDistance;
    occ.AirExponent = m_Mix.AirExponent;
    occ.AirMinHz = m_Mix.AirMinHz;
    m_Player.SetOcclusion(occ);
    LimiterSettings lim;
    lim.Enabled = m_Bus.MasterLimiterEnabled;
    lim.CeilingDb = m_Bus.MasterCeilingDb;
    lim.LookaheadMs = m_Bus.MasterLookaheadMs;
    lim.ReleaseMs = m_Bus.MasterReleaseMs;
    lim.TrimDb = m_Mix.MasterTrimDb;
    lim.GlueEnabled = m_Mix.GlueEnabled;
    lim.GlueThresholdDb = m_Mix.GlueThresholdDb;
    lim.GlueRatio = m_Mix.GlueRatio;
    lim.GlueKneeDb = m_Mix.GlueKneeDb;
    lim.GlueAttackMs = m_Mix.GlueAttackMs;
    lim.GlueReleaseMs = m_Mix.GlueReleaseMs;
    m_Player.Backend().ConfigureLimiter(lim);
}

void WeaponAudio::Stop() {
    if (m_Active && m_Probe.GetStats().Refreshes > 0) {
        const EnvironmentProbe::Stats& st = m_Probe.GetStats();
        std::printf("[WeaponAudio] environment probe: %d refreshes (%d rays), mean %.1f us, max %.1f us per refresh; %d zone(s)\n", st.Refreshes, st.Rays,
                    st.TotalMicros / st.Refreshes, st.MaxMicros, (int)m_Zones.Zones().size());
    }
    if (m_Active) {
        const AudioEngine::ReverbStats rs = AudioEngine::GetReverbStats();
        if (rs.Callbacks > 0)
            std::printf("[WeaponAudio] reverb bus: %llu audio callbacks (%d frames), mean %.1f us, max %.1f us per callback; tail worker %llu blocks, mean %.1f us, %llu late\n",
                        (unsigned long long)rs.Callbacks, rs.Frames, rs.TotalMicros / (double)rs.Callbacks, rs.MaxMicros, (unsigned long long)rs.WorkerJobs,
                        rs.WorkerJobs ? rs.WorkerMicros / (double)rs.WorkerJobs : 0.0, (unsigned long long)rs.LateBlocks);
        AudioEngine::ResetReverbStats();
    }
    if (m_Active && (m_PathCalls > 0 || m_Refreshes > 0))
        std::printf("[WeaponAudio] portals: %d path searches, mean %.2f us; %d zone/portal refreshes, mean %.2f us; %d portal(s), %d zone(s)\n", m_PathCalls,
                    m_PathCalls ? m_PathMicros / m_PathCalls : 0.0, m_Refreshes, m_Refreshes ? m_RefreshMicros / m_Refreshes : 0.0, (int)m_Zones.Portals().size(),
                    (int)m_Zones.Zones().size());
    StopAmbience();
    m_Player.StopAll();
    m_Player.SetBlockedFn(nullptr); // (a test's line-of-sight function must not outlive its locals)
    m_Player.SetPathFn(nullptr);
    m_World = nullptr;
    m_Profiles.clear();
    m_Foley.clear();
    m_Keyed.clear();
    m_Bursts.clear();
    m_Active = false;
}

void WeaponAudio::Update(float dt) {
    if (!m_Active) return;
    m_Player.SetListener(m_Listener);
    if (m_World) { // zones and portals that moved, opened, closed, appeared or went
        const auto t0 = std::chrono::steady_clock::now();
        m_Zones.Refresh(*m_World);
        m_RefreshMicros += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        ++m_Refreshes;
    }
    SoundPlayer::OcclusionSettings occ = m_Player.GetOcclusion();
    occ.Enabled = m_Bus.OcclusionEnabled;
    occ.CutoffHz = m_Bus.OcclusionCutoff;
    occ.Interval = m_Bus.OcclusionInterval;
    occ.RaysPerFrame = m_Bus.OcclusionRaysPerFrame;
    occ.MinDistance = m_Bus.OcclusionMinDistance;
    occ.GlideRate = m_Bus.OcclusionGlide;
    m_Player.SetOcclusion(occ);
    m_Ducker.Update(dt);
    m_Player.Update(dt);
    // The remote room's reverb (a voice heard through a portal brings its own room's): runs while such voices play, then rings out.
    if (m_Bus.Enabled) {
        if (m_Player.PortalVoices() > 0) m_RemoteHoldUntil = m_Player.Now() + (double)m_Bus.RemoteReverbHold;
        const bool active = m_Player.Now() < m_RemoteHoldUntil;
        if (active) {
            AudioEngine::ReverbSpec rs;
            AudioEngine::ReverbLayerSpec layer;
            if (BuildLayer(m_Player.RemoteRoom(), 1.0f, layer)) {
                rs.Count = 1;
                rs.Layers[0] = layer;
            }
            if (SpecDiffers(rs, m_RemoteSent)) {
                m_Player.Backend().SetReverb(rs, 1);
                m_RemoteSent = rs;
            }
        }
        if (active != m_RemoteActive) {
            m_Player.Backend().SetReverbBusActive(1, active);
            m_RemoteActive = active;
        }
    }
    // The reverb follows the listener's space: told when it changes enough to matter (the DSP glides to what it is told).
    if (m_Bus.Enabled) {
        const AudioEngine::ReverbSpec spec = ReverbAt(m_Listener);
        if (!m_ReverbValid || SpecDiffers(spec, m_ReverbSent)) {
            m_Player.Backend().SetReverb(spec, 0);
            m_ReverbSent = spec;
            m_ReverbValid = true;
            if (m_LogFile && m_Player.Now() - m_LastReverbLog >= 0.1) {
                m_LastReverbLog = m_Player.Now();
                std::fprintf(m_LogFile, "R %.4f %d", m_Player.Now(), spec.Count);
                for (int i = 0; i < spec.Count; ++i) std::fprintf(m_LogFile, " %s %.3f %.2f %.1f", spec.Layers[i].Ir.c_str(), spec.Layers[i].Weight, spec.Layers[i].WetDb, spec.Layers[i].PreDelayMs);
                std::fprintf(m_LogFile, "\n");
            }
        }
    }
    m_ListenerMix = m_Zones.Mix(m_Listener);
    UpdateAmbience(dt);
}

SoundPlayer::VoicePath WeaponAudio::PathFor(const glm::vec3& listener, const glm::vec3& source) {
    SoundPlayer::VoicePath vp;
    if (!m_Bus.PortalsEnabled || m_Zones.Portals().empty()) return vp;
    const auto t0 = std::chrono::steady_clock::now();
    PortalPath pp;
    if (m_Zones.FindPath(source, listener, pp)) {
        vp.ViaPortal = true;
        vp.Virtual = pp.Virtual;
        vp.Gain = pp.Gain;
        vp.CutoffHz = pp.CutoffHz;
        const ReverbZoneVolume* room = m_Zones.ZoneOfRoom(pp.SourceRoom);
        vp.Remote = room ? room->Reverb : ReverbPresetFor((int)SpaceClass::OutdoorOpen);
    }
    m_PathMicros += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    ++m_PathCalls;
    return vp;
}

void WeaponAudio::InstallRouting() {
    m_Player.SetPathFn([this](const glm::vec3& l, const glm::vec3& s) { return PathFor(l, s); });
    m_Player.SetRouting([this](const std::string& key) {
        SoundPlayer::Routing r;
        if (m_Bus.Enabled) r.Send = SendFor(key);
        r.Occlusion = m_Bus.OcclusionEnabled;
        return r;
    });
}

SoundPlayer::BlockedFn WeaponAudio::EnvironmentBlockedFn() {
    return [this](const glm::vec3& from, const glm::vec3& to) {
        const glm::vec3 d = to - from;
        const float dist = glm::length(d);
        if (dist < 1e-3f) return false;
        const glm::vec3 dir = d / dist;
        const float o[3] = {from.x, from.y, from.z}, dd[3] = {dir.x, dir.y, dir.z};
        QueryFilter f;
        f.HitTriggers = 0;
        RaycastHit hit;
        const bool recording = PhysicsWorld::GetQueryRecording();
        PhysicsWorld::SetQueryRecording(false); // plumbing: kept off the physics debug overlay
        const bool blocked = PhysicsWorld::RaycastSolid(o, dd, dist, f, hit) && hit.Hit && hit.Distance < dist - m_Player.GetOcclusion().Clearance;
        PhysicsWorld::SetQueryRecording(recording);
        return blocked;
    };
}

float WeaponAudio::SendFor(const std::string& key) const {
    const ReverbBusComponent& b = m_Bus;
    if (key.rfind("snd.foley.", 0) == 0) {
        const std::string rest = key.substr(10);
        const size_t dot = rest.find('.');
        const std::string cat = rest.substr(0, dot), el = dot == std::string::npos ? std::string() : rest.substr(dot + 1);
        const bool step = cat.rfind("step_", 0) == 0 || el == "walk" || el == "run" || el == "crouch";
        return ClampSend(step ? b.SendFootsteps : b.SendFoley);
    }
    if (key.rfind("snd.casing.", 0) == 0) return ClampSend(b.SendCasings);
    if (key.rfind("snd.impact.", 0) == 0 || key == "snd.flyby") return ClampSend(b.SendImpacts);
    if (key.rfind("snd.", 0) == 0) {
        const size_t dot = key.find('.', 4);
        const std::string el = dot == std::string::npos ? std::string() : key.substr(dot + 1);
        if (el == "fire_close" || el == "fire_mech" || el == "fire_sub") return ClampSend(b.SendShot);
        if (el.rfind("fire_tail", 0) == 0 || el == "fire_far") return ClampSend(b.SendTail);
        return ClampSend(b.SendActions);
    }
    return 0.0f;
}

std::string WeaponAudio::ClassIr(int tailClass) const {
    const SoundManifestEntry* e = m_Manifest.FirstFor(std::string("ir.") + SpaceClassName((SpaceClass)std::clamp(tailClass, 0, kSpaceClassCount - 1)));
    return e ? e->File : std::string();
}

float WeaponAudio::ClassPreDelayMs(int tailClass) const {
    const SoundManifestEntry* e = m_Manifest.FirstFor(std::string("ir.") + SpaceClassName((SpaceClass)std::clamp(tailClass, 0, kSpaceClassCount - 1)));
    return e ? e->PreDelayMs : 0.0f;
}

float WeaponAudio::ClassWetDb(int tailClass) const {
    float db = m_Bus.WetIndoorSmallDb;
    switch (tailClass) {
    case 0: db = m_Bus.WetOutdoorOpenDb; break;
    case 1: db = m_Bus.WetOutdoorUrbanDb; break;
    case 3: db = m_Bus.WetIndoorLargeDb; break;
    default: break;
    }
    return db + m_Bus.WetTrimDb + 20.0f * std::log10(std::max(m_Bus.WetScale, 1e-4f));
}

bool WeaponAudio::BuildLayer(const ReverbPreset& p, float weight, AudioEngine::ReverbLayerSpec& out) {
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
    out.LowCutHz = p.LowCutHz >= 0.0f ? p.LowCutHz : m_Bus.LowCutHz;
    return true;
}

bool WeaponAudio::SpecDiffers(const AudioEngine::ReverbSpec& a, const AudioEngine::ReverbSpec& b) {
    if (a.Count != b.Count) return true;
    for (int i = 0; i < a.Count; ++i) {
        const AudioEngine::ReverbLayerSpec &x = a.Layers[i], &y = b.Layers[i];
        if (x.Ir != y.Ir || std::fabs(x.Weight - y.Weight) > 0.002f || std::fabs(x.WetDb - y.WetDb) > 0.05f || std::fabs(x.PreDelayMs - y.PreDelayMs) > 0.2f ||
            std::fabs(x.HfDampDb - y.HfDampDb) > 0.05f || std::fabs(x.LowCutHz - y.LowCutHz) > 0.5f)
            return true;
    }
    return false;
}

AudioEngine::ReverbSpec WeaponAudio::ReverbAt(const glm::vec3& pos) {
    const ReverbZoneMix z = m_Zones.Mix(pos);
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
    for (int i = 0; i < z.ClaimCount; ++i) add(m_Zones.Zones()[(size_t)z.Claims[i].Zone].Reverb, z.Claims[i].Weight);
    if (z.ProbeShare > 1e-3f) {
        constexpr std::uint32_t kListenerProbe = 0xFFFFFFFDu;
        EnvironmentSettings env;
        if (const auto it = m_Profiles.find(ProjectAudio("audio.environment-profile").get<std::string>()); it != m_Profiles.end()) env = it->second.Env;
        else if (!m_Profiles.empty()) env = m_Profiles.begin()->second.Env;
        const EnvironmentReading& r = m_ListenerProbe.Query(kListenerProbe, pos, m_Player.Now(), env);
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

void WeaponAudio::StopAmbience() {
    for (Bed& b : m_Beds)
        if (b.Handle != AudioEngine::InvalidHandle) m_Player.Backend().Stop(b.Handle);
    m_Beds.clear();
    m_AmbienceDebug.clear();
}

void WeaponAudio::UpdateAmbience(float dt) {
    const std::string* keys[ReverbZoneMix::kMaxClaims];
    float take[ReverbZoneMix::kMaxClaims], vol[ReverbZoneMix::kMaxClaims];
    int n = 0;
    for (int i = 0; i < m_ListenerMix.ClaimCount; ++i) {
        const ReverbZoneVolume& z = m_Zones.Zones()[(size_t)m_ListenerMix.Claims[i].Zone];
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
                m_Player.Backend().Stop(b.Handle);
                b.Handle = AudioEngine::InvalidHandle;
            }
        } else if (b.Handle == AudioEngine::InvalidHandle || !m_Player.Backend().IsPlaying(b.Handle)) {
            b.Handle = AudioEngine::InvalidHandle;
            SoundSet* set = KeySet(b.Key, [](SoundSet& s) {
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
                const SoundPlayer::Played pl = m_Player.Play(*set, r);
                if (pl.Started) {
                    b.Handle = pl.Handle;
                    b.BaseVolume = pl.Volume / b.Level;
                }
            }
        } else {
            m_Player.Backend().SetVolume(b.Handle, b.BaseVolume * b.Level);
        }
        AmbienceDebug d;
        d.Key = b.Key;
        d.Target = b.Target;
        d.Level = b.Level;
        d.Playing = b.Handle != AudioEngine::InvalidHandle;
        m_AmbienceDebug.push_back(d);
    }
}

SoundSet* WeaponAudio::KeySet(const std::string& key, const std::function<void(SoundSet&)>& init) {
    auto it = m_Keyed.find(key);
    if (it != m_Keyed.end()) {
        if (it->second.Files.empty()) FillSet(it->second);
        return &it->second;
    }
    SoundSet s;
    s.Key = key;
    if (init) init(s);
    FillSet(s);
    it = m_Keyed.emplace(key, std::move(s)).first;
    for (const std::string& f : it->second.Files) m_Player.Backend().Preload(f);
    return &it->second;
}

SoundPlayer::Played WeaponAudio::PlayKeyed(SoundSet& set, const glm::vec3& pos, bool at2D, float gain, float pitchScale) {
    SoundPlayer::Request r;
    r.Position = pos;
    r.At2D = at2D;
    r.Gain = gain;
    r.PitchScale = pitchScale;
    // (not Note()d: the transcript is the player's own gun and gear - impacts and casings are the world's, and are in the W log)
    return m_Player.Play(set, r);
}

WeaponAudioProfile* WeaponAudio::Profile(const std::string& gun) {
    const auto it = m_Profiles.find(gun);
    if (it != m_Profiles.end()) return &it->second;
    if (!m_Active || gun.empty()) return nullptr;
    return &m_Profiles.emplace(gun, WeaponAudioProfile::Default(gun)).first->second;
}

void WeaponAudio::Note(const std::string& key, bool at2D, const std::string& space) {
    m_History.push_back({m_Player.Now(), key, at2D, space});
    if (m_Record) m_Transcript.push_back({m_Player.Now(), key, at2D, space});
    if (m_History.size() > kMaxHistory) m_History.pop_front();
}

int WeaponAudio::Shot(const std::string& gun, const glm::vec3& pos, bool at2D, std::uint32_t shooter) {
    if (!m_Active) return 0;
    WeaponAudioProfile* p = Profile(gun);
    if (!p || !p->Enabled) return 0;
    Scripting::AudioShotFrame f;f.PitchMin=p->ShotPitchMin;f.PitchMax=p->ShotPitchMax;f.Random=m_Player.Rand01();f.Distance=at2D?0:glm::length(pos-m_Listener);f.At2D=at2D;
    f.Volume=p->Volume;f.PlayerGain=p->PlayerGain;f.Now=m_Player.Now();f.LastTail=m_LastTail;f.TailMinInterval=p->TailMinInterval;f.TailDuckPerVoice=p->TailDuckPerVoice;f.TailVoices=TailVoices(*p);f.BurstGap=p->BurstGap;
    Burst& burst=m_Bursts[gun+(at2D?"/2d":"/3d")];f.Count=burst.Count;f.LastShot=burst.Last;
    WeaponAudioProfile::Layer* layers[]{&p->Close,&p->Mech,&p->Sub,&p->Far,&p->Tail};
    for(int i=0;i<5;++i){const auto& l=*layers[i];f.Player2D[i]=l.Player2D;f.Every[i]=l.Every;f.Near[i]=l.Curve.NearDistance;f.Far[i]=l.Curve.FarDistance;f.NearWeight[i]=l.Curve.NearWeight;f.FarWeight[i]=l.Curve.FarWeight;}
    if(!Scripting::InvokeProject("audio.shot",&f,sizeof f))throw std::runtime_error("Project shot sound policy unavailable");
    burst.Count=f.Count;burst.Last=f.LastShot;m_LastTail=f.LastTail;int started=0;
    for(int i=0;i<5;++i)if(f.Play[i]) {
        SoundPlayer::Request request;request.Position=pos;request.At2D=at2D;request.PitchScale=f.Pitch;request.Gain=f.Gain[i];
        if(i==4){started+=PlayTail(*p,request,shooter,pos,at2D);continue;}
        Note(layers[i]->Set.Key,at2D);if(m_Player.Play(layers[i]->Set,request).Started)++started;
    }
    m_ShotVoices += started;
    return started;
}

int WeaponAudio::TailVoices(const WeaponAudioProfile& p) const {
    int n = m_Player.AudibleVoices(p.Tail.Set.Key);
    for (const WeaponAudioProfile::Layer& l : p.TailClass) n += m_Player.AudibleVoices(l.Set.Key);
    return n;
}

namespace {
constexpr std::uint32_t kPlayerShooter = 0xFFFFFFFEu; // the first-person player (2D shots no one named)
}

WeaponAudio::SpaceMix WeaponAudio::ResolveSpace(const WeaponAudioProfile& p, std::uint32_t shooter, const glm::vec3& pos, bool at2D) {
    SpaceMix m;
    const EnvironmentSettings& s = p.Env;
    if (!s.Enabled) return m;
    m.Valid = true;
    const ReverbZoneMix z = m_Zones.Mix(pos);
    for (int i = 0; i < kSpaceClassCount; ++i) m.Weights[i] = z.Weights[i];
    m.Gain = z.Gain;
    m.ZoneShare = 1.0f - z.ProbeShare;
    if (z.ProbeShare > 1e-3f) { // whatever no zone claims (everything outside the zones) is the probe's call
        const std::uint32_t id = shooter != 0 ? shooter : (at2D ? kPlayerShooter : 0u);
        bool refreshed = false;
        m.Reading = m_Probe.Query(id, pos, m_Player.Now(), s, &refreshed);
        m.Probed = true;
        for (int i = 0; i < kSpaceClassCount; ++i) m.Weights[i] += z.ProbeShare * m.Reading.Weights[i];
        if (refreshed) {
            const EnvironmentReading& r = m.Reading;
            if (m_LogFile)
                std::fprintf(m_LogFile, "E %.4f %u %s %.3f %.3f %.3f %.3f cover %.2f wall %.2f wallDist %.1f enclosure %.2f\n", m_Player.Now(), (unsigned)id,
                             SpaceClassName(r.Dominant), r.Weights[0], r.Weights[1], r.Weights[2], r.Weights[3], r.Cover, r.Wall, r.MeanWallDistance, r.Enclosure);
            const std::string who = id == kPlayerShooter ? "player" : id != 0 ? "shooter " + std::to_string(id) : "npc";
            const auto it = m_LastDominant.find(who);
            if ((it == m_LastDominant.end() || it->second != r.Dominant) && m_SpaceLines < 48) {
                ++m_SpaceLines;
                std::printf("[WeaponAudio] %s space -> %s (cover %.2f wall %.2f wallDist %.1f m enclosure %.2f; weights open %.2f urban %.2f small %.2f large %.2f; zones %.0f%%)\n",
                            who.c_str(), SpaceClassName(r.Dominant), r.Cover, r.Wall, r.MeanWallDistance, r.Enclosure, m.Weights[0], m.Weights[1], m.Weights[2],
                            m.Weights[3], m.ZoneShare * 100.0f);
            }
            m_LastDominant[who] = r.Dominant;
        }
    }
    return m;
}

int WeaponAudio::PlayTail(WeaponAudioProfile& p, const SoundPlayer::Request& base, std::uint32_t shooter, const glm::vec3& pos, bool at2D) {
    const SpaceMix mix = ResolveSpace(p, shooter, pos, at2D);
    m_LastSpace = mix;
    int started = 0;
    if (!mix.Valid) { // environment off: the generic tail, as ever
        Note(p.Tail.Set.Key, at2D, "generic");
        return m_Player.Play(p.Tail.Set, base).Started ? 1 : 0;
    }
    // The two heaviest classes, renormalised; each plays equal-power (the tails are different recordings, not one signal).
    int order[kSpaceClassCount];
    for (int i = 0; i < kSpaceClassCount; ++i) order[i] = i;
    std::stable_sort(order, order + kSpaceClassCount, [&](int a, int b) { return mix.Weights[a] > mix.Weights[b]; });
    int kept[2];
    int nKept = 0;
    float sumW = 0.0f;
    for (int k = 0; k < 2; ++k)
        if (k == 0 || mix.Weights[order[k]] >= WeaponAudioProfile::kMinClassWeight) {
            kept[nKept++] = order[k];
            sumW += mix.Weights[order[k]];
        }
    float fallbackPower = 0.0f;
    std::string space;
    for (int k = 0; k < nKept; ++k) {
        const int c = kept[k];
        const float wn = sumW > 1e-6f ? mix.Weights[c] / sumW : 1.0f;
        const float g = p.Env.TailGain[c] * mix.Gain;
        SoundSet& set = p.TailClass[c].Set;
        if (!space.empty()) space += "+";
        if (set.Files.empty()) { // no recorded tail for this space (yet): its share plays the generic tail
            fallbackPower += wn * g * g;
            space += "generic";
            continue;
        }
        space += SpaceClassName((SpaceClass)c);
        SoundPlayer::Request r = base;
        r.Gain *= std::sqrt(wn) * g;
        if (m_Player.Play(set, r).Started) ++started;
    }
    if (fallbackPower > 0.0f) {
        SoundPlayer::Request r = base;
        r.Gain *= std::sqrt(fallbackPower);
        if (m_Player.Play(p.Tail.Set, r).Started) ++started;
    }
    Note(p.Tail.Set.Key, at2D, space);
    return started;
}

void AppendAudioDebugLines(const World& world, std::vector<float>& out, bool zones) {
    if (zones) {
        ReverbZones z;
        z.Build(world);
        z.DebugLines(out);
    }
    if (WeaponAudio::Get().Active()) WeaponAudio::Get().EnvironmentDebugLines(out, false);
}

void WeaponAudio::EnvironmentDebugLines(std::vector<float>& out, bool withZones) const {
    bool any = false;
    for (const auto& [gun, p] : m_Profiles) any |= p.Env.DebugDraw;
    if (!any) return;
    if (withZones) m_Zones.DebugLines(out);
    m_Probe.DebugLines(out);
    m_ListenerProbe.DebugLines(out);
}

bool WeaponAudio::ParseKey(const std::string& name,std::string& gun,std::string& element,float* leadMs) {
    const auto data=ProjectAudio("audio.parse",name);if(!data.value("valid",false))return false;
    gun=data.at("gun").get<std::string>();element=data.at("element").get<std::string>();if(leadMs)*leadMs=data.at("lead").get<float>();return true;
}

bool WeaponAudio::HasEventFiles(const std::string& gun, const std::string& element) {
    if (!m_Active) return false;
    WeaponAudioProfile* p = Profile(gun);
    return p && !EventSet(*p, element)->Files.empty();
}

SoundSet* WeaponAudio::EventSet(WeaponAudioProfile& p, const std::string& element) {
    auto it = p.Events.find(element);
    if (it == p.Events.end()) {
        SoundSet s;
        s.Key = "snd." + p.Gun + "." + element;
        s.MinDistance = p.EventMinDistance;
        s.MaxDistance = p.EventMaxDistance;
        s.MaxVoices = 4;
        s.PitchMin = 0.98f;
        s.PitchMax = 1.02f;
        s.VolumeJitterDb = 1.0f;
        FillSet(s);
        it = p.Events.emplace(element, std::move(s)).first;
        for (const std::string& f : it->second.Files) m_Player.Backend().Preload(f);
    } else if (it->second.Files.empty()) {
        FillSet(it->second);
    }
    return &it->second;
}

SoundSet* WeaponAudio::FoleySet(const std::string& category, const std::string& element) {
    const std::string key = "snd.foley." + category + "." + element;
    auto it = m_Foley.find(key);
    if (it == m_Foley.end()) {
        SoundSet s;
        s.Key = key;
        const auto gun = m_Profiles.find(ProjectAudio("audio.environment-profile").get<std::string>()) != m_Profiles.end() ? m_Profiles.find(ProjectAudio("audio.environment-profile").get<std::string>()) : m_Profiles.begin();
        s.MinDistance = gun != m_Profiles.end() ? gun->second.EventMinDistance : 1.5f; // gear sounds: the gun events' reach
        s.MaxDistance = gun != m_Profiles.end() ? gun->second.EventMaxDistance : 25.0f;
        s.MaxVoices = 4;
        s.StealFadeTime = 0.05f;
        FillSet(s);
        it = m_Foley.emplace(key, std::move(s)).first;
        for (const std::string& f : it->second.Files) m_Player.Backend().Preload(f);
    }
    return &it->second;
}

bool WeaponAudio::PlayEvent(const std::string& name, const std::string& gun, const glm::vec3& pos, bool at2D, float gain) {
    if (!m_Active) return false;
    std::string g = gun, element = name;
    float lead = -1.0f;
    if (std::string pg, pe; ParseKey(name, pg, pe, &lead)) {
        g = pg;
        element = pe;
    }
    if (g.empty() || element.empty()) return false;
    if (g != "foley") {
        WeaponAudioProfile* p = Profile(g);
        if (!p || !p->Enabled) return false;
        // A gear sound the shared foley owns (ads_in, equip ...): the same event, the foley's key.
        if (const auto al = p->Aliases.find(element); al != p->Aliases.end() && al->second != "snd." + g + "." + element) {
            std::string ag, ae;
            if (ParseKey(al->second, ag, ae)) {
                g = ag;
                element = ae;
            }
        }
    }
    SoundPlayer::Request r;
    r.Position = pos;
    r.At2D = at2D;
    r.Gain = gain;
    r.LeadMs = lead;
    if (g == "foley") { // snd.foley.<category>.<element>: shared gear / cloth sounds
        const size_t dot = element.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= element.size()) return false;
        SoundSet* fs = FoleySet(element.substr(0, dot), element.substr(dot + 1));
        Note(fs->Key, at2D);
        m_Player.Play(*fs, r);
        return true;
    }
    WeaponAudioProfile* p = Profile(g);
    if (!p || !p->Enabled) return false;
    SoundSet* set = EventSet(*p, element);
    r.Gain = p->Volume * gain;
    Note(set->Key, at2D);
    m_Player.Play(*set, r);
    return true;
}
