#pragma once

#include "AudioEngine.h"
#include "AudioMix.h"
#include "EnvironmentProbe.h"
#include "ReverbZones.h"
#include "SoundManifest.h"
#include "SoundPlayer.h"
#include "WeaponReverb.h"

#include <glm/glm.hpp>

#include <chrono>
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
// take is a file drop, never a code change. The pieces: SoundManifest (the manifest and the sets), SoundPlayer (voices, voice caps,
// occlusion), WeaponReverb (the listener's reverb and the ambience beds) and this facade (profiles, shots, events).

// How a layer's gain follows its distance from the listener: Near at NearDistance and closer, Far at
// FarDistance and beyond, a straight line between (0 = silent, 1 = full).
struct BlendCurve {
    float NearDistance = 18.0f, FarDistance = 43.0f;
    float NearWeight = 1.0f, FarWeight = 0.0f;
    float Weight(float distance) const;
};

// One gun's audio: the shot layers, the gear sets keyed by element, and the blend that moves the report
// from crack to distant thump.
struct WeaponAudioProfile {
    std::string Gun = kDefaultGunId;             // key prefix, snd.<Gun>.<element>
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
    std::FILE* LogFile() const { return m_LogFile; }

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
    const SpaceMix& LastSpace() const { return m_LastSpace; }
    EnvironmentProbe& Probe() { return m_Probe; }
    EnvironmentProbe& ListenerProbe() { return m_Reverb.ListenerProbe(); }
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

    // A gun's profile, or nullptr for one the scene never gave audio (lookup only: unknown names make nothing).
    WeaponAudioProfile* Profile(const std::string& gun);
    const SoundManifest& Manifest() const { return m_Manifest; }
    SoundPlayer& Player() { return m_Player; }
    // Sets for an element/key; fills from the manifest or the file layout on first use.
    SoundSet* EventSet(WeaponAudioProfile& p, const std::string& element);
    SoundSet* FoleySet(const std::string& category, const std::string& element);
    // Resolves a set's files from the manifest / layout when it has none of its own.
    void FillSet(SoundSet& set) { m_Manifest.FillSet(set, m_Root); }

