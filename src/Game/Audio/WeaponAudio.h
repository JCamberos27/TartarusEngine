#pragma once

#include "AudioEngine.h"
#include "AudioMix.h"
#include "EnvironmentProbe.h"
#include "ReverbZones.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct WeaponAudioComponent;
struct ReverbBusComponent;
class World;

// Engine-side weapon and foley audio. Everything here is data: a SoundSet
// is a bag of variants plus how to play them (volume, jitter, pitch, bus, 3D range, round robin, voice
// cap), a weapon profile maps animator event keys ("snd.ak.mag_out") and the shot layers (close, mech,
// sub, tail, far) onto sets, and the sets are filled from the audio manifest by key, so recording a new
// take is a file drop, never a code change.

// One playable sound: its variants and how it is played.
struct SoundSet {
    std::string Key;                       // "snd.ak.mag_out" - also the voice pool the cap counts in
    std::vector<std::string> Files;        // project-relative paths; picked round robin, never the same twice running
    float Volume = 1.0f;                   // linear gain
    float VolumeJitterDb = 0.0f;           // +- dB of random gain per play
    float PitchMin = 1.0f, PitchMax = 1.0f;// random playback rate per play
    AudioEngine::Bus Bus = AudioEngine::Bus::SFX;
    float MinDistance = 2.0f, MaxDistance = 40.0f; // 3D plays: full volume inside Min, rolled off to Max
    AudioEngine::Rolloff Rolloff = AudioEngine::Rolloff::Logarithmic;
    bool NoImmediateRepeat = true;         // round robin: a variant never plays twice in a row (with 2+ variants)
    int MaxVoices = 8;                     // simultaneous voices of this set (0 = unlimited); the oldest is stolen
    float StealFadeTime = 0.03f;           // seconds the stolen voice fades out over (0 = cut)
    bool Loop = false;
    // The reverb bus: this set's send level (linear, post fader), or -1 = by category (the Reverb Bus component's table: gun
    // tails 0, shot layers a little, foley / gear / impacts / casings more). Occlusion: -1 = by category (3D voices on),
    // 0 = off, 1 = a low-pass when geometry stands between the listener and the source.
    float ReverbSend = -1.0f;
    int Occlusion = -1;
    // Per file (parallel to Files; empty = zeros / defaults), from the manifest: where the contact transient sits in the file
    // (ms), the mix level (dB) and the true peak (dBTP). Variants of one key differ, so these are used per play, not per key.
    std::vector<float> FileAnchorMs, FileGainDb, FilePeakDb;
    std::vector<float> FileLufs;           // the files' LUFS-M max (empty: unknown), for the duck's key
    float AnchorMs(size_t i) const { return i < FileAnchorMs.size() ? FileAnchorMs[i] : 0.0f; }
    // The level contract: a voice plays at Volume x 10^(mix_db / 20) (GainLin), nothing else loudness-related on top. mix_db is the
    // manifest's (per file), or the set's own "mixDb" from a Data File (HasMixDb), which wins.
    bool HasMixDb = false;
    float MixDb = 0.0f;
    float GainLin(size_t i) const;
    float PeakLin(size_t i) const;
    bool FilesExplicit = false;            // the files came from a Data File: the manifest does not replace them
    // 3D plays of a category the mix spec places in the world (manifest mix.distance, by key): the level holds at the model's
    // reference distance, capped close up, its own slope beyond; MinDistance / MaxDistance / Rolloff above are then not used.
    DistanceModel Distance;

    static SoundSet FromJson(const std::string& key, const std::string& text, const SoundSet& base = SoundSet{});
    std::string ToJson() const;
};

// How a layer's gain follows its distance from the listener: Near at NearDistance and closer, Far at
// FarDistance and beyond, a straight line between (0 = silent, 1 = full).
struct BlendCurve {
    float NearDistance = 18.0f, FarDistance = 43.0f;
    float NearWeight = 1.0f, FarWeight = 0.0f;
    float Weight(float distance) const;
};

