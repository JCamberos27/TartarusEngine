#include "Inertialization.h"

#include <algorithm>
#include <cmath>

namespace IM = InertializationMath;

glm::quat IM::FromScaledAngleAxis(const glm::vec3& v) {
    const float angle = glm::length(v);
    if (angle < 1e-8f) return glm::normalize(glm::quat(1.0f, 0.5f * v.x, 0.5f * v.y, 0.5f * v.z));
    const glm::vec3 axis = v / angle;
    const float s = std::sin(0.5f * angle);
    return glm::quat(std::cos(0.5f * angle), axis.x * s, axis.y * s, axis.z * s);
}

glm::vec3 IM::ToScaledAngleAxis(const glm::quat& qIn) {
    glm::quat q = glm::normalize(qIn);
    if (q.w < 0.0f) q = -q; // the shortest way round
    const glm::vec3 v(q.x, q.y, q.z);
    const float s = glm::length(v);
    if (s < 1e-8f) return 2.0f * v;
    return v * (2.0f * std::atan2(s, q.w) / s);
}

glm::vec3 IM::AngularVelocity(const glm::quat& a, const glm::quat& b, float dt) {
    if (dt <= 1e-6f) return glm::vec3(0.0f);
    return ToScaledAngleAxis(b * glm::conjugate(a)) / dt;
}

float IM::Smoothstep(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

void Inertialization::Transition(const MotionPose& previous, const MotionPose& last, float dt) {
    const size_t n = last.Rot.size();
    m_Rot = last.Rot;
    m_AngVel.assign(n, glm::vec3(0.0f));
    if (previous.Rot.size() == n && dt > 1e-6f) {
        for (size_t i = 0; i < n; ++i) {
            glm::vec3 w = IM::AngularVelocity(previous.Rot[i], last.Rot[i], dt);
            const float m = glm::length(w);
            if (m > Params.MaxAngularVelocity) w *= Params.MaxAngularVelocity / m;
            m_AngVel[i] = w;
        }
        m_PelvisVel = (last.Pelvis - previous.Pelvis) / dt;
        const float m = glm::length(m_PelvisVel);
        if (m > Params.MaxLinearVelocity) m_PelvisVel *= Params.MaxLinearVelocity / m;
    } else {
        m_PelvisVel = glm::vec3(0.0f);
    }
    m_Pelvis = last.Pelvis;
    m_Time = 0.0f;
    m_Active = Params.BlendTime > 1e-4f;
}

float Inertialization::BlendWeight() const {
    return m_Active ? IM::Smoothstep(m_Time / Params.BlendTime) : 1.0f;
}

void Inertialization::Update(float dt, const MotionPose& target, MotionPose& out) {
    if (!m_Active || m_Rot.size() != target.Rot.size()) {
        m_Active = false;
        out = target;
        return;
    }
    // Extrapolate the source: velocity decays toward zero (exactly integrated: the travel over dt of a velocity
    // decaying as exp(-k t) is v (1 - exp(-k dt)) / k).
    const float k = 0.69314718f / std::max(1e-4f, Params.DecayHalflife);
    const float decay = std::exp(-k * dt);
    const float travel = (1.0f - decay) / k;
    for (size_t i = 0; i < m_Rot.size(); ++i) {
        m_Rot[i] = glm::normalize(IM::FromScaledAngleAxis(m_AngVel[i] * travel) * m_Rot[i]);
        m_AngVel[i] *= decay;
    }
    m_Pelvis += m_PelvisVel * travel;
    m_PelvisVel *= decay;

    m_Time += dt;
    const float a = BlendWeight();
    out.Rot.resize(target.Rot.size());
    for (size_t i = 0; i < m_Rot.size(); ++i) out.Rot[i] = glm::slerp(m_Rot[i], glm::dot(m_Rot[i], target.Rot[i]) < 0.0f ? -target.Rot[i] : target.Rot[i], a);
    out.Pelvis = glm::mix(m_Pelvis, target.Pelvis, a);
    if (m_Time >= Params.BlendTime) m_Active = false;
}
