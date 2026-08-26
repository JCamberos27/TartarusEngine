#include "AudioEngine.h"
#include "miniaudio.h"
#include <fstream>
#include <vector>
#include <iostream>

namespace {
ma_engine s_Engine;
bool s_Initialized = false;
std::vector<std::unique_ptr<ma_sound>> s_LoopingSounds;
std::unique_ptr<ma_sound> s_PreviewSound;
std::string s_PreviewPath;
}

void AudioEngine::Init() {
    if (s_Initialized) return;
    if (ma_engine_init(nullptr, &s_Engine) != MA_SUCCESS) {
        std::cerr << "Failed to initialize audio engine" << std::endl;
        return;
    }
    s_Initialized = true;
}

void AudioEngine::Shutdown() {
    if (!s_Initialized) return;
    StopPreview();
    for (auto& sound : s_LoopingSounds) {
        ma_sound_uninit(sound.get());
    }
    s_LoopingSounds.clear();
    ma_engine_uninit(&s_Engine);
    s_Initialized = false;
}

bool AudioEngine::Load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

void AudioEngine::Play(const std::string& path, float volume, bool loop) {
    if (!s_Initialized) return;

    if (!loop) {
        ma_engine_play_sound(&s_Engine, path.c_str(), nullptr);
        return;
    }

    auto sound = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(&s_Engine, path.c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr, sound.get()) != MA_SUCCESS) {
        std::cerr << "Failed to load sound: " << path << std::endl;
        return;
    }
    ma_sound_set_looping(sound.get(), MA_TRUE);
    ma_sound_set_volume(sound.get(), volume);
    ma_sound_start(sound.get());
    s_LoopingSounds.push_back(std::move(sound));
}

void AudioEngine::StopAll() {
    for (auto& sound : s_LoopingSounds) {
        ma_sound_stop(sound.get());
        ma_sound_uninit(sound.get());
    }
    s_LoopingSounds.clear();
}

void AudioEngine::PlayPreview(const std::string& path) {
    StopPreview();
    if (!s_Initialized) return;

    auto sound = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(&s_Engine, path.c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr, sound.get()) != MA_SUCCESS) {
        std::cerr << "Failed to load sound: " << path << std::endl;
        return;
    }
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
