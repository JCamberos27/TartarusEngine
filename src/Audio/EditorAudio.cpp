#include "AudioEngineInternal.h"
#include "Log.h"
#include "AssetDatabase.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace AudioInternal;

namespace AudioInternal {

void ResetEditorSlots() {
    for (int i = 0; i < kEditorSlots; ++i) g_Audio.EditorFree[i] = kEditorSlots - 1 - i;
    g_Audio.EditorFreeCount = kEditorSlots;
    g_Audio.EditorActiveCount = 0;
    g_Audio.EditorLive = 0;
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
    for (int i = 0; i < g_Audio.EditorActiveCount;) {
        const int slot = g_Audio.EditorActive[i];
        EditorVoice& v = g_Audio.EditorVoices[slot];
        if (ma_sound_at_end(&v.Sound) || !ma_sound_is_playing(&v.Sound)) {
            if (!v.Stolen) --g_Audio.EditorLive;
            FreeEditorVoice(v);
            g_Audio.EditorFree[g_Audio.EditorFreeCount++] = slot;
            g_Audio.EditorActive[i] = g_Audio.EditorActive[--g_Audio.EditorActiveCount];
        } else {
            ++i;
        }
    }
}

void FreeEditorLoop(EditorLoop& l) {
    ma_sound_stop(&l.Sound);
    ma_sound_uninit(&l.Sound);
    ma_audio_buffer_uninit(&l.Buffer);
    l.Clip.reset();
    l.Stopping = false;
}

// Frees every loop that has finished fading out.
void ReapEditorLoops() {
    for (EditorLoop& l : g_Audio.EditorLoops)
        if (l.Clip && l.Stopping && !ma_sound_is_playing(&l.Sound)) FreeEditorLoop(l);
}

} // namespace AudioInternal

namespace {
// The slot a LoopId names, or null for none / stale / already stopping.
EditorLoop* LiveEditorLoop(AudioEngine::LoopId id) {
    if (id < 0) return nullptr;
    const int slot = id & ((1 << kEditorLoopSlotBits) - 1);
    if (slot >= AudioEngine::kEditorLoops) return nullptr;
    EditorLoop& l = g_Audio.EditorLoops[slot];
    if (!l.Clip || l.Stopping || (uint32_t)(id >> kEditorLoopSlotBits) != (l.Generation & 0x7FFFFFu)) return nullptr;
    return &l;
}
} // namespace

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
    if (!g_Audio.Initialized) return;

    auto sound = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(&g_Audio.Engine, path.c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr, sound.get()) != MA_SUCCESS) {
        Log::Error("Audio: failed to load sound '" + path + "'.", LogContext::Asset(path));
        return;
    }
    ma_sound_set_spatialization_enabled(sound.get(), MA_FALSE);
    g_Audio.PreviewSound = std::move(sound);
    g_Audio.PreviewPath = path;
    ApplyMasterVolume(g_Audio.Engine); // its level
    ma_sound_start(g_Audio.PreviewSound.get());
}

AudioEngine::ClipId AudioEngine::LoadClip(const std::string& path) {
    const auto known = g_Audio.EditorClipIds.find(path);
    if (known != g_Audio.EditorClipIds.end()) return known->second;
    if (g_Audio.ClipFailed.count(path)) return -1; // failed before (logged then); Reload(path) lifts it
    // The file's own channel count (0 = native: the built-in set is mono, half the memory of stereo) at the engine's rate, so
    // a voice needs no resample; the engine node spreads a mono clip over both channels before it pans.
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, g_Audio.Initialized ? ma_engine_get_sample_rate(&g_Audio.Engine) : 48000);
    ma_uint64 frames = 0;
    void* pcm = nullptr;
    if (ma_decode_file(path.c_str(), &config, &frames, &pcm) != MA_SUCCESS || !pcm || frames == 0 || config.channels == 0) {
        ma_free(pcm, nullptr);
        if (!AssetDatabase::IsUnfetchedExternal(path)) {
            Log::Error("Audio: failed to load the editor sound '" + path + "'.", LogContext::Asset(path));
            g_Audio.ClipFailed.insert(path);
        }
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
    const ClipId id = (ClipId)g_Audio.EditorClips.size();
    g_Audio.EditorClips.push_back(std::move(clip));
    g_Audio.EditorClipIds.emplace(path, id);
    return id;
}

void AudioEngine::UnloadClip(ClipId id) {
    if (id < 0 || (size_t)id >= g_Audio.EditorClips.size() || !g_Audio.EditorClips[(size_t)id]) return;
    g_Audio.EditorClipIds.erase(g_Audio.EditorClips[(size_t)id]->Path);
    g_Audio.EditorClips[(size_t)id].reset(); // (a voice playing it holds its own reference: freed when that ends)
}

