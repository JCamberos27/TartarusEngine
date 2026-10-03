#include "AudioEngine.h"
#include "Log.h"
#include "miniaudio.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
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

// --- the reverb bus ------------------------------------------------------------------------------------------------
// One node, input bus 0 = every send (miniaudio sums what is attached to an input bus), output bus 0 -> the engine endpoint.
// The game thread writes the target parameters into atomics; the audio callback copies them into the FDN, which glides.
struct ReverbNode {
    ma_node_base Base;
    ReverbFdn* Fdn = nullptr;
    std::atomic<float> Target[7]; // the six ReverbParams, then the glide time
    std::atomic<bool> Enabled{true};
    std::atomic<std::uint64_t> Callbacks{0};
    std::atomic<double> TotalMicros{0.0}, MaxMicros{0.0};
    std::atomic<int> LastFrames{0};
};
ReverbNode* s_Reverb = nullptr;
bool s_ReverbInit = false;
float s_ReverbReturn = 1.0f;
ReverbParams s_ReverbTarget;

void ReverbProcess(ma_node* pNode, const float** ppIn, ma_uint32* pFrameCountIn, float** ppOut, ma_uint32* pFrameCountOut) {
    ReverbNode* n = reinterpret_cast<ReverbNode*>(pNode);
    const ma_uint32 frames = *pFrameCountOut;
    float* out = ppOut[0];
    if (!n->Fdn || !n->Enabled.load(std::memory_order_relaxed)) {
        std::fill(out, out + 2 * (size_t)frames, 0.0f);
        (void)pFrameCountIn;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    ReverbParams p;
    p.RoomSize = n->Target[0].load(std::memory_order_relaxed);
    p.DecayTime = n->Target[1].load(std::memory_order_relaxed);
    p.HfDamping = n->Target[2].load(std::memory_order_relaxed);
    p.PreDelayMs = n->Target[3].load(std::memory_order_relaxed);
    p.WetLevel = n->Target[4].load(std::memory_order_relaxed);
    p.EarlyLateMix = n->Target[5].load(std::memory_order_relaxed);
    n->Fdn->SetTarget(p);
    n->Fdn->SetSmoothingTime(n->Target[6].load(std::memory_order_relaxed));
    static thread_local std::vector<float> silence;
    const float* in = ppIn && ppIn[0] ? ppIn[0] : nullptr;
    if (!in) {
        if (silence.size() < 2 * (size_t)frames) silence.assign(2 * (size_t)frames, 0.0f); // (grows once, on the first callback)
        in = silence.data();
    }
    n->Fdn->Process(in, out, (int)frames);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    n->Callbacks.fetch_add(1, std::memory_order_relaxed);
    n->TotalMicros.store(n->TotalMicros.load(std::memory_order_relaxed) + us, std::memory_order_relaxed);
    if (us > n->MaxMicros.load(std::memory_order_relaxed)) n->MaxMicros.store(us, std::memory_order_relaxed);
    n->LastFrames.store((int)frames, std::memory_order_relaxed);
}

ma_node_vtable s_ReverbVtable = {ReverbProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};

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

void AudioEngine::Init() {
    if (s_Initialized) return;
    if (ma_engine_init(nullptr, &s_Engine) != MA_SUCCESS) {
        Log::Error("Audio: failed to initialize the audio engine.");
        return;
    }
    s_Initialized = true;
    s_BusesReady = true;
    for (int b = 0; b < kBusCount; ++b) {
        if (ma_sound_group_init(&s_Engine, 0, nullptr, &s_Buses[b]) != MA_SUCCESS) {
            Log::Error("Audio: failed to create mixer bus " + std::to_string(b) + "; playing without buses.");
            for (int k = 0; k < b; ++k) ma_sound_group_uninit(&s_Buses[k]);
            s_BusesReady = false;
            break;
        }
        ma_sound_group_set_volume(&s_Buses[b], s_BusVolume[b]);
    }
    ApplyMasterVolume(s_Engine);
    // The reverb bus. If it can't be built, voices simply start without a send.
    {
        const ma_uint32 sr = ma_engine_get_sample_rate(&s_Engine);
        ma_node_config nc = ma_node_config_init();
        ma_uint32 ch = 2;
        nc.vtable = &s_ReverbVtable;
        nc.pInputChannels = &ch;
        nc.pOutputChannels = &ch;
        auto* node = new ReverbNode();
        node->Fdn = new ReverbFdn((int)sr);
        node->Fdn->Reset(s_ReverbTarget);
        const float t[6] = {s_ReverbTarget.RoomSize, s_ReverbTarget.DecayTime, s_ReverbTarget.HfDamping, s_ReverbTarget.PreDelayMs,
                            s_ReverbTarget.WetLevel, s_ReverbTarget.EarlyLateMix};
        for (int i = 0; i < 6; ++i) node->Target[i].store(t[i]);
        node->Target[6].store(0.35f);
        if (ma_engine_get_channels(&s_Engine) == 2 && ma_node_init(ma_engine_get_node_graph(&s_Engine), &nc, nullptr, &node->Base) == MA_SUCCESS) {
            ma_node_attach_output_bus(&node->Base, 0, ma_engine_get_endpoint(&s_Engine), 0);
            ma_node_set_output_bus_volume(&node->Base, 0, s_ReverbReturn * s_BusVolume[0]);
            s_Reverb = node;
            s_ReverbInit = true;
        } else {
            Log::Warn("Audio: could not create the reverb bus; voices play without a send.");
            delete node->Fdn;
            delete node;
        }
    }
}

void AudioEngine::Shutdown() {
    if (!s_Initialized) return;
    StopPreview();
    StopAll();
    s_Voices.clear();
    s_FreeSlots.clear();
    UnloadAll();
    s_Paused = false;
    if (s_BusesReady) for (int b = 0; b < kBusCount; ++b) ma_sound_group_uninit(&s_Buses[b]);
    s_BusesReady = false;
    if (s_Reverb) {
        ma_node_uninit(&s_Reverb->Base, nullptr);
        delete s_Reverb->Fdn;
        delete s_Reverb;
        s_Reverb = nullptr;
    }
    s_ReverbInit = false;
    ma_engine_uninit(&s_Engine);
    s_Initialized = false;
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
            ma_lpf_node_config lc = ma_lpf_node_config_init(2, sr, 20000.0, 2);
            if (ma_lpf_node_init(ma_engine_get_node_graph(&s_Engine), &lc, nullptr, n.get()) == MA_SUCCESS) {
                ma_node_attach_output_bus(tail, 0, n.get(), 0);
                tail = n.get();
                lpf = std::move(n);
            }
        }
        if (fx->ReverbSend > 0.0f && s_Reverb) {
            auto n = std::make_unique<ma_splitter_node>();
            ma_splitter_node_config sc = ma_splitter_node_config_init(2);
            if (ma_splitter_node_init(ma_engine_get_node_graph(&s_Engine), &sc, nullptr, n.get()) == MA_SUCCESS) {
                ma_node_attach_output_bus(tail, 0, n.get(), 0);
                ma_node_attach_output_bus(n.get(), 0, group, 0);
                ma_node_attach_output_bus(n.get(), 1, &s_Reverb->Base, 0);
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

void AudioEngine::SetOcclusion(SoundHandle handle, float cutoffHz) {
    if (!Resolve(handle)) return;
    Voice& v = s_Voices[handle & kIndexMask];
    if (!v.Lpf) return;
    const double hz = (cutoffHz <= 0.0f || cutoffHz >= 20000.0f) ? 20000.0 : std::max(80.0, (double)cutoffHz);
    ma_lpf_node_config lc = ma_lpf_node_config_init(2, ma_engine_get_sample_rate(&s_Engine), hz, 2);
    ma_lpf_node_reinit(&lc.lpf, v.Lpf.get());
}

void AudioEngine::SetReverbSend(SoundHandle handle, float level) {
    if (!Resolve(handle)) return;
    Voice& v = s_Voices[handle & kIndexMask];
    if (v.Split) ma_node_set_output_bus_volume(v.Split.get(), 1, std::max(level, 0.0f));
}

void AudioEngine::SetReverbEnabled(bool enabled) {
    if (s_Reverb) s_Reverb->Enabled.store(enabled);
}
bool AudioEngine::ReverbEnabled() { return s_Reverb && s_Reverb->Enabled.load(); }

void AudioEngine::SetReverb(const ReverbParams& p) {
    s_ReverbTarget = p;
    if (!s_Reverb) return;
    const float t[6] = {p.RoomSize, p.DecayTime, p.HfDamping, p.PreDelayMs, p.WetLevel, p.EarlyLateMix};
    for (int i = 0; i < 6; ++i) s_Reverb->Target[i].store(t[i], std::memory_order_relaxed);
}

void AudioEngine::SetReverbGlide(float seconds) {
    if (s_Reverb) s_Reverb->Target[6].store(std::max(seconds, 0.001f), std::memory_order_relaxed);
}

void AudioEngine::SetReverbReturn(float level) {
    s_ReverbReturn = std::max(level, 0.0f);
    if (s_Reverb) ma_node_set_output_bus_volume(&s_Reverb->Base, 0, s_ReverbReturn * s_BusVolume[0]);
}

AudioEngine::ReverbStats AudioEngine::GetReverbStats() {
    ReverbStats st;
    if (!s_Reverb) return st;
    st.Callbacks = s_Reverb->Callbacks.load();
    st.TotalMicros = s_Reverb->TotalMicros.load();
    st.MaxMicros = s_Reverb->MaxMicros.load();
    st.Frames = s_Reverb->LastFrames.load();
    return st;
}

void AudioEngine::ResetReverbStats() {
    if (!s_Reverb) return;
    s_Reverb->Callbacks.store(0);
    s_Reverb->TotalMicros.store(0.0);
    s_Reverb->MaxMicros.store(0.0);
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
    if (s_Reverb && b == 0) ma_node_set_output_bus_volume(&s_Reverb->Base, 0, s_ReverbReturn * s_BusVolume[0]); // the return follows the SFX bus
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
