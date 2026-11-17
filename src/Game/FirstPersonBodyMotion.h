#pragma once

// The first-person body's motion matching pose source (docs/MOTION_MATCHING_HANDOFF.md, Phase 4).
//
// FirstPersonBody keeps everything it did around the pose - the capsule-led placement, the stair spring, foot IK and
// the foot lock, the eye, the gait phase, the arms - and, with Locomotion Source = Motion Matching, takes the pose from
// here instead of its locomotion Animator Controller (whose update it then holds: AnimatorControllerComponent::SkipUpdate).
//
//   - One database for every body (SharedMotionDatabase: loaded once, Phase 5's NPCs share it), a MotionMatcher on the body.
//   - The capsule is the simulated character: each frame the TrajectoryPredictor is put where the capsule is (position,
//     velocity, the body's heading) and predicts from the player's wish velocity and facing (with the capsule's sweep).
//   - Movement stays capsule-led. The pose's root is clamped to the capsule (the body stands at its feet; the animation's
//     own travel is never applied). Speed warping: the database plays at capsule speed / the matched frame's root speed
//     (within MinWarp..MaxWarp) in place of BodyLocomotion::LoopPlayRate. Orientation warping: moving, the heading follows
//     the view on a short spring (nothing of the clip's turn); standing still, it turns with the matched frame's root turn
//     plus a spring toward the facing wanted (Turn Threshold). The search schedule watches the move in the view's frame.
//   - Turn in place: standing still the heading holds until the view is Turn Threshold off it, then the body turns to it
//     (the database's turn-on-the-spot frames, found by the trajectory's facing); TurnInPlaceRate() (measured off the
//     database) is what FirstPersonBody gives Player::MaxYawRate.
//   - Crouch and sprint are label filters: crouched, Require Crouch (standing, Forbid it); sprinting, the Run gait
//     wanted (and the gait cost doubled).
//   - Airborne: label-locked playback of a baked jump (Require Airborne finds it; the segment plays from its take-off,
//     time-warped to the jump's flight, and holds its last airborne frame until landing), dead-blended in and out.
//
// The stream's pose is in the Quantum rig's model space under the generated root; its rest facing (0.03 deg on the
// bake) is ignored: the body's +Z is its heading.

#include "MotionMatching/MotionGait.h"
#include "MotionMatching/MotionMatcher.h"
#include "MotionMatching/TrajectoryPredictor.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Model;
struct LocalTRS;

// FirstPersonBodyComponent::LocomotionSource (temporary, until Phase 6 removes the legacy path).
enum class LocomotionSource : int { Legacy = 0, MotionMatching = 1 };