bool AudioEngine::PlayEditor(ClipId id, float volume, float pitch, float pan) {
    if (!g_Audio.Initialized || !g_Audio.EditorGroupReady || id < 0 || (size_t)id >= g_Audio.EditorClips.size()) return false;
    const std::shared_ptr<const EditorClip>& clip = g_Audio.EditorClips[(size_t)id];
    if (!clip) return false;
    if (g_Audio.EditorFreeCount == 0) ReapEditorVoices(); // every slot busy (stolen voices still fading): free any that have ended
    if (g_Audio.EditorFreeCount == 0) return false;
    const int slot = g_Audio.EditorFree[--g_Audio.EditorFreeCount];
    EditorVoice& v = g_Audio.EditorVoices[slot];
    ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, clip->Channels, clip->FrameCount, clip->Pcm, nullptr);
    bc.sampleRate = clip->SampleRate; // (a clip loaded before Init at another rate than the device's still plays at its own speed)
    if (ma_audio_buffer_init(&bc, &v.Buffer) != MA_SUCCESS) {
        g_Audio.EditorFree[g_Audio.EditorFreeCount++] = slot;
        return false;
    }
    // At pitch 1 no resampler at all (miniaudio would otherwise run one at 1.0, unfiltered); jittered and pitched cues keep it.
    const bool unpitched = std::fabs(pitch - 1.0f) < 0.001f;
    const ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION | (unpitched ? (ma_uint32)MA_SOUND_FLAG_NO_PITCH : 0u);
    if (ma_sound_init_from_data_source(&g_Audio.Engine, (ma_data_source*)&v.Buffer, flags, &g_Audio.EditorGroup, &v.Sound) != MA_SUCCESS) {
        ma_audio_buffer_uninit(&v.Buffer);
        g_Audio.EditorFree[g_Audio.EditorFreeCount++] = slot;
        return false;
    }
    ma_sound_set_volume(&v.Sound, std::max(volume, 0.0f));
    if (!unpitched) ma_sound_set_pitch(&v.Sound, std::clamp(pitch, 0.25f, 4.0f));
    ma_sound_set_pan(&v.Sound, std::clamp(pan, -1.0f, 1.0f));
    if (ma_sound_start(&v.Sound) != MA_SUCCESS) {
        FreeEditorVoice(v);
        g_Audio.EditorFree[g_Audio.EditorFreeCount++] = slot;
        return false;
    }
    // Full: the oldest voice still sounding fades out (its slot is freed once it has stopped) - never a hard stop mid-waveform.
    if (g_Audio.EditorLive >= kEditorVoices) {
        int oldest = -1;
        for (int i = 0; i < g_Audio.EditorActiveCount; ++i) {
            const int o = g_Audio.EditorActive[i];
            if (!g_Audio.EditorVoices[o].Stolen && (oldest < 0 || g_Audio.EditorVoices[o].Order < g_Audio.EditorVoices[oldest].Order)) oldest = o;
        }
        if (oldest >= 0) {
            ma_sound_stop_with_fade_in_milliseconds(&g_Audio.EditorVoices[oldest].Sound, kEditorStealFadeMs);
            g_Audio.EditorVoices[oldest].Stolen = true;
            --g_Audio.EditorLive;
        }
    }
    v.Clip = clip;
    v.Order = ++g_Audio.EditorOrder;
    v.Stolen = false;
    g_Audio.EditorActive[g_Audio.EditorActiveCount++] = slot;
    ++g_Audio.EditorLive;
    return true;
}

bool AudioEngine::PlayEditor(const std::string& path, float volume, float pitch, float pan) {
    if (!g_Audio.Initialized || !g_Audio.EditorGroupReady) return false;
    const ClipId id = LoadClip(path); // UI clips are tiny: decoded once, kept (cached by path)
    return id >= 0 && PlayEditor(id, volume, pitch, pan);
}

void AudioEngine::SetEditorVolume(float volume) {
    g_Audio.EditorVolume = std::clamp(volume, 0.0f, 1.0f);
    if (g_Audio.EditorGroupReady) ma_sound_group_set_volume(&g_Audio.EditorGroup, g_Audio.EditorVolume); // (glides: the group smooths its volume)
}

int AudioEngine::EditorVoiceCount() { return g_Audio.EditorLive; }

AudioEngine::LoopId AudioEngine::StartEditorLoop(ClipId id, float volume, float pitch, float pan, unsigned fadeInMs) {
    if (!g_Audio.Initialized || !g_Audio.EditorGroupReady || id < 0 || (size_t)id >= g_Audio.EditorClips.size()) return -1;
    const std::shared_ptr<const EditorClip>& clip = g_Audio.EditorClips[(size_t)id];
    if (!clip) return -1;
    int slot = -1;
    for (int pass = 0; pass < 2 && slot < 0; ++pass) {
        for (int i = 0; i < kEditorLoops; ++i)
            if (!g_Audio.EditorLoops[i].Clip) { slot = i; break; }
        if (slot < 0 && pass == 0) ReapEditorLoops(); // every slot busy (some still fading out): free any that have ended
    }
    if (slot < 0) return -1;
    EditorLoop& l = g_Audio.EditorLoops[slot];
    ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, clip->Channels, clip->FrameCount, clip->Pcm, nullptr);
    bc.sampleRate = clip->SampleRate;
    if (ma_audio_buffer_init(&bc, &l.Buffer) != MA_SUCCESS) return -1;
    // Pitch stays live (no NO_PITCH): the control moves it every frame.
    if (ma_sound_init_from_data_source(&g_Audio.Engine, (ma_data_source*)&l.Buffer, MA_SOUND_FLAG_NO_SPATIALIZATION, &g_Audio.EditorGroup, &l.Sound) != MA_SUCCESS) {
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
    const auto known = g_Audio.EditorClipIds.find(name);
    if (known != g_Audio.EditorClipIds.end()) return known->second;
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
    const ClipId id = (ClipId)g_Audio.EditorClips.size();
    g_Audio.EditorClips.push_back(std::move(clip));
    g_Audio.EditorClipIds.emplace(name, id);
    return id;
}

void AudioEngine::StopPreview() {
    if (g_Audio.PreviewSound) {
        ma_sound_stop(g_Audio.PreviewSound.get());
        ma_sound_uninit(g_Audio.PreviewSound.get());
        g_Audio.PreviewSound.reset();
    }
    g_Audio.PreviewPath.clear();
}

bool AudioEngine::IsPreviewPlaying(const std::string& path) {
    if (!g_Audio.PreviewSound || g_Audio.PreviewPath != path) return false;
    return ma_sound_is_playing(g_Audio.PreviewSound.get());
}
