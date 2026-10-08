#include "GravityGun.h"
#include "Scripting/ScriptRuntime.h"
#include <stdexcept>

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

glm::vec3 Vec(const float v[3]) { return {v[0], v[1], v[2]}; }

} // namespace

bool GravityGun::IsHolding() const { return PhysicsWorld::IsGrabbing(); }

void GravityGun::Run(int operation) {
    m_Frame.Operation=operation;
    if(!Scripting::InvokeProject("gravity",&m_Frame,sizeof m_Frame))throw std::runtime_error("Project gravity ability unavailable");
}
void GravityGun::Reset() { Run(0);m_Prediction.Clear(); }
float GravityGun::AssistReach(const GravityGunSettings& settings,float hitDistance) {
    Scripting::GravityFrame frame;frame.Operation=2;frame.AssistRange=settings.AssistRange;frame.Scroll=hitDistance;
    if(!Scripting::InvokeProject("gravity",&frame,sizeof frame))throw std::runtime_error("Project gravity ability unavailable");
    return frame.HoldDistance;
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

void GravityGun::Update(float dt,const Player& player) {
    auto pack=[](const glm::vec3& v){return Scripting::Vec3{v.x,v.y,v.z};};
    m_Frame.Dt=dt;m_Frame.Eye=pack(player.Cam.Position);m_Frame.Forward=pack(player.Cam.Front());m_Frame.CameraRight=pack(player.Cam.Right());
    m_Frame.Left=Input::IsMouseButtonDown(0);m_Frame.Right=Input::IsMouseButtonDown(1);m_Frame.Scroll=static_cast<float>(Input::GetScrollDeltaY());
    m_Frame.Alt=Input::IsKeyDown(GLFW_KEY_LEFT_ALT)||Input::IsKeyDown(GLFW_KEY_RIGHT_ALT);
    m_Frame.Control=Input::IsKeyDown(GLFW_KEY_LEFT_CONTROL)||Input::IsKeyDown(GLFW_KEY_RIGHT_CONTROL);
    m_Frame.Yaw=glm::radians(player.Cam.Yaw);
    m_Frame.GrabRange=Settings.GrabRange;m_Frame.AssistRange=Settings.AssistRange;m_Frame.AssistConeDeg=Settings.AssistConeDeg;m_Frame.ScrollTurnDeg=Settings.ScrollTurnDeg;
    m_Frame.MinThrowSpeed=Settings.MinThrowSpeed;m_Frame.MaxThrowSpeed=Settings.MaxThrowSpeed;m_Frame.ChargeTime=Settings.ChargeTime;m_Frame.Backspin=Settings.BackspinRevPerSec;
    Run(1);m_Prediction.Clear();
    if(m_Frame.PredictSpeed>0)PredictThrow(player.Cam.Front(),m_Frame.PredictSpeed);
}
