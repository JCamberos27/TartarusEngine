#include "AudioEngine.h"
#include "Log.h"
#include "AssetDatabase.h"
#include "ProjectPaths.h"
#include "ConvolutionReverb.h"
#include "LoudnessMeter.h"
#include "MasterLimiter.h"
#include "miniaudio.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
ma_engine s_Engine;
bool s_Initialized = false;

// Fully-decoded PCM data for a preloaded sound (#202), kept in the engine's own output format so
// playing it needs no runtime resample/convert. Shared (#171): every voice playing the clip reads
// this one buffer through a non-owning ma_audio_buffer and holds a reference, so Unload()ing a
// path can never invalidate a voice already playing from it. Each Play() used to deep-copy the
// whole PCM buffer, allocating megabytes per rapid-fire SFX shot.
struct PreloadedClip {
    std::vector<uint8_t> Data;
    ma_format Format = ma_format_unknown;
    ma_uint32 Channels = 0;
    ma_uint32 SampleRate = 0;
    ma_uint64 FrameCount = 0;
};
std::unordered_map<std::string, std::shared_ptr<const PreloadedClip>> s_Preloaded;

// One voice slot per live (or recently live) sound. Slots are recycled rather than erased so a
// handle's index stays meaningful; Generation is bumped on every reuse so handles into a
// previous occupant of the slot no longer resolve.
struct Voice {
    std::unique_ptr<ma_sound> Sound;         // null when the slot is free
    std::unique_ptr<ma_lpf_node> Lpf;        // occlusion low-pass, between the sound and its splitter / bus
    double LpfHz = 0.0;                      // its cutoff now (SetOcclusion skips a reinit that changes nothing)
    std::unique_ptr<ma_splitter_node> Split; // output 0: the bus, output 1: the reverb send (its volume is the send level)
    std::unique_ptr<ma_audio_buffer> Buffer; // non-null only for a voice playing preloaded data (#202)
    std::shared_ptr<const PreloadedClip> Clip; // keeps Buffer's PCM alive past Unload() (#171)
    uint16_t Generation = 1;                 // never 0, so a live handle is never InvalidHandle
    bool Looping = false;
    bool Paused = false;                     // stopped by SetPaused(true); resumed, not reaped (#171)
};

std::vector<Voice> s_Voices;

// --- the output graph ----------------------------------------------------------------------------------------------
// sound groups (the mixer buses) -> a meter node each -> the MASTER LIMITER node -> the endpoint, and the two reverb returns -> the limiter.
// All three kinds are pass-through / processing nodes written here; miniaudio sums what is attached to an input bus.

// Taps (the --audio-test, AUDIO_CAPTURE): three equal rings of stereo frames. The reverb nodes add their return at the position the
// limiter is about to write; the limiter writes the other two and advances the count (they run in that order within one graph read).
struct TapRing {
    static constexpr unsigned long long kFrames = 1ull << 19; // 10.9 s at 48 kHz
    std::vector<float> Master, Pre, Wet;                      // kFrames x 2 each
    std::atomic<unsigned long long> Written{0};
    unsigned long long Read = 0;
    unsigned long long Lost = 0;
};
std::atomic<TapRing*> s_Taps{nullptr};
std::unique_ptr<TapRing> s_TapStorage;

struct MeterNode {
    ma_node_base Base;
    LoudnessMeter* Meter = nullptr;
};
struct LimiterNode {
    ma_node_base Base;
    MasterLimiter* Lim = nullptr;
    LoudnessMeter* Master = nullptr;
    std::vector<float> Scratch;
    // Settings from the game thread: written under Lock, picked up by the audio thread when Dirty and the lock is free (it never waits).
    std::mutex Lock;
    LimiterSettings Pending;
    std::atomic<bool> Dirty{false};
    std::atomic<float> LastGrDb{0.0f}, LastGlueGrDb{0.0f};
};
LimiterNode* s_Limiter = nullptr;
MeterNode* s_BusMeters[AudioEngine::kBusCount] = {};

void MeterProcess(ma_node* pNode, const float** ppIn, ma_uint32* pFrameCountIn, float** ppOut, ma_uint32* pFrameCountOut) {
    MeterNode* n = reinterpret_cast<MeterNode*>(pNode);
    const ma_uint32 frames = *pFrameCountOut;
    float* out = ppOut[0];
    if (ppIn && ppIn[0]) std::copy(ppIn[0], ppIn[0] + 2 * (size_t)frames, out);
    else std::fill(out, out + 2 * (size_t)frames, 0.0f);
    if (n->Meter) n->Meter->Process(out, (int)frames);
    (void)pFrameCountIn;
}
ma_node_vtable s_MeterVtable = {MeterProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};

