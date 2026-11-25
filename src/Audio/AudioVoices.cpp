#include "AudioEngineInternal.h"
#include "Log.h"
#include "AssetDatabase.h"
#include "Core/Paths.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <filesystem>

using namespace AudioInternal;

namespace {
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
    if (index >= g_Audio.Voices.size()) return nullptr;
    Voice& voice = g_Audio.Voices[index];
    if (!voice.Sound || voice.Generation != generation) return nullptr;
    return voice.Sound.get();
}

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
} // namespace

namespace AudioInternal {

void FreeSlot(uint32_t index) {
    Voice& voice = g_Audio.Voices[index];
    if (!voice.Sound) return;
    ma_sound_stop(voice.Sound.get());
    ma_sound_uninit(voice.Sound.get());
    voice.Sound.reset();
    if (voice.Split) { // after the sound: nothing feeds them any more
        ma_splitter_node_uninit(voice.Split.get(), nullptr);
        voice.Split.reset();
    }
    if (voice.Lpf) {
        OcclusionUninit(*voice.Lpf);
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
    g_Audio.FreeSlots.push_back(index);
}

} // namespace AudioInternal

namespace {
// `path` decoded whole into the engine's own output format/rate (PreloadDecodeFormat), so playing a preloaded clip
// needs no per-instance resample - just a straight PCM copy into a fresh ma_audio_buffer. Null when it won't decode.
// No engine state: safe on any thread.
std::shared_ptr<PreloadedClip> DecodeClip(const std::string& path, AudioEngine::PreloadFormat format) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, format.Channels, format.SampleRate);
    ma_uint64 frameCount = 0;
    void* pFrames = nullptr;
    if (ma_decode_file(path.c_str(), &config, &frameCount, &pFrames) != MA_SUCCESS) return nullptr;
    auto clip = std::make_shared<PreloadedClip>();
    clip->Format = config.format;
    clip->Channels = config.channels;
    clip->SampleRate = config.sampleRate;
    clip->FrameCount = frameCount;
    const size_t bytes = (size_t)frameCount * config.channels * ma_get_bytes_per_sample(config.format);
    const uint8_t* begin = static_cast<const uint8_t*>(pFrames);
    clip->Data.assign(begin, begin + bytes);
    ma_free(pFrames, nullptr);
    return clip;
}

// Load's failure: logged and remembered (Reload(path) lifts it) - unless it is a Drive file not fetched yet, which the
// asset scan's warning already counts.
void NoteLoadFailed(const std::string& path) {
    if (AssetDatabase::IsUnfetchedExternal(path)) return;
    Log::Error("Audio: failed to preload '" + path + "'.", LogContext::Asset(path));
    g_Audio.LoadFailed.insert(path);
}
} // namespace

struct AudioEngine::DecodedSounds {
    std::vector<std::pair<std::string, std::shared_ptr<PreloadedClip>>> Clips; // null: the file wouldn't decode
};

AudioEngine::PreloadFormat AudioEngine::PreloadDecodeFormat() {
    PreloadFormat f;
    if (g_Audio.Initialized) {
        f.Channels = ma_engine_get_channels(&g_Audio.Engine);
        f.SampleRate = ma_engine_get_sample_rate(&g_Audio.Engine);
    }
    return f;
}

std::shared_ptr<AudioEngine::DecodedSounds> AudioEngine::DecodeForPreload(const std::vector<std::string>& paths,
                                                                          PreloadFormat format, const std::atomic<bool>* cancel,
                                                                          std::chrono::microseconds pause, const std::atomic<bool>* hurry) {
    auto out = std::make_shared<DecodedSounds>();
    out->Clips.reserve(paths.size());
    for (const std::string& p : paths) {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;
        out->Clips.emplace_back(p, DecodeClip(p, format));
        if (pause.count() > 0 && !(hurry && hurry->load(std::memory_order_relaxed)))
            std::this_thread::sleep_for(pause); // a breather between files for the frames it overlaps (none once waited on)
    }
    return out;
}

int AudioEngine::AdoptPreloaded(DecodedSounds& sounds) {
    int adopted = 0;
    for (auto& [path, clip] : sounds.Clips) {
        if (g_Audio.Preloaded.count(path) || g_Audio.LoadFailed.count(path)) continue; // Load or Reload got there first
        if (!clip) { NoteLoadFailed(path); continue; }
        g_Audio.Preloaded.emplace(path, std::move(clip));
        ++adopted;
    }
    sounds.Clips.clear();
    return adopted;
}

std::size_t AudioEngine::DecodedBytes(const DecodedSounds& sounds) {
    std::size_t n = 0;
    for (const auto& [path, clip] : sounds.Clips)
        if (clip) n += clip->Data.size();
    return n;
}

int AudioEngine::DecodedCount(const DecodedSounds& sounds) { return (int)sounds.Clips.size(); }

