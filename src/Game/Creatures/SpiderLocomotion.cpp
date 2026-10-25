#include "SpiderLocomotion.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Spider {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kFootSpeed = 4.0f; // m/s: a step's mean foot speed at most

bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
    const float l = glm::length(v);
    return l > 1e-6f ? v / l : fallback;
}

// The shortest rotation taking unit `from` onto unit `to` (half-angle form: exact down to parallel, IK.cpp's too).
glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to) {
    const float d = glm::dot(from, to);
    if (d < -1.0f + 1e-6f) {
        const glm::vec3 other = std::fabs(from.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 axis = glm::normalize(glm::cross(from, other));
        return glm::quat(0.0f, axis.x, axis.y, axis.z);
    }
    const glm::vec3 c = glm::cross(from, to);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

// The share of the way a value moves toward its target this frame at `rate` (1/s), frame-rate independent.
float Approach(float rate, float dt) { return 1.0f - std::exp(-std::max(0.0f, rate) * dt); }

// The angle `chord` makes from d1 toward d2 (Span apart) at lambda: an inside corner's body line, tail on the first face
// and head on the second, a body's length apart along them.
float ChordAngle(float lambda, float span) {
    lambda = std::clamp(lambda, 0.0f, 1.0f);
    return std::atan2(lambda * std::sin(span), (1.0f - lambda) + lambda * std::cos(span));
}

// An outside edge's roll: none before the edge's run starts, all of it at its end, eased in and out between.
float EdgeAngle(float p, float length, float span) {
    const float u = std::clamp(p / std::max(1e-4f, length) + 0.5f, 0.0f, 1.0f);
    return span * u * u * (3.0f - 2.0f * u);
}

glm::vec3 RotateAbout(const glm::vec3& v, const glm::vec3& axis, float angle) { return glm::angleAxis(angle, axis) * v; }

} // namespace

// ---- body ----

void Mover::Reset(const glm::vec3& pos, const glm::quat& rot) {
    *this = Mover{};
    m_Pos = pos;
    m_Rot = glm::normalize(rot);
    m_SurfaceNormal = Up();
}

void Mover::TurnUpToward(const glm::vec3& want, float rate, float maxRateDeg, float dt) {
    const glm::vec3 up = Up();
    const glm::vec3 w = SafeNormalize(want, up);
    const float angle = std::acos(std::clamp(glm::dot(up, w), -1.0f, 1.0f));
    if (angle < 1e-5f) return;
    // Eased toward it, never faster than its up may swing.
    const float turn = std::min(angle * Approach(rate, dt), glm::radians(maxRateDeg) * dt);
    const glm::quat full = RotationBetween(up, w);
    // The whole body turns with its up, so its forward is carried over an edge rather than re-aimed.
    m_Rot = glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), full, turn / angle) * m_Rot);
}

