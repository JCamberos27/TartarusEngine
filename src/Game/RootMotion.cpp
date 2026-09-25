#include "RootMotion.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace {

glm::mat4 RotY(float yaw) { return glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }

// A frame-to-frame rigid delta back out of its matrix (rotation about +Y plus translation).
RootMotionDelta FromMatrix(const glm::mat4& m) {
    RootMotionDelta d;
    d.Translation = glm::vec3(m[3]);
    d.Yaw = RootMotionYaw(glm::quat_cast(glm::mat3(m)));
    return d;
}

RootMotionDelta Inverse(const RootMotionDelta& d) {
    RootMotionDelta r;
    r.Yaw = -d.Yaw;
    r.Translation = -glm::vec3(RotY(-d.Yaw) * glm::vec4(d.Translation, 0.0f));
    return r;
}

} // namespace

glm::mat4 RootMotionDelta::Matrix() const {
    return glm::translate(glm::mat4(1.0f), Translation) * RotY(Yaw);
}

RootMotionDelta RootMotionDelta::Then(const RootMotionDelta& next) const {
    RootMotionDelta r;
    r.Translation = Translation + glm::vec3(RotY(Yaw) * glm::vec4(next.Translation, 0.0f));
    r.Yaw = Yaw + next.Yaw;
    return r;
}

void RootMotionMix::Add(const RootMotionDelta& d, float w) {
    if (!(w > 0.0f)) return;
    Translation += d.Translation * w;
    Yaw += d.Yaw * w;
    Weight += w;
}

RootMotionDelta RootMotionMix::Result() const {
    RootMotionDelta d;
    if (Weight > 1e-6f) {
        d.Translation = Translation / Weight;
        d.Yaw = Yaw / Weight;
    }
    return d;
}

float RootMotionYaw(const glm::quat& q) {
    // Twist about +Y: the rotation's (w, y) part. A root pitched a full 90 degrees has no
    // defined heading; that case reads as 0.
    const float len = std::sqrt(q.w * q.w + q.y * q.y);
    if (len < 1e-6f) return 0.0f;
    return 2.0f * std::atan2(q.y / len, q.w / len);
}

glm::mat4 RootMotionFrame(const glm::mat4& rootModel, const RootMotionSettings& s) {
    glm::vec3 p(rootModel[3]);
    if (!s.Vertical) p.y = 0.0f;
    glm::mat4 f = glm::translate(glm::mat4(1.0f), p);
    if (s.Rotation) {
        // Heading from the root's rotation alone, with its scale divided out.
        glm::mat3 r(rootModel);
        for (int c = 0; c < 3; ++c) {
            const float len = glm::length(r[c]);
            if (len > 1e-8f) r[c] /= len;
        }
        f = f * RotY(RootMotionYaw(glm::quat_cast(r)));
    }
    return f;
}

glm::mat4 RootMotionInPlace(const glm::mat4& rootAtT, const glm::mat4& rootAtStart, const RootMotionSettings& s) {
    const glm::mat4 f0 = RootMotionFrame(rootAtStart, s);
    const glm::mat4 ft = RootMotionFrame(rootAtT, s);
    return f0 * glm::inverse(ft) * rootAtT;
}

RootMotionDelta RootMotionBetween(const std::function<glm::mat4(float)>& sampleRoot, float t0, float t1,
                                  float length, bool loop, const RootMotionSettings& s) {
    if (!sampleRoot || !(length > 1e-5f) || t0 == t1) return {};
    if (t1 < t0) return Inverse(RootMotionBetween(sampleRoot, t1, t0, length, loop, s));

    const glm::mat4 f0 = RootMotionFrame(sampleRoot(0.0f), s);
    const glm::mat4 f0Inv = glm::inverse(f0);
    // One stretch inside a single pass: inverse(MT(a)) * MT(b) = F0 inv(F(a)) F(b) inv(F0).
    auto span = [&](float a, float b) {
        const glm::mat4 fa = RootMotionFrame(sampleRoot(a), s);
        const glm::mat4 fb = RootMotionFrame(sampleRoot(b), s);
        return FromMatrix(f0 * glm::inverse(fa) * fb * f0Inv);
    };

    if (!loop) {
        const float a = std::clamp(t0, 0.0f, length), b = std::clamp(t1, 0.0f, length);
        return a == b ? RootMotionDelta{} : span(a, b);
    }

    const double pa = std::floor((double)t0 / length), pb = std::floor((double)t1 / length);
    const float a = t0 - (float)(pa * length), b = t1 - (float)(pb * length);
    if (pa == pb) return span(a, b);
    // Across the wrap: to the end of this pass, whole passes, then into the new one.
    RootMotionDelta d = span(a, length);
    const int whole = (int)std::min(pb - pa - 1.0, 64.0); // a hitch can't spin the object forever
    if (whole > 0) {
        const RootMotionDelta pass = span(0.0f, length);
        for (int i = 0; i < whole; ++i) d = d.Then(pass);
    }
    return d.Then(span(0.0f, b));
}