bool AudioEngine::IsPreloaded(const std::string& path) { return g_Audio.Preloaded.count(path) != 0; }

bool AudioEngine::Load(const std::string& path) {
    if (g_Audio.Preloaded.find(path) != g_Audio.Preloaded.end()) return true; // already preloaded
    if (g_Audio.LoadFailed.count(path)) return false;                    // failed before (logged then); Reload(path) lifts it
    std::shared_ptr<PreloadedClip> clip = DecodeClip(path, PreloadDecodeFormat());
    if (!clip) {
        NoteLoadFailed(path);
        return false;
    }
    g_Audio.Preloaded.emplace(path, std::move(clip));
    return true;
}

bool AudioEngine::Reload(const std::string& path) {
    namespace fs = std::filesystem;
    const fs::path changed = fs::u8path(path).lexically_normal();
    auto same = [&](const std::string& key) { return key == path || fs::u8path(ProjectPaths::Resolve(key)).lexically_normal() == changed; };
    // a fixed file gets another chance: forget its remembered failures (decode, editor clip, impulse response, no-slot index)
    for (auto it = g_Audio.LoadFailed.begin(); it != g_Audio.LoadFailed.end();) it = same(*it) ? g_Audio.LoadFailed.erase(it) : std::next(it);
    for (auto it = g_Audio.ClipFailed.begin(); it != g_Audio.ClipFailed.end();) it = same(*it) ? g_Audio.ClipFailed.erase(it) : std::next(it);
    for (auto it = g_Audio.IrCache.begin(); it != g_Audio.IrCache.end();) it = same(it->first) ? g_Audio.IrCache.erase(it) : std::next(it);
    for (ReverbNode* r : g_Audio.Reverbs)
        if (r)
            for (auto it = r->IrIndex.begin(); it != r->IrIndex.end();) it = (it->second < 0 && same(it->first)) ? r->IrIndex.erase(it) : std::next(it);
    std::vector<std::string> keys;
    for (const auto& [key, clip] : g_Audio.Preloaded)
        if (key == path || fs::u8path(ProjectPaths::Resolve(key)).lexically_normal() == changed) keys.push_back(key);
    bool reloaded = false;
    for (const std::string& key : keys) {
        const std::shared_ptr<const PreloadedClip> previous = g_Audio.Preloaded[key];
        g_Audio.Preloaded.erase(key);
        if (Load(key)) reloaded = true;
        else g_Audio.Preloaded[key] = previous;
    }
    return reloaded;
}