// One entry of tools/audio's manifest (assets/Audio/audio_manifest.json): file, key, category, layer.
struct SoundManifestEntry {
    std::string File, Key, Category, Layer;
    float AnchorMs = 0.0f, MixDb = 0.0f, PeakDb = -3.0f;
    float LufsM = 0.0f; bool HasLufs = false; // lufs_m_max: the file's loudness (the mix's level of a voice is this + its gain - the reference)
    float Rt60S = 0.0f, PreDelayMs = 0.0f; // impulse responses (layer "ir", key ir.<class>)
    bool Loop = false;
};
struct SoundManifest {
    std::vector<SoundManifestEntry> Entries;
    // Top-level mix.distance (tools/audio/recipes/mix.json "distance"): npc_shot, impact, body_fall, npc_step, npc_gear, casing, flyby.
    std::map<std::string, DistanceModel> Distance;
    float ShotOverReferenceDb = 0.0f;  // mix.shot_lufs_m - mix.reference_lufs_m: a shot layer's mix_db is relative to the unscaled shot
    float ReferenceLufs = 0.0f; bool HasReference = false; // mix.reference_lufs_m: the player's shot as played (0 dB of the mix)
    DistanceModel DistanceFor(const std::string& name) const { const auto it = Distance.find(name); return it == Distance.end() ? DistanceModel{} : it->second; }
    float DistanceRef(const std::string& name) const { return DistanceFor(name).RefM; }
    const SoundManifestEntry* FirstFor(const std::string& key) const; // the first variant of `key` (file-name order), null when none
    static bool FromJson(const std::string& text, SoundManifest& out, std::string* error = nullptr);
    // Every variant of `key`, in file-name order (empty when none).
    std::vector<std::string> FilesFor(const std::string& key) const;
    std::vector<std::string> Keys() const;
};
// "snd.ak.mag_out" -> "assets/Audio/Weapons/AKS74U/mag_out_" (the variant prefix of the contract's file layout);
// "snd.foley.concrete.walk" -> "assets/Audio/Foley/concrete/walk_". Empty when the key doesn't parse.
std::string SoundKeyFilePrefix(const std::string& key);
// The contract's directory layout as a fallback when the manifest has no entry: every <prefix>*.wav
// beside `root` (the project folder), sorted.
std::vector<std::string> SoundFilesByLayout(const std::string& root, const std::string& key);
// The gun a key or a weapon asset belongs to: "ak" for ".../AKS74U/...", "870" for Remington870,
// otherwise the lower-cased file stem.
std::string WeaponAudioGunId(const std::string& weaponPath);
std::string WeaponAudioGunFolder(const std::string& gunId);

