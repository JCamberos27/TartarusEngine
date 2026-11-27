#pragma once

// TrajectoryPredictor: where the character will be over the next second, for the motion matching query
// (docs/MOTION_MATCHING_HANDOFF.md, Phase 3). Spring-based and collision-aware.
//
//   - Velocity follows the desired velocity through a critically damped spring (VelocityHalflife), integrated exactly
//     (position, velocity and acceleration in closed form - Holden's "simple spring damper"), so the prediction and
//     the simulated character agree at any time step. Or, for a host whose capsule has its own law (CapsuleLaw: the
//     player, the NPCs), that law frame by frame, so the predicted path is the capsule's at every horizon.
//   - Facing follows the desired yaw through a critically damped spring (FacingHalflife), the goal unwrapped to the
//     nearest turn.
//   - Collision: the path is swept in SubSteps per second through `Sweep` (the host's sphere or capsule cast); at a hit
//     the move stops at the contact, the rest of it slides along the surface, and the velocity, acceleration and
//     desired velocity lose their part into the surface for the rest of the prediction, so the predicted trajectory runs
//     along a wall instead of through it.
//
// The predictor is also the simulated character (Update): in Phase 4 the capsule takes its place; in `--npc-test mmdb`
// it moves the body. Yaw 0 faces +Z (MotionMath::YawOf).

#include <glm/glm.hpp>

#include <array>
#include <functional>

// The capsules' snaps, shared by the hosts and their predictions (TrajectoryParams::SnapToGoal / SnapToStop):
// PlayerApproachVelocity takes a velocity within 1 cm/s of the target as arrived; NpcDirector's pace under 2 cm/s is a stop.
inline constexpr float kPlayerArriveSnap = 0.01f; // m/s
inline constexpr float kNpcStopPace = 0.02f;      // m/s

struct TrajectoryParams {
    float VelocityHalflife = 0.20f; // s
    // The capsule's own law in place of the velocity spring: each frame (FrameDt) the velocity eases toward the goal as an
    // exponential, time constant AccelTime when the goal is at least as fast as the velocity and DecelTime when it is slower
    // (0: at once), then the position moves by the new velocity x the frame - PlayerApproachVelocity and the player's move,
    // the NPC director's pace and its move. A velocity within SnapToGoal of the goal takes it (the player's arrive); a speed
    // under SnapToStop is a stop (the director's). The director eases a scalar pace along its way where this eases the
    // velocity vector: the two agree on a straight leg, not on a slowdown during a turn.
    bool CapsuleLaw = false;
    float AccelTime = 0.0f, DecelTime = 0.0f; // s
    float FrameDt = 1.0f / 60.0f;             // s: the host's frame
    float SnapToGoal = 0.0f, SnapToStop = 0.0f; // m/s
    float FacingHalflife = 0.15f;   // s
    int SubSteps = 20;              // sweep steps per predicted second
};

struct TrajectoryState {
    glm::vec3 Position{0.0f}, Velocity{0.0f}, Acceleration{0.0f};
    float Yaw = 0.0f, YawRate = 0.0f;
};

class TrajectoryPredictor {
public:
    // From `from` to `to` (the character's ground position, metres): true when the move hits something before `to`, with
    // `stopAt` where it must stop (the contact, pulled back a little) and `normal` the surface's (horizontal) normal.
    using SweepFn = std::function<bool(const glm::vec3& from, const glm::vec3& to, glm::vec3& stopAt, glm::vec3& normal)>;

    TrajectoryParams Params;
    SweepFn Sweep; // empty: no collision

    void Reset(const glm::vec3& position, float yaw);
    // Moves the simulated character by dt toward the desired velocity and facing (with collision).
    void Update(float dt, const glm::vec3& desiredVelocity, float desiredYaw);
    // The state `times[k]` seconds ahead (ascending, > 0) under the same desires, without moving the character.
    // `goalTurnRate` (rad/s): the desired velocity and facing keep turning at that rate through the prediction (a stick
    // held into a curve, a path bending round a corner); 0 holds them.
    void Predict(const glm::vec3& desiredVelocity, float desiredYaw, const float* times, int count, TrajectoryState* out,
                 float goalTurnRate = 0.0f) const;
    // A route to follow (the NPCs' navmesh corners, Phase 5).
    struct PathGoal {
        const glm::vec3* Points = nullptr; // world, in order; the first is the next corner
        int Count = 0;
        float Speed = 0.0f;       // m/s along it
        float ArriveTime = 0.0f;  // s: >0, the last point is the goal - the speed eases to a stop on it (distance / ArriveTime)
        float Reached = 0.3f;     // m: a corner this close (flat) is passed and the next one steered for
    };
    // As Predict, but the desired velocity steers for the path's next point each step (at Speed, eased into the goal), and
    // the desired facing is the way it steers (the last facing held once there is nowhere left to go).
    void PredictAlong(const PathGoal& path, float desiredYaw, const float* times, int count, TrajectoryState* out) const;
    const TrajectoryState& State() const { return m_State; }
    // Puts the simulated character where a host's own simulation has it (Phase 4: the player's capsule, each frame).
    void SetState(const TrajectoryState& s) { m_State = s; }
    int Hits() const { return m_Hits; } // collisions the simulated character met (Update)

    // The closed-form springs (exposed for the tests).
    static float HalflifeToDamping(float halflife);
    // Velocity spring with position: x, v, a advanced by dt toward the goal velocity `goal`.
    static void SpringCharacter(glm::vec3& x, glm::vec3& v, glm::vec3& a, const glm::vec3& goal, float halflife, float dt);
    // The capsule's law (TrajectoryParams::CapsuleLaw): x, v (and a, the last frame's change) advanced by dt, frame by frame.
    static void CapsuleCharacter(glm::vec3& x, glm::vec3& v, glm::vec3& a, const glm::vec3& goal, const TrajectoryParams& p, float dt);
    // A critically damped spring on a scalar: x, v toward `goal`.
    static void SpringScalar(float& x, float& v, float goal, float halflife, float dt);

private:
    // Advances `s` by dt (with collision when `sweep`), counting hits; `goal` loses its part into any surface hit.
    void Step(TrajectoryState& s, glm::vec3& goal, float goalYaw, float dt, int* hits) const;

    TrajectoryState m_State;
    int m_Hits = 0;
};
