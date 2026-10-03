#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>
#include "MasterLimiter.h"

// Thin wrapper over miniaudio's ma_engine: loads sounds by path (wav/mp3/flac/ogg) and plays
// them as individually addressable voices.
//
// Every Play() returns a SoundHandle that stays valid until the voice stops (or is stopped) and
// gets reaped by Update(). Handles are generation-tagged: the low bits index a voice slot, the
// high bits carry the generation that slot was handed out at. A handle to a finished voice
// therefore resolves to null rather than addressing whatever sound later recycled that slot, so
// holding a stale handle is harmless — Stop/SetVolume/SetPosition on it are no-ops and
// IsPlaying() returns false.
class AudioEngine {
public:
    using SoundHandle = uint32_t;
    static constexpr SoundHandle InvalidHandle = 0;

    static void Init();
    static void Shutdown();

    // Reaps voices that have finished playing (uninit + free their slot) so repeated Play/Stop
    // cycles don't accumulate ma_sound objects and open streaming file handles. Call once per
    // frame; main.cpp does.
    static void Update();

    // Decodes a sound fully into memory up front (#202) so the first Play() of it has no
    // disk-read/decode hitch — worth it for short SFX/UI sounds, not for music/ambience (which
    // should keep streaming; simply never call Load() on those). Idempotent: a path already
    // preloaded returns true immediately without re-decoding. Returns false if the file doesn't
    // exist or can't be decoded.
    static bool Load(const std::string& path);
    // Frees one path's preloaded PCM data (or all of them). Safe to call while a Play()'d
    // instance of that sound is still audible — voices share the decoded data by reference
    // (#171), so the PCM lives until the last voice playing it is reaped.
    static bool Unload(const std::string& path);
    static void UnloadAll();

    // Starts a voice and returns its handle (InvalidHandle if the engine is down or the file
    // failed to load). Voices start unspatialized — full volume regardless of listener position,
    // which is what UI/one-shot sounds want. Call SetPosition() to turn a voice into a 3D source.
    // #171 - mixer buses (Unity's Audio Mixer groups, fixed set). Every voice plays through one;
    // its volume scales everything on that bus. Values are serialized by index.
    enum class Bus { SFX = 0, Music = 1, Ambient = 2, UI = 3, Voice = 4 };
    // #171 - Unity's rolloff modes. Logarithmic = miniaudio's inverse model; Linear fades to silence exactly at maxDistance;
    // Exponential = (d / min)^-rolloff past min: a fixed dB per doubling of distance (6.02 x rolloff), the mix's distance model.
    enum class Rolloff { Logarithmic = 0, Linear = 1, Exponential = 2 };
    static constexpr int kBusCount = 5;
    static void SetBusVolume(Bus bus, float volume);  // 0..1
    static float BusVolume(Bus bus);
    static void SetMasterVolume(float volume);        // 0..1, applied on top of every bus
    static float MasterVolume();

    // startOffsetSeconds > 0 starts the voice that far into the file (skips a lead-in: weapon sounds whose contact
    // transient has to land on an animation frame that is nearer than the file's lead).
    // Per-voice processing built into the voice's node chain when it starts: a send into the reverb bus (0 = none: the voice
    // is not routed through a splitter at all) and a low-pass for occlusion (SetOcclusion drives its cutoff).
    struct VoiceFx {
        float ReverbSend = 0.0f;   // linear level of the voice into the reverb (post fader, post spatialisation)
        bool Occlusion = false;
        float Cutoff = 20000.0f;   // the occlusion low-pass it starts with (Hz), and the gain after it (so the first block is already right)
        float Gain = 1.0f;
        int ReverbBus = 0;         // which reverb the send feeds (see SetReverbSendBus)
        // Set before the voice starts, so its first block is already placed, pitched and ducked (setting them after Play() let
        // the audio thread render a block unspatialised at the voice's full calibrated gain).
        float Pitch = 1.0f;
        float Duck = 1.0f;         // the mix's duck gain (the voice's fader; SetDuck moves it)
        bool Spatial = false;
        glm::vec3 Position{0.0f};
        Rolloff RolloffMode = Rolloff::Logarithmic;
        float MinDistance = 1.0f, MaxDistance = 40.0f;
        float RolloffFactor = 1.0f; // Exponential only
    };
    static SoundHandle Play(const std::string& path, float volume = 1.0f, bool loop = false, Bus bus = Bus::SFX,
                            float startOffsetSeconds = 0.0f, const VoiceFx* fx = nullptr);
    // Occlusion low-pass of a voice that was started with VoiceFx::Occlusion: cutoff in Hz (>= 20000 or <= 0 = open).
    // `gain` (linear) scales the voice after the filter - its dry and its reverb send alike (a portal's loss).
    static void SetOcclusion(SoundHandle handle, float cutoffHz, float gain = 1.0f);
    // Which reverb a voice's send feeds: 0 = the listener's room (the default), 1 = the "remote room" bus (a sound heard through a
    // portal carries its own room's reverb: its params are set separately, SetReverb(p, 1)).
    static void SetReverbSendBus(SoundHandle handle, int bus);
    // The send level of a voice that was started with a ReverbSend > 0.
    static void SetReverbSend(SoundHandle handle, float level);