    // Everything emitted since Start (sim time and key, in order): the weapon test asserts on it.
    struct Emitted { double Time = 0.0; std::string Key; bool At2D = true; std::string Space; /* tail: "outdoor_open" or "a+b" (the classes heard) */ };
    const std::deque<Emitted>& History() const { return m_History; }
    void ClearHistory() { m_History.clear(); }
    // Recording the history costs two string copies per layer played, so it is off in the game; StartForTest turns it on.
    void SetHistoryEnabled(bool on) { m_HistoryOn = on; if (!on) m_History.clear(); }
    bool HistoryEnabled() const { return m_HistoryOn; }
    // Tests: the manifest the class impulse responses and sets are looked up in.
    void SetManifestForTest(const SoundManifest& m) { m_Manifest = m; m_Player.SetReferenceLufs(m.HasReference, m.ReferenceLufs); m_Reverb.CacheClassIrs(m_Manifest); }
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
    // The category's send level for a set key ("snd.foley.step_wood.walk" -> footsteps ...); 0 for the gun tails.
    float SendFor(const std::string& key) const;
    // The reverb in use, the class impulse responses (WeaponReverb has the rules) and the zone ambience beds.
    const AudioEngine::ReverbSpec& CurrentReverb() const { return m_Reverb.CurrentReverb(); }
    std::string ClassIr(int tailClass) const { return m_Reverb.ClassIr(tailClass); }
    float ClassPreDelayMs(int tailClass) const { return m_Reverb.ClassPreDelayMs(tailClass); }
    using AmbienceDebug = WeaponReverb::AmbienceDebug;
    const std::vector<AmbienceDebug>& Ambience() const { return m_Reverb.Ambience(); }
    // What the editor's Audio panel shows: the zones at the listener (the last Update).
    const ReverbZoneMix& ListenerMix() const { return m_Reverb.ListenerMix(); }
    // The settings the listener's probe reads the space by: the default gun's, else any profile's, else the defaults.
    EnvironmentSettings ReverbEnvironment() const;
    // A set by exact key (filled from the manifest on first use): the impact, casing and flyby sets use this; `init` runs once
    // on creation for the set's defaults.
    SoundSet* KeySet(const std::string& key, const std::function<void(SoundSet&)>& init = nullptr);
    // Plays a keyed set (note + play), 3D at `pos` unless at2D.
    SoundPlayer::Played PlayKeyed(SoundSet& set, const glm::vec3& pos, bool at2D, float gain, float pitchScale = 1.0f);
    // The world the zones and portals are read from each frame (Refresh): set by Start; tests set their own.
    void SetWorld(const World* w) { m_World = w; }
    SoundPlayer::VoicePath PathForTest(const glm::vec3& l, const glm::vec3& s) { return PathFor(l, s); }

private:
    bool m_Active = false;
    std::string m_Root;
    SoundManifest m_Manifest;
    std::map<std::string, WeaponAudioProfile> m_Profiles;
    std::map<std::string, SoundSet> m_Foley;
    std::map<std::string, std::map<std::string, SoundSet*, std::less<>>, std::less<>> m_FoleyIndex; // category -> element -> the set in m_Foley
    SoundPlayer m_Player;
    glm::vec3 m_Listener{0.0f};
    std::FILE* m_LogFile = nullptr;
    std::deque<Emitted> m_History;
    bool m_HistoryOn = false;
    double m_LastTail = -1e9;
    struct Burst { int Count = 0; double Last = -1e9; };
    // One burst per gun and shooter (and 2D / 3D); the tail cooldown (m_LastTail) stays shared.
    std::map<std::pair<const WeaponAudioProfile*, std::uint64_t>, Burst> m_Bursts;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_RetryAt; // empty sets: when to look again
    bool RetryEmptySet(const std::string& key);
    friend class FoleyAudio;
    void Note(const std::string& key, bool at2D, const std::string& space = std::string());
    // Fresh state for a new session, shared by Start and StartForTest: the backend and the player's seed, nothing cached, the
    // built-in guns' default profiles, the default bus and mix.
    void Reset(SoundBackend* backend, std::uint32_t seed, bool recordHistory);
    // The set under `setKey` in `sets` (an element / category / key map of nodes that stay put), made on first use: its defaults by
    // `init`, its files from the manifest, preloaded. A set with no files is looked for again every few seconds.
    SoundSet* GetOrCreateSet(std::map<std::string, SoundSet>& sets, const std::string& mapKey, const std::string& setKey,
                             const std::function<void(SoundSet&)>& init);
    // The profile's sets (layers, tail classes, events) from the manifest, a Data File's own files first.
    void FillProfile(WeaponAudioProfile& p);
    // The space a shot is in (zones, then the shooter's probe).
    SpaceMix ResolveSpace(const WeaponAudioProfile& p, std::uint32_t shooter, const glm::vec3& pos, bool at2D);
    void InstallRouting();
    SoundPlayer::VoicePath PathFor(const glm::vec3& listener, const glm::vec3& source);
    const World* m_World = nullptr;
    int m_PathCalls = 0;
    double m_PathMicros = 0.0, m_RefreshMicros = 0.0;
    int m_Refreshes = 0;
    SoundPlayer::BlockedFn EnvironmentBlockedFn();
    int PlayTail(WeaponAudioProfile& p, const SoundPlayer::Request& base, std::uint32_t shooter, const glm::vec3& pos, bool at2D);
    int TailVoices(const WeaponAudioProfile& p) const;
    EnvironmentProbe m_Probe;
    WeaponReverb m_Reverb{*this};
    ReverbZones m_Zones;
    SpaceMix m_LastSpace;
    ReverbBusComponent m_Bus;
    AudioMixComponent m_Mix;
    MixDucker m_Ducker;
    std::map<std::string, SoundSet> m_Keyed;
    static float ClampSend(float s) { return s < 0.0f ? 0.0f : s; }
    std::map<std::string, SpaceClass> m_LastDominant; // per shooter key, for the console line when it changes
    int m_SpaceLines = 0;
    void InstallLog();
};
