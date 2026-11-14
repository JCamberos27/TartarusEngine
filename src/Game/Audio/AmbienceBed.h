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
    // 0..1: how much an open portal between an indoor zone and the outside (no zone, or an outdoor one) near `listener` lets the
    // outside in: the portal's open amount x (1 - distance / reach), the most of any portal.
    static float DoorRelief(const ReverbZones& zones, const glm::vec3& listener, float reach);
    // The bed's gain (linear) and low-pass (Hz, log-interpolated) at an indoor share.
    static float IndoorGain(const AmbienceBedComponent& c, float share);
    static float IndoorCutoff(const AmbienceBedComponent& c, float share);

    void Start(const AmbienceBedComponent& c);
    // Once a frame after the zones' mix: starts the speakers (when the sound has files), moves them, sets their level and filter.
    // `indoorShare` is the target; the bed's own indoor amount follows it at most 1 per Glide seconds.
    void Update(float dt, const glm::vec3& listener, float indoorShare, SoundPlayer& player, SoundSet* set);
    void Stop(SoundPlayer& player);
    bool Active() const { return m_On; }
    const AmbienceBedComponent& Settings() const { return m_C; }
    float Level() const { return m_Level; }
    float Indoor() const { return m_Indoor; }

private:
    AmbienceBedComponent m_C;
    bool m_On = false;
    float m_Level = 0.0f;              // the fade-in, 0..1
    float m_SentCutoff = 20000.0f;
    float m_Indoor = -1.0f;            // the glided indoor amount (-1: not yet set: the first frame takes the target)
    AudioEngine::SoundHandle m_Handle[kSpeakers] = {AudioEngine::InvalidHandle, AudioEngine::InvalidHandle, AudioEngine::InvalidHandle,
                                                   AudioEngine::InvalidHandle};
    float m_Base[kSpeakers] = {0, 0, 0, 0}; // each voice's volume as started (mix level, Volume)
};