    // The reverb bus: a send every voice with a ReverbSend feeds, one convolution reverb (Audio/ConvolutionReverb, real recorded
    // impulse responses) summed into the output through the master limiter. A space is up to two weighted layers; the DSP glides to
    // what it is told (a change of space is an equal-power crossfade, no clicks), so it is safe to call from the game thread every
    // frame. A layer whose IR cannot be loaded is dropped; with no layers the reverb fades to silence.
    struct ReverbLayerSpec {
        std::string Ir;                // the impulse response file (resolved path)
        float Weight = 1.0f;           // crossfade weight (the layers' weights sum to 1)
        float WetDb = 0.0f;            // return level of this layer (dB)
        float PreDelayMs = 0.0f;
        float HfDampDb = 0.0f;         // attenuation (dB) of a high shelf at 4 kHz on the reverb input
        float LowCutHz = 0.0f;         // high-pass on the reverb input (0 = off)
    };
    struct ReverbSpec {
        int Count = 0;
        ReverbLayerSpec Layers[2];
    };
    // Decodes and prepares an impulse response ahead of its first use (the first SetReverb naming it would otherwise do it on the
    // game thread). False if the file is missing or cannot be decoded.
    static bool PreloadIr(const std::string& path);
    static void SetReverbEnabled(bool enabled, int bus = 0); // bus 1 (the remote room) is off until a portal voice needs it
    static bool ReverbEnabled(int bus = 0);
    static void SetReverb(const ReverbSpec& target, int bus = 0);
    static void SetReverbReturn(float level); // linear gain of the wet signal into the output (times the SFX bus volume)
    static void SetReverbGlide(float seconds); // how long the reverb takes to follow a change of space (the crossfade)
    struct ReverbStats {
        std::uint64_t Callbacks = 0;
        double TotalMicros = 0.0, MaxMicros = 0.0;
        int Frames = 0;            // frames of the last callback
        std::uint64_t WorkerJobs = 0, LateBlocks = 0; // tail blocks run on the worker thread / not finished in time
        double WorkerMicros = 0.0;
        int ActiveConvolvers = 0;
    };
    static ReverbStats GetReverbStats(int bus = 0);
    static void ResetReverbStats();
    // What the reverb is doing now (the editor's Audio panel): the IRs it holds, which run and at what weight.
    struct ReverbInfo {
        int Instances = 0;
        struct Entry { std::string Ir; bool Running = false; float Weight = 0.0f; };
        std::vector<Entry> Entries;
    };
    static ReverbInfo GetReverbInfo(int bus = 0);

    // The master peak limiter: the last node before the output (after every bus and the reverb returns).
    static void SetMasterLimiter(const LimiterSettings& s);
    static LimiterSettings GetMasterLimiter();
    static float TakeLimiterGainReductionDb(); // the deepest gain reduction since the last call (dB, >= 0)
    static float TakeGlueGainReductionDb();    // the glue compressor's, the same way

    // Level meters: the master (after the limiter), each mixer bus, the reverb return. K-weighted loudness and peak.
    enum MeterId { MeterMaster = 0, MeterBusFirst = 1, MeterWet = 1 + kBusCount, MeterCount = 2 + kBusCount };
    struct MeterReading { float PeakDb = -120.0f, MomentaryLufs = -120.0f, ShortTermLufs = -120.0f; };
    static MeterReading GetMeter(int id);
    static int VoiceCount(); // voices that have been started and not yet reaped

