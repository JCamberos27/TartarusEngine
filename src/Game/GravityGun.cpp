#include "GravityGun.h"

#include "Player.h"
#include "Input.h"
#include "PhysicsWorld.h"
#include "ProjectSettings.h"
#include "GameModuleAPI.h" // RaycastHit, BodyState, QueryFilter

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace {

constexpr unsigned kNoEntity = 0xFFFFFFFFu;
constexpr float kGrabRange = 100.0f;
constexpr float kAssistRange = 30.0f;               // the forgiving search reaches this far
const float kAssistCos = std::cos(glm::radians(7.0f)); // within 7 degrees of the crosshair
constexpr float kScrollTurnDeg = 15.0f;             // Alt / Ctrl + one scroll notch

bool Grabbable(unsigned e) {
    BodyState bs;
    return e != kNoEntity && PhysicsWorld::GetBodyState(e, bs) && bs.Valid && !bs.Kinematic;
}

glm::vec3 Vec(const float v[3]) { return {v[0], v[1], v[2]}; }

} // namespace

bool GravityGun::IsHolding() const { return PhysicsWorld::IsGrabbing(); }

void GravityGun::Reset() {
    m_LmbPrev = m_RmbPrev = false;
    m_Charging = false;
    m_Charge = 0.0f;
    m_HaveHoldPoint = false;
    m_Prediction.Clear();
}

// The crosshair ray first; if it misses anything grabbable, the dynamic body nearest the aim
// within kAssistCos, closer than whatever the ray did hit and in plain sight - a single ray made
// small or distant objects fiddly to pick up.
unsigned GravityGun::FindGrabTarget(const glm::vec3& eye, const glm::vec3& fwd) const {
    QueryFilter solid;
    solid.HitTriggers = 0;
    const float o[3] = {eye.x, eye.y, eye.z}, d[3] = {fwd.x, fwd.y, fwd.z};
    RaycastHit hit;
    float reach = kAssistRange;
    if (PhysicsWorld::RaycastFiltered(o, d, kGrabRange, solid, hit)) {
        if (Grabbable(hit.Entity)) return hit.Entity;
        reach = std::min(reach, hit.Distance + 0.5f);
    }

    // Sweep overlap spheres down the aim line, each wide enough to cover the cone at its
    // distance, and keep the candidate closest in angle to the crosshair.
    unsigned best = kNoEntity;
    float bestCos = kAssistCos;
    unsigned ids[32];
    for (float t = 0.5f; t < reach;) {
        const float r = std::clamp(t * std::tan(glm::radians(7.0f)), 0.3f, 2.5f);
        const glm::vec3 c = eye + fwd * t;
        const float cf[3] = {c.x, c.y, c.z};
        const int n = PhysicsWorld::OverlapSphereFiltered(cf, r, solid, ids, 32);
        for (int i = 0; i < n; ++i) {
            if (ids[i] == best || !Grabbable(ids[i])) continue;
            float p[3];
            if (!PhysicsWorld::GetActorPosition(ids[i], p)) continue;
            const glm::vec3 to = Vec(p) - eye;
            const float dist = glm::length(to);
            if (dist < 1e-3f || dist > reach + 1.0f) continue;
            const float cosA = glm::dot(to / dist, fwd);
            if (cosA <= bestCos) continue;
            // Line of sight to its centre: the first solid thing on the way must be the body.
            const float td[3] = {to.x / dist, to.y / dist, to.z / dist};
            RaycastHit los;
            if (!PhysicsWorld::RaycastFiltered(o, td, dist + 0.5f, solid, los) || los.Entity != ids[i]) continue;
            best = ids[i];
            bestCos = cosA;
        }
        t += r; // spheres just touch, so the cone is covered without gaps
    }
    return best;
}

