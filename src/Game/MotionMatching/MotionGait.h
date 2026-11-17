#pragma once

// The gait speed table (docs/MOTION_MATCHING_HANDOFF.md decision "Speeds"): the player's and NPCs' walk, jog, run and
// crouch speeds are retuned to the speeds the mocap actually moves at, so the motion matcher only ever needs small speed
// warping.
//
// Measure() reads them off a database by "leg plateaus". The pack's takes cross a 9 m capture area back and forth, so a
// take is legs of speeding up, cruising and slowing down, with turnarounds between. A leg is a stretch of at least
// kLegSeconds where the root moves forward (not Strafe, not Airborne; crouched only for crouch takes) faster than
// 0.5 m/s, turning under kLegYawRateDeg. Its plateau is its best kWindowSeconds mean speed. The table speed is the
// median plateau over the legs of each take group (the first tag: walk, jog, run, crouch). For run, only legs whose
// plateau is labelled Run count: a run take's other legs are the walk-backs between sprints.
// Strafe speeds: the same over legs of Strafe frames (travel over 50 degrees off the facing); the pack's strafes are
// slower than its forward gaits. Run has no strafe takes.
// Per-frame percentiles don't work here: the runs barely cruise (43 steady frames above 2.9 m/s in the whole bake).
// kMeasured: what Measure() gives on the current bake (Manny.mmanim, 2026-11-16), for when no database is loaded.

#include <cstdint>
#include <string>

class MotionDatabase;

struct GaitSpeedTable {
    float Walk = 1.27f, Jog = 1.85f, Run = 3.91f, CrouchWalk = 0.79f; // m/s (kMeasured)
    int Legs[4] = {0, 0, 0, 0};                                         // legs measured per group: walk, jog, run, crouch
    float P25[4] = {0, 0, 0, 0}, P50[4] = {0, 0, 0, 0}, P75[4] = {0, 0, 0, 0}, Max[4] = {0, 0, 0, 0};
    float Strafe[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // m/s, median strafe-leg plateau per group (0: none measured)
    int StrafeLegs[4] = {0, 0, 0, 0};

    static constexpr float kLegSeconds = 1.0f;
    static constexpr float kLegYawRateDeg = 20.0f; // over +-0.25 s
    static constexpr float kWindowSeconds = 0.5f;

    static GaitSpeedTable Measure(const MotionDatabase& db);
    // The table's speed for a gait (MotionLabel::Gait), or crouch-walk when `crouch` and the gait is not idle.
    float SpeedFor(int gait, bool crouch) const;
    // The strafe speed for a gait (the nearest slower gait's strafe when it has none, else 0.75 x the forward speed).
    float StrafeFor(int gait, bool crouch) const;
    // The gait a speed falls under: the nearest table speed (idle under half a walk).
    int GaitFor(float speed, bool crouch) const;
    std::string Describe() const;
};
