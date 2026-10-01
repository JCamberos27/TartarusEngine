#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <random>

// The enemy AI's pure maths - no world, no physics - so every rule here is unit tested
// (Tests/UnitTests.cpp): how fast a soldier notices the player, what it remembers after losing
// sight, how likely a round is to hit, and the response curves its decisions are scored on.

// --- Perception ---------------------------------------------------------------------------------
struct PerceptionSettings {
    float FocalHalfAngle = 30.0f;      // degrees: sharp vision
    float PeripheralHalfAngle = 80.0f; // degrees: the edge of what it sees at all
    float Range = 90.0f;               // metres
    float BaseRate = 2.4f;             // awareness per second: a standing target, 8 m, dead ahead, fully seen
};

struct DetectionInput {
    float Distance = 10.0f;        // metres
    float AngleDeg = 0.0f;         // off the look direction
    int VisiblePoints = 5;         // of the 5 body points sight rays reach (head, chest, pelvis, shoulders)
    float TargetSpeed = 0.0f;      // m/s
    bool TargetCrouched = false;
    bool TargetFiring = false;     // a muzzle flash gives anyone away
    float Suppression = 0.0f;      // 0..1: rounds snapping past keep heads down
    float Alertness = 0.0f;        // 0..1: already searching notices sooner
};
// Awareness per second the target builds (0 when it can't be seen at all).
float DetectionRate(const DetectionInput& in, const PerceptionSettings& s = {});

// What a soldier knows about the player.
struct TargetMemory {
    bool Known = false;              // awareness reached 1: in combat
    bool Visible = false;            // seen this check
    float Awareness = 0.0f;          // 0..1 (stays at 1 once known)
    glm::vec3 LastKnown{0.0f};       // where it was last seen (or heard)
    glm::vec3 LastVelocity{0.0f};
    float LastSeen = -1e9f;          // time of the last sighting
    float LastHeard = -1e9f;
    float Uncertainty = 0.0f;        // metres: how far it could be from LastKnown
    glm::vec3 Predicted(float now) const; // LastKnown carried on by its velocity for a moment
};
// One perception update: `visible` with the target at `seenPos` moving at `vel`, `rate` from
// DetectionRate. Out of sight the awareness of an unknown target fades and a known one's
// uncertainty grows. Returns true when this update made the target Known.
bool UpdateMemory(TargetMemory& m, bool visible, const glm::vec3& seenPos, const glm::vec3& vel, float rate, float now, float dt);
// A noise at `pos` (radius it carries `radius`) heard from `listener`: raises awareness by `loudness`
// at the source falling to 0 at the edge, and points LastKnown at it (with uncertainty). Returns
// the awareness added.
float HearNoise(TargetMemory& m, const glm::vec3& listener, const glm::vec3& pos, float radius, float loudness, float now);

// --- Accuracy -----------------------------------------------------------------------------------
enum class WeaponClass : std::uint8_t { Rifle, Shotgun };
struct AccuracyInput {
    float Distance = 15.0f;
    float TargetSpeed = 0.0f;
    float TimeOnTarget = 0.0f;   // seconds aiming at it without losing it
    float SelfSpeed = 0.0f;
    float Suppression = 0.0f;
    float Skill = 0.5f;
    float Difficulty = 1.0f;
    float VisibleFraction = 1.0f;
    bool TargetCrouched = false;
    bool OutsideTargetView = false; // shooting from where the player isn't looking
    bool Flinching = false;
    WeaponClass Weapon = WeaponClass::Rifle;
};
// The chance one round (one pellet line, for a shotgun) is aimed to hit. 0..0.85.
float HitProbability(const AccuracyInput& in);
// How long to hold fire after first seeing the target: a reaction time. Seconds.
float ReactionTime(float skill, float difficulty, bool peripheral, float r01);

// --- Utility curves -----------------------------------------------------------------------------
// A response curve on a 0..1 input. Linear: m*x + b. Quadratic: m*x^k + b. Logistic: k steepness
// about midpoint c. Bell: peak at c, width k. Results clamp to 0..1.
struct UtilityCurve {
    enum Kind : std::uint8_t { Linear, Quadratic, Logistic, Bell } K = Linear;
    float M = 1.0f, Kk = 1.0f, B = 0.0f, C = 0.5f;
    float Eval(float x) const;
};
// Product of considerations with Dave Mark's compensation, so a score from many factors isn't
// dragged to zero just for having many: 0..1.
float CombineScores(const float* scores, int count);

// --- Misc ---------------------------------------------------------------------------------------
float Smoothstep01(float x);
// Exponential approach factor for a time constant (seconds).
float AiFollow(float dt, float timeConstant);