// The held body's flight if released now at `speed` along `fwd`: the same fixed-step integration
// PhysX does (gravity, then linear damping), sweeping the body's own shape along each step. At a
// hit the velocity bounces the way PhysX's contact will (restitution above its 2 m/s bounce
// threshold, Coulomb friction on the tangential part); the prediction stops after the second
// bounce, once the bounce is too weak to leave the surface, or after 5 s.
void GravityGun::PredictThrow(const glm::vec3& fwd, float speed) {
    m_Prediction.Clear();
    const unsigned held = PhysicsWorld::GrabbedEntity();
    float p0[3];
    if (held == kNoEntity || !PhysicsWorld::GetActorPosition(held, p0)) return;

    const auto& phys = ProjectSettings::Physics();
    const glm::vec3 g = phys.Gravity;
    const float step = std::clamp(phys.FixedTimestep, 1.0f / 240.0f, 1.0f / 30.0f);
    const float damping = PhysicsWorld::GetLinearDamping(held);
    constexpr int kMaxBounces = 2;
    constexpr float kBounceThreshold = 2.0f; // PhysX's default bounceThresholdVelocity
    m_Prediction.BodyRadius = PhysicsWorld::BodyRadius(held);

    glm::vec3 p = Vec(p0), v = fwd * speed;
    m_Prediction.Legs.push_back({p});
    int bounces = 0;
    for (float t = 0.0f; t < 5.0f; t += step) {
        v += g * step;
        v *= std::max(0.0f, 1.0f - damping * step);
        const glm::vec3 seg = v * step;
        const float len = glm::length(seg);
        if (len < 1e-6f) break;
        const glm::vec3 dir = seg / len;
        const float o[3] = {p.x, p.y, p.z}, d[3] = {dir.x, dir.y, dir.z};
        RaycastHit hit;
        float bounciness = 0.0f, friction = 0.0f;
        if (!PhysicsWorld::SweepBody(held, o, d, len, hit, bounciness, friction)) {
            p += seg;
            m_Prediction.Legs.back().push_back(p);
            continue;
        }
        // Contact: finish this leg where the body touches, mark the spot, then bounce.
        const glm::vec3 n = glm::normalize(Vec(hit.Normal));
        p += dir * hit.Distance;
        m_Prediction.Legs.back().push_back(p);
        m_Prediction.ContactPoints.push_back(Vec(hit.Point));
        m_Prediction.ContactNormals.push_back(n);
        const float vn = glm::dot(v, n);
        if (bounces == kMaxBounces || vn >= 0.0f) return;
        const float e = -vn > kBounceThreshold ? bounciness : 0.0f;
        const glm::vec3 vN = vn * n;
        glm::vec3 vT = v - vN;
        const float vtLen = glm::length(vT);
        if (vtLen > 1e-4f) vT *= std::max(0.0f, 1.0f - friction * (1.0f + e) * -vn / vtLen);
        v = vT - e * vN;
        if (e * -vn < 1.0f) return; // it won't leave the surface: it rolls or slides from here
        ++bounces;
        p += n * 0.002f;
        m_Prediction.Legs.push_back({p});
    }
}

