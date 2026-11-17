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
    // Loads `<base>.mmanim` / `.mmdb` once per process (kept while anything holds it). Null (with `error`) when missing.
    static std::shared_ptr<const SharedMotionDatabase> Get(const std::string& base, std::string* error = nullptr);
};

class FirstPersonBodyMotion {
public:
    struct Settings {
        float MinWarp = 0.8f, MaxWarp = 1.25f; // speed warping's play rate limits
        float MovingFacingHalflife = 0.03f;    // s: the heading's spring toward the view while moving (no clip turn added)
        float TurnFacingHalflife = 0.30f;      // s: ... turning on the spot (on top of the clips' own turn)
        float AirFacingHalflife = 0.30f;
        float TakeoffSeconds = 0.12f;          // a jump's segment starts this far before its first airborne frame
        float LandSeconds = 0.25f;             // ... and plays this far past its last before searching again
        float AirDelay = 0.12f;                // s off the ground (no jump) before a fall plays the segment
        float TransitionCost = 0.25f;          // a candidate must beat the playing frame by this much (typical costs 0.6-1.0)
        int MinRunway = 8;                     // frames a candidate must play before its labels change (no dead ends)
        float ForcedPoseWeight = 0.25f;        // MotionMatcher::ForcedPoseWeight
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
    bool Crouch = false, Sprint = false, Airborne = false, Locked = false, Still = false, Turning = false;
    bool Searched = false, Jumped = false;
    int Searches = 0, Jumps = 0, ForcedByInput = 0, ForcedByEnd = 0;
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
