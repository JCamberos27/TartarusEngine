#include "SpiderRagdoll.h"

#include "PhysicsWorld.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace SpiderRagdoll {

void CapsuleOver(const glm::vec3& a, const glm::vec3& b, float radius, const glm::vec3& up, glm::vec3& centre, glm::quat& rot,
                 float& halfLength) {
    const glm::vec3 d = b - a;
    const float len = glm::length(d);
    const glm::vec3 x = len > 1e-5f ? d / len : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 y = up - x * glm::dot(up, x);
    if (glm::length(y) < 1e-4f) { // along the up: any side will do
        y = glm::cross(x, glm::vec3(0.0f, 0.0f, 1.0f));
        if (glm::length(y) < 1e-4f) y = glm::cross(x, glm::vec3(1.0f, 0.0f, 0.0f));
    }
    y = glm::normalize(y);
    const glm::vec3 z = glm::cross(x, y);
    rot = glm::normalize(glm::quat_cast(glm::mat3(x, y, z)));
    centre = (a + b) * 0.5f;
    halfLength = std::max(0.5f * len - 0.5f * radius, 0.005f);
}

bool BuildParts(const Spider::PartDef* defs, int count, const PartPoint* points, const glm::vec3& bodyUp,
                PhysicsWorld::RagdollPart* out, glm::mat4* partWorld) {
    if (!defs || !points || !out || count <= 0 || count > Spider::kMaxParts) return false;
    int roots = 0;
    for (int i = 0; i < count; ++i) {
        if (defs[i].Parent < 0) ++roots;
        else if (defs[i].Parent >= count || defs[i].Parent == i) return false;
    }
    if (roots != 1) return false;
    const glm::vec3 up = glm::length(bodyUp) > 1e-4f ? glm::normalize(bodyUp) : glm::vec3(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < count; ++i) {
        const Spider::PartDef& d = defs[i];
        PhysicsWorld::RagdollPart& p = out[i];
        p = PhysicsWorld::RagdollPart{};
        glm::vec3 c;
        glm::quat r;
        float half;
        CapsuleOver(points[i].A, points[i].B, d.Radius, up, c, r, half);
        p.Parent = d.Parent;
        p.Position[0] = c.x; p.Position[1] = c.y; p.Position[2] = c.z;
        p.Rotation[0] = r.x; p.Rotation[1] = r.y; p.Rotation[2] = r.z; p.Rotation[3] = r.w;
        p.HalfLength = half;
        p.Radius = d.Radius;
        p.Mass = d.Mass;
        const glm::vec3 anchor = d.Parent < 0 ? c : points[i].Anchor;
        p.Anchor[0] = anchor.x; p.Anchor[1] = anchor.y; p.Anchor[2] = anchor.z;
        // Room for the curl (the cone is about the death pose), and no further: a leg can't fold through itself.
        p.SwingDeg = std::clamp(std::max(d.Swing, std::fabs(d.Curl) + 12.0f), 5.0f, 120.0f);
        p.TwistDeg = std::clamp(d.Twist, 2.0f, 60.0f);
        // Thin light segments spin through their limits under contact friction: a little more inertia and joint damping.
        p.JointDamping = d.Leg >= 0 ? 5.0f : 3.0f;
        p.InertiaScale = d.Leg >= 0 ? 1.8f : 1.0f;
        // The lower legs slide on the floor as they fold in (a grip would hold the tips out and keep the body propped).
        p.FrictionScale = d.Leg >= 0 && d.Segment > 0 ? 0.25f : 1.0f;
        p.IgnoreMask = 0u;
        if (d.Leg >= 0)
            for (int j = 0; j < count; ++j)
                if (j != i && defs[j].Leg >= 0 && defs[j].Leg != d.Leg) p.IgnoreMask |= 1u << j;
        if (partWorld) partWorld[i] = glm::translate(glm::mat4(1.0f), c) * glm::mat4_cast(r);
    }
    return true;
}

PhysicsWorld::RagdollParams BodyParams() {
    PhysicsWorld::RagdollParams p;
    p.LinearDamping = 0.12f;
    p.AngularDamping = 0.6f;
    p.SolverPosIters = 32;
    p.SolverVelIters = 8;
    p.Depenetration = 1.5f; // pushed out of the floor gently, never fired out
    p.SleepThreshold = 0.05f;
    p.StaticFriction = 0.9f;
    p.DynamicFriction = 0.75f;
    p.Restitution = 0.0f;
    p.Grip = true;
    p.StabilizationThreshold = 0.02f;
    p.SkinOffset = 0.008f;
    p.SelfCollision = true;
    p.TwistArmature = 0.08f;
    p.SwingArmature = 0.03f;
    return p;
}

