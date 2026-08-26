#pragma once
#include <string>
#include <memory>
#include <unordered_map>

// Thin wrapper over miniaudio's ma_engine: loads sounds by path (wav/mp3/flac/ogg)
// and plays fire-and-forget or looping instances.
class AudioEngine {
public:
    static void Init();
    static void Shutdown();

    // Loads (and caches) a sound resource from disk. Returns false on failure.
    static bool Load(const std::string& path);

    // Plays a cached sound. If not already loaded, loads it first.
    static void Play(const std::string& path, float volume = 1.0f, bool loop = false);
    static void StopAll();

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
