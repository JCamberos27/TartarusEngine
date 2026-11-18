#include "MotionDebugDraw.h"

#include "MotionFeatures.h"
#include "MotionMath.h"
#include "RootSynth.h"
#include "TrajectoryPredictor.h"

void MotionDebugDraw::Line(std::vector<float>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& c) {
    out.insert(out.end(), {a.x, a.y, a.z, c.r, c.g, c.b, c.a, b.x, b.y, b.z, c.r, c.g, c.b, c.a});
}

void MotionDebugDraw::Cross(std::vector<float>& out, const glm::vec3& at, float s, const glm::vec4& c) {
    Line(out, at - glm::vec3(s, 0, 0), at + glm::vec3(s, 0, 0), c);
    Line(out, at - glm::vec3(0, 0, s), at + glm::vec3(0, 0, s), c);
}

void MotionDebugDraw::Trajectory(std::vector<float>& out, const glm::vec3& charPos, float charYaw, float restYaw,
                                 const float* t, const glm::vec4& c, float lift) {
    const glm::mat4 R = RootSynth::RootMatrix(glm::vec3(charPos.x, 0.0f, charPos.z), charYaw, restYaw);
    const glm::mat3 rot(R);
    const glm::vec3 up(0.0f, charPos.y + lift, 0.0f);
    glm::vec3 prev = glm::vec3(charPos.x, 0.0f, charPos.z) + up;
    for (int k = 0; k < 3; ++k) {
        const glm::vec3 p = glm::vec3(R * glm::vec4(t[2 * k], 0.0f, t[2 * k + 1], 1.0f)) + up;
        glm::vec3 d = rot * glm::vec3(t[6 + 2 * k], 0.0f, t[6 + 2 * k + 1]);
        if (glm::length(d) > 1e-4f) d = glm::normalize(d);
        Line(out, prev, p, c);
        Line(out, p, p + d * 0.35f, c);
        Cross(out, p, 0.05f, c);
        prev = p;
    }
}

void MotionDebugDraw::Predicted(std::vector<float>& out, const TrajectoryState* s, int count, const glm::vec4& c, float lift) {
    for (int k = 0; k < count; ++k) {
        const glm::vec3 p = s[k].Position + glm::vec3(0, lift, 0);
        if (k > 0) Line(out, s[k - 1].Position + glm::vec3(0, lift, 0), p, c);
        Line(out, p, p + MotionMath::YawDir(s[k].Yaw) * 0.25f, c);
    }
}

void MotionDebugDraw::Character(std::vector<float>& out, const glm::vec3& pos, float yaw, const glm::vec4& c, float lift) {
    const glm::vec3 at = pos + glm::vec3(0, lift, 0);
    Cross(out, at, 0.12f, c);
    Line(out, at, at + MotionMath::YawDir(yaw) * 0.7f, c);
}

void MotionDebugDraw::Feet(std::vector<float>& out, const glm::vec3& charPos, float charYaw, float restYaw, const float* f,
                           std::uint8_t flags) {
    const glm::mat4 R = RootSynth::RootMatrix(charPos, charYaw, restYaw);
    for (int k = 0; k < 2; ++k) {
        const glm::vec3 p = glm::vec3(R * glm::vec4(f[3 * k], f[3 * k + 1], f[3 * k + 2], 1.0f));
        const bool planted = flags & (k == 0 ? MotionLabel::ContactL : MotionLabel::ContactR);
        const glm::vec4 c = planted ? glm::vec4(0.2f, 0.5f, 1.0f, 1.0f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.8f);
        Cross(out, p, 0.07f, c);
        Line(out, p, glm::vec3(p.x, charPos.y + 0.02f, p.z), glm::vec4(c.r, c.g, c.b, 0.4f));
    }
}
