#pragma once

#include "AudioEngine.h"
#include "AudioMix.h"
#include "Components.h"
#include "SoundManifest.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

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
        // Per-play replacements of the set's pitch range and volume jitter (Override) and of its 3D reach (OverrideDistance), so a
        // caller need not copy the whole set to change them.
        bool Override = false;
        float OverridePitchMin = 1.0f, OverridePitchMax = 1.0f, OverrideJitterDb = 0.0f;
        bool OverrideDistance = false;
        float OverrideMinDistance = 2.0f, OverrideMaxDistance = 40.0f;
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
        double NextPath = 0.0;      // the portal path search runs once an interval, on its own clock (it takes no ray budget)
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
    int m_PortalVoices = 0;
    ReverbPreset m_RemoteRoom;
    RoutingFn m_Routing;
    glm::vec3 m_ListenerPos{0.0f};
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