// A cooked database shared by every character that plays it, with what is measured off it once.
struct SharedMotionDatabase {
    MotionDatabase Db;
    GaitSpeedTable Gaits;
    float TurnInPlaceRate = 0.0f; // rad/s: the turn-on-the-spot frames' p75 root yaw rate
    std::uint32_t IdleFrame = 0;  // a standing idle frame to start on (and Spine Stability's upright reference)
    std::uint32_t CrouchIdleFrame = 0; // a crouched idle frame (Spine Stability's crouched reference)
    std::uint32_t HandsUpFrames = 0;   // frames excluded for a hand over its shoulder (HandsUpMargin), pads included
    // A frame (not airborne) with either hand more than this (m) over its shoulder is excluded, kHandsUpPad frames either
    // side: the pack's idles catching breath after a run (hands on the head), stretches, pivots with the arms flung up
    // - in the first-person view the hands came up into the camera (the owner's play test 2, Phase 4).
    static constexpr float kHandsUpMargin = -0.02f;
    static constexpr int kHandsUpPad = 6;
    // ... and a slow (root under kBentOverSpeed m/s), standing frame whose chest (spine_05) is tilted more than this off its
    // bind pose's upright: the run takes' ends, the actor bent over with the hands on the knees - it won the idle searches.
    static constexpr float kBentOverDeg = 30.0f;
    static constexpr float kBentOverSpeed = 1.0f;
    std::uint32_t BentOverFrames = 0; // (raw, before the pads)
    // ... and a moving frame (walk or faster, not strafing - strafes cross over on purpose) whose ankles cross more than this
    // (m, the left one that far right of the right one, root space): the snakes' and the turnarounds' crossover steps,
    // which a 180 flick of the view matched - the legs crossed 30 cm (the owner's play test 2, "knock-kneed").
    static constexpr float kCrossedFeet = 0.10f;
    std::uint32_t CrossedFrames = 0; // (raw)
    // The sprint loop: the pack is shot in a 9 m area, so it has no sustained straight sprint (every straight run over
    // 3.2 m/s is a 1-2.5 s fragment between a start and a stop). Held at the run speed the matcher stitched fragments of
    // three takes (~3 jumps a second: "limping"), and through a turn fell into their stops and starts. So the cleanest
    // steady straight stride (two steps whose end pose matches its start) is found at load and looped while sprinting.
    struct Loop {
        bool Valid = false;
        std::uint32_t First = 0, Last = 0; // frames: play First..Last, wrap back to First
        float Speed = 0.0f;                // its mean root speed as shown (m/s)
        float Score = 0.0f;                // FindSprintLoop's cost (lower is better)
        float SeamDeg = 0.0f, DriftPct = 0.0f, StepSpeedPct = 0.0f, StepTimePct = 0.0f; // its terms (for the log)
        std::string Clip;
        // The wrap is closed by an offset (offset inertialization): after it the stride plays its own start with the seam's
        // difference - the end pose against the start pose, and their velocities' difference - added on and decaying as a
        // critically damped spring (half-life kSeamHalflife). Continuous in pose and velocity at the wrap, and only the
        // stride's own frames ever play.
        // (The crossfade before it played the frames *past* the stride's end over the first 16 frames after every wrap. The
        // pack's sprints are dashes in a 9 m room: past the stride the actor is braking - 4.42 -> 3.48 m/s over those 16
        // frames - so every cycle began with a stop: the owner's "the sprint does a stop every few steps". Before the stride
        // it is still speeding up; no stride in the pack has steady running on either side. A correction spread linearly over
        // the whole cycle, tried first, shifted its average pose: the chest 8 deg, the feet out of reach.)
        static constexpr float kSeamHalflife = 0.1f; // s (~0.35 s to settle; the stride is ~0.6 s)
        // The seam offset's share of its start (`pos`) and of its start velocity (`vel`, s) `t` seconds after the wrap.
        static void SeamDecay(float t, float& pos, float& vel);
        // `pose` at loop frame `frame` + `frac`, after a wrap: the seam's decaying offset added (nothing once it has settled).
        void Close(const MotionDatabase& db, std::uint32_t frame, float frac, MotionPose& pose) const;
        // The root speed of the pose shown at `frame` + `frac` (m/s; `wrapped`: Close() applies): what speed warping must
        // divide by. The raw frames' speed jumps at the wrap while the pose shown doesn't - warped by the raw speed the play
        // rate stepped every stride (a hitch in the limbs' tempo with no pop in the pose).
        float ShownSpeed(std::uint32_t frame, float frac, bool wrapped) const;
        // Makes this the loop First..Last of `db` (Last + 2 must be in the same take: the hold past the end): the per-frame
        // speeds, the seam, Speed and Clip. FindSprintLoop's pick, or a test's.
        void SetSpan(const MotionDatabase& db, std::uint32_t first, std::uint32_t last);

