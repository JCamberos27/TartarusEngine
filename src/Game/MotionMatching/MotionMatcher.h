#pragma once

// MotionMatcher: the motion matching runtime loop on one character (docs/MOTION_MATCHING_HANDOFF.md, Phase 3) - plays
// the database from a frame, searches when MotionSearchSchedule says so (every ~0.1 s, on an input change, or when the
// playing frame runs out), jumps to a better frame, and hides the jump with dead blending (Inertialization).
//
// The query: the playing frame's own pose features (feet, pelvis velocity) and the desired trajectory - three future
// samples of where the character will be and face (TrajectoryPredictor), turned into the character's root space with
// RootSynth::RootMatrix (the convention the features were baked in).
//
// Movement stays capsule-led (decision "Movement stays capsule-led"): the matcher picks the pose; the host places the
// body at the simulated character (the capsule in Phase 4). Pure: no Model; the host binds MotionPose to its rig
// (MotionMatcher::WritePose).

#include "Inertialization.h"
#include "MotionDatabase.h"
#include "MotionSearch.h"

#include <array>
#include <vector>

struct LocalTRS;

class MotionMatcher {
public:
    MotionSearchParams SearchParams;
    MotionSearchSchedule Schedule;
    Inertialization Inertializer;

    // The desired trajectory, world space: the character now and at the database's three trajectory times.
    struct Input {
        glm::vec3 Position{0.0f};
        float Yaw = 0.0f;
        std::array<glm::vec3, 3> FuturePosition{};
        std::array<float, 3> FutureYaw{};
        glm::vec3 DesiredVelocity{0.0f}; // for the schedule's input-change test
        float DesiredYaw = 0.0f;
    };

    // Speed warping (Phase 4): the database plays at this rate (the advance only; the search schedule and the dead
    // blend keep real time). 1 = as captured.
    float PlayRate = 1.0f;
    // Label-locked playback (Phase 4's airborne segment): no searches; the playing frame plays on to HoldFrame (when
    // >= 0) or its take's end and holds there.
    bool Locked = false;
    // A forced search (the filter changed, or the playing frame can't go on) weighs the pose features (feet, pelvis velocity)
    // by this: the playing pose has no continuation, so the trajectory picks and the dead blend hides the pose change. At 1
    // an odd pose (a jump's deep landing) kept matching odd poses after a crouch (Phase 4). 1 = always the same weights.
    float ForcedPoseWeight = 1.0f;
    std::int64_t HoldFrame = -1;

    void Reset(const MotionDatabase* db, std::uint32_t frame);
    void Update(float dt, const Input& in);
    // Jumps to `frame` now, dead-blending from what is on screen (the airborne segment's entry and exit).
    void ForceFrame(std::uint32_t frame, float dt);

    const MotionPose& Pose() const { return m_Out; }
    const MotionPose& Target() const { return m_Target; } // the database's pose this update (before the blend)
    std::uint32_t Frame() const { return m_Frame; }
    float Frac() const { return m_Frac; }
    const MotionSearchResult& LastResult() const { return m_Last; }
    const float* LastQueryRaw() const { return m_QueryRaw; } // kDims, raw (the trajectory part: what was asked)
    bool SearchedThisUpdate() const { return m_Searched; }
    bool JumpedThisUpdate() const { return m_Jumped; }

    // Stats since Reset.
    int Searches = 0, Jumps = 0;
    double SearchMsTotal = 0.0, SearchMsMax = 0.0;
    std::vector<float> SearchMs;

    // World point / facing -> the character's root space (MotionDatabase's feature convention).
    static glm::vec3 ToRootSpace(const glm::vec3& charPos, float charYaw, float restYaw, const glm::vec3& world);
    static glm::vec3 DirToRootSpace(float charYaw, float restYaw, float yaw);
    // The 12 raw trajectory features (dims 15-26) of an Input.
    static void TrajectoryFeatures(const Input& in, float restYaw, float* out12);
    // Writes a pose into a model's local pose through a stream binding (MotionClipStream::Bind).
    static void WritePose(const MotionPose& pose, const MotionClipStream& stream, const MotionClipStream::Binding& binding,
                          std::vector<LocalTRS>& out);

private:
    void PoseAndBlend(float dt);
    const MotionDatabase* m_Db = nullptr;
    std::uint32_t m_Frame = 0;
    float m_Frac = 0.0f;
    MotionPose m_Out, m_Prev, m_Target;
    bool m_HaveOut = false, m_Searched = false, m_Jumped = false;
    MotionSearchResult m_Last;
    float m_QueryRaw[MotionDatabase::kDims] = {};
    std::uint32_t m_LastKey = 0xFFFFFFFFu;
};
