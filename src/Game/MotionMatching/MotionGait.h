#pragma once

// The gait speed table (docs/MOTION_MATCHING_HANDOFF.md decision "Speeds"): the player's and NPCs' stroll, walk, jog, run and
// crouch speeds are the speeds the mocap actually moves at, so the motion matcher only ever needs small speed warping.
//
// Measure() reads them off a database by "leg plateaus", per style (MotionLabel::Style - what the actor was doing, the
// bake's label from the take's intent). The pack's takes cross a 9 m capture area back and forth, so a take is legs of
// speeding up, cruising and slowing down, with turnarounds between. A leg is a stretch of at least kLegSeconds of one
// style where the root moves forward (not Strafe, not Airborne; crouched only for the crouch style) faster than 0.5 m/s,
// turning under kLegYawRateDeg. Its plateau is its best kWindowSeconds mean speed. The table speed is the median plateau
// over the style's legs. (Run: the run takes' frames are Run only at sprint speed - their walk-backs are Walk - so its legs
// are the dashes' peaks.)
// Strafe speeds: the same over legs of Strafe frames (travel over 50 degrees off the facing), sideways (under kBackDeg off
// the facing) and backwards (kBackDeg or more) apart: the pack's strafes are slower than its forward gaits, and the two
// directions differ by style (on the full bake: the stroll 0.59 sideways against 0.77 backwards, the jog 1.38 / 1.47, the
// walk 1.17 / 1.10 m/s) - one strafe speed for both had the capsule off one of them. Run has no strafe takes.
// Per-frame percentiles don't work here: the runs barely cruise (43 steady frames above 2.9 m/s in the whole bake).
// The defaults: what Measure() gives on the current bake, for when no database is loaded.

#include <cstdint>
#include <string>

class MotionDatabase;

struct GaitSpeedTable {
    // Per group: 0 walk, 1 jog, 2 run, 3 crouch, 4 stroll, 5 sprint (the order the table always had, stroll and sprint added).
    static constexpr int kGroups = 6;
    float Walk = 1.27f, Jog = 1.85f, Run = 3.91f, CrouchWalk = 0.79f, Stroll = 0.69f, Sprint = 3.26f; // m/s
    int Legs[kGroups] = {0, 0, 0, 0, 0, 0};
    float P25[kGroups] = {}, P50[kGroups] = {}, P75[kGroups] = {}, Max[kGroups] = {};
    float Strafe[kGroups] = {}; // m/s, median sideways strafe-leg plateau per group (0: none measured)
    int StrafeLegs[kGroups] = {};
    float Back[kGroups] = {};   // m/s, median backwards leg plateau per group (0: none measured)
    int BackLegs[kGroups] = {};
    static constexpr float kBackDeg = 135.0f;

    static constexpr float kLegSeconds = 1.0f;
    static constexpr float kLegYawRateDeg = 20.0f; // over +-0.25 s
    static constexpr float kWindowSeconds = 0.5f;

    // The group of a style (-1 for Stand), and back.
    static int GroupOf(int style);
    static int StyleOf(int group);

    static GaitSpeedTable Measure(const MotionDatabase& db);
    // The table's speed for a gait (MotionLabel::Gait), or crouch-walk when `crouch` and the gait is not idle.
    float SpeedFor(int gait, bool crouch) const;
    // The strafe speed for a gait (the nearest slower gait's strafe when it has none, else 0.75 x the forward speed).
    float StrafeFor(int gait, bool crouch) const;
    // ... and for a style (MotionLabel::Style; 0 for Stand).
    float StyleSpeed(int style) const;
    float StyleStrafe(int style) const;
    float StyleBack(int style) const; // backwards (the style's sideways speed when it has no backward legs)
    // The gait a speed falls under: the nearest table speed (idle under half a walk).
    int GaitFor(float speed, bool crouch) const;
    std::string Describe() const;
};
