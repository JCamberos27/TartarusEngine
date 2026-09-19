#include "AudioEngine.h"
#include "Log.h"
#include "miniaudio.h"
#include <algorithm>
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
    std::unique_ptr<ma_audio_buffer> Buffer; // non-null only for a voice playing preloaded data (#202)
    std::shared_ptr<const PreloadedClip> Clip; // keeps Buffer's PCM alive past Unload() (#171)
    uint16_t Generation = 1;                 // never 0, so a live handle is never InvalidHandle
    bool Looping = false;
    bool Paused = false;                     // stopped by SetPaused(true); resumed, not reaped (#171)
};

std::vector<Voice> s_Voices;

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

AudioEngine::SoundHandle AudioEngine::Play(const std::string& path, float volume, bool loop, Bus bus) {
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
    if (ma_sound_start(sound.get()) != MA_SUCCESS) {
        Log::Error("Audio: failed to start sound '" + path + "'.", LogContext::Asset(path));
        ma_sound_uninit(sound.get());
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
            return InvalidHandle;
        }
        index = static_cast<uint32_t>(s_Voices.size());
        s_Voices.emplace_back();
    }

    Voice& voice = s_Voices[index];
    voice.Sound = std::move(sound);
    voice.Buffer = std::move(buffer);
    voice.Clip = std::move(clipRef);
    voice.Looping = loop;
    return MakeHandle(index, voice.Generation);
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
