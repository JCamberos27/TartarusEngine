#include "AudioEngine.h"
#include "Log.h"
#include "ConvolutionReverb.h"
#include "LoudnessMeter.h"
#include "MasterLimiter.h"
#include "miniaudio.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
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
    std::atomic<bool> Enabled{true};
    std::atomic<float> CeilingDb{-1.0f}, LookaheadMs{1.5f}, ReleaseMs{80.0f};
    std::atomic<float> LastGrDb{0.0f};
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
    LimiterSettings want;
    want.Enabled = n->Enabled.load(std::memory_order_relaxed);
    want.CeilingDb = n->CeilingDb.load(std::memory_order_relaxed);
    want.LookaheadMs = n->LookaheadMs.load(std::memory_order_relaxed);
    want.ReleaseMs = n->ReleaseMs.load(std::memory_order_relaxed);
    const LimiterSettings& have = n->Lim->Settings();
    if (want.Enabled != have.Enabled || want.CeilingDb != have.CeilingDb || want.ReleaseMs != have.ReleaseMs ||
        std::fabs(want.LookaheadMs - have.LookaheadMs) > 1e-4f)
        n->Lim->Configure(want);
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
void ApplyMasterVolume(ma_engine& engine) {
    ma_engine_set_volume(&engine, s_MutedFlag ? 0.0f : s_MasterVolume);
}
std::vector<uint32_t> s_FreeSlots;
bool s_Paused = false;

std::unique_ptr<ma_sound> s_PreviewSound;
std::string s_PreviewPath;

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
    }
    if (ma_engine_init(&cfg, &s_Engine) != MA_SUCCESS) {
        Log::Error("Audio: failed to initialize the audio engine.");
        return false;
    }
    s_Initialized = true;
    s_Offline = offline;
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
    // Off until someone calls SetPosition: an unpositioned voice would otherwise sit at the
    // origin and get attenuated against the listener, which is wrong for UI and preview sounds.
    ma_sound_set_spatialization_enabled(sound.get(), MA_FALSE);
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
    s_Limiter->Enabled.store(s.Enabled);
    s_Limiter->CeilingDb.store(std::min(s.CeilingDb, 0.0f));
    s_Limiter->LookaheadMs.store(std::clamp(s.LookaheadMs, 0.1f, 10.0f));
    s_Limiter->ReleaseMs.store(std::max(s.ReleaseMs, 1.0f));
}

LimiterSettings AudioEngine::GetMasterLimiter() {
    LimiterSettings s;
    if (s_Limiter) {
        s.Enabled = s_Limiter->Enabled.load();
        s.CeilingDb = s_Limiter->CeilingDb.load();
        s.LookaheadMs = s_Limiter->LookaheadMs.load();
        s.ReleaseMs = s_Limiter->ReleaseMs.load();
    }
    return s;
}

float AudioEngine::TakeLimiterGainReductionDb() { return s_Limiter ? s_Limiter->LastGrDb.exchange(0.0f, std::memory_order_relaxed) : 0.0f; }

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

void AudioEngine::SetRolloff(SoundHandle handle, Rolloff mode, float minDistance, float maxDistance) {
    ma_sound* sound = Resolve(handle);
    if (!sound) return;
    minDistance = std::max(minDistance, 0.01f);
    maxDistance = std::max(maxDistance, minDistance + 0.01f);
    ma_sound_set_attenuation_model(sound, mode == Rolloff::Linear ? ma_attenuation_model_linear
                                                                  : ma_attenuation_model_inverse);
    ma_sound_set_min_distance(sound, minDistance);
    ma_sound_set_max_distance(sound, maxDistance);
    ma_sound_set_rolloff(sound, 1.0f);
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
    ma_sound_start(sound.get());
    s_PreviewSound = std::move(sound);
    s_PreviewPath = path;
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
