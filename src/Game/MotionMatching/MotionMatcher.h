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
    // A jump into an idle take (MotionLabel::IdleTake) from a frame outside one - coming to rest - blends this long (s) in
    // place of Inertializer.Params.BlendTime: the arms of a stop or a walk settle into the idle's instead of snapping to it.
    // 0 = every jump blends alike.
    float IdleEntryBlendTime = 0.0f;
    std::int64_t HoldFrame = -1;
    // Standing idle (the host sets it each update): no searching on the timer - the playing take plays on while its
    // continuation fits the wanted trajectory: its root 1 s on within IdleTravel of where it's wanted and facing within
    // IdleTurn (radians) of the wanted facing. Past either (at most once per Schedule.Interval), or on an input change or
    // the frame running out, it searches as usual. (Searched every 0.1 s, a constant idle query always picked the take's
    // stillest moment again: a 0.41 s rewind loop of the hands, and no idle variety - Phase 4.)
    bool Idle = false;
    float IdleTravel = 0.25f, IdleTurn = 0.52f;
    // Frames shown within this long are never candidates (the reselect history: a jump back into them is a rewind), nor are
    // the kReselectPad frames either side of them.
    float ReselectSeconds = 1.0f;
    static constexpr int kReselectPad = 10;

    void Reset(const MotionDatabase* db, std::uint32_t frame);
    // Plays on in `db`, another instance of the same frames (the frame, the history and the blend kept; a frame `db` refuses
    // is left at the next search, as one played into).
    void Rebind(const MotionDatabase* db) { m_Db = db; }
    void Update(float dt, const Input& in);
    // Jumps to `frame` now, dead-blending from what is on screen (the airborne segment's entry and exit).
    void ForceFrame(std::uint32_t frame);

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
    int IdleRefits = 0; // idle searches because the continuation stopped fitting
    int ForcedByExcluded = 0; // searches forced because the playing frame is Excluded (it played into an excluded stretch)
    // Searches and jumps by why the search ran (Update; print-only): the first update after a Reset or the lock, the timer,
    // an input change (the desired velocity, the facing, the filter: MotionSearchSchedule's thresholds), the playing frame
    // unable to go on (an Excluded stretch - as ForcedByExcluded - its take's last frame, its NoSearch tail, or other flags the filter refuses:
    // crouch, airborne), and the idle refit. A search that runs while the frame can't go on counts as that end cause even
    // when the timer was due too. Jumps also count ForceFrame's (the airborne segment's entry and exit) as Forced.
    enum Cause { CauseStart, CauseTimer, CauseVelocity, CauseFacing, CauseFilter, CauseTakeEnd, CauseNoSearch, CauseExcluded,
                 CauseRefused, CauseIdleRefit, CauseForced, CauseCount };
    static const char* CauseName(int cause);
    std::array<int, CauseCount> SearchesBy{}, JumpsBy{};
    // Searches where no frame passed the filter and it had to be relaxed (Update): landed once the reselect history was
    // dropped, then MinRunway, then the style (counted only when it moves to another frame); SearchFailed: nothing even
    // then (the frame stays). A normal course has none.
    int RelaxedAvoid = 0, RelaxedRunway = 0, RelaxedStyle = 0, SearchFailed = 0;
    double SearchMsTotal = 0.0, SearchMsMax = 0.0;
    // The latest kSearchSamples search times (ms; a ring once full, so a long Play doesn't grow it ~10 a second forever).
    static constexpr size_t kSearchSamples = 4096;
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
    // Starts a dead blend from what is on screen. Its velocity is the last two output poses' over the time between them -
    // not this update's dt: in Play the frame time varies, and a hitch's long frame (or the short one after it) would scale
    // the carried velocity by the ratio of the two (into the 4 pi rad/s clamp: a lurch at the jump). `to`: the frame jumped
    // to (IdleEntryBlendTime).
    void BeginBlend(std::uint32_t to);
    // The reselect history: the run of frames shown since the last jump starts at m_RunFirst; a jump files it (with its age).
    void EndRun();
    struct ShownRun {
        std::int64_t First = 0, Last = -1;
        float Age = 0.0f;
    };
    std::array<ShownRun, MotionSearchParams::kMaxAvoid - 1> m_Shown{};
    int m_ShownCount = 0;
    std::int64_t m_RunFirst = 0;
    bool IdleDrifted(const Input& in) const; // the idle continuation no longer fits (see Idle)
    const MotionDatabase* m_Db = nullptr;
    std::uint32_t m_Frame = 0;
    float m_Frac = 0.0f;
    MotionPose m_Out, m_Prev, m_Target;
    float m_OutInterval = 0.0f; // s between m_Prev and m_Out (0: only one pose out yet)
    bool m_HaveOut = false, m_Searched = false, m_Jumped = false;
    MotionSearchResult m_Last;
    float m_QueryRaw[MotionDatabase::kDims] = {};
    std::uint32_t m_LastKey = 0xFFFFFFFFu;
};