    // Capture. AUDIO_CAPTURE=<wav path> in the environment records the master output (after the limiter) as a float WAV for the whole
    // session. EnableTaps keeps the last seconds of three streams for DrainTaps (the --audio-test): the master output, the signal
    // entering the limiter (dry + wet) and the reverb returns alone.
    static bool StartCapture(const std::string& wavPath);
    static void StopCapture();
    static void EnableTaps(bool on);
    static size_t DrainTaps(std::vector<float>& master, std::vector<float>& beforeLimiter, std::vector<float>& wet); // appends stereo frames
    static int SampleRate();

    // An engine with no audio device that renders on demand (the --audio-test, unit tests): deterministic and faster than real
    // time. Everything else (voices, buses, the reverb, the limiter) is the same code; the reverb's tail runs inline.
    static void InitOffline(int sampleRate = 48000);
    static void RenderOffline(float* interleavedStereo, int frames);
    static bool IsOffline();

    // All no-ops / false for a stale or invalid handle.
    static void Stop(SoundHandle handle);
    static void SetVolume(SoundHandle handle, float volume);
    // The mix's duck gain on a voice (linear, on top of its volume), ramped over `seconds` from where it is now.
    static void SetDuck(SoundHandle handle, float gain, float seconds);
    static void SetPitch(SoundHandle handle, float pitch);
    static bool IsPlaying(SoundHandle handle);

    // Placing a voice in the world implicitly enables spatialization on it.
    static void SetPosition(SoundHandle handle, const glm::vec3& position);
    // Distance attenuation for a positioned voice: full volume within minDistance, silent-ish
    // past maxDistance, rolloff shaping the curve between them (miniaudio's inverse model).
    static void SetAttenuation(SoundHandle handle, float minDistance, float maxDistance,
                               float rolloff = 1.0f);
    static void SetRolloff(SoundHandle handle, Rolloff mode, float minDistance, float maxDistance, float factor = 1.0f);
    // Turns positional audio off (2D: no panning, no distance falloff) or back on.
    static void SetSpatial(SoundHandle handle, bool spatial);
    // #171 - Doppler. Unity's Doppler Level (0 = off, 1 = physical): scales the pitch shift from
    // the voice's and the listener's velocities (metres per second, set every frame by the caller).
    static void SetDopplerLevel(SoundHandle handle, float level);
    static void SetVelocity(SoundHandle handle, const glm::vec3& velocity);
    static void SetListenerVelocity(const glm::vec3& velocity);
    // Velocity from two frame positions: zero on the first frame (dt <= 0) and for anything faster
    // than kMaxDopplerSpeed, which is a teleport, not motion, and would otherwise shriek.
    static constexpr float kMaxDopplerSpeed = 100.0f;
    static glm::vec3 FrameVelocity(const glm::vec3& prev, const glm::vec3& cur, float dt) {
        if (dt <= 1e-6f) return glm::vec3(0.0f);
        const glm::vec3 v = (cur - prev) / dt;
        return glm::dot(v, v) > kMaxDopplerSpeed * kMaxDopplerSpeed ? glm::vec3(0.0f) : v;
    }

    // Drives the 3D listener. Called once per frame from the Play-mode camera; while not
    // playing, the listener simply stays wherever it was last put.
    static void SetListener(const glm::vec3& position, const glm::vec3& forward,
                            const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f));

    static void StopAll();

    // Editor Pause / Error Pause (#171): stops every audible voice in place and resumes them
    // from the same position on SetPaused(false). Paused voices aren't reaped by Update(). The
    // Asset Browser preview is unaffected. Idempotent, so it can be driven every frame.
    static void SetPaused(bool paused);
    static bool IsPaused();

    // Master mute — silences the whole engine output without stopping any voice (they keep
    // their playback position, so unmuting resumes mid-clip). Used by the editor's
    // View ▸ Mute Audio toggle (#236 R2 toolbar tail).
    static void SetMuted(bool muted);
    static bool IsMuted();

    // Decodes `path` and fills `outPeaks` with `buckets` normalized (0..1) max-amplitude
    // values — a cheap waveform envelope for the Asset Browser's sound cells (#236 G).
    // Returns false if the file can't be decoded; `outPeaks` is left empty then.
    static bool WaveformPeaks(const std::string& path, int buckets, std::vector<float>& outPeaks);

    // Asset Browser preview playback: at most one preview plays at a time — starting a new
    // one (even for a different file) stops whatever was previewing before it, so a Play/Stop
    // toggle per row always reflects reality.
    static void PlayPreview(const std::string& path);
    static void StopPreview();
    static bool IsPreviewPlaying(const std::string& path);

    static bool IsInitialized();

private:
    AudioEngine() = delete;
};
