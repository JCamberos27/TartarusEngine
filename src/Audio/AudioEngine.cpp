#include "AudioEngine.h"
#include "Log.h"
#include "miniaudio.h"
#include <fstream>
#include <memory>
#include <vector>

namespace {
ma_engine s_Engine;
bool s_Initialized = false;

// One voice slot per live (or recently live) sound. Slots are recycled rather than erased so a
// handle's index stays meaningful; Generation is bumped on every reuse so handles into a
// previous occupant of the slot no longer resolve.
struct Voice {
    std::unique_ptr<ma_sound> Sound;   // null when the slot is free
    uint16_t Generation = 1;           // never 0, so a live handle is never InvalidHandle
    bool Looping = false;
};

std::vector<Voice> s_Voices;
std::vector<uint32_t> s_FreeSlots;

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
}

void AudioEngine::Shutdown() {
    if (!s_Initialized) return;
    StopPreview();
    StopAll();
    s_Voices.clear();
    s_FreeSlots.clear();
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
        if (!voice.Sound || voice.Looping) continue;
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
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

AudioEngine::SoundHandle AudioEngine::Play(const std::string& path, float volume, bool loop) {
    if (!s_Initialized) return InvalidHandle;

    auto sound = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(&s_Engine, path.c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr,
                                sound.get()) != MA_SUCCESS) {
        Log::Error("Audio: failed to load sound '" + path + "'.");
        return InvalidHandle;
    }

    ma_sound_set_looping(sound.get(), loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume(sound.get(), volume);
    // Off until someone calls SetPosition: an unpositioned voice would otherwise sit at the
    // origin and get attenuated against the listener, which is wrong for UI and preview sounds.
    ma_sound_set_spatialization_enabled(sound.get(), MA_FALSE);
    if (ma_sound_start(sound.get()) != MA_SUCCESS) {
        Log::Error("Audio: failed to start sound '" + path + "'.");
        ma_sound_uninit(sound.get());
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
            Log::Error("Audio: voice limit reached; dropping '" + path + "'.");
            ma_sound_uninit(sound.get());
            return InvalidHandle;
        }
        index = static_cast<uint32_t>(s_Voices.size());
        s_Voices.emplace_back();
    }

    Voice& voice = s_Voices[index];
    voice.Sound = std::move(sound);
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

void AudioEngine::PlayPreview(const std::string& path) {
    StopPreview();
    if (!s_Initialized) return;

    auto sound = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(&s_Engine, path.c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr, sound.get()) != MA_SUCCESS) {
        Log::Error("Audio: failed to load sound '" + path + "'.");
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