glm::quat CurlTarget(const Spider::PartDef& def, const glm::quat& partRot, const glm::vec3& bodyUp, const glm::vec3& outward) {
    if (def.Leg < 0 || std::fabs(def.Curl) < 1e-3f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    // The leg's plane holds the body's up and the leg's outward line; folding about its normal cross(up, out) by a
    // positive angle takes the far end from out to down (and on, past down, to in under the body).
    glm::vec3 axis = glm::cross(bodyUp, outward);
    if (glm::length(axis) < 1e-4f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    axis = glm::normalize(glm::conjugate(partRot) * glm::normalize(axis));
    return glm::angleAxis(glm::radians(def.Curl), axis);
}

// ---- the limp motor ----

float LimpMotor::StrengthAt(const Settings& s, float t) {
    if (t < s.RiseTime) return std::clamp(t / std::max(s.RiseTime, 1e-3f), 0.0f, 1.0f);
    if (t < s.HoldTime) return 1.0f;
    if (t < s.FadeTime) {
        const float u = (t - s.HoldTime) / std::max(s.FadeTime - s.HoldTime, 1e-3f);
        const float e = u * u * (3.0f - 2.0f * u);
        return 1.0f + (s.Residual - 1.0f) * e;
    }
    return s.Residual;
}

void LimpMotor::Begin(int id, int count, const glm::quat* targets, const float* weight, const Settings& s) {
    m_Id = id;
    m_Count = std::clamp(count, 0, Spider::kMaxParts);
    m_Set = s;
    m_Time = 0.0f;
    m_Strength = 0.0f;
    m_Applied = -1.0f;
    for (int i = 0; i < m_Count; ++i) {
        m_Weight[(size_t)i] = weight ? std::max(weight[i], 0.0f) : 1.0f;
        const glm::quat q = targets ? glm::normalize(targets[i]) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const float xyzw[4] = {q.x, q.y, q.z, q.w};
        PhysicsWorld::SetRagdollDriveTarget(id, i, xyzw);
    }
    Update(0.0f);
}

void LimpMotor::Update(float dt) {
    if (m_Id < 0) return;
    m_Time += dt;
    m_Strength = StrengthAt(m_Set, m_Time);
    if (std::fabs(m_Strength - m_Applied) < 0.01f) return; // (held: nothing to tell the solver)
    m_Applied = m_Strength;
    const float damp = m_Set.Damping * std::max(m_Strength, 0.35f);
    for (int i = 0; i < m_Count; ++i)
        PhysicsWorld::SetRagdollPartDrive(m_Id, i, m_Set.Stiffness * m_Strength * m_Weight[(size_t)i], damp * m_Weight[(size_t)i]);
}

// ---- settling ----

void Settler::Update(float dt, int rootPart, const glm::vec3& upInRoot) {
    if (m_Id < 0) return;
    m_Time += dt;
    m_Age += dt;
    // On its side? Its up, from the root part's pose.
    glm::vec3 up(0.0f, 1.0f, 0.0f), at(0.0f);
    float p[3], q[4];
    if (rootPart >= 0 && PhysicsWorld::GetRagdollPart(m_Id, rootPart, p, q)) {
        up = glm::normalize(glm::quat(q[3], q[0], q[1], q[2]) * upInRoot);
        at = glm::vec3(p[0], p[1], p[2]);
    }
    constexpr float kFlat = 0.7f;    // |up.y| at or over this: on its belly or its back
    constexpr float kHeld = 0.9f;    // ...and rolled on to this before it is left to rest
    constexpr float kGiveUp = 12.0f; // (wedged on its side against something: it rests as it lies)
    const bool trying = m_Age < kGiveUp;
    m_OnSide = std::fabs(up.y) < kFlat && trying;
    if (m_Asleep) {
        if (!PhysicsWorld::RagdollAsleep(m_Id)) Wake(); // something shoved it
        else if (!m_OnSide) return;
        else Wake(); // (PhysX's own sleep caught it on its side)
    }
    if (m_Time > 0.3f && !m_OnSide && PhysicsWorld::RagdollAsleep(m_Id)) { // came to rest on its own (PhysX's sleep)
        m_Asleep = true;
        m_Target = 0.0f;
        if (m_AsleepAt < 0.0f) m_AsleepAt = m_Time;
        return;
    }
    float lin = 0.0f, ang = 0.0f;
    if (!PhysicsWorld::RagdollMotion(m_Id, &lin, &ang)) return;
#pragma warning(suppress : 4996)
    static const bool trace = std::getenv("TARTARUS_SPIDER_TRACE") != nullptr;
    if (trace && (int)(m_Age * 4.0f) != (int)((m_Age - dt) * 4.0f))
        std::printf("[SpiderSettle] %d t %.2f up.y %.2f lin %.2f ang %.2f side %d rolling to %.0f\n", m_Id, m_Age, up.y, lin, ang, (int)m_OnSide, m_Target);
    // Slowing: it heavies up, so it comes to rest instead of rocking on its curled legs.
    if (m_Time > 0.5f && lin < 0.3f && ang < 2.0f && m_Heavy < 0.0f) {
        m_Heavy = m_Time;
        PhysicsWorld::SetRagdollDamping(m_Id, 0.8f, 2.5f);
    }
    // Slowed on its side (propped on its round abdomen and the legs curled under it): a gentle roll onto the nearer of its
    // belly or its back, and held there till it sleeps - with its legs curled under, flat on its back is a crest it
    // tipped off again, back onto its side.
    if (trying && rootPart >= 0 && m_Time > 0.8f && lin < 0.8f && (m_Target != 0.0f || m_OnSide)) {
        if (m_Target == 0.0f) {
            m_Target = up.y >= 0.0f ? 1.0f : -1.0f;
            m_Best = -2.0f;
            m_SideFor = m_BestAt = 0.0f;
            m_PrevOff = -1.0f;
            m_Reached = false;
        }
        // Toward the nearer of belly and back; if that way won't go (a wall beside it, legs dug in), the other.
        if (std::fabs(up.y) >= kHeld) m_Reached = true;
        else if (std::fabs(up.y) < kFlat) m_Reached = false; // (tipped back onto its side)
        if (!m_Reached) {
            m_SideFor += dt;
            if (up.y * m_Target > m_Best + 0.02f) {
                m_Best = up.y * m_Target;
                m_BestAt = m_SideFor;
            } else if (m_SideFor - m_BestAt > 1.0f) {
                m_Target = -m_Target;
                m_PrevOff = -1.0f;
                m_Best = -2.0f;
                m_BestAt = m_SideFor;
            }
        } else if (m_Heavy >= 0.0f && m_Heavy < 1e8f) {
            // Rolled flat: it comes to rest there (heavy) rather than rocking.
            m_Heavy = 1e9f;
            PhysicsWorld::SetRagdollDamping(m_Id, 3.0f, 8.0f);
        }
        const glm::vec3 flat(0.0f, m_Target, 0.0f);
        glm::vec3 axis = glm::cross(up, flat);
        const float s = glm::length(axis);
        const float off = std::acos(std::clamp(glm::dot(up, flat), -1.0f, 1.0f));
        if (s > 1e-3f && !m_Reached) { // (once rolled flat it is left to settle, pushed again only if it tips back onto its side)
            axis /= s;
            // Rolled at a steady rate (what it already turns toward flat is measured from its up, and only the shortfall
            // added: a push that stacked up flung a part into the floor), easing in as it comes flat - never overshooting.
            constexpr float kRollRate = 2.5f; // rad/s (a quarter turn in some 0.6 s)
            const float rolling = m_PrevOff >= 0.0f ? (m_PrevOff - off) / std::max(dt, 1e-4f) : 0.0f;
            const float want = std::min(kRollRate, 3.0f * off);
            const float add = std::clamp(want - rolling, 0.0f, want);
            const glm::vec3 spin = axis * add;
            PhysicsWorld::RagdollAddSpin(m_Id, &spin.x);
        }
        m_PrevOff = off;
    }
    m_Still = lin < 0.08f && ang < 0.6f ? m_Still + dt : 0.0f;
    // Never put to sleep on its side (the old 5 s rule froze one mid-roll there), nor short of flat while it is rolled.
    const bool restful = !trying || std::fabs(up.y) >= (m_Target != 0.0f && !m_Reached ? kHeld : kFlat);
    if (restful && ((m_Time > 0.8f && m_Still > 0.35f) || (m_Time > 5.0f && lin < 0.5f && ang < 4.0f))) {
        PhysicsWorld::RagdollSleep(m_Id);
        m_Asleep = true;
        m_Target = 0.0f;
        if (m_AsleepAt < 0.0f) m_AsleepAt = m_Time;
    }
}

} // namespace SpiderRagdoll
