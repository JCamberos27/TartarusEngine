#include "WeaponAudio.h"

#include "Components.h"
#include "GameModuleAPI.h"
#include "PhysicsWorld.h"
#include "Core/Paths.h"
#include "SoundJson.h"
#include "World.h"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

using json = nlohmann::json;
using SoundJson::Flag;
using SoundJson::Num;

namespace {

constexpr size_t kMaxHistory = 4096;

// The profile's layers by name (LayerByName): the shot layers, then the tail by space.
constexpr const char* kProfileLayers[] = {"close", "mech", "sub", "tail", "far", "tail_outdoor_open", "tail_outdoor_urban", "tail_indoor_small", "tail_indoor_large"};

} // namespace

// --- BlendCurve --------------------------------------------------------------------------------

float BlendCurve::Weight(float distance) const {
    const float span = FarDistance - NearDistance;
    const float t = span > 1e-4f ? std::clamp((distance - NearDistance) / span, 0.0f, 1.0f) : (distance >= FarDistance ? 1.0f : 0.0f);
    return NearWeight + (FarWeight - NearWeight) * t;
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
    WeaponAudioProfile p;
    p.Gun = gun;
    const std::string k = "snd." + gun + ".fire_";
    auto layer = [&](Layer& l, const char* name, float full, float zero, float nearW, float farW, float minD, float maxD, int voices, float fade) {
        l.Set.Key = k + name;
        l.Set.MinDistance = minD;
        l.Set.MaxDistance = maxD;
        l.Set.MaxVoices = voices;
        l.Set.StealFadeTime = fade;
        l.Set.PitchMin = l.Set.PitchMax = 1.0f;
        l.Set.VolumeJitterDb = 0.5f;
        l.Curve = {full, zero, nearW, farW};
    };
    layer(p.Close, "close", 18.0f, 43.0f, 1.0f, 0.0f, 3.0f, 90.0f, 6, 0.03f);
    layer(p.Mech, "mech", 10.0f, 30.0f, 1.0f, 0.0f, 3.0f, 45.0f, 6, 0.03f);
    layer(p.Sub, "sub", 25.0f, 70.0f, 1.0f, 0.0f, 6.0f, 120.0f, 6, 0.05f);
    layer(p.Tail, "tail", 30.0f, 120.0f, 1.0f, 0.7f, 6.0f, 140.0f, 3, 0.25f);
    layer(p.Far, "far", 18.0f, 43.0f, 0.1f, 1.0f, 6.0f, 160.0f, 6, 0.1f);
    p.Far.Player2D = false;
    p.Tail.Every = 2; // full auto: close / sub / mech every shot, tail every 2nd, far every 3rd (S1's mix policy)
    p.Far.Every = 3;
    for (int c = 0; c < kSpaceClassCount; ++c) { // the tail by space: the same set of rules, its own files
        p.TailClass[c] = p.Tail;
        p.TailClass[c].Set.Key = k + "tail_" + SpaceClassName((SpaceClass)c);
    }
    for (const char* e : {"ads_in", "ads_out", "equip", "unequip"}) p.Aliases[e] = std::string("snd.foley.weapon.") + e;
    return p;
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
    Close.Set.MaxDistance = Mech.Set.MaxDistance = std::max(c.MaxDistance, Close.Set.MinDistance);
    Sub.Set.MaxDistance = std::max(c.MaxDistance, Sub.Set.MinDistance);
    Far.Set.MaxDistance = Tail.Set.MaxDistance = std::max(c.FarMaxDistance, Far.Set.MinDistance);
    Tail.Set.MaxVoices = std::max(1, c.TailMaxVoices);
    Tail.Set.StealFadeTime = c.TailFadeTime;
    TailMinInterval = c.TailMinInterval;
    TailDuckPerVoice = c.TailDuckPerVoice;
    Tail.Every = std::max(1, c.TailEvery);
    Far.Every = std::max(1, c.FarEvery);
    BurstGap = c.BurstGap;
    for (Layer& l : TailClass) { // the class tails follow the tail layer's voice policy
        l.Set.VolumeJitterDb = c.VolumeJitterDb;
        l.Set.MinDistance = Tail.Set.MinDistance;
        l.Set.MaxDistance = std::max(c.FarMaxDistance, l.Set.MinDistance);
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
    TailMinInterval = Num(j, "tailMinInterval", TailMinInterval);
    TailDuckPerVoice = Num(j, "tailDuckPerVoice", TailDuckPerVoice);
    BurstGap = Num(j, "burstGap", BurstGap);
    if (const auto it = j.find("layers"); it != j.end() && it->is_object())
        for (auto l = it->begin(); l != it->end(); ++l) {
            Layer* layer = LayerByName(l.key());
            if (!layer || !l.value().is_object()) continue;
            const std::string key = layer->Set.Key;
            layer->Set = SoundJson::ApplySet(l.value(), layer->Set);
            layer->Set.Key = key;
            if (layer == &Tail)
                for (Layer& tc : TailClass) {
                    const std::string ck = tc.Set.Key;
                    const std::vector<std::string> cf = tc.Set.Files;
                    const bool cx = tc.Set.FilesExplicit;
                    tc.Set = SoundJson::ApplySet(l.value(), tc.Set);
                    tc.Set.Key = ck;
                    tc.Set.Files = cf; // the generic tail's files are not the class tails'
                    tc.Set.FilesExplicit = cx; // (nor its explicit flag: the manifest's tail_<class> files still load)
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
            SoundSet s = SoundJson::ApplySet(e.value(), base);
            s.Key = "snd." + Gun + "." + e.key();
            Events[e.key()] = s;
        }
}

std::string WeaponAudioProfile::ToJson() const {
    json j;
    j["gun"] = Gun;
    j["tailMinInterval"] = TailMinInterval;
    j["tailDuckPerVoice"] = TailDuckPerVoice;
    j["burstGap"] = BurstGap;
    for (const auto& [name, l] : {std::pair<const char*, const Layer*>{"close", &Close}, {"mech", &Mech}, {"sub", &Sub}, {"tail", &Tail}, {"far", &Far}}) {
        json lj = SoundJson::SetToJson(l->Set);
        lj["player2d"] = l->Player2D;
        lj["every"] = l->Every;
        lj["curve"] = {{"nearDistance", l->Curve.NearDistance}, {"farDistance", l->Curve.FarDistance},
                       {"nearWeight", l->Curve.NearWeight}, {"farWeight", l->Curve.FarWeight}};
        j["layers"][name] = lj;
    }
    for (int c = 0; c < kSpaceClassCount; ++c) j["layers"][std::string("tail_") + SpaceClassName((SpaceClass)c)] = SoundJson::SetToJson(TailClass[c].Set);
    for (const auto& [e, s] : Events) j["events"][e] = SoundJson::SetToJson(s);
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

void WeaponAudio::Reset(SoundBackend* backend, std::uint32_t seed, bool recordHistory) {
    m_Player.SetBackend(backend);
    m_Player.SetBlockedFn(nullptr);
    m_Player.StopAll();
    m_Player.Seed(seed);
    m_HistoryOn = recordHistory;
    m_History.clear();
    m_Bursts.clear();
    m_RetryAt.clear();
    m_Profiles.clear();
    m_Foley.clear();
    m_FoleyIndex.clear();
    m_Keyed.clear();
    m_Manifest = SoundManifest{};
    m_Reverb.Reset();
    m_Reverb.CacheClassIrs(m_Manifest);
    m_Bed.Start(AmbienceBedComponent{false});
    m_LastTail = -1e9;
    m_Probe = EnvironmentProbe{};
    m_LastSpace = SpaceMix{};
    m_LastDominant.clear();
    m_SpaceLines = 0;
    m_Bus = ReverbBusComponent{};
    m_Mix = AudioMixComponent{};
    m_Ducker.Reset();
    m_Player.SetDucker(&m_Ducker);
    m_Player.SetReferenceLufs(false, 0.0f);
    m_World = nullptr;
    m_PathCalls = m_Refreshes = 0;
    m_PathMicros = m_RefreshMicros = 0.0;
    for (const char* g : kBuiltInGunIds) m_Profiles[g] = WeaponAudioProfile::Default(g);
}

// Tests differ from a Play session in: the player's seed and backend, the history on, no scene (no zones, the default bus and mix,
// the dynamic mix off until a test turns it on) and the occlusion settings back to their defaults.
void WeaponAudio::StartForTest(const std::string& projectRoot, SoundBackend* backend) {
    m_Root = projectRoot;
    m_Player.SetOcclusion(SoundPlayer::OcclusionSettings{});
    Reset(backend, 12345u, true);
    m_Zones.Set({});
    m_Mix.DuckEnabled = m_Mix.FocusEnabled = m_Mix.AirEnabled = false; // (tests turn them on themselves)
    ApplyMix();
    InstallLog();
    InstallRouting();
    m_Active = true;
}

void WeaponAudio::Start(World& world, const std::string& projectRoot) {
    Stop();
    m_Root = projectRoot.empty() ? ProjectPaths::Resolve("") : projectRoot;
    Reset(nullptr, 0xA5F00Du, false);
    {
        std::ifstream in(std::filesystem::path(m_Root) / "assets/Audio/audio_manifest.json", std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            SoundManifest::FromJson(ss.str(), m_Manifest);
        }
    }
    m_Reverb.CacheClassIrs(m_Manifest);
    for (const entt::entity e : world.Registry.view<WeaponAudioComponent>()) {
        const WeaponAudioComponent& c = world.Registry.get<WeaponAudioComponent>(e);
        if (c.Gun.empty()) continue;
        auto it = m_Profiles.find(c.Gun); // (a scene's Weapon Audio component is where a gun's profile is made)
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
        FillProfile(p);
        for (const char* name : kProfileLayers)
            for (const std::string& f : p.LayerByName(name)->Set.Files) m_Player.Backend().Preload(f);
    }
    // Every manifest key is a set: variants preloaded so the first play has no decode hitch.
    for (const std::string& key : m_Manifest.Keys())
        for (const std::string& f : m_Manifest.FilesFor(key)) m_Player.Backend().Preload(f);
    InstallLog();
    m_Zones.Build(world);
    if (const auto first = world.Registry.view<ReverbBusComponent>(); first.begin() != first.end()) { // the first one counts
        const entt::entity e = *first.begin();
        m_Bus = world.Registry.get<ReverbBusComponent>(e);
    }
    if (const auto first = world.Registry.view<AudioMixComponent>(); first.begin() != first.end()) { // the first one counts
        const entt::entity e = *first.begin();
        m_Mix = world.Registry.get<AudioMixComponent>(e);
    }
    if (const auto first = world.Registry.view<AmbienceBedComponent>(); first.begin() != first.end()) // the first one counts
        m_Bed.Start(world.Registry.get<AmbienceBedComponent>(*first.begin()));
    m_Player.SetReferenceLufs(m_Manifest.HasReference, m_Manifest.ReferenceLufs);
    m_World = &world;
    InstallRouting();
    m_Player.SetBlockedFn(EnvironmentBlockedFn());
    m_Player.Backend().ConfigureReverb(m_Bus.Enabled, m_Bus.ReturnLevel, m_Bus.GlideTime);
    ApplyMix(); // the ducking, the air, the master chain (trim, glue, limiter)
    m_Reverb.PreloadIrs();
    m_Active = true;
}

void WeaponAudio::FillProfile(WeaponAudioProfile& p) {
    for (const char* name : kProfileLayers) {
        WeaponAudioProfile::Layer* l = p.LayerByName(name);
        SoundSet fresh;
        fresh.Key = l->Set.Key;
        FillSet(fresh);
        l->Set.Distance = fresh.Distance;
        l->Set.AdoptFiles(fresh, true); // (a Data File's own files win over the manifest's)
    }
    for (auto& [element, es] : p.Events) {
        SoundSet fresh;
        fresh.Key = es.Key;
        FillSet(fresh);
        es.AdoptFiles(fresh, false);
    }
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
    m_Reverb.StopAmbience();
    m_Bed.Stop(m_Player);
    m_Player.StopAll();
    m_Player.SetBlockedFn(nullptr); // (a test's line-of-sight function must not outlive its locals)
    m_Player.SetPathFn(nullptr);
    m_World = nullptr;
    m_Profiles.clear();
    m_Foley.clear();
    m_FoleyIndex.clear();
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
    m_Reverb.Update(dt, m_Listener);
    if (m_Bed.Active()) {
        SoundSet* bed = KeySet(m_Bed.Settings().Sound, [](SoundSet& s) {
            s.Loop = true;
            s.Bus = AudioEngine::Bus::Ambient;
            s.MaxVoices = AmbienceBed::kSpeakers;
            s.StealFadeTime = 0.0f;
            s.ReverbSend = 0.0f;               // the bed already holds its space
            s.Occlusion = 0;
            s.PitchMin = s.PitchMax = 1.0f;
            s.VolumeJitterDb = 0.0f;
        });
        const float indoor = AmbienceBed::IndoorShare(m_Reverb.ListenerMix(), m_Zones) *
                             (1.0f - AmbienceBed::DoorRelief(m_Zones, m_Listener, m_Bed.Settings().DoorReach));
        m_Bed.Update(dt, m_Listener, indoor, m_Player, bed);
    }
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
        vp.CutoffHz = std::max(pp.CutoffHz, 20.0f); // (the smoothing takes its log)
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
        const bool step = cat.rfind("step_", 0) == 0 || cat.rfind("barefoot_", 0) == 0 || el == "walk" || el == "run" || el == "crouch";
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

EnvironmentSettings WeaponAudio::ReverbEnvironment() const {
    if (const auto it = m_Profiles.find(kDefaultGunId); it != m_Profiles.end()) return it->second.Env;
    if (!m_Profiles.empty()) return m_Profiles.begin()->second.Env;
    return EnvironmentSettings{};
}

// A set with no recordings is looked for again at most every few seconds (files imported meanwhile), not on every use:
// each look scans the manifest and the folder, and a surface without impact takes was asked for by every round into it.
bool WeaponAudio::RetryEmptySet(const std::string& key) {
    const auto now = std::chrono::steady_clock::now();
    auto& next = m_RetryAt[key];
    if (now < next) return false;
    next = now + std::chrono::seconds(5);
    return true;
}

SoundSet* WeaponAudio::GetOrCreateSet(std::map<std::string, SoundSet>& sets, const std::string& mapKey, const std::string& setKey,
                                      const std::function<void(SoundSet&)>& init) {
    auto it = sets.find(mapKey);
    if (it != sets.end()) {
        if (it->second.Files.empty() && RetryEmptySet(setKey)) FillSet(it->second);
        return &it->second;
    }
    SoundSet s;
    s.Key = setKey;
    if (init) init(s);
    FillSet(s);
    it = sets.emplace(mapKey, std::move(s)).first;
    for (const std::string& f : it->second.Files) m_Player.Backend().Preload(f);
    return &it->second;
}

SoundSet* WeaponAudio::KeySet(const std::string& key, const std::function<void(SoundSet&)>& init) {
    return GetOrCreateSet(m_Keyed, key, key, init);
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
    return nullptr;
}

void WeaponAudio::Note(const std::string& key, bool at2D, const std::string& space) {
    if (!m_HistoryOn) return;
    m_History.push_back({m_Player.Now(), key, at2D, space});
    if (m_History.size() > kMaxHistory) m_History.pop_front();
}

int WeaponAudio::Shot(const std::string& gun, const glm::vec3& pos, bool at2D, std::uint32_t shooter) {
    if (!m_Active) return 0;
    WeaponAudioProfile* p = Profile(gun);
    if (!p || !p->Enabled) return 0;
    const float pitch = p->ShotPitchMin + (p->ShotPitchMax - p->ShotPitchMin) * m_Player.Rand01();
    const float d = at2D ? 0.0f : glm::length(pos - m_Listener);
    int started = 0;
    // Full auto: which shot of the burst this is decides which layers sound (close / sub / mech every shot, the long tails and
    // the distant report on every Nth), so a burst doesn't stack a tail per round.
    if (m_Bursts.size() > 64) // (soldiers come and go: drop the bursts long over)
        for (auto b = m_Bursts.begin(); b != m_Bursts.end();) b = m_Player.Now() - b->second.Last > 5.0 ? m_Bursts.erase(b) : std::next(b);
    Burst& burst = m_Bursts[{p, ((std::uint64_t)shooter << 1) | (at2D ? 1u : 0u)}];
    if (m_Player.Now() - burst.Last > p->BurstGap) burst.Count = 0;
    const int shotNumber = burst.Count++;
    burst.Last = m_Player.Now();
    for (WeaponAudioProfile::Layer* l : {&p->Close, &p->Mech, &p->Sub, &p->Far, &p->Tail}) {
        if (at2D && !l->Player2D) continue;
        if (l->Every > 1 && shotNumber % l->Every != 0) continue;
        const float w = at2D ? 1.0f : l->Curve.Weight(d);
        if (w < 0.01f) continue;
        SoundPlayer::Request r;
        r.Position = pos;
        r.At2D = at2D;
        r.PitchScale = pitch;
        r.Gain = p->Volume * w * (at2D ? p->PlayerGain : 1.0f);
        if (l == &p->Tail) { // the tail follows the space the shooter is in (Reverb Zone, else the probe)
            if (m_Player.Now() - m_LastTail < p->TailMinInterval) continue;
            r.Gain /= 1.0f + p->TailDuckPerVoice * (float)TailVoices(*p);
            m_LastTail = m_Player.Now();
            started += PlayTail(*p, r, shooter, pos, at2D);
            continue;
        }
        Note(l->Set.Key, at2D);
        if (m_Player.Play(l->Set, r).Started) ++started;
    }
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

// The tail of one shot: the space's tail sets (the two heaviest classes, equal-power), or the generic one.
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
    m_Reverb.ListenerProbe().DebugLines(out);
}

bool WeaponAudio::ParseKey(const std::string& name, std::string& gun, std::string& element, float* leadMs) {
    if (name.rfind("snd.", 0) != 0) return false;
    std::string rest = name.substr(4);
    if (leadMs) *leadMs = -1.0f;
    if (const size_t at = rest.find('@'); at != std::string::npos) {
        if (leadMs) *leadMs = (float)std::atof(rest.c_str() + at + 1);
        rest.resize(at);
    }
    const size_t dot = rest.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= rest.size()) return false;
    gun = rest.substr(0, dot);
    element = rest.substr(dot + 1);
    return true;
}

bool WeaponAudio::HasEventFiles(const std::string& gun, const std::string& element) {
    if (!m_Active) return false;
    WeaponAudioProfile* p = Profile(gun);
    return p && !EventSet(*p, element)->Files.empty();
}

SoundSet* WeaponAudio::EventSet(WeaponAudioProfile& p, const std::string& element) {
    return GetOrCreateSet(p.Events, element, "snd." + p.Gun + "." + element, [&](SoundSet& s) {
        s.MinDistance = p.EventMinDistance;
        s.MaxDistance = p.EventMaxDistance;
        s.MaxVoices = 4;
        s.PitchMin = 0.98f;
        s.PitchMax = 1.02f;
        s.VolumeJitterDb = 1.0f;
    });
}

SoundSet* WeaponAudio::FoleySet(const std::string& category, const std::string& element) {
    // (category, element) -> the set, so a step finds its set without building the key string (m_Foley's nodes are stable)
    auto cat = m_FoleyIndex.find(category);
    if (cat != m_FoleyIndex.end()) {
        if (const auto hit = cat->second.find(element); hit != cat->second.end()) {
            SoundSet* s = hit->second;
            if (s->Files.empty() && RetryEmptySet(s->Key)) FillSet(*s);
            return s;
        }
    }
    const std::string key = "snd.foley." + category + "." + element;
    SoundSet* set = GetOrCreateSet(m_Foley, key, key, [&](SoundSet& s) {
        const auto gun = m_Profiles.find(kDefaultGunId) != m_Profiles.end() ? m_Profiles.find(kDefaultGunId) : m_Profiles.begin();
        s.MinDistance = gun != m_Profiles.end() ? gun->second.EventMinDistance : 1.5f; // gear sounds: the gun events' reach
        s.MaxDistance = gun != m_Profiles.end() ? gun->second.EventMaxDistance : 25.0f;
        s.MaxVoices = 4;
        s.StealFadeTime = 0.05f;
    });
    if (cat == m_FoleyIndex.end()) cat = m_FoleyIndex.emplace(category, std::map<std::string, SoundSet*, std::less<>>{}).first;
    cat->second[element] = set;
    return set;
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
        const WeaponAudioProfile* p = Profile(g);
        if (p && !p->Enabled) return false;
        // A gear sound the shared foley owns (ads_in, equip ...): the same event, the foley's key. A gun with no profile
        // (no recordings of its own) still gets the default's shared gear sounds.
        static const WeaponAudioProfile kShared = WeaponAudioProfile::Default("");
        const auto& aliases = p ? p->Aliases : kShared.Aliases;
        if (const auto al = aliases.find(element); al != aliases.end() && al->second != "snd." + g + "." + element) {
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