// Where voices actually start: AudioEngine in the game, a recording fake in the unit tests.
struct SoundVoice {
    std::string File;
    float Volume = 1.0f, Pitch = 1.0f;
    bool Loop = false;
    AudioEngine::Bus Bus = AudioEngine::Bus::SFX;
    bool Spatial = false;
    glm::vec3 Position{0.0f};
    float MinDistance = 1.0f, MaxDistance = 40.0f;
    AudioEngine::Rolloff Rolloff = AudioEngine::Rolloff::Logarithmic;
    float RolloffFactor = 1.0f;   // Exponential only
    float StartOffset = 0.0f; // seconds into the file
    float ReverbSend = 0.0f;  // see SoundSet
    bool Occlusion = false;
    float OcclusionHz = 20000.0f; // the low-pass it starts with (20000 = open)
    float PortalGain = 1.0f;      // the gain after that filter (a portal's loss)
    int ReverbBus = 0;            // 1 = the remote room's reverb (heard through a portal)
    MixGroup Group = MixGroup::Default;
    float Duck = 1.0f;            // the group's duck gain when it starts (SetDuck moves it after)
};
struct SoundBackend {
    virtual ~SoundBackend() = default;
    virtual AudioEngine::SoundHandle Start(const SoundVoice& v) = 0;
    virtual void Stop(AudioEngine::SoundHandle h) = 0;
    virtual void SetVolume(AudioEngine::SoundHandle h, float volume) = 0;
    virtual void SetDuck(AudioEngine::SoundHandle, float /*gain*/, float /*seconds*/) {}
    virtual bool IsPlaying(AudioEngine::SoundHandle h) = 0;
    virtual void Preload(const std::string&) {}
    // Low-pass cutoff (Hz; >= 20000 = open) of a voice started with Occlusion; the reverb the bus runs (targets, glided).
    virtual void SetOcclusion(AudioEngine::SoundHandle, float /*cutoffHz*/, float /*gain*/ = 1.0f) {}
    virtual void SetVoicePosition(AudioEngine::SoundHandle, const glm::vec3&) {}
    virtual void SetReverbSendBus(AudioEngine::SoundHandle, int /*bus*/) {}
    virtual void SetReverb(const AudioEngine::ReverbSpec&, int /*bus*/ = 0) {}
    virtual void SetReverbBusActive(int /*bus*/, bool /*active*/) {}
    virtual void ConfigureReverb(bool /*enabled*/, float /*returnLevel*/, float /*glideSeconds*/) {}
    virtual void PreloadIr(const std::string& /*file*/) {}
    virtual void ConfigureLimiter(const LimiterSettings&) {}
    static SoundBackend& Engine(); // AudioEngine-backed; paths resolved through ProjectPaths
};

// Plays sets: round robin, jitter, voice caps with steal-oldest (and a fade for the stolen), a log.
class SoundPlayer {
public:
    struct Request {
        glm::vec3 Position{0.0f};
        bool At2D = true;       // un-positioned: first-person player, UI
        float Gain = 1.0f;      // on top of the set's volume
        float PitchScale = 1.0f;
        // >= 0: this play is an animation event that fires LeadMs before the contact it sounds for. A file whose contact
        // transient sits AnchorMs into it starts (LeadMs - AnchorMs) later, or - when that is negative - that far into
        // the file, so the transient lands on the frame whatever the variant's lead-in. < 0: start now.
        float LeadMs = -1.0f;
    };
    struct Played {
        bool Started = false;   // a voice started (false: no files, or the engine is down)
        std::string File;
        float Volume = 0.0f, Pitch = 1.0f;
        int Voices = 0;         // this set's live voices after the play
        bool Pending = false;   // scheduled (a later start, see Request::LeadMs)
        AudioEngine::SoundHandle Handle = AudioEngine::InvalidHandle;
    };
    // Every play (and every play that found no file): sim time, key, file ("" none), the set's voices, request.
    using LogFn = std::function<void(double, const std::string&, const std::string&, int, const Request&, float, float, const SoundVoice*)>;

    explicit SoundPlayer(SoundBackend* backend = nullptr) : m_Backend(backend) {}
    void SetBackend(SoundBackend* backend) { m_Backend = backend; }
    SoundBackend& Backend() { return m_Backend ? *m_Backend : SoundBackend::Engine(); }
    void SetLog(LogFn fn) { m_Log = std::move(fn); }
    void Seed(std::uint32_t s) { m_Rng = s ? s : 1u; }

