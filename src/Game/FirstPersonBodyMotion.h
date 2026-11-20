#pragma once

// The first-person body's motion matching pose source (docs/MOTION_MATCHING_HANDOFF.md, Phase 4).
//
// FirstPersonBody keeps everything it did around the pose - the capsule-led placement, the stair spring, foot IK and
// the foot lock, the eye, the gait phase, the arms - and takes the pose from
// here instead of its locomotion Animator Controller (whose update it then holds: AnimatorControllerComponent::SkipUpdate).
//
//   - One database for every body (SharedMotionDatabase: loaded once, Phase 5's NPCs share it), a MotionMatcher on the body.
//   - The capsule is the simulated character's position: each frame the TrajectoryPredictor is put where the capsule is
//     (position, velocity) with the simulation facing, and predicts from the player's wish velocity and facing (with the
//     capsule's sweep).
//   - Facing (Holden, "Code vs Data Driven Displacement"): a simulation facing - a critically damped spring toward the
//     facing wanted (the view while moving; the Turn Threshold logic standing; SimFacingHalflife) - is what the trajectory
//     is predicted from; the character facing (the drawn body's heading) turns as the matched clip's root turns, and is
//     pulled toward the simulation (CharacterFacingHalflife), never more than MaxFacingOffset off it. The query is in the
//     character's frame, its trajectory the simulation's: a clip's own turn never feeds back into what is asked, and the
//     body turns as the mocap turns - its feet stay planted through a turn. One model standing, moving and in the air.
//   - Movement stays capsule-led. The pose's root is clamped to the capsule (the body stands at its feet; the animation's
//     own travel is never applied). Speed warping: the database plays at capsule speed / the matched frame's root speed
//     (within MinWarp..MaxWarp) - in place of the legacy controller's loop play rate. The search schedule watches the move in the
//     view's frame.
//   - Turn in place: standing still the heading holds until the view is Turn Threshold off it, then the body turns to it
//     (the database's turn-on-the-spot frames, found by the trajectory's facing); TurnInPlaceRate() (measured off the
//     database) is what FirstPersonBody gives Player::MaxYawRate.
//   - The query asks for a style (MotionLabel::Style: what the actor was doing - the bake's label from the take's intent),
//     not a speed band: walk by default, the jog with Shift, the stroll with the Walk action (or a partial stick), crouch
//     crouched - in every direction, so a Shift strafe plays the jog takes' own strafes at their own speed. Standing still it
//     asks for the idle gait and no style. Crouch is also a label filter: crouched, Require Crouch - the pack's own crouch
//     takes, as captured (standing, Forbid it). The jog is the fastest: the pack, shot in a 9 m room, has no sustained
//     sprint - an asset gap, not something faked here (docs/MOTION_MATCHING_HANDOFF.md, "Asset gaps").
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
namespace MmPlayerDebug { struct Snapshot; }

// Experiment knobs (TARTARUS_MM_TUNE="Key=value,..."): the value for `key` when the variable sets it, else `fallback`. Read at
// startup in .cpp files, so an experiment needs no rebuild (docs/MOTION_MATCHING_HANDOFF.md, "Fast iteration"). The keys are
// listed where they are read (FirstPersonBodyMotion::Start, FirstPersonBody::ApplyFootIK, SharedMotionDatabase::Get).
float MmTune(const char* key, float fallback);

// A cooked database shared by every character that plays it, with what is measured off it once.
struct SharedMotionDatabase {
    MotionDatabase Db;
    GaitSpeedTable Gaits;
    float TurnInPlaceRate = 0.0f; // rad/s: the turn-on-the-spot frames' p75 root yaw rate
    std::uint32_t StartFrame = 0; // the frame the matcher starts on: the first searchable standing idle-take frame
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
    // Each excluded run is widened to its whole gesture (Get): its ends, padded, walk out until the frame beyond is at the
    // take's rest on both measures - neither over it by more than kGestureHandRest (m, the higher hand over its shoulder) nor
    // kGestureTiltRest (deg, the chest's tilt) - at most kGestureMax frames further.
    static constexpr float kGestureHandRest = 0.05f, kGestureTiltRest = 5.0f;
    static constexpr int kGestureMax = 120;
    int IdleGestureCuts = -1; // the idle takes' excluded stretches entered mid-gesture (Get; -1: not measured - no rig)
    // Loads `<base>.mmanim` / `.mmdb` once per process per exclusion policy (kept while anything holds it). Null (with
    // `error`) when missing. `rig` (the body's driver): the hands-up frames are measured on it and excluded - a load with a rig
    // and one without are different databases (their excluded frames differ), so each is cached under its own key: the
    // base and the rig's file (none: no rig). (The first caller's policy used to win for everyone.)
    static std::shared_ptr<const SharedMotionDatabase> Get(const std::string& base, std::string* error = nullptr, const Model* rig = nullptr);
    // The cache key Get files a load under: the base and the rig's file (`hasRig` false: none).
    static std::string CacheKey(const std::string& base, bool hasRig, const std::string& rigPath);
    // Per frame, the higher hand's height over its shoulder (upper arm bone), m, model space of `rig`.
    // ... and (chestTilt) the chest's tilt off its bind pose's upright (deg).
    static std::vector<float> HandOverShoulder(const MotionDatabase& db, const Model& rig, std::vector<float>* chestTilt = nullptr);
};

