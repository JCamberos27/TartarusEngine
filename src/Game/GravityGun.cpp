#include "GravityGun.h"

#include "Player.h"
#include "Input.h"
#include "PhysicsWorld.h"
#include "GameModuleAPI.h" // RaycastHit, BodyState

#include <glm/glm.hpp>
#include <algorithm>

bool GravityGun::IsHolding() const { return PhysicsWorld::IsGrabbing(); }

void GravityGun::Reset() {
    m_LmbPrev = m_RmbPrev = false;
    m_Charging = false;
    m_Charge = 0.0f;
}

void GravityGun::Update(float dt, const Player& player) {
    const glm::vec3 eye = player.Cam.Position, fwd = player.Cam.Front();
    const float of[3] = {eye.x, eye.y, eye.z}, df[3] = {fwd.x, fwd.y, fwd.z};
    const bool lmb = Input::IsMouseButtonDown(0);
    const bool rmb = Input::IsMouseButtonDown(1);

    if (rmb && !m_RmbPrev && !PhysicsWorld::IsGrabbing()) {
        RaycastHit hit;
        BodyState bs;
        if (PhysicsWorld::Raycast(of, df, 100.0f, hit) && hit.Entity != 0xFFFFFFFFu &&
            PhysicsWorld::GetBodyState(hit.Entity, bs) && !bs.Kinematic) {
            PhysicsWorld::GrabBody(hit.Entity);
            m_HoldDistance = glm::clamp(hit.Distance, 1.5f, 8.0f);
            m_Charging = false;
        }
    }

    if (PhysicsWorld::IsGrabbing()) {
        const double scroll = Input::GetScrollDeltaY();
        if (scroll != 0.0)
            m_HoldDistance = glm::clamp(m_HoldDistance + (float)scroll * 0.6f, 1.5f, 8.0f);
        const glm::vec3 t = eye + fwd * m_HoldDistance;
        const float tf[3] = {t.x, t.y, t.z};
        PhysicsWorld::UpdateGrab(tf);

        // Left mouse charges while held and throws on release: a tap lobs, a full charge fires.
        if (lmb && !m_LmbPrev) { m_Charging = true; m_Charge = 0.0f; }
        if (m_Charging && lmb)
            m_Charge = std::min(1.0f, m_Charge + dt / std::max(Settings.ChargeTime, 0.01f));
        if (m_Charging && !lmb) {
            const float speed = Settings.MinThrowSpeed + (Settings.MaxThrowSpeed - Settings.MinThrowSpeed) * m_Charge;
            const glm::vec3 imp = fwd * speed;
            const float impf[3] = {imp.x, imp.y, imp.z};
            PhysicsWorld::ReleaseBody(/*launch=*/true, impf, Settings.BackspinRevPerSec * 6.2831853f);
            m_Charging = false;
            m_Charge = 0.0f;
        } else if (!rmb && m_RmbPrev) {
            const float zero[3] = {0.0f, 0.0f, 0.0f};
            PhysicsWorld::ReleaseBody(/*launch=*/false, zero);
            m_Charging = false;
            m_Charge = 0.0f;
        }
    } else {
        m_Charging = false;
        m_Charge = 0.0f;
        if (lmb && !m_LmbPrev) {
            RaycastHit hit;
            if (PhysicsWorld::Raycast(of, df, 100.0f, hit) && hit.Entity != 0xFFFFFFFFu) {
                const float f[3] = {fwd.x * 6.0f, fwd.y * 6.0f + 2.0f, fwd.z * 6.0f};
                PhysicsWorld::AddForceAtPosition(hit.Entity, f, hit.Point, /*Impulse*/ 1u);
            }
        }
    }

    m_LmbPrev = lmb;
    m_RmbPrev = rmb;
}