    // What a set sends to the reverb bus and whether it is occluded, by its key (sets with -1 ask this). Set by WeaponAudio.
    struct Routing {
        float Send = 0.0f;
        bool Occlusion = false;
    };
    using RoutingFn = std::function<Routing(const std::string& key)>;
    void SetRouting(RoutingFn fn) { m_Routing = std::move(fn); }
    // Occlusion: a throttled line of sight from the listener to each positioned voice that wants it; a blocked one is low-passed
    // (a smoothed amount, so a source stepping behind a corner closes up rather than clicks).
    struct OcclusionSettings {
        bool Enabled = true;
        float CutoffHz = 900.0f;       // where a fully occluded voice's low-pass sits
        float Interval = 0.15f;        // seconds between checks of one voice
        int RaysPerFrame = 8;          // line-of-sight casts a frame, over every tracked voice
        float MinDistance = 3.0f;      // closer sources are never occluded
        float GlideRate = 10.0f;       // per second: how fast the amount follows a change
        float Clearance = 0.3f;        // a hit this close to the source is the source's own surface, not an occluder
        // Air absorption (every 3D voice, occluded or not): AirCutoffHz of the mix settings, folded into the same low-pass.
        bool AirEnabled = false;
        float AirStartDistance = 15.0f, AirExponent = 0.6f, AirMinHz = 4000.0f;
    };
    // The way a sound takes to a listener in another room (WeaponAudio's portals): heard from the portal, as far away as the path is long,
    // quieter and darker by the portals' open amount and the bend, with its own room's reverb (`Remote`, the source room's preset).
    struct VoicePath {
        bool ViaPortal = false;
        glm::vec3 Virtual{0.0f};
        float Gain = 1.0f;
        float CutoffHz = 20000.0f;
        ReverbPreset Remote;
    };
    using PathFn = std::function<VoicePath(const glm::vec3& listener, const glm::vec3& source)>;
    void SetPathFn(PathFn fn) { m_Path = std::move(fn); }
    int PortalVoices() const { return m_PortalVoices; }
    int PathChecks() const { return m_PathChecks; }
    const ReverbPreset& RemoteRoom() const { return m_RemoteRoom; }
    // True when something solid is between the two points.
    using BlockedFn = std::function<bool(const glm::vec3& from, const glm::vec3& to)>;
    void SetOcclusion(const OcclusionSettings& s) { m_Occ = s; }
    const OcclusionSettings& GetOcclusion() const { return m_Occ; }
    void SetBlockedFn(BlockedFn fn) { m_Blocked = std::move(fn); }
    void SetListener(const glm::vec3& p) { m_ListenerPos = p; }
    // The mix's duck gains by group (null = none); every live voice follows its group's gain.
    // A Weapon / Threat voice keys the duck with its level at the listener: the file's loudness + its gain + the distance's, against
    // the mix reference (LUFS-M of the player's shot as played). No reference: no keying.
    void SetDucker(MixDucker* d) { m_Ducker = d; }
    void SetReferenceLufs(bool has, float lufs) { m_HasRef = has; m_RefLufs = lufs; }
    // Live voices by mix group (the Audio panel).
    void GroupVoices(int (&out)[(int)MixGroup::Count]) const;
    static float AirCutoff(const OcclusionSettings& s, float distance);
    int OccludedVoices() const { return (int)m_Occluded.size(); }
    int OcclusionChecks() const { return m_OccChecks; }
    // Cutoff for an occlusion amount 0..1 (log-spaced from open to CutoffHz).
    static float OcclusionCutoff(const OcclusionSettings& s, float amount);