void LimiterProcess(ma_node* pNode, const float** ppIn, ma_uint32* pFrameCountIn, float** ppOut, ma_uint32* pFrameCountOut) {
    LimiterNode* n = reinterpret_cast<LimiterNode*>(pNode);
    const ma_uint32 frames = *pFrameCountOut;
    float* out = ppOut[0];
    const float* in = ppIn && ppIn[0] ? ppIn[0] : nullptr;
    if (!in) {
        if (n->Scratch.size() < 2 * (size_t)frames) n->Scratch.assign(2 * (size_t)frames, 0.0f); // (grows on the first callbacks only)
        std::fill(n->Scratch.begin(), n->Scratch.begin() + 2 * (size_t)frames, 0.0f);
        in = n->Scratch.data();
    }
    if (n->Dirty.load(std::memory_order_acquire) && n->Lock.try_lock()) {
        const LimiterSettings want = n->Pending;
        n->Dirty.store(false, std::memory_order_relaxed);
        n->Lock.unlock();
        n->Lim->Configure(want);
    }
    TapRing* taps = s_Taps.load(std::memory_order_acquire);
    unsigned long long w = 0;
    if (taps) {
        w = taps->Written.load(std::memory_order_relaxed);
        for (ma_uint32 i = 0; i < frames; ++i) {
            const size_t at = (size_t)((w + i) & (TapRing::kFrames - 1)) * 2;
            taps->Pre[at] = in[2 * (size_t)i];
            taps->Pre[at + 1] = in[2 * (size_t)i + 1];
        }
    }
    n->Lim->Process(in, out, (int)frames);
    n->Master->Process(out, (int)frames);
    const float gr = n->Lim->TakeGainReductionDb();
    if (gr > 0.0f) n->LastGrDb.store(std::max(gr, n->LastGrDb.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    const float glue = n->Lim->TakeGlueReductionDb();
    if (glue > 0.0f) n->LastGlueGrDb.store(std::max(glue, n->LastGlueGrDb.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    if (taps) {
        for (ma_uint32 i = 0; i < frames; ++i) {
            const size_t at = (size_t)((w + i) & (TapRing::kFrames - 1)) * 2;
            taps->Master[at] = out[2 * (size_t)i];
            taps->Master[at + 1] = out[2 * (size_t)i + 1];
        }
        taps->Written.store(w + frames, std::memory_order_release);
    }
    (void)pFrameCountIn;
}
ma_node_vtable s_LimiterVtable = {LimiterProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};

// --- the reverb bus ------------------------------------------------------------------------------------------------
// One node, input bus 0 = every send (miniaudio sums what is attached to an input bus), output bus 0 -> the limiter.
// The game thread states where the reverb is heading (ConvolutionReverb::SetTarget); the audio callback glides there.
struct ReverbNode {
    ma_node_base Base;
    ConvolutionReverb* Verb = nullptr;
    LoudnessMeter* Meter = nullptr;
    std::atomic<float> Return{1.0f};    // linear gain of the wet signal (the reverb return level x the SFX bus volume)
    std::atomic<bool> Enabled{true};
    std::atomic<std::uint64_t> Callbacks{0};
    std::atomic<double> TotalMicros{0.0}, MaxMicros{0.0};
    std::atomic<int> LastFrames{0};
    std::map<std::string, int> IrIndex; // game thread only: path -> the convolver's index
    std::vector<std::string> IrPaths;   // game thread only: by index
};
ReverbNode* s_Reverbs[2] = {nullptr, nullptr}; // 0: the listener's room, 1: the remote room (a portal voice's own)
bool s_ReverbInit = false;
float s_ReverbReturn = 1.0f;
float s_ReverbGlide = 0.35f;
bool s_Offline = false;
std::map<std::string, std::shared_ptr<const IrData>> s_IrCache; // game thread

void ReverbProcess(ma_node* pNode, const float** ppIn, ma_uint32* pFrameCountIn, float** ppOut, ma_uint32* pFrameCountOut) {
    ReverbNode* n = reinterpret_cast<ReverbNode*>(pNode);
    const ma_uint32 frames = *pFrameCountOut;
    float* out = ppOut[0];
    (void)pFrameCountIn;
    if (!n->Verb || !n->Enabled.load(std::memory_order_relaxed)) {
        std::fill(out, out + 2 * (size_t)frames, 0.0f);
        if (n->Meter) n->Meter->Process(out, (int)frames);
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    static thread_local std::vector<float> silence;
    const float* in = ppIn && ppIn[0] ? ppIn[0] : nullptr;
    if (!in) {
        if (silence.size() < 2 * (size_t)frames) silence.assign(2 * (size_t)frames, 0.0f); // (grows once, on the first callback)
        in = silence.data();
    }
    n->Verb->Process(in, out, (int)frames);
    const float gain = n->Return.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 2 * (size_t)frames; ++i) out[i] *= gain;
    if (n->Meter) n->Meter->Process(out, (int)frames);
    if (TapRing* taps = s_Taps.load(std::memory_order_acquire)) {
        const unsigned long long w = taps->Written.load(std::memory_order_relaxed);
        for (ma_uint32 i = 0; i < frames; ++i) {
            const size_t at = (size_t)((w + i) & (TapRing::kFrames - 1)) * 2;
            taps->Wet[at] += out[2 * (size_t)i];
            taps->Wet[at + 1] += out[2 * (size_t)i + 1];
        }
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    n->Callbacks.fetch_add(1, std::memory_order_relaxed);
    n->TotalMicros.store(n->TotalMicros.load(std::memory_order_relaxed) + us, std::memory_order_relaxed);
    if (us > n->MaxMicros.load(std::memory_order_relaxed)) n->MaxMicros.store(us, std::memory_order_relaxed);
    n->LastFrames.store((int)frames, std::memory_order_relaxed);
}

ma_node_vtable s_ReverbVtable = {ReverbProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};

LoudnessMeter* s_WetMeter = nullptr;
LoudnessMeter* s_BusLoudness[AudioEngine::kBusCount] = {};
MasterLimiter* s_LimiterDsp = nullptr;
LoudnessMeter* s_MasterMeter = nullptr;
ma_encoder s_Encoder;
bool s_EncoderOpen = false;

// #171 - mixer buses: every voice plays through one sound group, whose volume is the bus
// volume. Master is the engine volume (times the editor's Mute).
ma_sound_group s_Buses[AudioEngine::kBusCount];
bool s_BusesReady = false;
float s_BusVolume[AudioEngine::kBusCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
float s_MasterVolume = 1.0f;
bool s_MutedFlag = false;
bool s_EnvMuted = false; // TARTARUS_MUTE=1: a test run on a desktop someone is using - everything mixes as ever, silently
std::vector<uint32_t> s_FreeSlots;
bool s_Paused = false;

std::unique_ptr<ma_sound> s_PreviewSound;
std::string s_PreviewPath;

// The editor's UI sounds: their own group, through a clip guard to the endpoint, beside the limiter rather than through it.
// Clips are a store of their own (apart from Load()'s game clips): decoded once at the file's own channel count, miniaudio's
// buffer kept as is. Shared: a voice holds a reference, so UnloadClip never pulls PCM out from under a voice still playing it.
struct EditorClip {
    float* Pcm = nullptr; // from ma_decode_file; ma_free'd with the clip
    ma_uint32 Channels = 0;
    ma_uint32 SampleRate = 0;
    ma_uint64 FrameCount = 0;
    std::string Path;
    EditorClip() = default;
    EditorClip(const EditorClip&) = delete;
    EditorClip& operator=(const EditorClip&) = delete;
    ~EditorClip() { ma_free(Pcm, nullptr); }
};
std::vector<std::shared_ptr<const EditorClip>> s_EditorClips;          // by ClipId; null once unloaded (ids are never reused)
std::unordered_map<std::string, AudioEngine::ClipId> s_EditorClipIds; // path -> id, for LoadClip only (no lookup per play)

// Fixed slots, the ma_sound and its buffer in place: a play allocates no voice. Twice the voice cap, so a voice fading out
// after being stolen (it keeps its slot until the fade ends) never blocks the play that stole it.
struct EditorVoice {
    ma_sound Sound;
    ma_audio_buffer Buffer;
    std::shared_ptr<const EditorClip> Clip; // null when the slot is free
    uint64_t Order = 0;                     // when it started: the oldest is stolen first
    bool Stolen = false;                    // fading out to make room: freed by the reap once it has stopped
};
constexpr int kEditorSlots = 2 * AudioEngine::kEditorVoices;
constexpr unsigned kEditorStealFadeMs = 8; // a voice cut mid-sound fades over this instead of stopping on a click
ma_sound_group s_EditorGroup;
bool s_EditorGroupReady = false;
float s_EditorVolume = 1.0f;
EditorVoice s_EditorVoices[kEditorSlots];
int s_EditorFree[kEditorSlots];   // free slot indices (a stack)
int s_EditorFreeCount = 0;
int s_EditorActive[kEditorSlots]; // slots in use, in no order (swap-and-pop)
int s_EditorActiveCount = 0;
int s_EditorLive = 0;             // in use and not stolen: what EditorVoiceCount reports
uint64_t s_EditorOrder = 0;

void ResetEditorSlots() {
    for (int i = 0; i < kEditorSlots; ++i) s_EditorFree[i] = kEditorSlots - 1 - i;
    s_EditorFreeCount = kEditorSlots;
    s_EditorActiveCount = 0;
    s_EditorLive = 0;
}

void FreeEditorVoice(EditorVoice& v) {
    ma_sound_stop(&v.Sound);
    ma_sound_uninit(&v.Sound);
    ma_audio_buffer_uninit(&v.Buffer);
    v.Clip.reset();
    v.Stolen = false;
}

// Frees every voice that has stopped (played out, or a stolen one whose fade has ended).
void ReapEditorVoices() {
    for (int i = 0; i < s_EditorActiveCount;) {
        const int slot = s_EditorActive[i];
        EditorVoice& v = s_EditorVoices[slot];
        if (ma_sound_at_end(&v.Sound) || !ma_sound_is_playing(&v.Sound)) {
            if (!v.Stolen) --s_EditorLive;
            FreeEditorVoice(v);
            s_EditorFree[s_EditorFreeCount++] = slot;
            s_EditorActive[i] = s_EditorActive[--s_EditorActiveCount];
        } else {
            ++i;
        }
    }
}

// Held loops: a handful of slots of their own on the same group. A loop's level rides the sound's fader (a short glide per
// change), so a control moved every frame never zippers; the sound's own volume stays 1.
struct EditorLoop {
    ma_sound Sound;
    ma_audio_buffer Buffer;
    std::shared_ptr<const EditorClip> Clip; // null when the slot is free
    uint32_t Generation = 0;                // bumped per start: a stale LoopId no longer matches
    bool Stopping = false;                  // fading out: freed by the reap once it has stopped
};
constexpr unsigned kEditorLoopGlideMs = 25;
constexpr int kEditorLoopSlotBits = 8;
EditorLoop s_EditorLoops[AudioEngine::kEditorLoops];

void FreeEditorLoop(EditorLoop& l) {
    ma_sound_stop(&l.Sound);
    ma_sound_uninit(&l.Sound);
    ma_audio_buffer_uninit(&l.Buffer);
    l.Clip.reset();
    l.Stopping = false;
}

// Frees every loop that has finished fading out.
void ReapEditorLoops() {
    for (EditorLoop& l : s_EditorLoops)
        if (l.Clip && l.Stopping && !ma_sound_is_playing(&l.Sound)) FreeEditorLoop(l);
}

// The slot a LoopId names, or null for none / stale / already stopping.
EditorLoop* LiveEditorLoop(AudioEngine::LoopId id) {
    if (id < 0) return nullptr;
    const int slot = id & ((1 << kEditorLoopSlotBits) - 1);
    if (slot >= AudioEngine::kEditorLoops) return nullptr;
    EditorLoop& l = s_EditorLoops[slot];
    if (!l.Clip || l.Stopping || (uint32_t)(id >> kEditorLoopSlotBits) != (l.Generation & 0x7FFFFFu)) return nullptr;
    return &l;
}

// The editor output's clip guard: unity below -3 dBFS, above it a tanh knee (slope 1 where it starts, so no corner) that
// approaches -0.3 dBFS and never passes it. Per sample, no state, no lookahead: it adds no latency, and a lone tick never
// reaches it - only a pile-up of loud voices does.
struct SoftClipNode {
    ma_node_base Base;
};
SoftClipNode* s_EditorClipGuard = nullptr;
constexpr float kClipKnee = 0.708f;    // -3 dBFS
constexpr float kClipCeiling = 0.966f; // -0.3 dBFS

void SoftClipProcess(ma_node*, const float** ppIn, ma_uint32* pFrameCountIn, float** ppOut, ma_uint32* pFrameCountOut) {
    const ma_uint32 frames = std::min(*pFrameCountIn, *pFrameCountOut);
    const float* in = ppIn[0];
    float* out = ppOut[0];
    constexpr float range = kClipCeiling - kClipKnee;
    for (size_t i = 0; i < 2 * (size_t)frames; ++i) {
        const float x = in[i];
        const float a = std::fabs(x);
        out[i] = a <= kClipKnee ? x : std::copysign(kClipKnee + range * std::tanh((a - kClipKnee) / range), x);
    }
    *pFrameCountIn = frames;
    *pFrameCountOut = frames;
}
ma_node_vtable s_SoftClipVtable = {SoftClipProcess, nullptr, 1, 1, 0}; // (not continuous: no editor sound, no processing)

// The game's mix ends in the limiter, so master and Mute go on its output: the editor's sounds beside it on the endpoint keep
// their own level (and so does the Asset Browser preview, which plays at the master level by its own volume). Without a limiter
// (a mono device) everything shares the endpoint and master stays the engine volume. TARTARUS_MUTE is the endpoint: all of it.
void ApplyMasterVolume(ma_engine& engine) {
    const float master = s_MutedFlag ? 0.0f : s_MasterVolume;
    if (s_Limiter) {
        ma_node_set_output_bus_volume(&s_Limiter->Base, 0, master);
        ma_engine_set_volume(&engine, s_EnvMuted ? 0.0f : 1.0f);
    } else {
        ma_engine_set_volume(&engine, s_EnvMuted ? 0.0f : master);
    }
    if (s_PreviewSound) ma_sound_set_volume(s_PreviewSound.get(), s_Limiter ? master : 1.0f);
}

constexpr uint32_t kIndexBits = 16;
constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1u;

AudioEngine::SoundHandle MakeHandle(uint32_t index, uint16_t generation) {
    return (static_cast<uint32_t>(generation) << kIndexBits) | (index & kIndexMask);
}

// Resolves a handle to its ma_sound, or null if the handle is invalid, stale (the slot has since
// been recycled), or points at a slot whose voice has already been reaped.
ma_sound* Resolve(AudioEngine::SoundHandle handle) {
    if (handle == AudioEngine::InvalidHandle) return nullptr;
    uint32_t index = handle & kIndexMask;
    uint16_t generation = static_cast<uint16_t>(handle >> kIndexBits);
    if (index >= s_Voices.size()) return nullptr;
    Voice& voice = s_Voices[index];
    if (!voice.Sound || voice.Generation != generation) return nullptr;
    return voice.Sound.get();
}

void FreeSlot(uint32_t index) {
    Voice& voice = s_Voices[index];
    if (!voice.Sound) return;
    ma_sound_stop(voice.Sound.get());
    ma_sound_uninit(voice.Sound.get());
    voice.Sound.reset();
    if (voice.Split) { // after the sound: nothing feeds them any more
        ma_splitter_node_uninit(voice.Split.get(), nullptr);
        voice.Split.reset();
    }
    if (voice.Lpf) {
        ma_lpf_node_uninit(voice.Lpf.get(), nullptr);
        voice.Lpf.reset();
    }
    if (voice.Buffer) {
        ma_audio_buffer_uninit(voice.Buffer.get());
        voice.Buffer.reset();
    }
    voice.Clip.reset();
    voice.Paused = false;
    // Bumping here (rather than on allocation) invalidates every outstanding handle to this slot
    // the moment its sound goes away. Wrapping past 65535 back to 1 keeps the generation nonzero.
    voice.Generation = voice.Generation == 0xFFFF ? 1 : static_cast<uint16_t>(voice.Generation + 1);
    s_FreeSlots.push_back(index);
}
}

static bool s_TapsExternal = false;
namespace {

void UpdateReturnGains() {
    for (ReverbNode* r : s_Reverbs)
        if (r) r->Return.store(s_ReverbReturn * s_BusVolume[0], std::memory_order_relaxed); // the returns follow the SFX bus
}

ma_node* AsNode(ma_node_base* n) { return (ma_node*)n; }

void ApplyRolloff(ma_sound* sound, AudioEngine::Rolloff mode, float minDistance, float maxDistance, float factor) {
    minDistance = std::max(minDistance, 0.01f);
    maxDistance = std::max(maxDistance, minDistance + 0.01f);
    ma_sound_set_attenuation_model(sound, mode == AudioEngine::Rolloff::Linear        ? ma_attenuation_model_linear
                                          : mode == AudioEngine::Rolloff::Exponential ? ma_attenuation_model_exponential
                                                                                      : ma_attenuation_model_inverse);
    ma_sound_set_min_distance(sound, minDistance);
    ma_sound_set_max_distance(sound, maxDistance);
    ma_sound_set_rolloff(sound, mode == AudioEngine::Rolloff::Exponential ? std::max(factor, 0.0f) : 1.0f);
}

bool InitNode(ma_node_vtable* vt, ma_node_base* node) {
    ma_node_config nc = ma_node_config_init();
    ma_uint32 ch = 2;
    nc.vtable = vt;
    nc.pInputChannels = &ch;
    nc.pOutputChannels = &ch;
    return ma_node_init(ma_engine_get_node_graph(&s_Engine), &nc, nullptr, node) == MA_SUCCESS;
}

bool InitImpl(bool offline, int rate) {
    if (s_Initialized) return true;
    ma_engine_config cfg = ma_engine_config_init();
    std::string capture;
#ifdef _WIN32
    {
        char* env = nullptr;
        size_t len = 0;
        if (_dupenv_s(&env, &len, "AUDIO_CAPTURE") == 0 && env) {
            capture = env;
            std::free(env);
        }
    }
#else
    if (const char* env = std::getenv("AUDIO_CAPTURE")) capture = env;
#endif
    if (offline) {
        cfg.noDevice = MA_TRUE;
        cfg.channels = 2;
        cfg.sampleRate = (ma_uint32)rate;
    } else if (!capture.empty()) {
        cfg.sampleRate = 48000; // a capture is a 48 kHz file
    } else {
        // A short period (5.3 ms at 48 kHz, the convolver's head block) instead of miniaudio's 10 ms default: a UI tick reaches
        // the speaker that much sooner. The device may grant something else; the line below says what.
        cfg.periodSizeInFrames = 256;
    }
    if (ma_engine_init(&cfg, &s_Engine) != MA_SUCCESS) {
        Log::Error("Audio: failed to initialize the audio engine.");
        return false;
    }
    s_Initialized = true;
    s_Offline = offline;
    if (const ma_device* dev = offline ? nullptr : ma_engine_get_device(&s_Engine)) {
        const ma_uint32 period = dev->playback.internalPeriodSizeInFrames, hz = dev->playback.internalSampleRate;
        ma_uint32 buffer = period * dev->playback.internalPeriods;
#ifdef MA_SUPPORT_WASAPI
        // WASAPI's real buffer (the period alone undersells it): the queue a sound waits behind before it reaches the mixer.
        if (dev->pContext->backend == ma_backend_wasapi && dev->wasapi.actualBufferSizeInFramesPlayback)
            buffer = dev->wasapi.actualBufferSizeInFramesPlayback;
#endif
        char line[200];
        std::snprintf(line, sizeof(line), "Audio: output period %u frames (%.1f ms at %u Hz), buffer %u frames (%.1f ms), %s.", period,
                      hz ? 1000.0 * period / hz : 0.0, hz, buffer, hz ? 1000.0 * buffer / hz : 0.0, ma_get_backend_name(dev->pContext->backend));
        Log::Info(line);
    }
#ifdef _WIN32
    {
        char* env = nullptr;
        size_t len = 0;
        if (_dupenv_s(&env, &len, "TARTARUS_MUTE") == 0 && env) {
            s_EnvMuted = *env && *env != '0';
            std::free(env);
        }
    }
#else
    if (const char* env = std::getenv("TARTARUS_MUTE")) s_EnvMuted = *env && *env != '0';
#endif
    ApplyMasterVolume(s_Engine);
    s_BusesReady = true;
    for (int b = 0; b < AudioEngine::kBusCount; ++b) {
        if (ma_sound_group_init(&s_Engine, 0, nullptr, &s_Buses[b]) != MA_SUCCESS) {
            Log::Error("Audio: failed to create mixer bus " + std::to_string(b) + "; playing without buses.");
            for (int k = 0; k < b; ++k) ma_sound_group_uninit(&s_Buses[k]);
            s_BusesReady = false;
            break;
        }
        ma_sound_group_set_volume(&s_Buses[b], s_BusVolume[b]);
    }
    ApplyMasterVolume(s_Engine);
    const int sr = (int)ma_engine_get_sample_rate(&s_Engine);
    // The output graph: buses -> meters -> limiter -> endpoint, reverb returns -> limiter. Without it (a mono device) every group keeps
    // its default attachment to the endpoint and voices play without a send.
    if (ma_engine_get_channels(&s_Engine) == 2) {
        auto* lim = new LimiterNode();
        lim->Lim = new MasterLimiter(sr);
        lim->Master = new LoudnessMeter(sr);
        if (InitNode(&s_LimiterVtable, &lim->Base)) {
            ma_node_attach_output_bus(&lim->Base, 0, ma_engine_get_endpoint(&s_Engine), 0);
            s_Limiter = lim;
            s_LimiterDsp = lim->Lim;
            s_MasterMeter = lim->Master;
        } else {
            Log::Warn("Audio: could not create the master limiter; playing without it.");
            delete lim->Lim;
            delete lim->Master;
            delete lim;
        }
        if (s_Limiter && s_BusesReady) {
            for (int b = 0; b < AudioEngine::kBusCount; ++b) {
                auto* m = new MeterNode();
                m->Meter = new LoudnessMeter(sr);
                if (InitNode(&s_MeterVtable, &m->Base)) {
                    ma_node_attach_output_bus(AsNode(&m->Base), 0, &s_Limiter->Base, 0);
                    ma_node_attach_output_bus((ma_node*)&s_Buses[b], 0, &m->Base, 0);
                    s_BusMeters[b] = m;
                    s_BusLoudness[b] = m->Meter;
                } else {
                    delete m->Meter;
                    delete m;
                }
            }
        }
        // The reverb buses. If they can't be built, voices simply start without a send.
        for (int bus = 0; bus < 2; ++bus) {
            auto* node = new ReverbNode();
            node->Verb = new ConvolutionReverb(sr, !offline);
            node->Meter = new LoudnessMeter(sr);
            node->Verb->SetTarget(ReverbRtTarget{});
            node->Enabled.store(bus == 0); // the remote room's reverb runs only while a portal voice feeds it
            if (InitNode(&s_ReverbVtable, &node->Base)) {
                ma_node_attach_output_bus(&node->Base, 0, s_Limiter ? &s_Limiter->Base : ma_engine_get_endpoint(&s_Engine), 0);
                s_Reverbs[bus] = node;
                s_ReverbInit = true;
            } else {
                Log::Warn("Audio: could not create the reverb bus; voices play without a send.");
                delete node->Verb;
                delete node->Meter;
                delete node;
            }
        }
        UpdateReturnGains();
    }
    // The editor's UI sounds: a group of their own (no parent group) -> the clip guard -> the endpoint, beside the limiter. Its volume
    // glides over 256 frames, so a Volume slider dragged while ticks play doesn't zipper; no pitch (the group's resampler would run
    // at 1.0 for nothing).
    ma_sound_group_config gc = ma_sound_group_config_init_2(&s_Engine);
    gc.flags = MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION;
    gc.volumeSmoothTimeInPCMFrames = 256;
    s_EditorGroupReady = ma_sound_group_init_ex(&s_Engine, &gc, &s_EditorGroup) == MA_SUCCESS;
    ResetEditorSlots();
    if (s_EditorGroupReady) {
        ma_sound_group_set_volume(&s_EditorGroup, s_EditorVolume); // (the first gain set is immediate, not a glide)
        if (ma_engine_get_channels(&s_Engine) == 2) { // (a mono device: the group stays on the endpoint, unguarded)
            auto* guard = new SoftClipNode();
            if (InitNode(&s_SoftClipVtable, &guard->Base)) {
                ma_node_attach_output_bus(&guard->Base, 0, ma_engine_get_endpoint(&s_Engine), 0);
                ma_node_attach_output_bus((ma_node*)&s_EditorGroup, 0, &guard->Base, 0);
                s_EditorClipGuard = guard;
            } else {
                delete guard;
            }
        }
    } else {
        Log::Warn("Audio: could not create the editor sound group; editor UI sounds are off.");
    }
    ApplyMasterVolume(s_Engine); // again, now that the limiter it applies to exists
    if (!capture.empty()) AudioEngine::StartCapture(capture);
    return true;
}

} // namespace

void AudioEngine::Init() { InitImpl(false, 0); }

void AudioEngine::InitOffline(int sampleRate) { InitImpl(true, sampleRate); }

bool AudioEngine::IsOffline() { return s_Offline; }

int AudioEngine::SampleRate() { return s_Initialized ? (int)ma_engine_get_sample_rate(&s_Engine) : 48000; }

void AudioEngine::RenderOffline(float* out, int frames) {
    if (!s_Initialized || frames <= 0) return;
    static std::vector<float> scratch;
    if (!out) {
        if (scratch.size() < 2 * (size_t)frames) scratch.assign(2 * (size_t)frames, 0.0f);
        out = scratch.data();
    }
    ma_uint64 read = 0;
    ma_engine_read_pcm_frames(&s_Engine, out, (ma_uint64)frames, &read);
}

void AudioEngine::Shutdown() {
    if (!s_Initialized) return;
    StopCapture();
    StopPreview();
    StopAll();
    s_Voices.clear();
    s_FreeSlots.clear();
    for (int i = 0; i < s_EditorActiveCount; ++i) FreeEditorVoice(s_EditorVoices[s_EditorActive[i]]);
    ResetEditorSlots();
    for (EditorLoop& l : s_EditorLoops)
        if (l.Clip) FreeEditorLoop(l);
    if (s_EditorGroupReady) ma_sound_group_uninit(&s_EditorGroup);
    s_EditorGroupReady = false;
    if (s_EditorClipGuard) { // after the group: nothing feeds it any more
        ma_node_uninit(&s_EditorClipGuard->Base, nullptr);
        delete s_EditorClipGuard;
        s_EditorClipGuard = nullptr;
    }
    // (The editor clips stay: plain PCM at their own rate, still playable by a later Init, and their ids stay meaningful.)
    UnloadAll();
    s_Paused = false;
    if (s_BusesReady) for (int b = 0; b < kBusCount; ++b) ma_sound_group_uninit(&s_Buses[b]);
    s_BusesReady = false;
    for (ReverbNode*& r : s_Reverbs)
        if (r) {
            ma_node_uninit(&r->Base, nullptr);
            delete r->Verb;
            delete r->Meter;
            delete r;
            r = nullptr;
        }
    for (int b = 0; b < kBusCount; ++b)
        if (s_BusMeters[b]) {
            ma_node_uninit(&s_BusMeters[b]->Base, nullptr);
            delete s_BusMeters[b]->Meter;
            delete s_BusMeters[b];
            s_BusMeters[b] = nullptr;
            s_BusLoudness[b] = nullptr;
        }
    if (s_Limiter) {
        ma_node_uninit(&s_Limiter->Base, nullptr);
        delete s_Limiter->Lim;
        delete s_Limiter->Master;
        delete s_Limiter;
        s_Limiter = nullptr;
        s_LimiterDsp = nullptr;
        s_MasterMeter = nullptr;
    }
    s_ReverbInit = false;
    s_IrCache.clear();
    ma_engine_uninit(&s_Engine);
    s_Taps.store(nullptr);
    s_TapStorage.reset();
    s_Initialized = false;
    s_Offline = false;
}

void AudioEngine::Update() {
    if (!s_Initialized) return;

    // Reap one-shots that have run to their end (#200: nothing used to free these, so every
    // Play leaked a ma_sound plus its open streaming file handle for the life of the process).
    // Looping voices never end on their own — they only leave via Stop()/StopAll().
    for (uint32_t i = 0; i < s_Voices.size(); ++i) {
        Voice& voice = s_Voices[i];
        // A paused voice is stopped too, but it's waiting to resume, not finished.
        if (!voice.Sound || voice.Looping || voice.Paused) continue;
        if (ma_sound_at_end(voice.Sound.get()) || !ma_sound_is_playing(voice.Sound.get())) {
            FreeSlot(i);
        }
    }

    // Same treatment for the Asset Browser preview: a preview that played to its end used to sit
    // on its file handle until the next preview or shutdown.
    if (s_PreviewSound && (ma_sound_at_end(s_PreviewSound.get()) ||
                           !ma_sound_is_playing(s_PreviewSound.get()))) {
        StopPreview();
    }
    ReapEditorVoices();
    ReapEditorLoops();
    if (s_EncoderOpen && !s_TapsExternal) { // AUDIO_CAPTURE: keep the file fed
        std::vector<float> a, b, c;
        DrainTaps(a, b, c);
    }
}

bool AudioEngine::Load(const std::string& path) {
    if (s_Preloaded.find(path) != s_Preloaded.end()) return true; // already preloaded

    // Force the decode to the engine's own output format/rate so playing a preloaded clip needs
    // no per-instance resample - just a straight PCM copy into a fresh ma_audio_buffer.
    ma_decoder_config config = ma_decoder_config_init(
        ma_format_f32,
        s_Initialized ? ma_engine_get_channels(&s_Engine) : 2,
        s_Initialized ? ma_engine_get_sample_rate(&s_Engine) : 48000);

    ma_uint64 frameCount = 0;
    void* pFrames = nullptr;
    if (ma_decode_file(path.c_str(), &config, &frameCount, &pFrames) != MA_SUCCESS) {
        if (!AssetDatabase::IsUnfetchedExternal(path)) // a Drive file not fetched yet: the scan's warning counts it
            Log::Error("Audio: failed to preload '" + path + "'.", LogContext::Asset(path));
        return false;
    }

    auto clip = std::make_shared<PreloadedClip>();
    clip->Format = config.format;
    clip->Channels = config.channels;
    clip->SampleRate = config.sampleRate;
    clip->FrameCount = frameCount;
    const size_t bytes = (size_t)frameCount * config.channels * ma_get_bytes_per_sample(config.format);
    const uint8_t* begin = static_cast<const uint8_t*>(pFrames);
    clip->Data.assign(begin, begin + bytes);
    ma_free(pFrames, nullptr);

    s_Preloaded.emplace(path, std::move(clip));
    return true;
}

bool AudioEngine::Unload(const std::string& path) {
    return s_Preloaded.erase(path) > 0;
}

void AudioEngine::UnloadAll() {
    s_Preloaded.clear();
}

bool AudioEngine::Reload(const std::string& path) {
    namespace fs = std::filesystem;
    const fs::path changed = fs::u8path(path).lexically_normal();
    std::vector<std::string> keys;
    for (const auto& [key, clip] : s_Preloaded)
        if (key == path || fs::u8path(ProjectPaths::Resolve(key)).lexically_normal() == changed) keys.push_back(key);
    bool reloaded = false;
    for (const std::string& key : keys) {
        const std::shared_ptr<const PreloadedClip> previous = s_Preloaded[key];
        s_Preloaded.erase(key);
        if (Load(key)) reloaded = true;
        else s_Preloaded[key] = previous;
    }
    return reloaded;
}

AudioEngine::SoundHandle AudioEngine::Play(const std::string& path, float volume, bool loop, Bus bus, float startOffsetSeconds, const VoiceFx* fx) {
    if (!s_Initialized) return InvalidHandle;
    const int busIndex = std::clamp((int)bus, 0, kBusCount - 1);
    ma_sound_group* group = s_BusesReady ? &s_Buses[busIndex] : nullptr;

    auto sound = std::make_unique<ma_sound>();
    std::unique_ptr<ma_audio_buffer> buffer;
    std::shared_ptr<const PreloadedClip> clipRef;

    auto cached = s_Preloaded.find(path);
    if (cached != s_Preloaded.end()) {
        // Preloaded (#202): play the already-decoded PCM instead of re-decoding from disk on
        // every Play(). The non-owning ma_audio_buffer_init reads the shared clip in place; the
        // voice's Clip reference (#171) keeps that storage alive even if the cache entry is
        // Unload()ed mid-playback.
        clipRef = cached->second;
        const PreloadedClip& clip = *clipRef;
        buffer = std::make_unique<ma_audio_buffer>();
        ma_audio_buffer_config config = ma_audio_buffer_config_init(
            clip.Format, clip.Channels, clip.FrameCount, clip.Data.data(), nullptr);
        if (ma_audio_buffer_init(&config, buffer.get()) != MA_SUCCESS) {
            Log::Error("Audio: failed to instantiate preloaded '" + path + "'.", LogContext::Asset(path));
            return InvalidHandle;
        }
        if (ma_sound_init_from_data_source(&s_Engine, (ma_data_source*)buffer.get(), 0, group,
                                           sound.get()) != MA_SUCCESS) {
            Log::Error("Audio: failed to play preloaded '" + path + "'.", LogContext::Asset(path));
            ma_audio_buffer_uninit(buffer.get());
            return InvalidHandle;
        }
    } else if (ma_sound_init_from_file(&s_Engine, path.c_str(), MA_SOUND_FLAG_STREAM, group, nullptr,
                                       sound.get()) != MA_SUCCESS) {
        Log::Error("Audio: failed to load sound '" + path + "'.", LogContext::Asset(path));
        return InvalidHandle;
    }

    ma_sound_set_looping(sound.get(), loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume(sound.get(), volume);
    // Off until someone calls SetPosition (or the fx place it): an unpositioned voice would otherwise sit at the
    // origin and get attenuated against the listener, which is wrong for UI and preview sounds.
    ma_sound_set_spatialization_enabled(sound.get(), MA_FALSE);
    if (fx) {
        ma_sound_set_pitch(sound.get(), fx->Pitch);
        if (fx->Duck != 1.0f) ma_sound_set_fade_in_pcm_frames(sound.get(), std::max(fx->Duck, 0.0f), std::max(fx->Duck, 0.0f), 0);
        if (fx->Spatial) {
            ma_sound_set_spatialization_enabled(sound.get(), MA_TRUE);
            ma_sound_set_position(sound.get(), fx->Position.x, fx->Position.y, fx->Position.z);
            ApplyRolloff(sound.get(), fx->RolloffMode, fx->MinDistance, fx->MaxDistance, fx->RolloffFactor);
        }
    }
    if (startOffsetSeconds > 0.0f) {
        ma_uint32 sampleRate = 0;
        if (ma_sound_get_data_format(sound.get(), nullptr, nullptr, &sampleRate, nullptr, 0) == MA_SUCCESS && sampleRate > 0)
            ma_sound_seek_to_pcm_frame(sound.get(), (ma_uint64)(startOffsetSeconds * (float)sampleRate));
    }
    // The node chain: sound -> [occlusion low-pass] -> [splitter: bus | reverb send]. Built before the voice starts, so its first
    // block already goes through it. A failure leaves the voice on the plain path.
    std::unique_ptr<ma_lpf_node> lpf;
    std::unique_ptr<ma_splitter_node> split;
    if (fx && group && ma_engine_get_channels(&s_Engine) == 2) {
        ma_node* tail = sound.get();
        const ma_uint32 sr = ma_engine_get_sample_rate(&s_Engine);
        if (fx->Occlusion) {
            auto n = std::make_unique<ma_lpf_node>();
            ma_lpf_node_config lc = ma_lpf_node_config_init(2, sr, std::clamp((double)fx->Cutoff, 80.0, 20000.0), 2);
            if (ma_lpf_node_init(ma_engine_get_node_graph(&s_Engine), &lc, nullptr, n.get()) == MA_SUCCESS) {
                ma_node_set_output_bus_volume(n.get(), 0, std::max(fx->Gain, 0.0f));
                ma_node_attach_output_bus(tail, 0, n.get(), 0);
                tail = n.get();
                lpf = std::move(n);
            }
        }
        if (fx->ReverbSend > 0.0f && s_Reverbs[0]) {
            auto n = std::make_unique<ma_splitter_node>();
            ma_splitter_node_config sc = ma_splitter_node_config_init(2);
            if (ma_splitter_node_init(ma_engine_get_node_graph(&s_Engine), &sc, nullptr, n.get()) == MA_SUCCESS) {
                ma_node_attach_output_bus(tail, 0, n.get(), 0);
                ma_node_attach_output_bus(n.get(), 0, group, 0);
                ma_node_attach_output_bus(n.get(), 1, &s_Reverbs[fx->ReverbBus == 1 && s_Reverbs[1] ? 1 : 0]->Base, 0);
                ma_node_set_output_bus_volume(n.get(), 1, fx->ReverbSend);
                tail = n.get();
                split = std::move(n);
            }
        }
        if (lpf && !split) ma_node_attach_output_bus(lpf.get(), 0, group, 0);
    }
    auto dropChain = [&] {
        if (split) ma_splitter_node_uninit(split.get(), nullptr);
        if (lpf) ma_lpf_node_uninit(lpf.get(), nullptr);
    };
    if (ma_sound_start(sound.get()) != MA_SUCCESS) {
        Log::Error("Audio: failed to start sound '" + path + "'.", LogContext::Asset(path));
        ma_sound_uninit(sound.get());
        dropChain();
        if (buffer) ma_audio_buffer_uninit(buffer.get());
        return InvalidHandle;
    }

    uint32_t index;
    if (!s_FreeSlots.empty()) {
        index = s_FreeSlots.back();
        s_FreeSlots.pop_back();
    } else {
        // Slot indices have to fit in kIndexBits. Hitting this means ~65k simultaneous voices,
        // which is a bug elsewhere; drop the sound rather than hand out an aliasing handle.
        if (s_Voices.size() > kIndexMask) {
            Log::Error("Audio: voice limit reached; dropping '" + path + "'.", LogContext::Asset(path));
            ma_sound_uninit(sound.get());
            dropChain();
            return InvalidHandle;
        }
        index = static_cast<uint32_t>(s_Voices.size());
        s_Voices.emplace_back();
    }

    Voice& voice = s_Voices[index];
    voice.Sound = std::move(sound);
    voice.LpfHz = lpf ? std::clamp((double)fx->Cutoff, 80.0, 20000.0) : 0.0;
    voice.Lpf = std::move(lpf);
    voice.Split = std::move(split);
    voice.Buffer = std::move(buffer);
    voice.Clip = std::move(clipRef);
    voice.Looping = loop;
    return MakeHandle(index, voice.Generation);
}

void AudioEngine::SetReverbSendBus(SoundHandle handle, int bus) {
    if (!Resolve(handle)) return;
    Voice& v = s_Voices[handle & kIndexMask];
    ReverbNode* r = s_Reverbs[bus == 1 ? 1 : 0];
    if (v.Split && r) ma_node_attach_output_bus(v.Split.get(), 1, &r->Base, 0);
}

void AudioEngine::SetOcclusion(SoundHandle handle, float cutoffHz, float gain) {
    if (!Resolve(handle)) return;
    Voice& v = s_Voices[handle & kIndexMask];
    if (!v.Lpf) return;
    ma_node_set_output_bus_volume(v.Lpf.get(), 0, std::max(gain, 0.0f));
    const double hz = (cutoffHz <= 0.0f || cutoffHz >= 20000.0f) ? 20000.0 : std::max(80.0, (double)cutoffHz);
    // The coefficients are rewritten under the audio thread, which reads them as it filters: only when the cutoff
    // really moves (half a percent is inaudible), not every frame the glide holds still.
    if (std::abs(hz - v.LpfHz) <= v.LpfHz * 0.005) return;
    v.LpfHz = hz;
    ma_lpf_node_config lc = ma_lpf_node_config_init(2, ma_engine_get_sample_rate(&s_Engine), hz, 2);
    ma_lpf_node_reinit(&lc.lpf, v.Lpf.get());
}

void AudioEngine::SetReverbSend(SoundHandle handle, float level) {
    if (!Resolve(handle)) return;
    Voice& v = s_Voices[handle & kIndexMask];
    if (v.Split) ma_node_set_output_bus_volume(v.Split.get(), 1, std::max(level, 0.0f));
}

void AudioEngine::SetReverbEnabled(bool enabled, int bus) {
    if (ReverbNode* r = s_Reverbs[bus == 1 ? 1 : 0]) r->Enabled.store(enabled);
}
bool AudioEngine::ReverbEnabled(int bus) { return s_Reverbs[bus == 1 ? 1 : 0] && s_Reverbs[bus == 1 ? 1 : 0]->Enabled.load(); }

namespace {
// Decodes an impulse response (any wav / flac / ogg / mp3) to the engine's rate and prepares it. Cached by path; null on failure.
std::shared_ptr<const IrData> LoadIr(const std::string& path) {
    const auto it = s_IrCache.find(path);
    if (it != s_IrCache.end()) return it->second;
    std::shared_ptr<const IrData> ir;
    if (s_Initialized) {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, ma_engine_get_sample_rate(&s_Engine));
        ma_uint64 frames = 0;
        void* pcm = nullptr;
        if (ma_decode_file(path.c_str(), &cfg, &frames, &pcm) == MA_SUCCESS && pcm && frames > 0) {
            const float* in = static_cast<const float*>(pcm);
            std::vector<float> l((size_t)frames), r((size_t)frames);
            for (size_t i = 0; i < (size_t)frames; ++i) {
                l[i] = in[2 * i];
                r[i] = in[2 * i + 1];
            }
            ma_free(pcm, nullptr);
            ir = IrData::FromSamples(l.data(), r.data(), (int)frames, (int)ma_engine_get_sample_rate(&s_Engine));
        } else {
            Log::Warn("Audio: could not load the impulse response '" + path + "'.", LogContext::Asset(path));
        }
    }
    s_IrCache[path] = ir; // (a failure is remembered too: one warning, not one per frame)
    return ir;
}

// The convolver of `path` on a bus (registered on first use).
int IrIndexFor(int bus, const std::string& path) {
    ReverbNode* r = s_Reverbs[bus];
    if (!r || path.empty()) return -1;
    const auto it = r->IrIndex.find(path);
    if (it != r->IrIndex.end()) return it->second;
    const std::shared_ptr<const IrData> ir = LoadIr(path);
    if (!ir) {
        r->IrIndex[path] = -1;
        return -1;
    }
    const int idx = r->Verb->AddIr(ir);
    r->IrIndex[path] = idx;
    if (idx >= 0) {
        if ((int)r->IrPaths.size() <= idx) r->IrPaths.resize((size_t)idx + 1);
        r->IrPaths[(size_t)idx] = path;
    } else {
        Log::Warn("Audio: too many impulse responses on one reverb bus; '" + path + "' is not used.");
    }
    return idx;
}
} // namespace

bool AudioEngine::PreloadIr(const std::string& path) {
    if (!s_Initialized || path.empty()) return false;
    const bool ok = LoadIr(path) != nullptr;
    if (ok) IrIndexFor(0, path);
    return ok;
}

void AudioEngine::SetReverb(const ReverbSpec& spec, int bus) {
    const int b = bus == 1 ? 1 : 0;
    ReverbNode* r = s_Reverbs[b];
    if (!r) return;
    ReverbRtTarget t;
    t.GlideSeconds = s_ReverbGlide;
    float total = 0.0f;
    for (int i = 0; i < spec.Count && i < 2; ++i) {
        const ReverbLayerSpec& L = spec.Layers[i];
        const int idx = IrIndexFor(b, L.Ir);
        if (idx < 0 || L.Weight <= 0.0f) continue;
        ReverbRtLayer& o = t.Layers[t.Count++];
        o.Instance = idx;
        o.Weight = L.Weight;
        o.WetLin = std::pow(10.0f, L.WetDb / 20.0f);
        o.PreDelayMs = L.PreDelayMs;
        o.HfDampDb = L.HfDampDb;
        o.LowCutHz = L.LowCutHz;
        total += L.Weight;
    }
    for (int i = 0; i < t.Count; ++i) t.Layers[i].Weight /= total; // (the layers that could be loaded carry the whole weight)
    r->Verb->SetTarget(t);
}

AudioEngine::ReverbInfo AudioEngine::GetReverbInfo(int bus) {
    ReverbInfo out;
    ReverbNode* r = s_Reverbs[bus == 1 ? 1 : 0];
    if (!r) return out;
    const ConvolutionReverb::Info info = r->Verb->GetInfo();
    out.Instances = info.Instances;
    for (int k = 0; k < info.Instances; ++k) {
        ReverbInfo::Entry e;
        if ((size_t)k < r->IrPaths.size()) e.Ir = r->IrPaths[(size_t)k];
        e.Running = info.On[k];
        e.Weight = info.Weight[k];
        out.Entries.push_back(e);
    }
    return out;
}

void AudioEngine::SetReverbGlide(float seconds) {
    s_ReverbGlide = std::max(seconds, 0.01f);
    for (ReverbNode* r : s_Reverbs)
        if (r) {
            ReverbRtTarget t = r->Verb->Target();
            t.GlideSeconds = s_ReverbGlide;
            r->Verb->SetTarget(t);
        }
}

void AudioEngine::SetReverbReturn(float level) {
    s_ReverbReturn = std::max(level, 0.0f);
    UpdateReturnGains();
}

AudioEngine::ReverbStats AudioEngine::GetReverbStats(int bus) {
    ReverbStats st;
    ReverbNode* r = s_Reverbs[bus == 1 ? 1 : 0];
    if (!r) return st;
    st.Callbacks = r->Callbacks.load();
    st.TotalMicros = r->TotalMicros.load();
    st.MaxMicros = r->MaxMicros.load();
    st.Frames = r->LastFrames.load();
    st.WorkerJobs = r->Verb->WorkerJobs();
    st.WorkerMicros = r->Verb->WorkerMicros();
    st.LateBlocks = r->Verb->LateBlocks();
    st.ActiveConvolvers = r->Verb->GetInfo().Active;
    return st;
}

void AudioEngine::ResetReverbStats() {
    for (ReverbNode* r : s_Reverbs)
        if (r) {
            r->Callbacks.store(0);
            r->TotalMicros.store(0.0);
            r->MaxMicros.store(0.0);
        }
}

void AudioEngine::SetMasterLimiter(const LimiterSettings& s) {
    if (!s_Limiter) return;
    std::lock_guard<std::mutex> lk(s_Limiter->Lock);
    LimiterSettings& p = s_Limiter->Pending;
    p = s;
    p.CeilingDb = std::min(s.CeilingDb, 0.0f);
    p.LookaheadMs = std::clamp(s.LookaheadMs, 0.1f, 10.0f);
    p.ReleaseMs = std::max(s.ReleaseMs, 1.0f);
    s_Limiter->Dirty.store(true, std::memory_order_release);
}

LimiterSettings AudioEngine::GetMasterLimiter() {
    if (!s_Limiter) return LimiterSettings{};
    std::lock_guard<std::mutex> lk(s_Limiter->Lock);
    return s_Limiter->Pending;
}

float AudioEngine::TakeLimiterGainReductionDb() { return s_Limiter ? s_Limiter->LastGrDb.exchange(0.0f, std::memory_order_relaxed) : 0.0f; }
float AudioEngine::TakeGlueGainReductionDb() { return s_Limiter ? s_Limiter->LastGlueGrDb.exchange(0.0f, std::memory_order_relaxed) : 0.0f; }

AudioEngine::MeterReading AudioEngine::GetMeter(int id) {
    MeterReading m;
    const LoudnessMeter* lm = nullptr;
    if (id == MeterMaster) lm = s_MasterMeter;
    else if (id >= MeterBusFirst && id < MeterBusFirst + kBusCount) lm = s_BusLoudness[id - MeterBusFirst];
    else if (id == MeterWet) {
        const LoudnessMeter* a = s_Reverbs[0] ? s_Reverbs[0]->Meter : nullptr;
        const LoudnessMeter* b = s_Reverbs[1] ? s_Reverbs[1]->Meter : nullptr;
        lm = (b && (!a || b->MomentaryLufs() > a->MomentaryLufs())) ? b : a;
    }
    if (!lm) return m;
    m.PeakDb = lm->PeakDb();
    m.MomentaryLufs = lm->MomentaryLufs();
    m.ShortTermLufs = lm->ShortTermLufs();
    return m;
}

int AudioEngine::VoiceCount() {
    int n = 0;
    for (const Voice& v : s_Voices)
        if (v.Sound) ++n;
    return n;
}

void AudioEngine::EnableTaps(bool on) {
    if (!s_Initialized) return;
    s_TapsExternal = on;
    if (on) {
        if (!s_TapStorage) {
            s_TapStorage = std::make_unique<TapRing>();
            s_TapStorage->Master.assign(2 * (size_t)TapRing::kFrames, 0.0f);
            s_TapStorage->Pre.assign(2 * (size_t)TapRing::kFrames, 0.0f);
            s_TapStorage->Wet.assign(2 * (size_t)TapRing::kFrames, 0.0f);
        }
        s_Taps.store(s_TapStorage.get(), std::memory_order_release);
    } else if (!s_EncoderOpen) {
        s_Taps.store(nullptr, std::memory_order_release);
    }
}

size_t AudioEngine::DrainTaps(std::vector<float>& master, std::vector<float>& pre, std::vector<float>& wet) {
    TapRing* t = s_TapStorage.get();
    if (!t || !s_Taps.load()) return 0;
    const unsigned long long w = t->Written.load(std::memory_order_acquire);
    if (w - t->Read > TapRing::kFrames) {
        t->Lost += w - t->Read - TapRing::kFrames;
        t->Read = w - TapRing::kFrames;
    }
    const size_t n = (size_t)(w - t->Read);
    const size_t at0 = master.size();
    master.resize(at0 + 2 * n);
    pre.resize(at0 + 2 * n);
    wet.resize(at0 + 2 * n);
    for (size_t i = 0; i < n; ++i) {
        const size_t slot = (size_t)((t->Read + i) & (TapRing::kFrames - 1)) * 2;
        master[at0 + 2 * i] = t->Master[slot];
        master[at0 + 2 * i + 1] = t->Master[slot + 1];
        pre[at0 + 2 * i] = t->Pre[slot];
        pre[at0 + 2 * i + 1] = t->Pre[slot + 1];
        wet[at0 + 2 * i] = t->Wet[slot];
        wet[at0 + 2 * i + 1] = t->Wet[slot + 1];
        t->Wet[slot] = t->Wet[slot + 1] = 0.0f; // the returns add into zeros next lap
    }
    if (s_EncoderOpen && n > 0) {
        ma_uint64 written = 0;
        ma_encoder_write_pcm_frames(&s_Encoder, master.data() + at0, (ma_uint64)n, &written);
    }
    t->Read = w;
    return n;
}

bool AudioEngine::StartCapture(const std::string& wavPath) {
    if (!s_Initialized || s_EncoderOpen || wavPath.empty()) return false;
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, ma_engine_get_sample_rate(&s_Engine));
    if (ma_encoder_init_file(wavPath.c_str(), &cfg, &s_Encoder) != MA_SUCCESS) {
        Log::Warn("Audio: could not open the capture file '" + wavPath + "'.");
        return false;
    }
    s_EncoderOpen = true;
    Log::Info("Audio: capturing the master output to '" + wavPath + "'.");
    if (!s_TapStorage) {
        s_TapStorage = std::make_unique<TapRing>();
        s_TapStorage->Master.assign(2 * (size_t)TapRing::kFrames, 0.0f);
        s_TapStorage->Pre.assign(2 * (size_t)TapRing::kFrames, 0.0f);
        s_TapStorage->Wet.assign(2 * (size_t)TapRing::kFrames, 0.0f);
    }
    s_Taps.store(s_TapStorage.get(), std::memory_order_release);
    return true;
}

void AudioEngine::StopCapture() {
    if (!s_EncoderOpen) return;
    std::vector<float> a, b, c;
    DrainTaps(a, b, c); // (the last frames)
    ma_encoder_uninit(&s_Encoder);
    s_EncoderOpen = false;
    if (!s_TapsExternal) s_Taps.store(nullptr, std::memory_order_release);
}

void AudioEngine::Stop(SoundHandle handle) {
    if (!Resolve(handle)) return;
    FreeSlot(handle & kIndexMask);
}

void AudioEngine::SetVolume(SoundHandle handle, float volume) {
    if (ma_sound* sound = Resolve(handle)) ma_sound_set_volume(sound, volume);
}

void AudioEngine::SetDuck(SoundHandle handle, float gain, float seconds) {
    if (ma_sound* sound = Resolve(handle))
        ma_sound_set_fade_in_milliseconds(sound, -1.0f, std::max(gain, 0.0f), (ma_uint64)std::max(seconds * 1000.0f, 0.0f));
}

void AudioEngine::SetPitch(SoundHandle handle, float pitch) {
    if (ma_sound* sound = Resolve(handle)) ma_sound_set_pitch(sound, pitch);
}

bool AudioEngine::IsPlaying(SoundHandle handle) {
    ma_sound* sound = Resolve(handle);
    return sound && ma_sound_is_playing(sound) == MA_TRUE;
}

void AudioEngine::SetPosition(SoundHandle handle, const glm::vec3& position) {
    ma_sound* sound = Resolve(handle);
    if (!sound) return;
    ma_sound_set_spatialization_enabled(sound, MA_TRUE);
    ma_sound_set_position(sound, position.x, position.y, position.z);
}

void AudioEngine::SetAttenuation(SoundHandle handle, float minDistance, float maxDistance,
                                 float rolloff) {
    ma_sound* sound = Resolve(handle);
    if (!sound) return;
    ma_sound_set_attenuation_model(sound, ma_attenuation_model_inverse);
    ma_sound_set_min_distance(sound, minDistance);
    ma_sound_set_max_distance(sound, maxDistance);
    ma_sound_set_rolloff(sound, rolloff);
}

void AudioEngine::SetRolloff(SoundHandle handle, Rolloff mode, float minDistance, float maxDistance, float factor) {
    if (ma_sound* sound = Resolve(handle)) ApplyRolloff(sound, mode, minDistance, maxDistance, factor);
}

void AudioEngine::SetSpatial(SoundHandle handle, bool spatial) {
    ma_sound* sound = Resolve(handle);
    if (!sound) return;
    ma_sound_set_spatialization_enabled(sound, spatial ? MA_TRUE : MA_FALSE);
}

void AudioEngine::SetDopplerLevel(SoundHandle handle, float level) {
    if (ma_sound* sound = Resolve(handle)) ma_sound_set_doppler_factor(sound, level < 0.0f ? 0.0f : level);
}

void AudioEngine::SetVelocity(SoundHandle handle, const glm::vec3& v) {
    if (ma_sound* sound = Resolve(handle)) ma_sound_set_velocity(sound, v.x, v.y, v.z);
}

void AudioEngine::SetListenerVelocity(const glm::vec3& v) {
    if (s_Initialized) ma_engine_listener_set_velocity(&s_Engine, 0, v.x, v.y, v.z);
}

void AudioEngine::SetListener(const glm::vec3& position, const glm::vec3& forward,
                              const glm::vec3& up) {
    if (!s_Initialized) return;
    ma_engine_listener_set_position(&s_Engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&s_Engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&s_Engine, 0, up.x, up.y, up.z);
}

void AudioEngine::StopAll() {
    for (uint32_t i = 0; i < s_Voices.size(); ++i) FreeSlot(i);
}

void AudioEngine::SetPaused(bool paused) {
    if (paused == s_Paused || !s_Initialized) return;
    s_Paused = paused;
    for (Voice& voice : s_Voices) {
        if (!voice.Sound) continue;
        if (paused) {
            // Only voices audible right now; ma_sound_stop keeps the cursor, so resuming
            // continues mid-clip. A one-shot that already ended is left for Update() to reap.
            if (ma_sound_is_playing(voice.Sound.get()) && !ma_sound_at_end(voice.Sound.get())) {
                ma_sound_stop(voice.Sound.get());
                voice.Paused = true;
            }
        } else if (voice.Paused) {
            ma_sound_start(voice.Sound.get());
            voice.Paused = false;
        }
    }
}
bool AudioEngine::IsPaused() { return s_Paused; }

void AudioEngine::SetMuted(bool muted) {
    s_MutedFlag = muted;
    if (s_Initialized) ApplyMasterVolume(s_Engine);
}
bool AudioEngine::IsMuted() { return s_MutedFlag; }

void AudioEngine::SetMasterVolume(float volume) {
    s_MasterVolume = std::clamp(volume, 0.0f, 1.0f);
    if (s_Initialized) ApplyMasterVolume(s_Engine);
}
float AudioEngine::MasterVolume() { return s_MasterVolume; }

void AudioEngine::SetBusVolume(Bus bus, float volume) {
    const int b = std::clamp((int)bus, 0, kBusCount - 1);
    s_BusVolume[b] = std::clamp(volume, 0.0f, 1.0f);
    if (s_BusesReady) ma_sound_group_set_volume(&s_Buses[b], s_BusVolume[b]);
    if (b == 0) UpdateReturnGains(); // the returns follow the SFX bus
}
float AudioEngine::BusVolume(Bus bus) { return s_BusVolume[std::clamp((int)bus, 0, kBusCount - 1)]; }

bool AudioEngine::WaveformPeaks(const std::string& path, int buckets, std::vector<float>& outPeaks) {
    outPeaks.clear();
    if (buckets <= 0) return false;

    ma_decoder decoder;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0); // mono f32, native rate
    if (ma_decoder_init_file(path.c_str(), &cfg, &decoder) != MA_SUCCESS) return false;

    ma_uint64 totalFrames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
    if (totalFrames == 0) { ma_decoder_uninit(&decoder); return false; }

    outPeaks.assign(buckets, 0.0f);
    const ma_uint64 framesPerBucket = (totalFrames + buckets - 1) / (ma_uint64)buckets;

    std::vector<float> chunk(4096);
    ma_uint64 frameIndex = 0;
    for (;;) {
        ma_uint64 read = 0;
        if (ma_decoder_read_pcm_frames(&decoder, chunk.data(), chunk.size(), &read) != MA_SUCCESS || read == 0)
            break;
        for (ma_uint64 i = 0; i < read; ++i) {
            const int b = (int)std::min<ma_uint64>((frameIndex + i) / framesPerBucket, (ma_uint64)buckets - 1);
            const float a = std::fabs(chunk[i]);
            if (a > outPeaks[b]) outPeaks[b] = a;
        }
        frameIndex += read;
    }
    ma_decoder_uninit(&decoder);
    return true;
}

void AudioEngine::PlayPreview(const std::string& path) {
    StopPreview();
    if (!s_Initialized) return;

    auto sound = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(&s_Engine, path.c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr, sound.get()) != MA_SUCCESS) {
        Log::Error("Audio: failed to load sound '" + path + "'.", LogContext::Asset(path));
        return;
    }
    ma_sound_set_spatialization_enabled(sound.get(), MA_FALSE);
    s_PreviewSound = std::move(sound);
    s_PreviewPath = path;
    ApplyMasterVolume(s_Engine); // its level
    ma_sound_start(s_PreviewSound.get());
}

AudioEngine::ClipId AudioEngine::LoadClip(const std::string& path) {
    const auto known = s_EditorClipIds.find(path);
    if (known != s_EditorClipIds.end()) return known->second;
    // The file's own channel count (0 = native: the built-in set is mono, half the memory of stereo) at the engine's rate, so
    // a voice needs no resample; the engine node spreads a mono clip over both channels before it pans.
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, s_Initialized ? ma_engine_get_sample_rate(&s_Engine) : 48000);
    ma_uint64 frames = 0;
    void* pcm = nullptr;
    if (ma_decode_file(path.c_str(), &config, &frames, &pcm) != MA_SUCCESS || !pcm || frames == 0 || config.channels == 0) {
        ma_free(pcm, nullptr);
        if (!AssetDatabase::IsUnfetchedExternal(path))
            Log::Error("Audio: failed to load the editor sound '" + path + "'.", LogContext::Asset(path));
        return -1;
    }
    // ma_decode_file grows its buffer by doubling: trim the slack (the decode keeps its own buffer, no copy).
    const size_t bytes = (size_t)frames * config.channels * sizeof(float);
    if (void* trimmed = ma_realloc(pcm, bytes, nullptr)) pcm = trimmed;
    auto clip = std::make_shared<EditorClip>();
    clip->Pcm = static_cast<float*>(pcm);
    clip->Channels = config.channels;
    clip->SampleRate = config.sampleRate;
    clip->FrameCount = frames;
    clip->Path = path;
    const ClipId id = (ClipId)s_EditorClips.size();
    s_EditorClips.push_back(std::move(clip));
    s_EditorClipIds.emplace(path, id);
    return id;
}

void AudioEngine::UnloadClip(ClipId id) {
    if (id < 0 || (size_t)id >= s_EditorClips.size() || !s_EditorClips[(size_t)id]) return;
    s_EditorClipIds.erase(s_EditorClips[(size_t)id]->Path);
    s_EditorClips[(size_t)id].reset(); // (a voice playing it holds its own reference: freed when that ends)
}

bool AudioEngine::PlayEditor(ClipId id, float volume, float pitch, float pan) {
    if (!s_Initialized || !s_EditorGroupReady || id < 0 || (size_t)id >= s_EditorClips.size()) return false;
    const std::shared_ptr<const EditorClip>& clip = s_EditorClips[(size_t)id];
    if (!clip) return false;
    if (s_EditorFreeCount == 0) ReapEditorVoices(); // every slot busy (stolen voices still fading): free any that have ended
    if (s_EditorFreeCount == 0) return false;
    const int slot = s_EditorFree[--s_EditorFreeCount];
    EditorVoice& v = s_EditorVoices[slot];
    ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, clip->Channels, clip->FrameCount, clip->Pcm, nullptr);
    bc.sampleRate = clip->SampleRate; // (a clip loaded before Init at another rate than the device's still plays at its own speed)
    if (ma_audio_buffer_init(&bc, &v.Buffer) != MA_SUCCESS) {
        s_EditorFree[s_EditorFreeCount++] = slot;
        return false;
    }
    // At pitch 1 no resampler at all (miniaudio would otherwise run one at 1.0, unfiltered); jittered and pitched cues keep it.
    const bool unpitched = std::fabs(pitch - 1.0f) < 0.001f;
    const ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION | (unpitched ? (ma_uint32)MA_SOUND_FLAG_NO_PITCH : 0u);
    if (ma_sound_init_from_data_source(&s_Engine, (ma_data_source*)&v.Buffer, flags, &s_EditorGroup, &v.Sound) != MA_SUCCESS) {
        ma_audio_buffer_uninit(&v.Buffer);
        s_EditorFree[s_EditorFreeCount++] = slot;
        return false;
    }
    ma_sound_set_volume(&v.Sound, std::max(volume, 0.0f));
    if (!unpitched) ma_sound_set_pitch(&v.Sound, std::clamp(pitch, 0.25f, 4.0f));
    ma_sound_set_pan(&v.Sound, std::clamp(pan, -1.0f, 1.0f));
    if (ma_sound_start(&v.Sound) != MA_SUCCESS) {
        FreeEditorVoice(v);
        s_EditorFree[s_EditorFreeCount++] = slot;
        return false;
    }
    // Full: the oldest voice still sounding fades out (its slot is freed once it has stopped) - never a hard stop mid-waveform.
    if (s_EditorLive >= kEditorVoices) {
        int oldest = -1;
        for (int i = 0; i < s_EditorActiveCount; ++i) {
            const int o = s_EditorActive[i];
            if (!s_EditorVoices[o].Stolen && (oldest < 0 || s_EditorVoices[o].Order < s_EditorVoices[oldest].Order)) oldest = o;
        }
        if (oldest >= 0) {
            ma_sound_stop_with_fade_in_milliseconds(&s_EditorVoices[oldest].Sound, kEditorStealFadeMs);
            s_EditorVoices[oldest].Stolen = true;
            --s_EditorLive;
        }
    }
    v.Clip = clip;
    v.Order = ++s_EditorOrder;
    v.Stolen = false;
    s_EditorActive[s_EditorActiveCount++] = slot;
    ++s_EditorLive;
    return true;
}