class FirstPersonBodyMotion {
public:
    struct Settings {
        float MinWarp = 0.8f, MaxWarp = 1.25f; // speed warping's play rate limits
        // (Holden's demo values, "Code vs Data Driven Displacement": simulation rotation 0.27 s, adjustment 0.2 s, clamp 90 deg.
        // At 0.12 / 0.15 / 45 deg the simulation turned with every sweep of the view faster than the walk data turns: the
        // matcher hopped between poor turning candidates - 6.4 jumps a second looking about, against 2 walking straight.)
        float SimFacingHalflife = 0.4f;        // s: the simulation facing's spring toward the facing wanted (responsiveness)
        float CharacterFacingHalflife = 0.2f;  // s: the character facing's pull toward the simulation's (on top of the clip's turn)
        // ... while a sprint frame plays: the sprint set (the old locomotion's jog) has no curved runs, so its turns are steered -
        // the forward loop plays on while the facing follows. Measured on `M sprint turning` (60 deg/s): at 0.2, 2.25 jumps/s,
        // 17 % cruising, the planted feet sliding 16.7 cm (turning starts stitched); at 0.08, 0.5 jumps/s, 100 %, 2.3 cm.
        float SprintCharacterFacingHalflife = 0.08f;
        float MaxFacingOffset = 1.5708f;       // rad (90 deg): the character facing at most this far off the simulation's
        float TakeoffSeconds = 0.12f;          // a jump's segment starts this far before its first airborne frame
        float LandSeconds = 0.25f;             // ... and plays this far past its last before searching again
        float AirDelay = 0.12f;                // s off the ground (no jump) before a fall plays the segment
        float TransitionCost = 0.25f;          // a candidate must beat the playing frame by this much (typical costs 0.6-1.0)
        int MinRunway = 8;                     // frames a candidate must play before its labels change (no dead ends)
        float ForcedPoseWeight = 0.25f;        // MotionMatcher::ForcedPoseWeight (Stage 3: at 1.0 the stops jumped 46.3 / s against 40.7, the crouch 5.1 against 4.9)
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
        int Style = MotionLabel::StyleWalk; // the style asked for while moving (crouched: StyleCrouch regardless)
        float JumpSpeed = 5.5f, Gravity = -18.0f;
        float TurnThreshold = 0.0f; // radians: standing still, the heading holds within this of the view (0: always faces it)
        float TurnEndAngle = 0.0f;  // radians: a turn on the spot ends this close to the view
        float VelocityHalflife = 0.2f; // s: how the capsule's speed follows the wish (the predictor's spring)
        TrajectoryPredictor::SweepFn Sweep; // the capsule's cast (empty: none)
        // A route to follow (Phase 5, the NPCs: the navmesh path's corners, world): moving, the trajectory is predicted along
        // it at the wish's speed, facing the travel (TrajectoryPredictor::PredictAlong), not straight down the wish. Empty: the wish.
        const glm::vec3* Path = nullptr;
        int PathCount = 0;
        float ArriveTime = 0.0f; // s: the path's last point is the goal, its stop eased over this (0: it isn't)
    };

    Settings Config;

    // Where this body's diagnostics go (Start / Stop clear it; Tick fills it): the player's is MmPlayerDebug::Info() (FirstPersonBody
    // sets it), an NPC's its own. Null: none kept (and no debug lines drawn).
    void SetDebug(MmPlayerDebug::Snapshot* d) { m_Debug = d; }

    // Binds the stream to every piece (the driver's pelvis height sets the translation scale for all). False (with
    // `error`) when the database or a binding is missing. `carry`: a stopped body's motion (the same database) plays on from
    // where it was - the frame, its reselect history, the facings, the air state - instead of from the start frame (an
    // outfit change in Play re-takes the pieces; the pose went back to the start frame and the next search replayed what
    // had just been shown).
    bool Start(const std::vector<std::shared_ptr<Model>>& models, size_t driver, std::string& error,
               const FirstPersonBodyMotion* carry = nullptr);
    void Stop();
    bool Active() const { return m_Shared != nullptr; }

