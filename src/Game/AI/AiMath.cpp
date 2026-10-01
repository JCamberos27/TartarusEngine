#include "AiMath.h"

#include <algorithm>
#include <cmath>

float Smoothstep01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

float AiFollow(float dt, float timeConstant) {
    return timeConstant > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / timeConstant) : 1.0f;
}

// --- Perception ---------------------------------------------------------------------------------

float DetectionRate(const DetectionInput& in, const PerceptionSettings& s) {
    if (in.VisiblePoints <= 0 || in.Distance > s.Range || in.AngleDeg > s.PeripheralHalfAngle) return 0.0f;
    // Distance: full to 8 m, then down to ~6% at the range limit.
    const float dn = std::clamp((in.Distance - 8.0f) / std::max(s.Range - 8.0f, 1.0f), 0.0f, 1.0f);
    const float distance = 1.0f - 0.94f * std::sqrt(dn);
    // Angle: sharp inside the focal cone, falling across the periphery to 15% at its edge.
    float angle = 1.0f;
    if (in.AngleDeg > s.FocalHalfAngle) {
        const float t = (in.AngleDeg - s.FocalHalfAngle) / std::max(s.PeripheralHalfAngle - s.FocalHalfAngle, 1.0f);
        angle = 0.45f - 0.30f * std::clamp(t, 0.0f, 1.0f);
    }
    const float visible = 0.3f + 0.7f * std::clamp(in.VisiblePoints / 5.0f, 0.0f, 1.0f);
    float stance = in.TargetCrouched ? 0.6f : 1.0f;
    // Movement catches the eye; standing still in the open less so.
    const float motion = 0.8f + 0.25f * std::clamp(in.TargetSpeed, 0.0f, 5.0f);
    const float firing = in.TargetFiring ? 4.0f : 1.0f;
    const float suppression = 1.0f - 0.5f * std::clamp(in.Suppression, 0.0f, 1.0f);
    const float alert = 1.0f + 1.5f * std::clamp(in.Alertness, 0.0f, 1.0f);
    if (in.TargetFiring) stance = 1.0f;
    return s.BaseRate * distance * angle * visible * stance * motion * firing * suppression * alert;
}

glm::vec3 TargetMemory::Predicted(float now) const {
    // Carry the last movement on for at most a second and a half: past that it's a guess.
    const float t = std::clamp(now - LastSeen, 0.0f, 1.5f);
    return LastKnown + glm::vec3(LastVelocity.x, 0.0f, LastVelocity.z) * t;
}

bool UpdateMemory(TargetMemory& m, bool visible, const glm::vec3& seenPos, const glm::vec3& vel, float rate, float now, float dt) {
    m.Visible = visible && rate > 0.0f;
    bool becameKnown = false;
    if (m.Visible) {
        m.Awareness = std::min(1.0f, m.Awareness + rate * dt);
        if (m.Awareness >= 1.0f && !m.Known) { m.Known = true; becameKnown = true; }
        // While only suspicious it has a rough idea where; once known, exactly.
        if (m.Known || m.Awareness > 0.35f) {
            m.LastKnown = seenPos;
            m.LastVelocity = vel;
            m.LastSeen = now;
            m.Uncertainty = m.Known ? 0.0f : 2.0f * (1.0f - m.Awareness);
        }
    } else {
        if (!m.Known) m.Awareness = std::max(0.0f, m.Awareness - 0.12f * dt);
        else if (m.LastSeen > -1e8f) m.Uncertainty = std::min(14.0f, m.Uncertainty + 1.3f * dt);
    }
    return becameKnown;
}