    Played Play(const SoundSet& set, const Request& req);
    // Advances time, fades stolen voices out and forgets finished ones. Once per frame.
    void Update(float dt);
    void StopAll();
    // Live voices of `key` (stolen ones fading out are not counted) / including them.
    int Voices(const std::string& key) const;
    int AudibleVoices(const std::string& key) const;
    double Now() const { return m_Now; }
    float Rand01();

private:
    struct Voice {
        AudioEngine::SoundHandle Handle = AudioEngine::InvalidHandle;
        double Started = 0.0;
        float Volume = 1.0f;
        float FadeLeft = -1.0f, FadeTotal = 0.0f; // >= 0: fading out
        MixGroup Group = MixGroup::Default;
        float SentDuck = 1.0f;
    };
    struct Pool {
        std::vector<Voice> Voices;
        int LastIndex = -1;
    };
    struct Pending {
        SoundSet Set;
        int Index = 0;
        Request Req;
        double Due = 0.0;
    };
    struct Tracked {
        AudioEngine::SoundHandle Handle = AudioEngine::InvalidHandle;
        glm::vec3 Pos{0.0f};
        float Amount = 0.0f, Target = 0.0f, LastCutoff = 20000.0f;
        double NextCheck = 0.0;
        float MinDist = 3.0f;
        bool Raycast = true;               // occlusion / portals (false: tracked for the air absorption only)
        bool Portal = false;               // heard through a portal
        glm::vec3 CurPos{0.0f}, TargetPos{0.0f}, SentPos{0.0f};
        float Gain = 1.0f, TargetGain = 1.0f, SentGain = 1.0f;
        float PortalCut = 20000.0f, TargetPortalCut = 20000.0f;
    };
    std::vector<Tracked> m_Occluded;
    OcclusionSettings m_Occ;
    BlockedFn m_Blocked;
    PathFn m_Path;
    int m_PortalVoices = 0, m_PathChecks = 0;
    ReverbPreset m_RemoteRoom;
    RoutingFn m_Routing;
    glm::vec3 m_ListenerPos{0.0f};
    int m_OccChecks = 0;
    size_t m_OccCursor = 0;
    void UpdateOcclusion(float dt);
    std::vector<Pending> m_Pending;
    MixDucker* m_Ducker = nullptr;
    bool m_HasRef = false;
    float m_RefLufs = 0.0f;
    Played Start(const SoundSet& set, int index, const Request& req, float seek);
    SoundBackend* m_Backend = nullptr;
    LogFn m_Log;
    std::unordered_map<std::string, Pool> m_Pools;
    double m_Now = 0.0;
    std::uint32_t m_Rng = 0x9E3779B9u;
    void Reap(Pool& p);
};

// One gun's audio: the shot layers, the gear sets keyed by element, and the blend that moves the report
// from crack to distant thump.
struct WeaponAudioProfile {
    std::string Gun = "ak";                // key prefix, snd.<Gun>.<element>
    bool Enabled = true;
    float Volume = 1.0f;                   // master gain for this gun
    float PlayerGain = 0.75f;              // the 2D (first-person) report's gain: the rifle is in your hands, not across a field
    float ShotPitchMin = 0.96f, ShotPitchMax = 1.04f; // one pitch per shot, shared by its layers so they stay coherent
    struct Layer {
        SoundSet Set;
        BlendCurve Curve;
        bool Player2D = true;              // the first-person shooter hears this layer (the far one is for those at distance)
        int Every = 1;                     // full auto: plays on every Nth shot of a burst
    };
    // close: the crack. mech: the action. sub: the low thump. tail: the room / field decay. far: the distant report.
    Layer Close, Mech, Sub, Tail, Far;
    // The tail by space (snd.<gun>.fire_tail_<class>, indexed by SpaceClass): a class with no files plays Tail (the generic
    // fire_tail) instead. They share Tail's distance curve, decimation and voice policy; each is its own voice pool.
    Layer TailClass[kSpaceClassCount];
    EnvironmentSettings Env;               // how the space is read (Weapon Audio "Environment")
    static constexpr float kMinClassWeight = 0.03f; // a class lighter than this is not played (the two heaviest are, renormalised)
    // Full auto: a tail per shot would pile up and clip. Starting one steals the oldest past Tail.Set.MaxVoices
    // (it fades out over Tail.Set.StealFadeTime), a tail never starts within TailMinInterval of the last, and each
    // voice still ringing ducks the new one by TailDuckPerVoice.
    float TailMinInterval = 0.0f;
    float TailDuckPerVoice = 0.3f;
    // Full auto: layer.Every = N plays that layer on every Nth shot of a burst (the first shot of a burst always plays all);
    // a burst ends after BurstGap seconds without a shot.
    float BurstGap = 0.4f;
    // 3D reach of the gun's gear / foley sounds in the world (Weapon Audio: Event Min / Max Distance).
    float EventMinDistance = 1.5f, EventMaxDistance = 25.0f;
    // Gear sounds that are the shared foley's (snd.foley.weapon.*) rather than the gun's own: element -> full key.
    std::map<std::string, std::string> Aliases;
    std::map<std::string, SoundSet> Events; // element -> set; an element not listed gets a default set from the manifest

