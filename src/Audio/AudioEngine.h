#pragma once
#include <string>
#include <cstdint>
#include <glm/glm.hpp>

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

    // Loads (and caches) a sound resource from disk. Returns false on failure.
    static bool Load(const std::string& path);

    // Starts a voice and returns its handle (InvalidHandle if the engine is down or the file
    // failed to load). Voices start unspatialized — full volume regardless of listener position,
    // which is what UI/one-shot sounds want. Call SetPosition() to turn a voice into a 3D source.
    static SoundHandle Play(const std::string& path, float volume = 1.0f, bool loop = false);

    // All no-ops / false for a stale or invalid handle.
    static void Stop(SoundHandle handle);
    static void SetVolume(SoundHandle handle, float volume);
    static void SetPitch(SoundHandle handle, float pitch);
    static bool IsPlaying(SoundHandle handle);

    // Placing a voice in the world implicitly enables spatialization on it.
    static void SetPosition(SoundHandle handle, const glm::vec3& position);
    // Distance attenuation for a positioned voice: full volume within minDistance, silent-ish
    // past maxDistance, rolloff shaping the curve between them (miniaudio's inverse model).
    static void SetAttenuation(SoundHandle handle, float minDistance, float maxDistance,
                               float rolloff = 1.0f);

    // Drives the 3D listener. Called once per frame from the Play-mode camera; while not
    // playing, the listener simply stays wherever it was last put.
    static void SetListener(const glm::vec3& position, const glm::vec3& forward,
                            const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f));

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
