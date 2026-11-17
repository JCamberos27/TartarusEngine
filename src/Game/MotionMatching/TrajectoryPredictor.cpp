#include "TrajectoryPredictor.h"

#include "MotionMath.h"

#include <algorithm>
#include <cmath>

float TrajectoryPredictor::HalflifeToDamping(float halflife) {
    return (4.0f * 0.69314718f) / std::max(1e-5f, halflife);
}

void TrajectoryPredictor::SpringCharacter(glm::vec3& x, glm::vec3& v, glm::vec3& a, const glm::vec3& goal, float halflife,
                                          float dt) {
    const float y = HalflifeToDamping(halflife) / 2.0f;
    const glm::vec3 j0 = v - goal;
    const glm::vec3 j1 = a + j0 * y;
    const float eydt = std::exp(-y * dt);
    x = eydt * (((-j1) / (y * y)) + ((-j0 - j1 * dt) / y)) + (j1 / (y * y)) + j0 / y + goal * dt + x;
    v = eydt * (j0 + j1 * dt) + goal;
    a = eydt * (a - j1 * y * dt);
}

void TrajectoryPredictor::SpringScalar(float& x, float& v, float goal, float halflife, float dt) {
    const float y = HalflifeToDamping(halflife) / 2.0f;
    const float j0 = x - goal;
    const float j1 = v + j0 * y;
    const float eydt = std::exp(-y * dt);
    x = eydt * (j0 + j1 * dt) + goal;
    v = eydt * (v - j1 * y * dt);
}

void TrajectoryPredictor::Reset(const glm::vec3& position, float yaw) {
    m_State = TrajectoryState{};
    m_State.Position = position;
    m_State.Yaw = yaw;
    m_Hits = 0;
}

void TrajectoryPredictor::Step(TrajectoryState& s, glm::vec3& goal, float goalYaw, float dt, int* hits) const {
    const glm::vec3 from = s.Position;
    glm::vec3 x = s.Position;
    SpringCharacter(x, s.Velocity, s.Acceleration, goal, Params.VelocityHalflife, dt);
    x.y = from.y;
    s.Velocity.y = s.Acceleration.y = 0.0f;
    glm::vec3 stop, n;
    if (Sweep && glm::length(x - from) > 1e-6f && Sweep(from, x, stop, n)) {
        n.y = 0.0f;
        const float nl = glm::length(n);
        n = nl > 1e-5f ? n / nl : glm::normalize(from - x);
        if (hits) ++*hits;
        // The rest of the move slides along the surface (one more sweep for it: a corner stops it).
        glm::vec3 rest = x - stop;
        rest -= n * std::min(0.0f, glm::dot(rest, n));
        glm::vec3 to = stop + rest;
        glm::vec3 stop2, n2;
        if (glm::length(rest) > 1e-6f && Sweep(stop, to, stop2, n2)) to = stop2;
        x = glm::vec3(to.x, from.y, to.z);
        const auto clip = [&](glm::vec3& v) { v -= n * std::min(0.0f, glm::dot(v, n)); };
        clip(s.Velocity);
        clip(s.Acceleration);
        clip(goal);
    }
    s.Position = x;
    const float g = s.Yaw + MotionMath::WrapPi(goalYaw - s.Yaw);
    SpringScalar(s.Yaw, s.YawRate, g, Params.FacingHalflife, dt);
}

void TrajectoryPredictor::Update(float dt, const glm::vec3& desiredVelocity, float desiredYaw) {
    glm::vec3 goal(desiredVelocity.x, 0.0f, desiredVelocity.z);
    Step(m_State, goal, desiredYaw, dt, &m_Hits);
}

void TrajectoryPredictor::Predict(const glm::vec3& desiredVelocity, float desiredYaw, const float* times, int count,
                                  TrajectoryState* out, float goalTurnRate) const {
    TrajectoryState s = m_State;
    glm::vec3 goal(desiredVelocity.x, 0.0f, desiredVelocity.z);
    float goalYaw = desiredYaw;
    const float step = 1.0f / (float)std::max(1, Params.SubSteps);
    float t = 0.0f;
    for (int k = 0; k < count; ++k) {
        while (t < times[k] - 1e-6f) {
            const float dt = std::min(step, times[k] - t);
            if (goalTurnRate != 0.0f) {
                // The goal at the middle of the step (yaw about +Y: YawDir(a) turned by b is YawDir(a + b)).
                const float a = goalTurnRate * 0.5f * dt, c = std::cos(a), sn = std::sin(a);
                goal = glm::vec3(goal.x * c + goal.z * sn, 0.0f, -goal.x * sn + goal.z * c);
                goalYaw += a;
            }
            Step(s, goal, goalYaw, dt, nullptr);
            if (goalTurnRate != 0.0f) {
                const float a = goalTurnRate * 0.5f * dt, c = std::cos(a), sn = std::sin(a);
                goal = glm::vec3(goal.x * c + goal.z * sn, 0.0f, -goal.x * sn + goal.z * c);
                goalYaw += a;
            }
            t += dt;
        }
        out[k] = s;
    }
}