    Layer* LayerByName(const std::string& name);
    static WeaponAudioProfile Default(const std::string& gun);
    void ApplyComponent(const WeaponAudioComponent& c);
    void ApplyJson(const std::string& text);
    std::string ToJson() const;
};

// Wire boxes / spheres of the scene's Reverb Zones (when `zones`) and, in Play, each shooter's probe rays for the guns with Env Debug
// Draw on: 7 floats per vertex (xyz rgba), two vertices per line. Called by the editor's collider gizmo.
void AppendAudioDebugLines(const World& world, std::vector<float>& out, bool zones);

class WeaponAudio {
public:
    static WeaponAudio& Get();

    // Builds each gun's profile (defaults, then the scene's Weapon Audio components, then their data files),
    // fills every set from the manifest, preloads. Called when Play starts.
    void Start(World& world, const std::string& projectRoot = std::string());
    void Stop();
    bool Active() const { return m_Active; }
    void Update(float dt);
    void SetListener(const glm::vec3& pos) { m_Listener = pos; }
    void SetLogFile(std::FILE* f) { m_LogFile = f; }

    // The report of a round from `gun` at `pos`: every layer weighted by `distance` (from the listener) when 3D.
    // `shooter` names who fired (any stable id, e.g. an NPC index + 1) so the tail's environment probe is cached per shooter;
    // 0 = not named: the player when `at2D`, otherwise told apart by position. Returns the number of voices started.
    int Shot(const std::string& gun, const glm::vec3& pos, bool at2D, std::uint32_t shooter = 0);

    // The space a shot from `pos` is in: Reverb Zone volumes first (layered by priority, faded at their edges), the shooter's
    // cached raycast probe for what no zone claims. Weights sum to 1; Gain is the zones' tail gain. Valid = false when the
    // gun's environment is off (the generic tail then).
    struct SpaceMix {
        bool Valid = false;
        float Weights[kSpaceClassCount] = {1.0f, 0.0f, 0.0f, 0.0f};
        float Gain = 1.0f;
        float ZoneShare = 0.0f;            // how much of the mix the zones claimed
        bool Probed = false;               // the probe's reading was part of it
        EnvironmentReading Reading;        // the probe's (when Probed)
    };
    SpaceMix ResolveSpace(const WeaponAudioProfile& p, std::uint32_t shooter, const glm::vec3& pos, bool at2D);
    const SpaceMix& LastSpace() const { return m_LastSpace; }
    EnvironmentProbe& Probe() { return m_Probe; }
    EnvironmentProbe& ListenerProbe() { return m_ListenerProbe; }
    ReverbZones& Zones() { return m_Zones; }
    // Overlay lines (zones, and the probe rays of guns with Env Debug Draw): 7 floats per vertex, two vertices per line.
    void EnvironmentDebugLines(std::vector<float>& out, bool withZones = true) const;
    // An animator event / gear sound: "snd.ak.mag_out" (or "mag_out" with `gun`), at the gun (3D) or on the player (2D).
    // False when it isn't a weapon key or the gun has no audio.
    bool PlayEvent(const std::string& name, const std::string& gun, const glm::vec3& pos, bool at2D, float gain = 1.0f);
    // The key without its "snd.<gun>." - "mag_out" - and the gun id, from an event name. False if not "snd.".
    // An event may end in "@<ms>": the lead before the contact (apply_sync_map.py), returned in `leadMs` (-1 = none).
    static bool ParseKey(const std::string& name, std::string& gun, std::string& element, float* leadMs = nullptr);
    // Has this gun's element any recorded file (so the old placeholder cues stay quiet)?
    bool HasEventFiles(const std::string& gun, const std::string& element);