    private:
        float SeamTime(std::uint32_t frame, float frac) const { return ((float)(frame - First) + frac) / Fps; }
        float Fps = 60.0f;
        std::vector<float> Speeds;                 // per loop frame (First..Last), the root speed
        std::vector<glm::vec3> SeamRot, SeamSpin; // per stream bone: the end's rotation off the start's (scaled angle-axis,
                                                   // in the parent's frame) and their angular velocities' difference (rad/s)
        glm::vec3 SeamPelvis{0.0f}, SeamPelvisVel{0.0f};
        float SeamSpeed = 0.0f, SeamSpeedRate = 0.0f;
    } SprintLoop;
    static Loop FindSprintLoop(const MotionDatabase& db);
    // Loads `<base>.mmanim` / `.mmdb` once per process (kept while anything holds it). Null (with `error`) when missing.
    // `rig` (the body's driver): the hands-up frames are measured on it and excluded (on first load only).
    static std::shared_ptr<const SharedMotionDatabase> Get(const std::string& base, std::string* error = nullptr, const Model* rig = nullptr);
    // Per frame, the higher hand's height over its shoulder (upper arm bone), m, model space of `rig`.
    // ... and (chestTilt) the chest's tilt off its bind pose's upright (deg).
    static std::vector<float> HandOverShoulder(const MotionDatabase& db, const Model& rig, std::vector<float>* chestTilt = nullptr);
};

class FirstPersonBodyMotion {
public:
    struct Settings {
        float MinWarp = 0.8f, MaxWarp = 1.25f; // speed warping's play rate limits
        float MovingFacingHalflife = 0.03f;    // s: the heading's spring toward the view while moving (no clip turn added)
        float MaxMovingYawRate = 7.854f;       // rad/s (450 deg/s): the heading's fastest turn while moving - a 180 flick of the
                                               // view takes 0.4 s, the chest leading (Spine Twist), not a spin over planted feet
        float TurnFacingHalflife = 0.30f;      // s: ... turning on the spot (on top of the clips' own turn)
        float AirFacingHalflife = 0.30f;
        float TakeoffSeconds = 0.12f;          // a jump's segment starts this far before its first airborne frame
        float LandSeconds = 0.25f;             // ... and plays this far past its last before searching again
        float AirDelay = 0.12f;                // s off the ground (no jump) before a fall plays the segment
        float TransitionCost = 0.25f;          // a candidate must beat the playing frame by this much (typical costs 0.6-1.0)
        int MinRunway = 8;                     // frames a candidate must play before its labels change (no dead ends)
        float ForcedPoseWeight = 0.25f;        // MotionMatcher::ForcedPoseWeight
        bool SprintLoop = true;                // play SharedMotionDatabase::SprintLoop while sprinting ahead at speed
        // The crouch is the walk, lowered (FirstPersonBody lowers the hips and re-solves the legs): the pack's crouch takes
        // are all one style - a deep, high-knee sneak, hunched over (the owner: "crouch still looks off"). false: those
        // takes (Require Crouch). TARTARUS_MM_CROUCH=walk|pack overrides it at Start.
        bool CrouchFromWalk = true;
        // The loop starts above SprintLoopEnter of the run speed. It leaves on intent - Shift released, the move off ahead, a
        // crouch, a jump, a reversal (the capsule's velocity over SprintLoopReverse radians off the heading) - or when the
        // capsule is blocked (under SprintLoopBlocked of the run speed for SprintLoopBlockedSeconds: a wall). Not on a speed
        // dip: turning with the mouse the capsule's velocity cuts the corner (in the owner's recording 4.4 -> 1.5 m/s with the
        // old turn slowdown), and leaving on speed dropped the sprint into the pack's starts and stops - "a stop" mid-sprint.
        float SprintLoopEnter = 0.85f, SprintLoopBlocked = 0.3f, SprintLoopBlockedSeconds = 0.2f, SprintLoopReverse = 1.745f;
        float SprintLoopSettle = 0.15f, SprintLoopSettleRate = 1.571f; // s the heading turns under this (rad/s, 90 deg/s) first
        // Standing idle (MotionMatcher::Idle): below this capsule speed (m/s) on an Idle frame, the take plays on while its
        // root 1 s on stays within IdleTravel (m; the pack's idles move it under 0.17 m in a second, p97) and its facing within
        // IdleTurn (rad) - a sanity bound only: standing still the heading follows the take's root turn (the feet stay planted)
        // and the Turn Threshold turns the body back to the view. (At the Turn Threshold's 30 deg, Stand_Idle_LookAround - its
        // root half the shoulder line, past 30 deg in a second on 22 % of its frames - re-searched every 0.8 s.)
        float IdleSpeed = 0.1f, IdleTravel = 0.25f, IdleTurn = 1.571f;
        int IdleRunway = 180; // frames (3 s) an idle candidate plays before its labels change
    };
    struct Input {
        float Dt = 0.0f;
        glm::vec3 Feet{0.0f};      // the capsule's feet (world)
        glm::vec3 Velocity{0.0f};  // the capsule's velocity (world; y is the vertical)
        glm::vec3 Wish{0.0f};      // the move asked for (world, horizontal, m/s)
        float ViewYaw = 0.0f;      // radians, MotionMath::YawOf convention (0 = +Z)
        bool Grounded = true, Crouched = false, Sprint = false, Jumped = false;
        float JumpSpeed = 5.5f, Gravity = -18.0f;
        float TurnThreshold = 0.0f; // radians: standing still, the heading holds within this of the view (0: always faces it)
        float TurnEndAngle = 0.0f;  // radians: a turn on the spot ends this close to the view
        float VelocityHalflife = 0.2f; // s: how the capsule's speed follows the wish (the predictor's spring)
        TrajectoryPredictor::SweepFn Sweep; // the capsule's cast (empty: none)
    };