    // The matcher and the heading (`yaw`, in/out, radians about +Y) for this step.
    void Tick(const Input& in, float& yaw);
    // This step's pose onto every piece (the bind pose, the stream's bones over it).
    void Pose(const std::vector<std::shared_ptr<Model>>& models);

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

    // One step of the character facing: `yaw` turned by the matched clip's own root turn (`clipTurn`, radians), pulled
    // toward the simulation facing `simYaw` (CharacterFacingHalflife), then clamped to MaxFacingOffset off it.
    static float StepCharacterFacing(float yaw, float clipTurn, float simYaw, const Settings& cfg, float dt);
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
    float m_DesiredYaw = 0.0f, m_YawRate = 0.0f;  // the facing wanted; the character facing's turn rate (rad/s)
    float m_SimYaw = 0.0f, m_SimYawRate = 0.0f;   // the simulation facing and its rate
    bool m_HaveYaw = false, m_Still = true, m_Turning = false;
    Air m_Air = Air::None;
    float m_AirTime = 0.0f, m_LandTimer = 0.0f, m_AirRate = 1.0f;
    std::uint32_t m_AirLast = 0;
    float m_WarpRate = 1.0f, m_AnimSpeed = 0.0f, m_CapsuleSpeed = 0.0f;
    MotionPose m_LastOut; // the last update's output pose (the pose-step metric)
    std::vector<float> m_Lines;
    float m_GroupWeight[MotionFeature::GroupCount] = {}; // the search's feature weights (MotionSearchParams' defaults, or MmTune's)
    MmPlayerDebug::Snapshot* m_Debug = nullptr;
};

// What the player's motion matching did this frame, for the Inspector and --npc-test mmplayer (like BodyDebug).
namespace MmPlayerDebug {
struct Snapshot {
    bool Valid = false;
    std::uint32_t Frame = 0;
    std::string Clip;
    std::uint8_t Flags = 0, Gait = 0;
    int DesiredGait = -1;
    int DesiredStyle = -1, Style = 0; // the style asked for (-1: none, standing still) and the matched frame's
    bool Crouch = false, Sprint = false, Airborne = false, Locked = false, Still = false, Turning = false;
    bool Searched = false, Jumped = false;
    int Searches = 0, Jumps = 0, ForcedByInput = 0, ForcedByEnd = 0;
    bool Idle = false;   // MotionMatcher::Idle this update
    int IdleRefits = 0;  // idle searches because the continuation stopped fitting (since Start)
    int ForcedByExcluded = 0; // searches forced because the playing frame ran into an excluded one (since Start)
    int IdleGestureCuts = -1; // SharedMotionDatabase::IdleGestureCuts
    float LastCost = 0.0f, PlayingCost = 0.0f; // the last search: the winner's cost, the playing frame's (-1: it didn't pass)
    double SearchMsAvg = 0.0, SearchMsMax = 0.0;
    float PlayRate = 1.0f, AnimSpeed = 0.0f, CapsuleSpeed = 0.0f;
    float Yaw = 0.0f, DesiredYaw = 0.0f, YawRate = 0.0f, TurnInPlaceRate = 0.0f;
    float SimYaw = 0.0f;       // the simulation facing (radians)
    float FacingOffset = 0.0f; // the character facing off the simulation's (radians, wrapped)
    float ClipTurn = 0.0f;     // the matched clip's own root turn this update (radians)
    float TrajErr = 0.0f;   // the last search: the matched frame's root at +1 s off the wanted one (m)
    float PoseStepDeg = 0.0f; // the worst stream bone's turn between this output pose and the last (deg) ...
    std::string PoseStepBone; // ... and which bone
    float Query[27] = {}, Matched[27] = {}; // the last search's raw query and the matched frame's raw features
    float MatchedTravel = 0.0f, MatchedTravelDeg = 0.0f; // the matched frame's root 1 s on: how far, and how far off ahead (deg)
    float MatchedAccel = 0.0f; // the matched frame's root acceleration over +-0.5 s in its take (m/s^2): cruising, or a dash's rise / brake
    float RootOffset = 0.0f; // the drawn body's root off the capsule's feet (m, horizontal): the clamp keeps it 0
    std::vector<float> Lines; // FirstPersonBodyMotion::Lines() (the mmplayer scenario draws them)
};
Snapshot& Info();
// Whether the player's body fills Info() (FirstPersonBody::Start): only something that reads it asks (the npc tests). Filled
// every frame it cost the clip name, a copy of the drawn lines and the pose, and three feature arrays, read by nothing in Play.
bool& Wanted();
} // namespace MmPlayerDebug