    WeaponAudioProfile* Profile(const std::string& gun);
    const SoundManifest& Manifest() const { return m_Manifest; }
    SoundPlayer& Player() { return m_Player; }
    // Sets for an element/key; fills from the manifest or the file layout on first use.
    SoundSet* EventSet(WeaponAudioProfile& p, const std::string& element);
    SoundSet* FoleySet(const std::string& category, const std::string& element);
    const std::string& ProjectRoot() const { return m_Root; }
    // Resolves a set's files from the manifest / layout when it has none of its own.
    void FillSet(SoundSet& set);

    // Everything emitted since Start (sim time and key, in order): the weapon test asserts on it.
    struct Emitted { double Time = 0.0; std::string Key; bool At2D = true; std::string Space; /* tail: "outdoor_open" or "a+b" (the classes heard) */ };
    const std::deque<Emitted>& History() const { return m_History; }
    void ClearHistory() { m_History.clear(); }
    // Tests: keep every emission since Start (unbounded, unlike History); the flag survives Start / Stop.
    void SetRecording(bool on) { m_Record = on; }
    const std::vector<Emitted>& Transcript() const { return m_Transcript; }
    int ShotVoicesStarted() const { return m_ShotVoices; }
    // Tests: the manifest the class impulse responses and sets are looked up in.
    void SetManifestForTest(const SoundManifest& m) { m_Manifest = m; m_Player.SetReferenceLufs(m.HasReference, m.ReferenceLufs); }
    // Tests: starts with a given set of profiles and no scene.
    void StartForTest(const std::string& projectRoot, SoundBackend* backend);