bool Mover::FindCorner(const SurfaceQuery& q, const MoverSettings& s, float look, Corner& out, float& start) const {
    const glm::vec3 up = Up(), fwd = Forward();
    const float lift = s.BodyLift, len = s.BodyFront + s.BodyBack;
    float best = 1e9f;
    float wallAt = 1e9f; // (the edge probe stops short of a wall ahead)

    // Inside: a face ahead rising off this one (a wall from the floor, the ceiling from a wall), just over a step's height.
    SurfaceHit ahead;
    const bool aheadHit = q.Ray(m_Pos + up * s.StepUp, fwd, look + s.BodyFront + lift + 0.05f, ahead);
    m_Reject = !aheadHit ? 1 : glm::dot(ahead.Normal, up) >= 0.7f ? 2 : glm::dot(fwd, ahead.Normal) >= -0.2f ? 3 : 0;
    if (aheadHit && glm::dot(ahead.Normal, up) < 0.7f && glm::dot(fwd, ahead.Normal) < -0.2f) {
        wallAt = ahead.Distance;
        Corner c;
        c.Kind = 1;
        const glm::vec3 n = ahead.Normal;
        c.Axis = SafeNormalize(glm::cross(up, n), glm::vec3(0.0f));
        if (glm::length(c.Axis) > 0.5f) {
            c.D1 = glm::normalize(glm::cross(c.Axis, up)); // along this face, toward the wall
            if (glm::dot(c.D1, n) > 0.0f) c.D1 = -c.D1;
            c.D2 = glm::normalize(glm::cross(n, c.Axis)); // along the wall, away from this face
            if (glm::dot(c.D2, up) < 0.0f) c.D2 = -c.D2;
            c.U0 = up;
            c.Span = std::acos(std::clamp(glm::dot(up, n), -1.0f, 1.0f));
            const float across = std::max(0.25f, glm::dot(fwd, c.D1)); // how squarely it meets the wall
            // The belly-level corner line: this face and the wall, each lifted by the body's height off it.
            const glm::vec3 belly = m_Pos + up * lift;
            const float facing = -glm::dot(c.D1, n); // (a wall it meets edge-on has no corner line worth the name)
            const float toWall = facing > 0.3f ? (glm::dot(belly - ahead.Point, n) - lift) / facing : -1.0f;
            // (Its belly may be nearer the wall than its height - turned to face it from alongside, or squeezed in by
            // furniture: still a corner, so long as the belly is outside the wall. Refused, it stood pressed there.)
            if (toWall < -lift / std::max(facing, 0.3f) || toWall > look + len + 1.0f) {
                c.Axis = glm::vec3(0.0f);
                m_Reject = toWall < 0.0f ? 5 : 6;
            }
            c.Point = belly + c.D1 * toWall;
            c.Length = len * across;
            // The wall must run up as far as the body will stand on it; a low one (a step, a box) gives a shorter turn.
            for (int k = 0; k < 4; ++k) {
                const glm::vec3 end = c.Point + c.D2 * (c.Length * (s.BodyBack / len)) - n * lift;
                SurfaceHit wall;
                if (q.Ray(end + n * 0.05f, -n, 0.1f, wall) && glm::dot(wall.Normal, n) > 0.9f) break;
                c.Length *= 0.5f;
            }
            float x = toWall - c.Length * (s.BodyFront / len); // its head reaches the corner line after this far (across)
            c.Facing = std::max(0.3f, facing);
            c.WallLift = lift;
            if (x < 0.0f) {
                // Already nearer (it turned to face the wall close by): the turn starts now on the body's full length,
                // the corner line drawn where its head is - nearer the wall than a body's height - and eased back out to
                // that height as it turns. (A shorter turn can't hold a rigid body: its abdomen dug into the floor.)
                // (Met at a glancing angle the corner shows late, its head already in the face: the line starts there,
                // under the face, and eases out. Held at the face instead, the turn ended with it under the floor.)
                c.WallLift = lift + x * c.Facing;
                c.Point = belly + c.D1 * (toWall - x);
                x = 0.0f;
            }
            const float along = x / across;
            if (glm::length(c.Axis) > 0.5f && along > look) m_Reject = 7;
            if (glm::length(c.Axis) > 0.5f && along <= look) {
                best = along;
                out = c;
            }
        }
    }

    // Outside: the face ahead drops away (a ledge, a wall's top, a box's side) - found from past the edge, looking back.
    const float probeAt = std::min(look + 1.5f * s.EdgeRun, wallAt - 0.02f);
    if (probeAt > 0.02f) {
        const glm::vec3 past = m_Pos + fwd * probeAt;
        SurfaceHit h;
        const bool ground = q.Ray(past + up * s.StepUp, -up, 2.0f * s.StepUp, h) && glm::dot(h.Normal, up) > 0.7f;
        SurfaceHit side;
        if (!ground && q.Ray(past - up * 0.02f, -fwd, probeAt + 0.05f, side) && glm::dot(side.Normal, up) < 0.7f &&
            glm::dot(side.Normal, fwd) > 0.2f) {
            Corner c;
            c.Kind = 2;
            const glm::vec3 n = side.Normal;
            c.Axis = SafeNormalize(glm::cross(up, n), glm::vec3(0.0f));
            if (glm::length(c.Axis) > 0.5f) {
                c.D1 = glm::normalize(glm::cross(c.Axis, up)); // along this face, toward the edge
                if (glm::dot(c.D1, n) < 0.0f) c.D1 = -c.D1;
                c.U0 = up;
                c.Span = std::acos(std::clamp(glm::dot(up, n), -1.0f, 1.0f));
                const float across = std::max(0.25f, glm::dot(fwd, c.D1));
                // The edge line: this face's plane and the side's.
                const float facing = glm::dot(c.D1, n);
                const float toEdge = facing > 0.3f ? std::max(0.0f, glm::dot(side.Point - m_Pos, n) / facing) : 1e9f;
                c.Point = m_Pos + c.D1 * std::min(toEdge, probeAt);
                // The roll's run: long enough that at this speed its up swings no faster than MaxUpRate (a smoothstep's
                // peak rate is 1.5x its mean), never under a short pace.
                c.Length = std::clamp(1.5f * c.Span * m_Speed / glm::radians(s.MaxUpRate), s.EdgeRun, 3.0f * s.EdgeRun) * across;
                float x = toEdge - 0.5f * c.Length;
                if (x < 0.0f) {
                    c.Length = std::max(0.02f, 2.0f * toEdge);
                    x = 0.0f;
                }
                // The side must run on as far as the roll ends on it; a thin one (a door's top) gives a shorter roll.
                const glm::vec3 down = RotateAbout(c.D1, c.Axis, c.Span);
                bool sideThere = false;
                for (int k = 0; k < 4 && !sideThere; ++k) {
                    const glm::vec3 end = c.Point + down * (0.5f * c.Length);
                    SurfaceHit onSide;
                    sideThere = q.Ray(end + n * 0.05f, -n, 0.1f, onSide) && glm::dot(onSide.Normal, n) > 0.9f;
                    if (!sideThere) c.Length *= 0.5f;
                }
                x = std::max(0.0f, toEdge - 0.5f * c.Length);
                c.At = -0.5f * c.Length; // (the roll's run starts this far before the edge)
                const float along = x / across;
                if (sideThere && toEdge <= probeAt + 0.05f && along <= look && along < best) {
                    best = along;
                    out = c;
                }
            }
        }
    }
    if (best > look) return false;
    start = best;
    return true;
}

float Mover::CornerSpeed(const MoverSettings& s, const Corner& c) const {
    // The fastest it may take the turn without its up swinging faster than MaxUpRate.
    const float maxUp = glm::radians(s.MaxUpRate);
    if (c.Kind == 1) {
        const float len = s.BodyFront + s.BodyBack, w = s.BodyBack / len;
        const float k = glm::length(c.D1 * (1.0f - w) + c.D2 * w);
        // dtheta/dlambda peaks at the turn's middle: 1 / (sin(span/2)^2 ...) - sampled rather than worked out.
        float peak = 0.0f;
        for (int i = 0; i < 8; ++i) {
            const float a = (float)i / 8.0f, b = (float)(i + 1) / 8.0f;
            peak = std::max(peak, (ChordAngle(b, c.Span) - ChordAngle(a, c.Span)) * 8.0f);
        }
        return maxUp * c.Length * k / std::max(1e-3f, peak);
    }
    return maxUp * c.Length / std::max(1e-3f, 1.5f * c.Span);
}