AudioEngine::SoundHandle AudioEngine::Play(const std::string& path, float volume, bool loop, Bus bus, float startOffsetSeconds, const VoiceFx* fx) {
    if (!g_Audio.Initialized) return InvalidHandle;
    const int busIndex = std::clamp((int)bus, 0, kBusCount - 1);
    ma_sound_group* group = g_Audio.BusesReady ? &g_Audio.Buses[busIndex] : nullptr;

    auto sound = std::make_unique<ma_sound>();
    std::unique_ptr<ma_audio_buffer> buffer;
    std::shared_ptr<const PreloadedClip> clipRef;

    auto cached = g_Audio.Preloaded.find(path);
    if (cached != g_Audio.Preloaded.end()) {
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
        if (ma_sound_init_from_data_source(&g_Audio.Engine, (ma_data_source*)buffer.get(), 0, group,
                                           sound.get()) != MA_SUCCESS) {
            Log::Error("Audio: failed to play preloaded '" + path + "'.", LogContext::Asset(path));
            ma_audio_buffer_uninit(buffer.get());
            return InvalidHandle;
        }
    } else if (ma_sound_init_from_file(&g_Audio.Engine, path.c_str(), MA_SOUND_FLAG_STREAM, group, nullptr,
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
    std::unique_ptr<OcclusionNode> lpf;
    std::unique_ptr<ma_splitter_node> split;
    if (fx && group && ma_engine_get_channels(&g_Audio.Engine) == 2) {
        ma_node* tail = sound.get();
        const ma_uint32 sr = ma_engine_get_sample_rate(&g_Audio.Engine);
        if (fx->Occlusion) {
            auto n = std::make_unique<OcclusionNode>();
            const double hz = std::clamp((double)fx->Cutoff, 80.0, 20000.0);
            const ma_lpf_config lc = ma_lpf_config_init(ma_format_f32, 2, sr, hz, 2);
            if (ma_lpf_init(&lc, nullptr, &n->Lpf) == MA_SUCCESS) {
                n->Rate = sr;
                n->Target.store(hz);
                n->Applied = hz;
                if (InitNode(&OcclusionVtable, &n->Base)) {
                    ma_node_set_output_bus_volume(&n->Base, 0, std::max(fx->Gain, 0.0f));
                    ma_node_attach_output_bus(tail, 0, &n->Base, 0);
                    tail = &n->Base;
                    lpf = std::move(n);
                } else {
                    ma_lpf_uninit(&n->Lpf, nullptr);
                }
            }
        }
        if (fx->ReverbSend > 0.0f && g_Audio.Reverbs[0]) {
            auto n = std::make_unique<ma_splitter_node>();
            ma_splitter_node_config sc = ma_splitter_node_config_init(2);
            if (ma_splitter_node_init(ma_engine_get_node_graph(&g_Audio.Engine), &sc, nullptr, n.get()) == MA_SUCCESS) {
                ma_node_attach_output_bus(tail, 0, n.get(), 0);
                ma_node_attach_output_bus(n.get(), 0, group, 0);
                ma_node_attach_output_bus(n.get(), 1, &g_Audio.Reverbs[fx->ReverbBus == 1 && g_Audio.Reverbs[1] ? 1 : 0]->Base, 0);
                ma_node_set_output_bus_volume(n.get(), 1, fx->ReverbSend);
                tail = n.get();
                split = std::move(n);
            }
        }
        if (lpf && !split) ma_node_attach_output_bus(&lpf->Base, 0, group, 0);
    }
    auto dropChain = [&] {
        if (split) ma_splitter_node_uninit(split.get(), nullptr);
        if (lpf) OcclusionUninit(*lpf);
    };
    if (ma_sound_start(sound.get()) != MA_SUCCESS) {
        Log::Error("Audio: failed to start sound '" + path + "'.", LogContext::Asset(path));
        ma_sound_uninit(sound.get());
        dropChain();
        if (buffer) ma_audio_buffer_uninit(buffer.get());
        return InvalidHandle;
    }

    uint32_t index;
    if (!g_Audio.FreeSlots.empty()) {
        index = g_Audio.FreeSlots.back();
        g_Audio.FreeSlots.pop_back();
    } else {
        // Slot indices have to fit in kIndexBits. Hitting this means ~65k simultaneous voices,
        // which is a bug elsewhere; drop the sound rather than hand out an aliasing handle.
        if (g_Audio.Voices.size() > kIndexMask) {
            Log::Error("Audio: voice limit reached; dropping '" + path + "'.", LogContext::Asset(path));
            ma_sound_uninit(sound.get());
            dropChain();
            if (buffer) ma_audio_buffer_uninit(buffer.get()); // (a preloaded clip's buffer, after the sound that read it)
            return InvalidHandle;
        }
        index = static_cast<uint32_t>(g_Audio.Voices.size());
        g_Audio.Voices.emplace_back();
    }

    Voice& voice = g_Audio.Voices[index];
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
    Voice& v = g_Audio.Voices[handle & kIndexMask];
    ReverbNode* r = g_Audio.Reverbs[bus == 1 ? 1 : 0];
    if (v.Split && r) ma_node_attach_output_bus(v.Split.get(), 1, &r->Base, 0);
}

void AudioEngine::SetOcclusion(SoundHandle handle, float cutoffHz, float gain) {
    if (!Resolve(handle)) return;
    Voice& v = g_Audio.Voices[handle & kIndexMask];
    if (!v.Lpf) return;
    ma_node_set_output_bus_volume(&v.Lpf->Base, 0, std::max(gain, 0.0f));
    const double hz = (cutoffHz <= 0.0f || cutoffHz >= 20000.0f) ? 20000.0 : std::max(80.0, (double)cutoffHz);
    // Only when the cutoff really moves (half a percent is inaudible). The audio thread applies the target itself, between blocks.
    if (std::abs(hz - v.LpfHz) <= v.LpfHz * 0.005) return;
    v.LpfHz = hz;
    v.Lpf->Target.store(hz, std::memory_order_relaxed);
}

int AudioEngine::VoiceCount() {
    int n = 0;
    for (const Voice& v : g_Audio.Voices)
        if (v.Sound) ++n;
    return n;
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
    if (g_Audio.Initialized) ma_engine_listener_set_velocity(&g_Audio.Engine, 0, v.x, v.y, v.z);
}

void AudioEngine::SetListener(const glm::vec3& position, const glm::vec3& forward,
                              const glm::vec3& up) {
    if (!g_Audio.Initialized) return;
    ma_engine_listener_set_position(&g_Audio.Engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&g_Audio.Engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&g_Audio.Engine, 0, up.x, up.y, up.z);
}

void AudioEngine::StopAll() {
    for (uint32_t i = 0; i < g_Audio.Voices.size(); ++i) FreeSlot(i);
}

void AudioEngine::SetBusVolume(Bus bus, float volume) {
    const int b = std::clamp((int)bus, 0, kBusCount - 1);
    g_Audio.BusVolume[b] = std::clamp(volume, 0.0f, 1.0f);
    if (g_Audio.BusesReady) ma_sound_group_set_volume(&g_Audio.Buses[b], g_Audio.BusVolume[b]);
    if (b == 0) UpdateReturnGains(); // the returns follow the SFX bus
}