bool AudioEngine::PlayEditor(const std::string& path, float volume, float pitch, float pan) {
    if (!s_Initialized || !s_EditorGroupReady) return false;
    const ClipId id = LoadClip(path); // UI clips are tiny: decoded once, kept (cached by path)
    return id >= 0 && PlayEditor(id, volume, pitch, pan);
}

void AudioEngine::SetEditorVolume(float volume) {
    s_EditorVolume = std::clamp(volume, 0.0f, 1.0f);
    if (s_EditorGroupReady) ma_sound_group_set_volume(&s_EditorGroup, s_EditorVolume); // (glides: the group smooths its volume)
}

int AudioEngine::EditorVoiceCount() { return s_EditorLive; }

AudioEngine::LoopId AudioEngine::StartEditorLoop(ClipId id, float volume, float pitch, float pan, unsigned fadeInMs) {
    if (!s_Initialized || !s_EditorGroupReady || id < 0 || (size_t)id >= s_EditorClips.size()) return -1;
    const std::shared_ptr<const EditorClip>& clip = s_EditorClips[(size_t)id];
    if (!clip) return -1;
    int slot = -1;
    for (int pass = 0; pass < 2 && slot < 0; ++pass) {
        for (int i = 0; i < kEditorLoops; ++i)
            if (!s_EditorLoops[i].Clip) { slot = i; break; }
        if (slot < 0 && pass == 0) ReapEditorLoops(); // every slot busy (some still fading out): free any that have ended
    }
    if (slot < 0) return -1;
    EditorLoop& l = s_EditorLoops[slot];
    ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, clip->Channels, clip->FrameCount, clip->Pcm, nullptr);
    bc.sampleRate = clip->SampleRate;
    if (ma_audio_buffer_init(&bc, &l.Buffer) != MA_SUCCESS) return -1;
    // Pitch stays live (no NO_PITCH): the control moves it every frame.
    if (ma_sound_init_from_data_source(&s_Engine, (ma_data_source*)&l.Buffer, MA_SOUND_FLAG_NO_SPATIALIZATION, &s_EditorGroup, &l.Sound) != MA_SUCCESS) {
        ma_audio_buffer_uninit(&l.Buffer);
        return -1;
    }
    ma_sound_set_looping(&l.Sound, MA_TRUE);
    ma_sound_set_pitch(&l.Sound, std::clamp(pitch, 0.25f, 4.0f));
    ma_sound_set_pan(&l.Sound, std::clamp(pan, -1.0f, 1.0f));
    ma_sound_set_fade_in_milliseconds(&l.Sound, 0.0f, std::max(volume, 0.0f), fadeInMs);
    if (ma_sound_start(&l.Sound) != MA_SUCCESS) {
        ma_sound_uninit(&l.Sound);
        ma_audio_buffer_uninit(&l.Buffer);
        return -1;
    }
    l.Clip = clip;
    l.Stopping = false;
    l.Generation = (l.Generation + 1) & 0x7FFFFFu;
    return (LoopId)(slot | (int32_t)(l.Generation << kEditorLoopSlotBits));
}