float Mover::FollowCorner(const SurfaceQuery& q, const MoverSettings& s, float dist, float dt, float& turnBudget) {
    Corner& c = m_Corner;
    const float len = s.BodyFront + s.BodyBack, w = s.BodyBack / len;
    const glm::vec3 fwd = Forward();
    const float along = glm::dot(fwd, c.Axis);
    float used = dist, next = c.At, turn = 0.0f;
    bool done = false, back = false;
    if (c.Kind == 1) {
        const glm::vec3 chord = glm::normalize(RotateAbout(c.D1, c.Axis, c.Turned));
        const float k = std::max(1e-3f, glm::length(c.D1 * (1.0f - w) + c.D2 * w));
        next = c.At + glm::dot(fwd, chord) * dist / (c.Length * k);
        if (next >= 1.0f || next <= 0.0f) {
            const float end = next >= 1.0f ? 1.0f : 0.0f;
            used = dist * std::clamp((end - c.At) / (next - c.At), 0.0f, 1.0f); // (next != At: it crossed an end)
            next = end;
            done = true;
            back = end == 0.0f;
        }
        turn = ChordAngle(next, c.Span) - c.Turned;
    } else {
        const glm::vec3 f = RotateAbout(c.D1, c.Axis, c.Turned);
        next = c.At + glm::dot(fwd, f) * dist;
        const float half = 0.5f * c.Length;
        if (next >= half || next <= -half) {
            const float end = next >= half ? half : -half;
            used = dist * std::clamp((end - c.At) / (next - c.At), 0.0f, 1.0f); // (next != At: it crossed an end)
            next = end;
            done = true;
            back = end < 0.0f;
        }
        turn = EdgeAngle(next, c.Length, c.Span) - c.Turned;
    }
    // Never faster than its up may swing, and never swinging the origin round faster than it travels (a tight turn's
    // body pivots about its belly or the edge - unbounded, its origin whirled round while it crept): what it can't turn
    // this frame, it doesn't travel.
    // (Over an edge the body swings round the edge line - its centre this far from it; an inside corner turns about the
    // belly-level corner line, which its centre travels along.)
    const float arm = c.Kind == 1 ? 0.0f : std::fabs(c.At) + s.BodyLift;
    const float budget = arm > 0.0f ? std::min(turnBudget, (0.4f * used + 0.3f * dt) / arm) : turnBudget;
    if (std::fabs(turn) > budget) {
        const float share = budget / std::fabs(turn);
        used *= share;
        next = c.At + (next - c.At) * share;
        turn = c.Kind == 1 ? ChordAngle(next, c.Span) - c.Turned : EdgeAngle(next, c.Length, c.Span) - c.Turned;
        done = back = false;
    }
    turnBudget -= std::fabs(turn);
    // Along the corner line it goes only as far as there's room: a wall-to-wall corner run down into the floor (or up into
    // the ceiling) has a third face across its line. Unchecked, it slid its body down through the floor slab.
    float drift = along * used;
    if (std::fabs(drift) > 1e-6f) {
        constexpr float kSide = 0.15f; // (its legs' spread past its belly, across the line)
        const glm::vec3 dir = c.Axis * (drift > 0.0f ? 1.0f : -1.0f);
        SurfaceHit third;
        if (q.Ray(m_Pos + Up() * s.BodyLift, dir, kSide + std::fabs(drift), third))
            drift = std::copysign(std::clamp(third.Distance - kSide, 0.0f, std::fabs(drift)), drift);
    }
    c.Point += c.Axis * drift;
    c.At = next;
    c.Turned += turn;
    m_Rot = glm::normalize(glm::angleAxis(turn, c.Axis) * m_Rot);
    const glm::vec3 u = Up();
    if (c.Kind == 1) {
        // (The corner line moves off the wall as the head's offset eases from where it started to the body's height.)
        const glm::vec3 line = c.Point - c.D1 * ((s.BodyLift - c.WallLift) * c.At / c.Facing);
        const glm::vec3 belly = line - c.D1 * (c.Length * (1.0f - c.At) * (1.0f - w)) + c.D2 * (c.Length * w * c.At);
        m_Pos = belly - u * s.BodyLift;
        // Off both faces by the chord's sag (the legs bridge it).
        const float off1 = glm::dot(m_Pos - (c.Point - c.U0 * s.BodyLift), c.U0);
        const glm::vec3 n2 = RotateAbout(c.U0, c.Axis, c.Span);
        const float off2 = glm::dot(m_Pos - (c.Point - n2 * s.BodyLift), n2);
        m_SurfaceGap = std::max(0.0f, std::min(off1, off2));
    } else {
        m_Pos = c.Point + RotateAbout(c.D1, c.Axis, c.Turned) * c.At;
        m_SurfaceGap = 0.0f;
    }
    m_SurfaceNormal = u;
    m_Moves |= c.Kind == 1 ? kMoveCorner : kMoveEdge;
    if (done) {
        c.Kind = 0;
        if (!back) m_SurfaceNormal = Up();
    }
    return dist - used;
}