    Settings Config;

    // Binds the stream to every piece (the driver's pelvis height sets the translation scale for all). False (with
    // `error`) when the database or a binding is missing.
    bool Start(const std::vector<std::shared_ptr<Model>>& models, size_t driver, std::string& error);
    void Stop();
    bool Active() const { return m_Shared != nullptr; }

    // The matcher and the heading (`yaw`, in/out, radians about +Y) for this step.
    void Tick(const Input& in, float& yaw);
    // This step's pose onto every piece (the bind pose, the stream's bones over it).
    void Pose(const std::vector<std::shared_ptr<Model>>& models);
    // Piece `piece`'s local pose at the database's standing (or crouched) idle frame: Spine Stability's reference, as the
    // locomotion controller's idle clips are for it. False before Start or for a piece it couldn't bind.
    bool ReferencePose(size_t piece, bool crouched, std::vector<LocalTRS>& out) const;

    bool Still() const { return m_Still; }       // no move asked: the heading holds (turn in place past the threshold)
    bool Turning() const { return m_Turning; }   // a turn on the spot is under way
    bool Airborne() const { return m_Air != Air::None; }
    bool Looping() const { return m_Looping; }
    bool CrouchFromWalk() const { return Config.CrouchFromWalk; }
    // The sprint: the loop's own speed x kSprintOfLoop when there is a loop (it plays near its natural rate), else the
    // gait table's run.
    static constexpr float kSprintOfLoop = 0.95f;
    float SprintSpeed() const;
    // The sprint loop carries the run (it needs no curved-run data: the heading turns it).
    bool HasSprintLoop() const { return m_Shared && m_Shared->SprintLoop.Valid && Config.SprintLoop; }
    float TurnInPlaceRate() const { return m_Shared ? m_Shared->TurnInPlaceRate : 0.0f; }
    const GaitSpeedTable& Gaits() const { return m_Shared ? m_Shared->Gaits : m_NoGaits; }
    const MotionDatabase* Database() const { return m_Shared ? &m_Shared->Db : nullptr; }
    const MotionMatcher& Matcher() const { return m_Matcher; }
    float PlayRate() const { return m_Matcher.PlayRate; }
    // The matched frame's own root speed (m/s, as captured) and the capsule's, this step.
    float AnimSpeed() const { return m_AnimSpeed; }
    float CapsuleSpeed() const { return m_CapsuleSpeed; }
    // The debug draw (MotionDebugDraw, HUD line format): the wanted trajectory (yellow), the matched frame's (magenta),
    // the root (green), the matched frame's feet.
    const std::vector<float>& Lines() const { return m_Lines; }