void AudioEngine::SetEditorLoop(LoopId loop, float volume, float pitch, float pan) {
    EditorLoop* l = LiveEditorLoop(loop);
    if (!l) return;
    ma_sound_set_fade_in_milliseconds(&l->Sound, -1.0f, std::max(volume, 0.0f), kEditorLoopGlideMs); // (-1: from where it is now)
    ma_sound_set_pitch(&l->Sound, std::clamp(pitch, 0.25f, 4.0f));
    ma_sound_set_pan(&l->Sound, std::clamp(pan, -1.0f, 1.0f));
}

void AudioEngine::StopEditorLoop(LoopId loop, unsigned fadeOutMs) {
    EditorLoop* l = LiveEditorLoop(loop);
    if (!l) return;
    ma_sound_stop_with_fade_in_milliseconds(&l->Sound, fadeOutMs);
    l->Stopping = true; // freed by the reap once it has stopped
}

bool AudioEngine::EditorLoopPlaying(LoopId loop) {
    EditorLoop* l = LiveEditorLoop(loop);
    return l && ma_sound_is_playing(&l->Sound);
}

AudioEngine::ClipId AudioEngine::CreateClip(const std::string& name, const float* pcm, uint64_t frames, uint32_t channels, uint32_t sampleRate) {
    if (!pcm || frames == 0 || channels == 0 || sampleRate == 0) return -1;
    const auto known = s_EditorClipIds.find(name);
    if (known != s_EditorClipIds.end()) return known->second;
    const size_t bytes = (size_t)frames * channels * sizeof(float);
    void* copy = ma_malloc(bytes, nullptr);
    if (!copy) return -1;
    std::memcpy(copy, pcm, bytes);
    auto clip = std::make_shared<EditorClip>();
    clip->Pcm = static_cast<float*>(copy);
    clip->Channels = channels;
    clip->SampleRate = sampleRate;
    clip->FrameCount = frames;
    clip->Path = name;
    const ClipId id = (ClipId)s_EditorClips.size();
    s_EditorClips.push_back(std::move(clip));
    s_EditorClipIds.emplace(name, id);
    return id;
}

void AudioEngine::StopPreview() {
    if (s_PreviewSound) {
        ma_sound_stop(s_PreviewSound.get());
        ma_sound_uninit(s_PreviewSound.get());
        s_PreviewSound.reset();
    }
    s_PreviewPath.clear();
}

bool AudioEngine::IsPreviewPlaying(const std::string& path) {
    if (!s_PreviewSound || s_PreviewPath != path) return false;
    return ma_sound_is_playing(s_PreviewSound.get());
}

bool AudioEngine::IsInitialized() { return s_Initialized; }