    // --- the reverb bus ---
    // Send levels by category, and what the reverb is told as the listener moves (zones first, the probe outside them).
    ReverbBusComponent& Bus() { return m_Bus; }
    const ReverbBusComponent& Bus() const { return m_Bus; }
    // --- the dynamic mix ---
    // The settings in use (the scene's Audio Mix component at Start, or the defaults); ApplyMix after changing them live.
    AudioMixComponent& Mix() { return m_Mix; }
    void ApplyMix();
    const MixDucker& Ducker() const { return m_Ducker; }
    // Aiming down sights (the player's own): the focus state.
    void SetFocus(bool aiming) { m_Ducker.SetFocus(aiming); }
    // A loud event heard at the listener (dB re the player's own shot): ducks the groups under it.
    void KeyDuck(float levelDb) { m_Ducker.Key(levelDb); }
    // The category's send level for a set key ("snd.foley.step_wood.walk" -> footsteps ...); 0 for the gun tails.
    float SendFor(const std::string& key) const;
    // The reverb at `pos`: the zones' reverbs (layered, faded), the probe's class reverbs for what no zone claims; the two heaviest
    // impulse responses, weights summing to 1 (Count 0: no impulse response for this space, the reverb is off).
    AudioEngine::ReverbSpec ReverbAt(const glm::vec3& pos);
    const AudioEngine::ReverbSpec& CurrentReverb() const { return m_ReverbSent; }
    // The class impulse response / pre-delay (from the manifest's ir.<class>) and the final wet level of a space (dB): the Reverb Bus's
    // calibrated level for the class + the preset's trim.
    std::string ClassIr(int tailClass) const;
    float ClassPreDelayMs(int tailClass) const;
    float ClassWetDb(int tailClass) const;
    // --- zone ambience beds ---
    struct AmbienceDebug { std::string Key; float Target = 0.0f, Level = 0.0f; bool Playing = false; };
    const std::vector<AmbienceDebug>& Ambience() const { return m_AmbienceDebug; }
    // What the editor's Audio panel shows: the zones at the listener (the last Update) and the spec sent to the reverb.
    const ReverbZoneMix& ListenerMix() const { return m_ListenerMix; }
    const AudioEngine::ReverbSpec& RemoteSpec() const { return m_RemoteSent; }
    // A set by exact key (filled from the manifest on first use): the impact, casing and flyby sets use this; `init` runs once
    // on creation for the set's defaults.
    SoundSet* KeySet(const std::string& key, const std::function<void(SoundSet&)>& init = nullptr);
    // Plays a keyed set (note + play), 3D at `pos` unless at2D.
    SoundPlayer::Played PlayKeyed(SoundSet& set, const glm::vec3& pos, bool at2D, float gain, float pitchScale = 1.0f);
    // The world the zones and portals are read from each frame (Refresh): set by Start; tests set their own.
    void SetWorld(const World* w) { m_World = w; }
    SoundPlayer::VoicePath PathForTest(const glm::vec3& l, const glm::vec3& s) { return PathFor(l, s); }
    // Closest listener position the game last gave (SetListener).
    const glm::vec3& Listener() const { return m_Listener; }

private:
    bool m_Active = false;
    std::string m_Root;
    SoundManifest m_Manifest;
    std::map<std::string, WeaponAudioProfile> m_Profiles;
    std::map<std::string, SoundSet> m_Foley;
    SoundPlayer m_Player;
    glm::vec3 m_Listener{0.0f};
    std::FILE* m_LogFile = nullptr;
    std::deque<Emitted> m_History;
    std::vector<Emitted> m_Transcript;
    bool m_Record = false;
    double m_LastTail = -1e9;
    int m_ShotVoices = 0;
    struct Burst { int Count = 0; double Last = -1e9; };
    std::map<std::string, Burst> m_Bursts;
    friend class FoleyAudio;
    void Note(const std::string& key, bool at2D, const std::string& space = std::string());
    // The tail of one shot: the space's tail sets (the two heaviest classes, equal-power), or the generic one.
    void InstallRouting();
    SoundPlayer::VoicePath PathFor(const glm::vec3& listener, const glm::vec3& source);
    const World* m_World = nullptr;
    double m_RemoteHoldUntil = -1e9;
    bool m_RemoteActive = false;
    int m_PathCalls = 0;
    double m_PathMicros = 0.0, m_RefreshMicros = 0.0;
    int m_Refreshes = 0;
    SoundPlayer::BlockedFn EnvironmentBlockedFn();
    int PlayTail(WeaponAudioProfile& p, const SoundPlayer::Request& base, std::uint32_t shooter, const glm::vec3& pos, bool at2D);
    int TailVoices(const WeaponAudioProfile& p) const;
    EnvironmentProbe m_Probe;
    EnvironmentProbe m_ListenerProbe;   // the listener's space, for the reverb (its own cache and cast count)
    ReverbZones m_Zones;
    SpaceMix m_LastSpace;
    ReverbBusComponent m_Bus;
    AudioMixComponent m_Mix;
    MixDucker m_Ducker;
    AudioEngine::ReverbSpec m_ReverbSent, m_RemoteSent;
    ReverbZoneMix m_ListenerMix;
    bool m_ReverbValid = false;
    std::vector<AmbienceDebug> m_AmbienceDebug;
    struct Bed {
        std::string Key;
        AudioEngine::SoundHandle Handle = AudioEngine::InvalidHandle;
        float BaseVolume = 1.0f;     // the file's volume with its mix_db
        float Level = 0.0f;          // the fade, 0 .. the target
        float Target = 0.0f;
    };
    std::vector<Bed> m_Beds;
    void UpdateAmbience(float dt);
    bool BuildLayer(const ReverbPreset& p, float weight, AudioEngine::ReverbLayerSpec& out);
    static bool SpecDiffers(const AudioEngine::ReverbSpec& a, const AudioEngine::ReverbSpec& b);
    void StopAmbience();
    unsigned m_IrWarned = 0;         // bit per tail class: the "no impulse response" line was printed
    double m_LastReverbLog = -1e9;
    std::map<std::string, SoundSet> m_Keyed;
    static float ClampSend(float s) { return s < 0.0f ? 0.0f : s; }
    std::map<std::string, SpaceClass> m_LastDominant; // per shooter key, for the console line when it changes
    int m_SpaceLines = 0;
    void InstallLog();
};
