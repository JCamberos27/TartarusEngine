#pragma once

// MotionSearch: finds the database frame whose features best match a query (docs/MOTION_MATCHING_HANDOFF.md, Phase 3).
//
// Cost of a frame = sum over the 27 dims of weight[group] * (query - frame)^2 (both normalized, MotionDatabase), plus a
// soft gait cost GaitWeight * |gait - desired gait| (the gait labels overlap - a jog through a turn is slower than a
// walk's peak - so they bias, they don't filter), plus TransitionCost for any frame but the one already playing.
//
// The database's features are 16-bit (MotionDatabase::Quantized): the query is mapped into their space once per search
// (q' = (q - QB) / QA, the weights times QA^2), so every frame and box costs the same per-dim terms, as integers turned to
// floats - the cost is the normalized distance, and the boxes' bound stays exact.
//
// Filters (hard): a frame is a candidate only if it has every Require flag and none of the Forbid flags, and - when a
// style is asked for (DesiredStyle) - is of that style (MotionLabel::Style: what the actor was doing). The style is a filter,
// not a cost: the pack covers every style it has with its own strafes, starts, stops and turns, and as a bias it lost to the
// trajectory - a strafe's costs run 3-15, and at a weight of 1 the walk's strafes played the jog's (Step 2's first run). Excluded and
// NoSearch are always forbidden; Airborne is forbidden by default (the airborne segments are label-locked playback,
// not search). Crouch is a filter (Require it to crouch, Forbid it to stand).
//
// Speed: the 64-frame boxes, then the 16-frame boxes inside them, are skipped when their lower bound (the query's
// weighted distance to the box) can't beat the best cost so far, or when their flags say no frame in them can pass the
// filter; a frame's cost stops accumulating once it can't win. The result is exactly brute force's (SearchBrute): the
// bound never exceeds a frame's cost, in float too, since both sum the same per-dim terms in the same order.
//
// Deterministic ties: frames are visited in ascending order and only a strictly lower cost wins, so of equal frames the
// lowest index wins, and the frame already playing beats any equal candidate.
//
// MotionSearchSchedule says when to search: every Interval (~0.1 s), or at once when the input changed (the desired
// velocity or facing moved past a threshold, or the filter changed), or when the playing frame runs out of searchable
// frames or into frames the filter refuses.

#include "MotionDatabase.h"

#include <array>
#include <cstdint>

struct MotionSearchParams {
    float GroupWeight[MotionFeature::GroupCount] = {0.75f, 1.0f, 1.0f, 1.0f, 1.5f}; // footPos footVel pelvisVel trajPos trajDir
    float GaitWeight = 0.5f;     // per gait step away from the desired one
    float TransitionCost = 0.0f; // added to every candidate but the frame playing
    int IgnoreNear = 20;         // frames of the playing clip within this many of the playing frame are not candidates
    int MinRunway = 0;           // candidates (not the playing frame) need this many frames under the same labels (Runway)
    int MinIdleRunway = 0;       // ... and this many frames of the Idle gait from there on (IdleRunway; the standing idle search)
    std::uint8_t Require = 0;
    std::uint8_t Forbid = MotionLabel::Airborne;
    int DesiredGait = -1; // -1: no gait cost
    int DesiredStyle = -1; // MotionLabel::Style: only frames of it are candidates; -1: any
    // Frames recently shown, never candidates (MotionMatcher's reselect history: [first, last] global frames, padded). Jumping
    // back into what just played is a rewind - the same snippet again, a stutter (the idle's 0.41 s hand loop, Phase 4).
    static constexpr int kMaxAvoid = 8;
    std::array<std::int64_t, 2 * kMaxAvoid> Avoid{};
    int AvoidCount = 0;
};

struct MotionSearchResult {
    std::int64_t Frame = -1; // -1: nothing passed the filter (or the playing frame stays)
    float Cost = 0.0f;
    float CurrentCost = -1.0f;   // the playing frame's own cost (-1: none was playing or it doesn't pass)
    bool Switched = false;       // Frame differs from the playing frame
    std::uint32_t FramesScored = 0;
};

namespace MotionSearch {

// Flags no search ever matches.
inline constexpr std::uint8_t kAlwaysForbid = MotionLabel::Excluded | MotionLabel::NoSearch;

// The query from the playing frame's pose features (feet, pelvis velocity: dims 0-14, raw) and the desired trajectory
// (dims 15-26, raw, in the character's root space: MotionDatabase's convention). Writes kDims normalized floats.
void BuildQuery(const MotionDatabase& db, const float* poseRaw15, const float* trajRaw12, float* outNormalized);

// A frame's cost under `p` (no early out); +inf when it doesn't pass the filter.
float Cost(const MotionDatabase& db, const float* query, std::uint32_t frame, const MotionSearchParams& p);
bool Passes(std::uint8_t flags, const MotionSearchParams& p);
bool PassesStyle(std::uint8_t style, const MotionSearchParams& p);

// `current`: the frame playing (-1 if none): it competes as itself (no TransitionCost) and its clip's neighbours within
// IgnoreNear are skipped.
MotionSearchResult Search(const MotionDatabase& db, const float* query, const MotionSearchParams& p, std::int64_t current = -1);
// The same answer by scoring every frame (tests and the mmdb scenario check the boxes against it).
MotionSearchResult SearchBrute(const MotionDatabase& db, const float* query, const MotionSearchParams& p, std::int64_t current = -1);

} // namespace MotionSearch

struct MotionSearchSchedule {
    float Interval = 0.1f;       // s
    float VelocityChange = 0.5f; // m/s of desired velocity since the last search
    float FacingChangeDeg = 20.0f;
    // Held: no search on the timer (the input or filter changing, or the frame running out, still search). Standing idle the
    // input is constant and the playing take's continuation is known: re-asked every Interval, the search could only pick
    // the stillest frame again and again (Phase 4, the idle's rewind loop).
    bool Hold = false;
    // Call once per update; true when a search is due now (then it resets). `filterKey`: anything that identifies the
    // filter (Require/Forbid/DesiredGait packed), a change searches at once. `outOfFrames`: the playing frame can't go on.
    bool Tick(float dt, const glm::vec3& desiredVelocity, float desiredYaw, std::uint32_t filterKey, bool outOfFrames);
    void Reset() { m_First = true; }
    float SinceLast() const { return m_Timer; }
    int Searches = 0, ForcedByInput = 0, ForcedByEnd = 0;

private:
    bool m_First = true;
    float m_Timer = 0.0f;
    glm::vec3 m_Vel{0.0f};
    float m_Yaw = 0.0f;
    std::uint32_t m_Key = 0;
};