    // The root speed (m/s) and turn rate (rad/s) a database frame's root has, as captured (over +-5 frames in its take).
    static float RootSpeed(const MotionDatabase& db, std::uint32_t frame);
    static float RootYawRate(const MotionDatabase& db, std::uint32_t frame);

private:
    enum class Air { None, Rising, Holding, Landing };
    void EnterAir(const Input& in, const MotionMatcher::Input& query);

    std::shared_ptr<const SharedMotionDatabase> m_Shared;
    GaitSpeedTable m_NoGaits;
    MotionMatcher m_Matcher;
    TrajectoryPredictor m_Predictor;
    std::vector<MotionClipStream::Binding> m_Bind; // per piece
    std::vector<std::vector<LocalTRS>> m_BindPose;
    std::vector<LocalTRS> m_Scratch;
    float m_DesiredYaw = 0.0f, m_YawRate = 0.0f;
    bool m_HaveYaw = false, m_Still = true, m_Turning = false;
    Air m_Air = Air::None;
    bool m_Looping = false; // playing the sprint loop
    bool m_LoopWrapped = false; // ... and it has wrapped since it was entered (the seam's offset applies)
    float m_LoopBlocked = 0.0f; // s the capsule has been under SprintLoopBlocked of the run speed while looping
    int m_LoopExits = 0;        // loop exits since Start (MmPlayerDebug)
    float m_HeadingSettled = 0.0f; // s the heading has turned slower than SprintLoopSettleRate
    float m_AirTime = 0.0f, m_LandTimer = 0.0f, m_AirRate = 1.0f;
    std::uint32_t m_AirLast = 0;
    float m_WarpRate = 1.0f, m_AnimSpeed = 0.0f, m_CapsuleSpeed = 0.0f;
    MotionPose m_LastOut; // the last update's output pose (the pose-step metric)
    std::vector<float> m_Lines;
};

// What the player's motion matching did this frame, for the Inspector and --npc-test mmplayer (like BodyDebug).
namespace MmPlayerDebug {
struct Snapshot {
    bool Valid = false;
    std::uint32_t Frame = 0;
    std::string Clip;
    std::uint8_t Flags = 0, Gait = 0;
    int DesiredGait = -1;
    bool Crouch = false, Sprint = false, Airborne = false, Locked = false, Still = false, Turning = false, Looping = false;
    bool Searched = false, Jumped = false;
    int Searches = 0, Jumps = 0, ForcedByInput = 0, ForcedByEnd = 0;
    bool Idle = false;   // MotionMatcher::Idle this update
    int IdleRefits = 0;  // idle searches because the continuation stopped fitting (since Start)
    int LoopExits = 0;   // sprint loop exits (since Start)
    float LastCost = 0.0f, PlayingCost = 0.0f; // the last search: the winner's cost, the playing frame's (-1: it didn't pass)
    double SearchMsAvg = 0.0, SearchMsMax = 0.0;
    float PlayRate = 1.0f, AnimSpeed = 0.0f, CapsuleSpeed = 0.0f;
    float Yaw = 0.0f, DesiredYaw = 0.0f, YawRate = 0.0f, TurnInPlaceRate = 0.0f;
    float TrajErr = 0.0f;   // the last search: the matched frame's root at +1 s off the wanted one (m)
    float PoseStepDeg = 0.0f; // the worst stream bone's turn between this output pose and the last (deg) ...
    std::string PoseStepBone; // ... and which bone
    float Query[27] = {}, Matched[27] = {}; // the last search's raw query and the matched frame's raw features
    float MatchedTravel = 0.0f, MatchedTravelDeg = 0.0f; // the matched frame's root 1 s on: how far, and how far off ahead (deg)
    float RootOffset = 0.0f; // the drawn body's root off the capsule's feet (m, horizontal): the clamp keeps it 0
    std::vector<float> Lines; // FirstPersonBodyMotion::Lines() (the mmplayer scenario draws them)
};
Snapshot& Info();
} // namespace MmPlayerDebug
