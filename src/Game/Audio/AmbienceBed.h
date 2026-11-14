#pragma once

#include "AudioEngine.h"
#include "Components.h"

#include <glm/glm.hpp>

class SoundPlayer;
struct SoundSet;
struct ReverbZoneMix;
class ReverbZones;

// The scene's Ambience Bed component at runtime: four looping 3D voices (the sound's variants, one per speaker) a Radius from the
// listener at fixed compass points. Each frame the speakers move with the listener (never with its turning), and the bed's gain and
// low-pass follow the share of the mix the listener's indoor Reverb Zones claim. Owned by WeaponAudio, next to the zone beds.
class AmbienceBed {
public:
    static constexpr int kSpeakers = 4;

    // Speaker i (0 north +Z, 1 east +X, 2 south, 3 west) relative to the listener.
    static glm::vec3 SpeakerOffset(int i, float radius, float height);
    // 0..1: how much of the listener's zone mix indoor zones (Indoor Small / Large) claim.
    static float IndoorShare(const ReverbZoneMix& mix, const ReverbZones& zones);
    // The bed's gain (linear) and low-pass (Hz, log-interpolated) at an indoor share.
    static float IndoorGain(const AmbienceBedComponent& c, float share);
    static float IndoorCutoff(const AmbienceBedComponent& c, float share);

    void Start(const AmbienceBedComponent& c);
    // Once a frame after the zones' mix: starts the speakers (when the sound has files), moves them, sets their level and filter.
    void Update(float dt, const glm::vec3& listener, float indoorShare, SoundPlayer& player, SoundSet* set);
    void Stop(SoundPlayer& player);
    bool Active() const { return m_On; }
    const AmbienceBedComponent& Settings() const { return m_C; }
    float Level() const { return m_Level; }

private:
    AmbienceBedComponent m_C;
    bool m_On = false;
    float m_Level = 0.0f;              // the fade-in, 0..1
    float m_SentCutoff = 20000.0f;
    AudioEngine::SoundHandle m_Handle[kSpeakers] = {AudioEngine::InvalidHandle, AudioEngine::InvalidHandle, AudioEngine::InvalidHandle,
                                                   AudioEngine::InvalidHandle};
    float m_Base[kSpeakers] = {0, 0, 0, 0}; // each voice's volume as started (mix level, Volume)
};
