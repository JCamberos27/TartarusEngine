#pragma once

#include "AudioEngine.h"
#include "EnvironmentProbe.h"
#include "ReverbZones.h"
#include "SoundManifest.h"

#include <glm/glm.hpp>

#include <cstdio>
#include <string>
#include <vector>

class WeaponAudio;

// The reverb side of the weapon audio: the reverb the listener's space asks for (zones first, the listener's raycast probe for what
// no zone claims), the remote room's reverb for voices heard through a portal, and the zones' looping ambience beds. Owned by
// WeaponAudio, which feeds it the listener each frame; the bus settings, the zones and the player it reads through its owner.
class WeaponReverb {
public:
    explicit WeaponReverb(WeaponAudio& owner) : m_Owner(owner) {}

    // Fresh state (a new Play session): nothing sent, no beds, no class impulse responses (CacheClassIrs after the manifest loads).
    void Reset();
    // The manifest's ir.<class> entries, resolved once per manifest (ClassIr / ClassPreDelayMs run for every layer of every frame's reverb).
    void CacheClassIrs(const SoundManifest& manifest);
    // The impulse responses: the classes' (manifest ir.<class>) and the zones' own, decoded and prepared now, not on the first frame in a room.
    void PreloadIrs();
    // Once a frame, after the player's update: the listener's zones, the remote room's reverb, the listener's reverb, the beds.
    void Update(float dt, const glm::vec3& listener);
    void StopAmbience();

    // The reverb at `pos`: the zones' reverbs (layered, faded), the probe's class reverbs for what no zone claims; the two heaviest
    // impulse responses, weights summing to 1 (Count 0: no impulse response for this space, the reverb is off).
    AudioEngine::ReverbSpec ReverbAt(const glm::vec3& pos);
    AudioEngine::ReverbSpec ReverbAt(const glm::vec3& pos, const ReverbZoneMix& zones); // (`zones` = Zones().Mix(pos), already computed)
    const AudioEngine::ReverbSpec& CurrentReverb() const { return m_ReverbSent; }
    // The class impulse response / pre-delay (from the manifest's ir.<class>) and the final wet level of a space (dB): the Reverb Bus's
    // calibrated level for the class + the preset's trim.
    std::string ClassIr(int tailClass) const;
    float ClassPreDelayMs(int tailClass) const;
    float ClassWetDb(int tailClass) const;

    // --- zone ambience beds ---
    struct AmbienceDebug { std::string Key; float Target = 0.0f, Level = 0.0f; bool Playing = false; };
    const std::vector<AmbienceDebug>& Ambience() const { return m_AmbienceDebug; }
    // What the editor's Audio panel shows: the zones at the listener (the last Update).
    const ReverbZoneMix& ListenerMix() const { return m_ListenerMix; }
    // The listener's space, for the reverb (its own cache and cast count).
    EnvironmentProbe& ListenerProbe() { return m_ListenerProbe; }
    const EnvironmentProbe& ListenerProbe() const { return m_ListenerProbe; }

private:
    WeaponAudio& m_Owner;
    EnvironmentProbe m_ListenerProbe;
    AudioEngine::ReverbSpec m_ReverbSent, m_RemoteSent;
    ReverbZoneMix m_ListenerMix;
    bool m_ReverbValid = false;
    double m_RemoteHoldUntil = -1e9;
    bool m_RemoteActive = false;
    double m_LastReverbLog = -1e9;
    unsigned m_IrWarned = 0;         // bit per tail class: the "no impulse response" line was printed
    struct ClassIrEntry { std::string File; float PreDelayMs = 0.0f; };
    ClassIrEntry m_ClassIr[kSpaceClassCount];
    std::vector<AmbienceDebug> m_AmbienceDebug;
    struct Bed {
        std::string Key;
        AudioEngine::SoundHandle Handle = AudioEngine::InvalidHandle;
        float BaseVolume = 1.0f;     // the file's volume with its mix_db
        float Level = 0.0f;          // the fade, 0 .. the target
        float Target = 0.0f;
    };
    std::vector<Bed> m_Beds;
    void UpdateRemoteRoom();
    void UpdateListenerReverb(const glm::vec3& listener);
    void UpdateAmbience(float dt);
    bool BuildLayer(const ReverbPreset& p, float weight, AudioEngine::ReverbLayerSpec& out);
    static bool SpecDiffers(const AudioEngine::ReverbSpec& a, const AudioEngine::ReverbSpec& b);
};
