#pragma once

#include <string>

struct AudioMixComponent;

// The dynamic half of the mix (docs/AUDIO.md, "Mix hierarchy" and "Dynamic mix"). The static half is the manifest: every file's
// mix_db puts it on its tier, and a DistanceModel per 3D category says how that level holds with distance. On top of that:
//
//  - Mix groups: every voice belongs to one, from its key and whether it is the player's own (2D) or in the world (3D).
//  - Ducking: a loud event at the listener (a shot, a round cracking past) ducks the groups under it - the beds, the player's
//    own foley, the debris - by their depth, scaled by how loud the event was; it holds, then releases. The threat and feedback
//    groups (soldiers' shots and steps, flybys, hit markers) are never ducked: they are what the player has to hear.
//  - Focus: aiming down sights quietens the beds and the player's own foley a little, so the world comes forward.
//  - Air: a 3D voice is low-passed with distance (high frequencies die first), on top of occlusion.

enum class MixGroup {
    Default = 0,  // anything not classified
    Weapon,       // the player's own gun: shot layers, tails
    Threat,       // soldiers' shots, rounds passing the listener
    Feedback,     // hit markers, flesh hits
    OwnFoley,     // the player's own steps, cloth, gear, reloads
    NpcFoley,     // soldiers' steps, gear, reloads, melee
    World,        // bullet impacts, bodies falling
    Debris,       // shell casings
    Bed,          // ambience loops
    Count
};
const char* MixGroupName(MixGroup g);
MixGroup MixGroupForKey(const std::string& key, bool at2D);

// How a 3D category's level holds with distance. Its spec level holds at RefM; closer, the level rises SlopeDb per halving up to
// NearDb above it (reached at MinDistance()); farther, it falls SlopeDb per doubling, held past MaxM. OffsetDb is added to the
// file's own mix_db when it plays in the world (a soldier's shot is the player's shot files, quieter; a soldier's step is the
// player's step files, louder).
struct DistanceModel {
    float RefM = 0.0f;
    float SlopeDb = 6.0f;
    float NearDb = 0.0f;
    float MaxM = 40.0f;
    float OffsetDb = 0.0f;
    bool Valid() const { return RefM > 0.0f && SlopeDb > 0.0f; }
    float Factor() const;          // miniaudio's exponential rolloff factor: SlopeDb / 6.02
    float MinDistance() const;     // where the close-up cap is reached
    float StartGainDb() const { return OffsetDb + NearDb; } // the voice's gain on top of mix_db (the rolloff takes it down from there)
    float GainDbAt(float d) const; // OffsetDb at RefM; the curve the engine plays
};

// Cutoff (Hz) of the air low-pass at `distance` (20 kHz = open).
float AirCutoffHz(const AudioMixComponent& s, float distance);

class MixDucker {
public:
    void Configure(const AudioMixComponent& s);
    // A loud event at the listener, dB re the player's own shot.
    void Key(float levelDb);
    void SetFocus(bool on) { m_FocusOn = on; }
    void Update(float dt);
    float GainDb(MixGroup g) const;
    float Gain(MixGroup g) const;
    float Amount() const { return m_Amount; }  // 0..1, the duck envelope
    float Focus() const { return m_Focus; }    // 0..1
    float AttackSeconds() const { return m_Attack; }
    void Reset();

private:
    bool m_Enabled = true, m_FocusEnabled = true, m_FocusOn = false;
    float m_ThresholdDb = -20.0f, m_FullDb = -6.0f;
    float m_Attack = 0.015f, m_Hold = 0.15f, m_Release = 0.8f, m_FocusTime = 0.25f;
    float m_DuckDb[(int)MixGroup::Count] = {};
    float m_FocusDb[(int)MixGroup::Count] = {};
    float m_Amount = 0.0f, m_HoldLeft = 0.0f, m_Focus = 0.0f;
};
