#pragma once

#include "AudioEngine.h"
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
class World;

// Engine-side weapon and foley audio (docs/AUDIO.md, "Engine"). Everything here is data: a SoundSet
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
    // Per file (parallel to Files; empty = zeros / defaults), from the manifest: where the contact transient sits in the file
    // (ms), the mix level (dB) and the true peak (dBTP). Variants of one key differ, so these are used per play, not per key.
    std::vector<float> FileAnchorMs, FileGainDb, FilePeakDb;
    float AnchorMs(size_t i) const { return i < FileAnchorMs.size() ? FileAnchorMs[i] : 0.0f; }
    float GainLin(size_t i) const;
    float PeakLin(size_t i) const;

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
    bool Loop = false;
};
struct SoundManifest {
    std::vector<SoundManifestEntry> Entries;
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
    float StartOffset = 0.0f; // seconds into the file
};
struct SoundBackend {
    virtual ~SoundBackend() = default;
    virtual AudioEngine::SoundHandle Start(const SoundVoice& v) = 0;
    virtual void Stop(AudioEngine::SoundHandle h) = 0;
    virtual void SetVolume(AudioEngine::SoundHandle h, float volume) = 0;
    virtual bool IsPlaying(AudioEngine::SoundHandle h) = 0;
    virtual void Preload(const std::string&) {}
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
    // The weapon bus's peak limiter, as gain ducking (there is no DSP in AudioEngine): every voice's peak (file true peak x
    // its volume) counts toward a running estimate that decays over `window` seconds (a transient's life); a voice that
    // would push the estimate past the ceiling is played quieter, down to minGain.
    struct Limiter {
        bool Enabled = true;
        float CeilingDb = -1.0f;
        float Window = 0.15f;
        float MinGain = 0.1f;
    };
    void SetLimiter(const Limiter& l) { m_Limiter = l; }
    const Limiter& GetLimiter() const { return m_Limiter; }
    // The estimate right now (linear), for tests / the log.
    float LimiterLoad() const;
    void Seed(std::uint32_t s) { m_Rng = s ? s : 1u; }

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
    struct Peak {
        double Time = 0.0;
        float Amp = 0.0f;
    };
    std::vector<Pending> m_Pending;
    std::vector<Peak> m_Peaks;
    Limiter m_Limiter;
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
    // Gear sounds that are the shared foley's (snd.foley.weapon.*) rather than the gun's own: element -> full key.
    std::map<std::string, std::string> Aliases;
    std::map<std::string, SoundSet> Events; // element -> set; an element not listed gets a default set from the manifest

    Layer* LayerByName(const std::string& name);
    static WeaponAudioProfile Default(const std::string& gun);
    void ApplyComponent(const WeaponAudioComponent& c);
    void ApplyJson(const std::string& text);
    std::string ToJson() const;
};

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
    ReverbZones& Zones() { return m_Zones; }
    // Overlay lines (zones, and the probe rays of guns with Env Debug Draw): 7 floats per vertex, two vertices per line.
    void EnvironmentDebugLines(std::vector<float>& out) const;
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
    // Tests: starts with a given set of profiles and no scene.
    void StartForTest(const std::string& projectRoot, SoundBackend* backend);

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
    int PlayTail(WeaponAudioProfile& p, const SoundPlayer::Request& base, std::uint32_t shooter, const glm::vec3& pos, bool at2D);
    int TailVoices(const WeaponAudioProfile& p) const;
    EnvironmentProbe m_Probe;
    ReverbZones m_Zones;
    SpaceMix m_LastSpace;
    std::map<std::string, SpaceClass> m_LastDominant; // per shooter key, for the console line when it changes
    int m_SpaceLines = 0;
    void InstallLog();
};