float HearNoise(TargetMemory& m, const glm::vec3& listener, const glm::vec3& pos, float radius, float loudness, float now) {
    const float d = glm::length(pos - listener);
    if (radius <= 0.0f || d > radius) return 0.0f;
    const float k = 1.0f - d / radius;
    const float add = loudness * (0.35f + 0.65f * k);
    const float before = m.Awareness;
    m.Awareness = std::min(1.0f, m.Awareness + add);
    if (m.Awareness >= 1.0f) m.Known = true;
    m.LastHeard = now;
    // A sound only gives a direction and a rough distance: the further, the vaguer.
    const float vague = 0.5f + d * 0.12f;
    if (now - m.LastSeen > 1.0f || m.Uncertainty > vague) {
        m.LastKnown = pos;
        m.LastVelocity = glm::vec3(0.0f);
        m.Uncertainty = vague;
        if (!m.Known) m.LastSeen = std::max(m.LastSeen, now - 0.01f);
    }
    return m.Awareness - before;
}

// --- Accuracy -----------------------------------------------------------------------------------

float HitProbability(const AccuracyInput& in) {
    const float skill = std::clamp(in.Skill, 0.0f, 1.0f);
    float p = 0.42f + 0.38f * skill;
    // Range: rifles hold accuracy out to ~40 m, shotgun lines (each pellet) fall off fast.
    const float scale = in.Weapon == WeaponClass::Shotgun ? 16.0f : 48.0f;
    p *= std::exp(-std::max(in.Distance - 6.0f, 0.0f) / scale);
    // Settling on the target: a third of full accuracy at once, full after ~1.4 s.
    p *= 0.3f + 0.7f * Smoothstep01(in.TimeOnTarget / (1.4f - 0.5f * skill));
    p *= 1.0f / (1.0f + 0.16f * std::max(in.TargetSpeed, 0.0f));
    p *= 1.0f / (1.0f + 0.35f * std::max(in.SelfSpeed, 0.0f));
    p *= 1.0f - 0.65f * std::clamp(in.Suppression, 0.0f, 1.0f);
    p *= 0.35f + 0.65f * std::clamp(in.VisibleFraction, 0.0f, 1.0f);
    if (in.TargetCrouched) p *= 0.85f;
    if (in.OutsideTargetView) p *= 0.5f;
    if (in.Flinching) p *= 0.35f;
    p *= std::clamp(in.Difficulty, 0.25f, 2.0f);
    return std::clamp(p, 0.0f, 0.85f);
}

float ReactionTime(float skill, float difficulty, bool peripheral, float r01) {
    const float s = std::clamp(skill, 0.0f, 1.0f);
    float t = 0.42f - 0.2f * s + 0.18f * std::clamp(r01, 0.0f, 1.0f);
    if (peripheral) t += 0.18f;
    return t / std::clamp(difficulty, 0.25f, 2.0f);
}

// --- Utility ------------------------------------------------------------------------------------

float UtilityCurve::Eval(float x) const {
    x = std::clamp(x, 0.0f, 1.0f);
    float y = 0.0f;
    switch (K) {
    case Linear: y = M * x + B; break;
    case Quadratic: y = M * std::pow(x, Kk) + B; break;
    case Logistic: y = 1.0f / (1.0f + std::exp(-Kk * (x - C))) * M + B; break;
    case Bell: {
        const float d = (x - C) / std::max(Kk, 1e-4f);
        y = std::exp(-d * d) * M + B;
        break;
    }
    }
    return std::clamp(y, 0.0f, 1.0f);
}

float CombineScores(const float* scores, int count) {
    if (count <= 0) return 0.0f;
    float product = 1.0f;
    for (int i = 0; i < count; ++i) product *= std::clamp(scores[i], 0.0f, 1.0f);
    if (count == 1) return product;
    // Compensation: each factor's shortfall is partly made up, more so the more factors there are.
    const float mod = 1.0f - 1.0f / (float)count;
    float out = 1.0f;
    for (int i = 0; i < count; ++i) {
        const float s = std::clamp(scores[i], 0.0f, 1.0f);
        out *= s + (1.0f - s) * mod * s;
    }
    return std::clamp(out, 0.0f, 1.0f);
}