void Mover::Step(const SurfaceQuery& q, const MoverSettings& s, const MoveInput& in, float dt) {
    if (dt <= 0.0f) return;
    if (!Finite(m_Pos) || !std::isfinite(m_Rot.w)) Reset(glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    if (!m_Attached) {
        Fall(q, s, dt);
        return;
    }
    const glm::vec3 before = m_Pos;
    glm::vec3 up = Up(), fwd = Forward();

    // Steering: toward the goal over the surface it is on. A goal on another surface (the ceiling, from the floor)
    // still has a direction along this one, and that leads to the wall between them.
    float wantSpeed = 0.0f;
    m_Arrived = false;
    if (in.HasGoal) {
        const glm::vec3 d = in.Goal - m_Pos;
        const float dist = glm::length(d);
        if (dist < s.Arrive) {
            m_Arrived = true;
        } else {
            const glm::vec3 along = SafeNormalize(d - up * glm::dot(d, up), fwd);
            const float angle = std::atan2(glm::dot(glm::cross(fwd, along), up), glm::dot(fwd, along));
            // Mid-corner it holds its heading: the turn's line (abdomen on one face, head on the next) is the body's.
            const float maxTurn = m_Corner.Kind != 0 ? 0.0f : glm::radians(s.TurnRate) * dt;
            m_Rot = glm::normalize(glm::angleAxis(std::clamp(angle, -maxTurn, maxTurn), up) * m_Rot);
            fwd = Forward();
            // It slows into a sharp turn (a spider pivots on the spot rather than circling) and into the goal.
            const float facing = std::max(0.2f, std::cos(std::min(std::fabs(angle), kPi * 0.5f)));
            wantSpeed = std::min(s.Speed * std::max(0.0f, in.SpeedScale) * facing, dist * 3.0f);
        }
    }

    // A corner ahead: it slows to what the turn over it allows, braking in time.
    Corner next;
    float nextAt = 0.0f;
    bool haveNext = false;
    if (m_Corner.Kind != 0) {
        wantSpeed = std::min(wantSpeed, CornerSpeed(s, m_Corner));
    } else if (wantSpeed > 0.0f || m_Speed > 0.0f) {
        const float brake = (m_Speed * m_Speed) / (2.0f * std::max(0.1f, s.Accel));
        haveNext = FindCorner(q, s, brake + (m_Speed + s.Accel * dt) * dt + 0.05f, next, nextAt);
        if (haveNext) {
            const float v = CornerSpeed(s, next);
            wantSpeed = std::min(wantSpeed, std::sqrt(v * v + 2.0f * s.Accel * nextAt));
        }
    }
    const float accel = s.Accel * dt;
    m_Speed = std::clamp(wantSpeed, m_Speed - accel, m_Speed + accel);

    // Travel: along the face it is on, into a corner's turn when it meets one, out onto the next face.
    float dist = m_Speed * dt, turnBudget = glm::radians(s.MaxUpRate) * dt;
    for (int seg = 0; seg < 4 && dist > 1e-6f; ++seg) {
        if (m_Corner.Kind != 0) {
            const float left = FollowCorner(q, s, dist, dt, turnBudget);
            if (m_Corner.Kind != 0) break; // still turning (or out of turn for this frame)
            dist = left;
            continue;
        }
        if (seg == 0 && haveNext && nextAt <= dist) {
            m_Pos += Forward() * nextAt;
            dist -= nextAt;
            m_Corner = next;
            continue;
        }
        m_Pos += Forward() * dist;
        dist = 0.0f;
    }
    if (m_Corner.Kind != 0) {
        m_Unsupported = 0.0f;
        m_Vel = (m_Pos - before) / dt;
        return;
    }
    up = Up();
    fwd = Forward();

    // The face under it: a rise or a drop of under a step's height is walked up or down (never jumped), and its up
    // follows the face's tilt; nothing under it for a moment and it lets go.
    glm::vec3 wantUp = up;
    SurfaceHit under;
    const float probeLift = s.StepUp + 0.02f;
    const bool supported = q.Ray(m_Pos + up * probeLift, -up, probeLift + s.StepUp, under) && glm::dot(under.Normal, up) > 0.5f;
    if (supported) {
        const float off = glm::dot(m_Pos - under.Point, up);
        const float maxFix = 0.6f * m_Speed * dt + 0.002f;
        m_Pos -= up * std::clamp(off, -maxFix, maxFix);
        m_SurfaceNormal = under.Normal;
        m_SurfaceGap = off;
        wantUp = under.Normal;
        m_Unsupported = 0.0f;
        m_Moves |= kMoveSupport;
    } else {
        m_SurfaceGap = -1.0f;
        m_Moves |= kMoveFree;
        m_Unsupported += dt;
        if (m_Unsupported > s.LetGoAfter) {
            m_Moves |= kMoveLetGo;
            m_Attached = false;
            m_Left = SurfaceLeft{m_Pos, m_SurfaceNormal};
            m_Vel = fwd * m_Speed;
            m_Speed = 0.0f;
            return;
        }
    }
    // The legs feel the surface too and smooth the body over its bumps - but only the surface it stands on.
    if (glm::length(in.LegUp) > 0.5f && supported) {
        const glm::vec3 legUp = glm::normalize(in.LegUp);
        if (glm::dot(legUp, wantUp) > 0.85f) wantUp = SafeNormalize(wantUp * 0.65f + legUp * 0.35f, wantUp);
    }
    TurnUpToward(wantUp, s.UpRate, glm::degrees(turnBudget / dt), dt);

    // The body itself, not just its origin, stays out of the surfaces it passes (furniture's corners, a door's edge):
    // whatever its head or abdomen would sink into pushes it back along itself, and failing room, up off its face. Corners
    // are turned before the body meets them, so this is a guard - and moves it no faster than it walks.
    constexpr float kHeadroom = 0.08f;
    glm::vec3 push(0.0f);
    {
        const glm::vec3 u = Up(), f = Forward(), mid = m_Pos + u * s.BodyLift;
        constexpr float kLook = 0.5f;
        SurfaceHit h;
        const float aheadHit = q.Ray(mid, f, s.BodyFront + kLook, h) ? h.Distance : s.BodyFront + kLook;
        const glm::vec3 aheadN = h.Normal;
        const float behindHit = q.Ray(mid, -f, s.BodyBack + kLook, h) ? h.Distance : s.BodyBack + kLook;
        const glm::vec3 behindN = h.Normal;
        // A wall met at a glancing angle (walking along it, head or abdomen grazing it): out of it along its face's normal,
        // free to go on along it. Pushed back along itself instead, it was held there, walking into the push.
        auto glancing = [&](const glm::vec3& n, float into) { return std::fabs(glm::dot(n, u)) < 0.7f && into < 0.7f; };
        const float frontRoom = s.BodyFront;
        const float front = std::max(0.0f, frontRoom - aheadHit), back = std::max(0.0f, s.BodyBack - behindHit);
        if (front > 1e-4f || back > 1e-4f) {
            const float roomAhead = std::max(0.0f, aheadHit - frontRoom), roomBehind = std::max(0.0f, behindHit - s.BodyBack);
            float lift = 0.0f;
            if (front > 0.0f && back > 0.0f) {
                lift = std::max(front, back);
            } else if (front > 0.0f && glancing(aheadN, -glm::dot(f, aheadN))) {
                push += aheadN * (front * std::max(0.0f, -glm::dot(f, aheadN)) + 0.002f);
            } else if (back > 0.0f && glancing(behindN, glm::dot(f, behindN))) {
                push += behindN * (back * std::max(0.0f, glm::dot(f, behindN)) + 0.002f);
            } else if (front > 0.0f) {
                const float slide = std::min(front, roomBehind);
                push -= f * slide;
                lift = front - slide;
            } else {
                const float slide = std::min(back, roomAhead);
                push += f * slide;
                lift = back - slide;
            }
            SurfaceHit over;
            if (!supported) lift = 0.0f;
            if (lift > 0.0f && q.Ray(mid, u, lift + kHeadroom, over)) lift = std::min(lift, over.Distance - kHeadroom);
            push += u * std::max(0.0f, lift);
        }
    }
    const float pushMax = 0.5f * m_Speed * dt + 0.004f;
    if (glm::length(push) > 1e-5f) {
        if (glm::length(push) > pushMax) push *= pushMax / glm::length(push);
        m_Pos += push;
        m_Moves |= kMoveSlide;
    }
    // Never under something: the origin sits on the surface - in 301 on the floor slab's top, 5 mm below the walls'
    // bottoms - while the probes that find walls look 10 cm up and more. A hunter that landed on a wall's face and held
    // there for a bite slid under the wall; inside it, its probes passed through the wall from within and it walked on out
    // of the building. A spot with something solid just over it (no room for a body) is refused.
    if (glm::dot(up, Up()) > 0.95f) {
        constexpr float kRoomOver = 0.05f;
        SurfaceHit lid;
        auto covered = [&](const glm::vec3& at, const glm::vec3& u) { return q.Ray(at + u * 0.001f, u, kRoomOver, lid); };
        if (covered(m_Pos, Up())) { m_Pos = before; m_Moves |= kMoveRefused; } // (one landed on a wall's face is covered already: it may only move out)
    }
    m_Vel = (m_Pos - before) / dt;
}

void Mover::Fall(const SurfaceQuery& q, const MoverSettings& s, float dt) {
    m_Vel.y -= s.Gravity * dt;
    const glm::vec3 move = m_Vel * dt;
    const float len = glm::length(move);
    SurfaceHit land;
    // It rights itself on the way down: feet first onto the floor.
    TurnUpToward(glm::vec3(0.0f, 1.0f, 0.0f), 6.0f, s.MaxUpRate, dt);
    if (len > 1e-6f) {
        const glm::vec3 dir = move / len;
        // The ray looks back a little (it may sit right on what it lands on) - except while it leaves the face it let go
        // of: from there, looking back would start inside that face (or behind a thin one, a door's leaf) and land it
        // straight back on it.
        const bool leaving = glm::dot(dir, m_Left.Normal) > 0.0f && glm::dot(m_Pos - m_Left.Point, m_Left.Normal) < 0.1f;
        const float back = leaving ? 0.0f : 0.05f;
        // Only what this frame's fall reaches: looking further ahead pulled the body onto the floor before it got there.
        if (q.Ray(m_Pos - dir * back, dir, len + back + 0.005f, land) && (!leaving || land.Distance > 1e-3f)) {
            m_Pos = land.Point;
            m_Moves |= kMoveLand;
            m_SurfaceNormal = land.Normal;
            m_SurfaceGap = 0.0f;
            m_Vel = glm::vec3(0.0f);
            m_Speed = 0.0f;
            m_Unsupported = 0.0f;
            m_Attached = true;
            m_Corner = Corner{};
            return;
        }
    }
    m_Pos += move;
    m_Moves |= kMoveFall;
    m_SurfaceGap = -1.0f;
}

void Mover::Detach(const glm::vec3& velocity) {
    if (!m_Attached) {
        m_Vel += velocity;
        return;
    }
    // It lets go from where it is (Fall's first rays don't look back into the face it leaves).
    m_Left = SurfaceLeft{m_Pos, Up()};
    m_Moves |= kMoveDetach;
    m_Vel = velocity;
    m_Attached = false;
    m_Arrived = false;
    m_Speed = 0.0f;
    m_Unsupported = 0.0f;
    m_SurfaceGap = -1.0f;
    m_Corner = Corner{};
}

// ---- legs ----

void Gait::Init(const std::array<LegRest, kLegs>& rest, const glm::mat4& model) {
    m_Rest = rest;
    const glm::vec3 up = SafeNormalize(glm::vec3(model[1]), glm::vec3(0.0f, 1.0f, 0.0f));
    for (int i = 0; i < kLegs; ++i) {
        LegState& l = m_Legs[(size_t)i];
        l = LegState{};
        l.Foot = l.Want = l.From = l.To = glm::vec3(model * glm::vec4(rest[(size_t)i].Foot, 1.0f));
        l.Normal = l.WantNormal = l.FromNormal = l.ToNormal = up;
        l.WantGround = l.Grounded = true;
        l.Local = rest[(size_t)i].Foot;
    }
    m_NextProbe = m_Landed = m_BothLifted = 0;
    m_LastModel = model;
}

void Gait::FindFoothold(const SurfaceQuery& q, const GaitSettings& s, const glm::mat4& model, const glm::vec3& bodyPos,
                        const glm::vec3& up, const glm::vec3& lead, int leg) {
    const LegRest& r = m_Rest[(size_t)leg];
    LegState& l = m_Legs[(size_t)leg];
    const glm::vec3 restModel = r.Hip + (r.Foot - r.Hip) * s.StanceScale;
    const glm::vec3 hip = glm::vec3(model * glm::vec4(r.Hip, 1.0f));
    const glm::vec3 target = glm::vec3(model * glm::vec4(restModel, 1.0f)) + lead;
    SurfaceHit h;
    // Something between the hip and the spot - the wall it is walking into, a step up - is where the foot goes.
    const glm::vec3 reach = target - hip;
    const float reachLen = glm::length(reach);
    bool found = reachLen > 1e-4f && q.Ray(hip, reach / reachLen, reachLen, h);
    // The surface under the spot.
    if (!found) found = q.Ray(target + up * s.ProbeLift, -up, s.ProbeLift + s.ProbeDepth, h);
    // Past an edge: the face below it, looked for back toward the body.
    if (!found) {
        const glm::vec3 from = target - up * 0.08f;
        const glm::vec3 to = bodyPos - up * 0.08f;
        const float len = glm::length(to - from);
        found = len > 1e-4f && q.Ray(from, (to - from) / len, len, h);
    }
    if (found && glm::length(h.Point - hip) <= r.Reach * 1.02f) {
        l.WantNormal = SafeNormalize(h.Normal, up);
        l.Want = h.Point + l.WantNormal * r.Lift;
        l.WantGround = true;
    } else {
        // Nothing in reach: the leg hangs at its rest spot, a little below the body.
        l.Want = target - up * 0.05f;
        l.WantNormal = up;
        l.WantGround = false;
    }
}

void Gait::Step(const SurfaceQuery& q, const GaitSettings& s, const glm::mat4& model, const glm::vec3& bodyPos,
                const glm::vec3& up, const glm::vec3& velocity, bool attached, float dt) {
    if (dt <= 0.0f) return;
    const glm::vec3 travel = velocity - up * glm::dot(velocity, up);
    const float speed01 = std::clamp(glm::length(travel) / 2.5f, 0.0f, 1.0f);
    const glm::vec3 lead = travel * (s.StepTime * s.Lead);

    // Footholds: every stepping leg (its target moves with the body), and a few of the others in turn.
    for (int i = 0; i < kLegs; ++i)
        if (m_Legs[(size_t)i].Stepping) FindFoothold(q, s, model, bodyPos, up, lead, i);
    for (int k = 0, probed = 0; k < kLegs && probed < s.ProbesPerFrame; ++k) {
        const int i = m_NextProbe;
        m_NextProbe = (m_NextProbe + 1) % kLegs;
        if (m_Legs[(size_t)i].Stepping) continue;
        FindFoothold(q, s, model, bodyPos, up, lead, i);
        ++probed;
    }

    // A foot on nothing (falling, or hanging off an edge) goes with the body and swings to where it would hang, no faster
    // than a leg swings: letting go never snaps the legs to their rest, and a hanging foot never jumps when it is re-probed.
    const glm::mat4 toModel = glm::inverse(model);
    auto swing = [&](LegState& l, const glm::vec3& wantWorld) {
        const glm::vec3 local = l.Local;
        glm::vec3 d = glm::vec3(toModel * glm::vec4(wantWorld, 1.0f)) - local;
        const float maxMove = kLegSwing * dt;
        if (glm::length(d) > maxMove) d *= maxMove / glm::length(d);
        l.Local = local + d;
        l.Foot = glm::vec3(model * glm::vec4(l.Local, 1.0f));
    };
    if (!attached) {
        // Falling: the legs let go and hang where the rig holds them.
        for (int i = 0; i < kLegs; ++i) {
            LegState& l = m_Legs[(size_t)i];
            const LegRest& r = m_Rest[(size_t)i];
            if (l.Planted || l.Stepping) l.Local = glm::vec3(glm::inverse(m_LastModel) * glm::vec4(l.Foot, 1.0f)); // (just let go: where it was on the body)
            swing(l, glm::vec3(model * glm::vec4(r.Hip + (r.Foot - r.Hip) * s.StanceScale, 1.0f)));
            l.Normal = up;
            l.Planted = l.Stepping = l.Grounded = false;
        }
        m_LastModel = model;
        return;
    }

    // A run quickens the legs more than it lengthens the stride: a leg waiting for its group's turn mustn't be left
    // stretched past its reach.
    const float duration = std::max(s.MinStepTime, s.StepTime * (1.0f - 0.65f * speed01));
    const float stepDistance = s.StepDistance * (1.0f + speed01);

    // New steps, in strict turns: a group lifts only once every leg stands again, and the turn passes to the other group
    // whenever it has a leg to move - so four legs always hold on, and neither half is left behind (a group whose legs
    // landed one by one would otherwise keep the turn for ever).
    if (Stepping(0) == 0 && Stepping(1) == 0) {
        bool need[kLegs] = {};
        bool groupNeeds[2] = {false, false};
        for (int i = 0; i < kLegs; ++i) {
            const LegState& l = m_Legs[(size_t)i];
            const LegRest& r = m_Rest[(size_t)i];
            const glm::vec3 hip = glm::vec3(model * glm::vec4(r.Hip, 1.0f));
            need[i] = !l.Planted || glm::length(l.Foot - l.Want) > stepDistance || glm::length(l.Foot - hip) > r.Reach * 0.97f ||
                      glm::dot(l.Normal, l.WantNormal) < 0.8f;
            groupNeeds[r.Group] = groupNeeds[r.Group] || need[i];
        }
        const int g = groupNeeds[1 - m_LastGroup] ? 1 - m_LastGroup : m_LastGroup;
        if (groupNeeds[g]) {
            for (int i = 0; i < kLegs; ++i) {
                LegState& l = m_Legs[(size_t)i];
                if (m_Rest[(size_t)i].Group != g || !need[i]) continue;
                l.From = l.Foot;
                l.FromNormal = l.Normal;
                l.To = l.Want;
                l.ToNormal = l.WantNormal;
                l.ToGround = l.WantGround;
                l.T = 0.0f;
                // A long reach (a foot off the floor onto the wall round a corner) takes longer: never a whip.
                l.Duration = std::max(duration, glm::length(l.To - l.From) / kFootSpeed);
                l.Stepping = true;
                l.Planted = false;
            }
            m_LastGroup = g;
        }
    }

    // A stepping foot's target follows its foothold as the body carries it along; a foothold that jumps (re-found on the
    // next face over, the wall it is turning onto) restarts the step from where the foot is - never a dart across. In its
    // last quarter the foot is committed: it comes down on the foothold it was going to (it slows to nothing landing).
    const float drift = (glm::length(travel) + 0.3f) * dt + 0.08f; // (a foothold's drift as the body moves; past it, a jump)
    for (int i = 0; i < kLegs; ++i) {
        LegState& l = m_Legs[(size_t)i];
        if (l.Stepping) {
            if (glm::length(l.Want - l.To) <= drift) {
                l.To = l.Want;
                l.ToNormal = l.WantNormal;
                l.ToGround = l.WantGround;
            } else if (l.T < 0.75f) {
                l.From = l.Foot;
                l.FromNormal = l.Normal;
                l.To = l.Want;
                l.ToNormal = l.WantNormal;
                l.ToGround = l.WantGround;
                l.T = 0.0f;
                l.Duration = std::max(duration, glm::length(l.To - l.From) / kFootSpeed);
            }
            l.T += dt / std::max(1e-3f, l.Duration);
            const float t = std::min(l.T, 1.0f);
            const float e = t * t * (3.0f - 2.0f * t);
            const glm::vec3 n = SafeNormalize(glm::mix(l.FromNormal, l.ToNormal, e), up);
            l.Foot = glm::mix(l.From, l.To, e) + n * (s.StepHeight * std::sin(kPi * t));
            l.Normal = n;
            if (l.T >= 1.0f) {
                l.Foot = l.To;
                l.Normal = l.ToNormal;
                l.Stepping = false;
                l.Planted = true;
                l.Grounded = l.ToGround;
                l.Local = glm::vec3(toModel * glm::vec4(l.Foot, 1.0f));
                ++m_Landed;
            }
        } else if (!(l.Planted && l.Grounded)) {
            swing(l, l.Want); // hanging (or not yet down after a landing): it goes where the body takes it
            l.Normal = l.WantNormal;
        } else {
            l.Local = glm::vec3(toModel * glm::vec4(l.Foot, 1.0f));
        }
    }
    if (Stepping(0) > 0 && Stepping(1) > 0) ++m_BothLifted;
    m_LastModel = model;
}

glm::vec3 Gait::MeanNormal() const {
    glm::vec3 sum(0.0f);
    for (const LegState& l : m_Legs)
        if (l.Planted && l.Grounded) sum += l.Normal;
    return glm::length(sum) > 1e-4f ? glm::normalize(sum) : glm::vec3(0.0f);
}

int Gait::Stepping(int group) const {
    int n = 0;
    for (int i = 0; i < kLegs; ++i)
        if (m_Rest[(size_t)i].Group == group && m_Legs[(size_t)i].Stepping) ++n;
    return n;
}

// ---- rig ----

bool ParseRig(const std::string& text, RigDef& out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return fail("not a JSON object");
    out = RigDef{};
    if (const auto b = j.find("body"); b != j.end() && b->is_object()) {
        out.Thorax = b->value("thorax", std::string());
        out.Abdomen = b->value("abdomen", std::string());
    }
    out.StanceClip = j.value("stanceClip", std::string());
    const auto legs = j.find("legs");
    if (legs == j.end() || !legs->is_array() || legs->size() != (size_t)kLegs) return fail("needs eight legs");
    for (int i = 0; i < kLegs; ++i) {
        const auto& l = (*legs)[(size_t)i];
        if (!l.is_object()) return fail("a leg isn't an object");
        out.LegNames[(size_t)i] = l.value("name", std::string());
        if (const auto bs = l.find("bones"); bs != l.end() && bs->is_array())
            for (const auto& b : *bs)
                if (b.is_string()) out.LegBones[(size_t)i].push_back(b.get<std::string>());
        if (out.LegBones[(size_t)i].empty() || out.LegBones[(size_t)i].size() > 8) return fail("a leg needs 1-8 bones");
        out.LegGroup[(size_t)i] = -1;
    }
    const auto groups = j.find("gaitGroups");
    if (groups == j.end() || !groups->is_array() || groups->size() != 2) return fail("needs two gait groups");
    for (int g = 0; g < 2; ++g)
        for (const auto& name : (*groups)[(size_t)g])
            for (int i = 0; i < kLegs; ++i)
                if (name.is_string() && name.get<std::string>() == out.LegNames[(size_t)i]) out.LegGroup[(size_t)i] = g;
    for (int i = 0; i < kLegs; ++i)
        if (out.LegGroup[(size_t)i] < 0) return fail("a leg is in no gait group");

    // The body parts (optional): named, their parents by name, resolved once all are read.
    auto zoneOf = [](const std::string& z, PartZone& out) {
        for (int k = 0; k < kZones; ++k)
            if (z == ZoneName((PartZone)k)) { out = (PartZone)k; return true; }
        return false;
    };
    if (const auto zs = j.find("zones"); zs != j.end() && zs->is_object())
        for (int k = 0; k < kZones; ++k) {
            const auto z = zs->find(ZoneName((PartZone)k));
            if (z == zs->end() || !z->is_object()) continue;
            out.ZoneMultiplier[k] = z->value("damage", out.ZoneMultiplier[k]);
            out.FlinchDeg[k] = z->value("flinch", out.FlinchDeg[k]);
        }
    out.FlinchTime = std::max(0.05f, j.value("flinchTime", out.FlinchTime));
    if (const auto ps = j.find("parts"); ps != j.end()) {
        if (!ps->is_array()) return fail("parts isn't an array");
        if (ps->size() > (size_t)kMaxParts) return fail("more than 32 parts");
        std::vector<std::string> parents;
        for (const auto& p : *ps) {
            if (!p.is_object()) return fail("a part isn't an object");
            PartDef d;
            d.Name = p.value("name", std::string());
            d.Bone = p.value("bone", std::string());
            d.Start = p.value("start", std::string());
            d.End = p.value("end", std::string());
            d.Length = p.value("length", 0.0f);
            d.Radius = p.value("radius", d.Radius);
            d.Mass = p.value("mass", d.Mass);
            d.Swing = p.value("swing", d.Swing);
            d.Twist = p.value("twist", d.Twist);
            d.Curl = p.value("curl", d.Curl);
            d.Segment = p.value("segment", 0);
            if (d.Name.empty() || d.Bone.empty()) return fail("a part needs a name and a bone");
            if (d.End.empty() && d.Length <= 0.0f) return fail("a part needs an end bone or a length");
            if (!(d.Radius > 0.0f) || !(d.Mass > 0.0f)) return fail("a part needs a radius and a mass");
            if (!zoneOf(p.value("zone", std::string("leg")), d.Zone)) return fail("a part's zone is head, abdomen or leg");
            const std::string leg = p.value("leg", std::string());
            for (int i = 0; i < kLegs && !leg.empty(); ++i)
                if (out.LegNames[(size_t)i] == leg) d.Leg = i;
            if (!leg.empty() && d.Leg < 0) return fail("a part names a leg the rig hasn't");
            parents.push_back(p.value("parent", std::string()));
            out.Parts.push_back(std::move(d));
        }
        int roots = 0;
        for (size_t i = 0; i < out.Parts.size(); ++i) {
            if (parents[i].empty()) { ++roots; continue; }
            for (size_t k = 0; k < out.Parts.size(); ++k)
                if (k != i && out.Parts[k].Name == parents[i]) out.Parts[i].Parent = (int)k;
            if (out.Parts[i].Parent < 0) return fail("a part's parent isn't a part");
        }
        if (!out.Parts.empty() && roots != 1) return fail("the parts need exactly one root");
        // No loops: every part reaches the root within the part count.
        for (size_t i = 0; i < out.Parts.size(); ++i) {
            int at = (int)i, guard = 0;
            while (at >= 0 && guard++ <= (int)out.Parts.size()) at = out.Parts[(size_t)at].Parent;
            if (at >= 0) return fail("the parts' parents loop");
        }
    }
    return true;
}

const char* ZoneName(PartZone z) {
    switch (z) {
    case PartZone::Head: return "head";
    case PartZone::Abdomen: return "abdomen";
    case PartZone::Leg: return "leg";
    }
    return "leg";
}

bool BarrierQuery::Ray(const glm::vec3& from, const glm::vec3& dir, float maxDistance, SurfaceHit& out) const {
    const bool hit = World.Ray(from, dir, maxDistance, out);
    float best = hit ? out.Distance : maxDistance;
    int found = -1, axis = -1;
    float sign = 1.0f;
    for (size_t i = 0; i < Barriers.size(); ++i) { // slabs, in the box's own frame
        const Barrier& b = Barriers[i];
        const glm::quat inv = glm::conjugate(b.Rotation);
        const glm::vec3 o = inv * (from - b.Centre), d = inv * dir;
        float t0 = 0.0f, t1 = best;
        int a0 = -1;
        float s0 = 1.0f;
        bool miss = false;
        for (int a = 0; a < 3 && !miss; ++a) {
            if (std::fabs(d[a]) < 1e-9f) {
                miss = o[a] < -b.Half[a] || o[a] > b.Half[a];
                continue;
            }
            float ta = (-b.Half[a] - o[a]) / d[a], tb = (b.Half[a] - o[a]) / d[a];
            const float s = d[a] > 0.0f ? -1.0f : 1.0f; // the face it enters through faces back along the ray
            if (ta > tb) std::swap(ta, tb);
            if (ta > t0) { t0 = ta; a0 = a; s0 = s; }
            t1 = std::min(t1, tb);
            miss = t0 > t1;
        }
        if (miss || a0 < 0 || t0 >= best) continue;
        best = t0;
        found = (int)i;
        axis = a0;
        sign = s0;
    }
    if (found < 0) return hit;
    glm::vec3 n(0.0f);
    n[axis] = sign;
    out.Hit = true;
    out.Distance = best;
    out.Point = from + dir * best;
    out.Normal = Barriers[(size_t)found].Rotation * n;
    return true;
}

bool BarrierQuery::Occupied(const glm::vec3& centre, float radius) const {
    for (const Barrier& b : Barriers) {
        const glm::vec3 c = glm::conjugate(b.Rotation) * (centre - b.Centre);
        if (glm::length(c - glm::clamp(c, -b.Half, b.Half)) < radius) return true;
    }
    return World.Occupied(centre, radius);
}

} // namespace Spider
