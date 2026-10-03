#pragma once

#include "WeaponAudio.h"

#include "../Components.h"

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>

class World;

// Counts footfalls from distance travelled: one every `stepDistance` metres of planar travel, however the
// speed changes frame to frame (so the cadence follows speed exactly and a stop leaves the next step half done).
struct FoleyStepper {
    float Accum = 0.0f;
    int Advance(float distance, float stepDistance);
    void Reset() { Accum = 0.0f; }
};

// Footfalls from an animated body: each foot's height above the character's feet, every animated frame. A foot is
// swinging once it rises `lift` above its planted height; it touches down when it comes back to within `contact` of
// that height, or - a step up (stairs, foot IK) - when it was coming down and stops. Each touch-down re-bases the
// planted height; a planted foot's height follows the lowest it reaches, relaxing upward slowly (a slope, a crouch).
struct FootContactDetector {
    float Floor[2] = {0.0f, 0.0f}, Peak[2] = {0.0f, 0.0f}, Prev[2] = {0.0f, 0.0f};
    bool Lifted[2] = {false, false}, Falling[2] = {false, false};
    bool Primed = false;
    static constexpr float kRelax = 0.05f;    // m/s a planted foot's height creeps up toward where it stands
    static constexpr float kFallSpeed = 0.3f; // m/s down: the swing is coming down ...
    static constexpr float kStopSpeed = 0.1f; // ... and slower than this: it has landed (on higher ground)
    // Returns the feet that touched down this update: bit 0 left, bit 1 right.
    int Update(const float height[2], float dt, float lift, float contact);
    void Reset() { Primed = false; }
};

struct FoleyPlayerInput {
    glm::vec3 Velocity{0.0f};
    glm::vec3 Feet{0.0f};
    bool Grounded = true;
    bool Sprinting = false;
    bool Crouched = false;
    bool Jumped = false;
    // The view bob's strides (FirstPersonProcedural Bob.WalkStride / SprintStride, metres per cycle): the body
    // plants a foot twice per cycle, so footfalls come every half stride.
    float WalkStride = 1.6f, SprintStride = 2.2f;
    // The player's animated body (FirstPersonBody::FootHeights): with it the steps land on its feet's touch-downs.
    bool HaveFootHeights = false;
    float FootHeight[2] = {0.0f, 0.0f};
};

// Player and soldier foley (docs/AUDIO.md). Surfaces are foley categories (Audio/Foley/<surface>/<element>_<n>.wav,
// keys snd.foley.<surface>.<element>): walk, run, crouch, jump, land for each.
class FoleyAudio {
public:
    static FoleyAudio& Get();
    void Start(World& world);
    void Stop();
    bool Active() const { return m_Active; }
    // Once per frame while the player is in control.
    void UpdatePlayer(World& world, float dt, const FoleyPlayerInput& in);
    // A soldier's footfall at `feet` (3D, rolled off by the tuning's NPC distances).
    void NpcStep(World& world, const glm::vec3& feet, bool sprint);
    // A soldier walking: footfalls by distance on the same stride rule as the player's (his gait's, planar speed), each 3D.
    // `id` keeps each soldier's stride count. Soldiers beyond the NPC step range of the listener are skipped.
    void NpcWalk(World& world, int id, const glm::vec3& feet, const glm::vec3& velocity, bool sprint, float dt);
    // A soldier's animated feet (NpcBody::FootHeights), each frame it is animated: a step on each touch-down. `dt` = the time
    // since its last animated frame. Returns false (nothing done) with Steps From Feet off: walk him with NpcWalk instead.
    bool NpcFeet(World& world, int id, const glm::vec3& feet, const float height[2], bool sprint, float dt);
    const FoleyAudioComponent& Tuning() const { return m_T; }
    void SetTuning(const FoleyAudioComponent& t) { m_T = t; }
    void StartForTest(const FoleyAudioComponent& t) { m_T = t; m_Active = true; m_Stepper.Reset(); m_Feet.Reset(); m_PrevGrounded = true; m_PrevVy = 0.0f; m_Steps = 0; m_NpcSteppers.clear(); m_NpcFeet.clear(); }

    // --- pure rules (unit-tested) ---
    // The first surface in `table` ("wood=wood,plank;metal=metal") with a word in `name` (case-insensitive), else `fallback`.
    static std::string SurfaceFromName(const std::string& table, const std::string& name, const std::string& fallback);
    // Metres between footfalls: half the (walk .. sprint) stride, scaled.
    static float StepDistance(const FoleyAudioComponent& t, const FoleyPlayerInput& in);
    static float LandGain(const FoleyAudioComponent& t, float fallSpeed); // 0 below LandMinSpeed
    // The ground under `feet`: a downward ray's entity -> its physics material, tag and name -> the surface table.
    std::string SurfaceAt(World& world, const glm::vec3& feet) const;
    // What the player's last frame emitted, for tests.
    int StepsPlayed() const { return m_Steps; }

private:
    bool m_Active = false;
    FoleyAudioComponent m_T;
    FoleyStepper m_Stepper;
    FootContactDetector m_Feet;
    std::unordered_map<int, FoleyStepper> m_NpcSteppers;
    std::unordered_map<int, FootContactDetector> m_NpcFeet;
    bool m_PrevGrounded = true;
    float m_PrevVy = 0.0f;
    int m_Steps = 0;
    void Play(const std::string& surface, const std::string& element, float gain, bool at2D, const glm::vec3& pos);
};
