#include "SpiderLocomotion.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>

namespace Spider {

namespace {

constexpr float kPi = 3.14159265358979f;

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

} // namespace

// ---- body ----

void Mover::Reset(const glm::vec3& pos, const glm::quat& rot) {
    *this = Mover{};
    m_Pos = pos;
    m_Rot = glm::normalize(rot);
    m_SurfaceNormal = Up();
}

void Mover::Detach(const glm::vec3& velocity) {
    m_Attached = false;
    m_Vel = velocity;
    m_Speed = 0.0f;
    m_Arrived = false;
    // Clear of the surface first, or the landing probe finds the surface it leapt from.
    m_Pos += Up() * 0.06f;
}

void Mover::TurnUpToward(const glm::vec3& want, float rate, float dt) {
    const glm::vec3 up = Up();
    const glm::vec3 w = SafeNormalize(want, up);
    const glm::quat full = RotationBetween(up, w);
    // The whole body turns with its up, so its forward is carried over an edge rather than re-aimed.
    m_Rot = glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), full, Approach(rate, dt)) * m_Rot);
}

void Mover::Step(const SurfaceQuery& q, const MoverSettings& s, const MoveInput& in, float dt) {
    if (dt <= 0.0f) return;
    if (!Finite(m_Pos) || !std::isfinite(m_Rot.w)) Reset(glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    if (!m_Attached) {
        Fall(q, s, dt);
        return;
    }
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
            const float maxTurn = glm::radians(s.TurnRate) * dt;
            m_Rot = glm::normalize(glm::angleAxis(std::clamp(angle, -maxTurn, maxTurn), up) * m_Rot);
            fwd = Forward();
            // It slows into a sharp turn (a spider pivots on the spot rather than circling) and into the goal.
            const float facing = std::max(0.2f, std::cos(std::min(std::fabs(angle), kPi * 0.5f)));
            wantSpeed = std::min(s.Speed * std::max(0.0f, in.SpeedScale) * facing, dist * 3.0f);
        }
    }
    const float accel = s.Accel * dt;
    m_Speed = std::clamp(wantSpeed, m_Speed - accel, m_Speed + accel);
    glm::vec3 step = fwd * m_Speed * dt;

    // A surface ahead (a wall from the floor, the ceiling from a wall): it stops short of it and turns onto it, more
    // the closer it gets.
    glm::vec3 wantUp = up;
    float wallWeight = 0.0f;
    glm::vec3 wallNormal = up;
    SurfaceHit ahead;
    if (q.Ray(m_Pos + up * s.ProbeHeight, fwd, s.Reach + glm::length(step), ahead) && glm::dot(ahead.Normal, up) < 0.7f) {
        const float room = std::max(0.0f, ahead.Distance - s.Clearance);
        if (glm::length(step) > room) step = fwd * room;
        // ... only while it means to go on: one that has stopped in front of a wall stays on its own surface.
        const float going = std::clamp(m_Speed / 0.15f, 0.0f, 1.0f);
        wallWeight = std::clamp(1.0f - room / std::max(1e-3f, s.Reach - s.Clearance), 0.0f, 1.0f) * going;
        wallNormal = ahead.Normal;
    }
    const glm::vec3 cand = m_Pos + step;

    // The surface under it, straight down its own up; failing that, the face below an edge it has walked over (a
    // ledge's side, a wall's far face), looked for back under it.
    SurfaceHit under;
    bool supported = q.Ray(cand + up * s.ProbeLift, -up, s.ProbeLift + s.ProbeDepth, under);
    if (supported) {
        m_SurfaceGap = under.Distance - s.ProbeLift;
    } else {
        const float drop = 0.06f;
        supported = q.Ray(cand - up * drop, -fwd, s.Reach + glm::length(step) + 0.1f, under);
        m_SurfaceGap = supported ? drop : -1.0f;
    }
    const glm::vec3 before = m_Pos;
    if (supported) {
        m_Pos = under.Point;
        m_SurfaceNormal = under.Normal;
        wantUp = under.Normal;
        m_Unsupported = 0.0f;
    } else {
        m_Pos = cand;
        m_Unsupported += dt;
        if (m_Unsupported > s.LetGoAfter) {
            m_Attached = false;
            m_Vel = fwd * m_Speed;
            m_Speed = 0.0f;
            return;
        }
    }
    if (wallWeight > 0.0f) wantUp = SafeNormalize(glm::mix(wantUp, wallNormal, wallWeight), wantUp);
    // The legs feel the surface too and smooth the body over its bumps - but only the surface it stands on: legs out on
    // a neighbouring wall don't tip it toward that wall (the probe ahead does that when it means to climb).
    if (glm::length(in.LegUp) > 0.5f && wallWeight <= 0.0f && supported) {
        const glm::vec3 legUp = glm::normalize(in.LegUp);
        if (glm::dot(legUp, wantUp) > 0.85f) wantUp = SafeNormalize(wantUp * 0.65f + legUp * 0.35f, wantUp);
    }
    TurnUpToward(wantUp, s.UpRate * (wallWeight > 0.0f ? 1.6f : 1.0f), dt);
    m_Vel = (m_Pos - before) / dt;
}

void Mover::Fall(const SurfaceQuery& q, const MoverSettings& s, float dt) {
    m_Vel.y -= s.Gravity * dt;
    const glm::vec3 move = m_Vel * dt;
    const float len = glm::length(move);
    SurfaceHit land;
    // It rights itself on the way down: feet first onto the floor.
    TurnUpToward(glm::vec3(0.0f, 1.0f, 0.0f), 6.0f, dt);
    if (len > 1e-6f && q.Ray(m_Pos - move / len * 0.05f, move / len, len + 0.1f, land)) {
        m_Pos = land.Point;
        m_SurfaceNormal = land.Normal;
        m_SurfaceGap = 0.0f;
        m_Vel = glm::vec3(0.0f);
        m_Speed = 0.0f;
        m_Unsupported = 0.0f;
        m_Attached = true;
        return;
    }
    m_Pos += move;
    m_SurfaceGap = -1.0f;
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
    }
    m_NextProbe = m_Landed = m_BothLifted = 0;
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

    if (!attached) {
        // Falling: the legs let go and hang where the rig holds them.
        for (int i = 0; i < kLegs; ++i) {
            LegState& l = m_Legs[(size_t)i];
            const LegRest& r = m_Rest[(size_t)i];
            l.Foot = glm::vec3(model * glm::vec4(r.Hip + (r.Foot - r.Hip) * s.StanceScale, 1.0f));
            l.Normal = up;
            l.Planted = l.Stepping = l.Grounded = false;
        }
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
                l.T = 0.0f;
                l.Duration = duration;
                l.Stepping = true;
                l.Planted = false;
            }
            m_LastGroup = g;
        }
    }

    for (int i = 0; i < kLegs; ++i) {
        LegState& l = m_Legs[(size_t)i];
        if (l.Stepping) {
            l.To = l.Want;
            l.ToNormal = l.WantNormal;
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
                l.Grounded = l.WantGround;
                ++m_Landed;
            }
        } else if (l.Planted && !l.Grounded) {
            l.Foot = l.Want; // hanging, not standing: it goes where the body takes it
            l.Normal = l.WantNormal;
        }
    }
    if (Stepping(0) > 0 && Stepping(1) > 0) ++m_BothLifted;
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
    return true;
}

} // namespace Spider