void GravityGun::Update(float dt, const Player& player) {
    const glm::vec3 eye = player.Cam.Position, fwd = player.Cam.Front();
    const float of[3] = {eye.x, eye.y, eye.z}, df[3] = {fwd.x, fwd.y, fwd.z};
    const bool lmb = Input::IsMouseButtonDown(0);
    const bool rmb = Input::IsMouseButtonDown(1);
    const float yaw = glm::radians(player.Cam.Yaw);
    m_Prediction.Clear();

    if (rmb && !m_RmbPrev && !PhysicsWorld::IsGrabbing()) {
        const unsigned target = FindGrabTarget(eye, fwd);
        float p[3];
        if (target != kNoEntity && PhysicsWorld::GetActorPosition(target, p)) {
            PhysicsWorld::GrabBody(target);
            m_HoldDistance = glm::clamp(glm::length(Vec(p) - eye), 1.5f, 8.0f);
            float q[4];
            m_HoldRotation = PhysicsWorld::GetActorRotation(target, q) ? glm::quat(q[3], q[0], q[1], q[2])
                                                                       : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            m_HoldYaw = yaw;
            m_HaveHoldPoint = false;
            m_Charging = false;
        }
    }

    if (PhysicsWorld::IsGrabbing()) {
        // The held body turns with the view (Camera yaw grows clockwise seen from above).
        const float dYaw = std::remainder(yaw - m_HoldYaw, 6.2831853f);
        m_HoldYaw = yaw;
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        m_HoldRotation = glm::angleAxis(-dYaw, up) * m_HoldRotation;

        // Scroll: Alt turns it left / right, Ctrl tips it up / down, plain scroll moves it
        // nearer / further.
        const double scroll = Input::GetScrollDeltaY();
        if (scroll != 0.0) {
            const bool alt = Input::IsKeyDown(GLFW_KEY_LEFT_ALT) || Input::IsKeyDown(GLFW_KEY_RIGHT_ALT);
            const bool ctrl = Input::IsKeyDown(GLFW_KEY_LEFT_CONTROL) || Input::IsKeyDown(GLFW_KEY_RIGHT_CONTROL);
            const float turn = glm::radians(kScrollTurnDeg) * (float)scroll;
            if (alt)
                m_HoldRotation = glm::angleAxis(turn, up) * m_HoldRotation;
            else if (ctrl)
                m_HoldRotation = glm::angleAxis(turn, player.Cam.Right()) * m_HoldRotation;
            else
                m_HoldDistance = glm::clamp(m_HoldDistance + (float)scroll * 0.6f, 1.5f, 8.0f);
        }
        m_HoldRotation = glm::normalize(m_HoldRotation);

        // The hold point and how fast it's moving (walking, turning), which PhysicsWorld feeds
        // forward so the body keeps up instead of trailing behind.
        const glm::vec3 t = eye + fwd * m_HoldDistance;
        if (!m_HaveHoldPoint) m_HoldVelocity = glm::vec3(0.0f);
        if (m_HaveHoldPoint && dt > 1e-4f) {
            glm::vec3 raw = (t - m_PrevHoldPoint) / dt;
            const float speed = glm::length(raw);
            if (speed > 40.0f) raw *= 40.0f / speed; // a teleport / respawn, not motion
            m_HoldVelocity += (raw - m_HoldVelocity) * (1.0f - std::exp(-dt * 25.0f)); // ~40 ms smoothing
        }
        m_PrevHoldPoint = t;
        m_HaveHoldPoint = true;
        const glm::vec3 vel = m_HoldVelocity;
        const float tf[3] = {t.x, t.y, t.z}, vf[3] = {vel.x, vel.y, vel.z};
        const float qf[4] = {m_HoldRotation.x, m_HoldRotation.y, m_HoldRotation.z, m_HoldRotation.w};
        PhysicsWorld::UpdateGrab(tf, vf, qf);

        // Left mouse charges while held and throws on release: a tap lobs, a full charge fires.
        if (lmb && !m_LmbPrev) { m_Charging = true; m_Charge = 0.0f; }
        if (m_Charging && lmb)
            m_Charge = std::min(1.0f, m_Charge + dt / std::max(Settings.ChargeTime, 0.01f));
        const float speed = Settings.MinThrowSpeed + (Settings.MaxThrowSpeed - Settings.MinThrowSpeed) * m_Charge;
        if (m_Charging && !lmb) {
            const glm::vec3 imp = fwd * speed;
            const float impf[3] = {imp.x, imp.y, imp.z};
            PhysicsWorld::ReleaseBody(/*launch=*/true, impf, Settings.BackspinRevPerSec * 6.2831853f);
            m_Charging = false;
            m_Charge = 0.0f;
            m_HaveHoldPoint = false;
        } else if (!rmb && m_RmbPrev) {
            const float zero[3] = {0.0f, 0.0f, 0.0f};
            PhysicsWorld::ReleaseBody(/*launch=*/false, zero);
            m_Charging = false;
            m_Charge = 0.0f;
            m_HaveHoldPoint = false;
        } else if (m_Charging) {
            PredictThrow(fwd, speed);
        }
    } else {
        m_Charging = false;
        m_Charge = 0.0f;
        m_HaveHoldPoint = false;
        if (lmb && !m_LmbPrev) {
            RaycastHit hit;
            if (PhysicsWorld::Raycast(of, df, 100.0f, hit) && hit.Entity != kNoEntity) {
                const float f[3] = {fwd.x * 6.0f, fwd.y * 6.0f + 2.0f, fwd.z * 6.0f};
                PhysicsWorld::AddForceAtPosition(hit.Entity, f, hit.Point, /*Impulse*/ 1u);
            }
        }
    }

    m_LmbPrev = lmb;
    m_RmbPrev = rmb;
}
