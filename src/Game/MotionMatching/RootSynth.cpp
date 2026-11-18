#include "RootSynth.h"

#include "MotionMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace RootSynth {

glm::vec3 ForwardFromRight(const glm::vec3& right) {
    glm::vec3 f = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), right);
    f.y = 0.0f;
    const float l = glm::length(f);
    return l > 1e-6f ? f / l : glm::vec3(0.0f);
}

std::vector<float> RawYaw(const RootSynthInput& in, float hipsWeight) {
    const size_t n = in.Pelvis.size();
    std::vector<float> yaw(n, 0.0f);
    const bool shoulders = in.ShoulderRight.size() == n;
    const float wh = shoulders ? std::clamp(hipsWeight, 0.0f, 1.0f) : 1.0f;
    float last = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        glm::vec3 f = wh * ForwardFromRight(i < in.HipsRight.size() ? in.HipsRight[i] : glm::vec3(1, 0, 0));
        if (shoulders) f += (1.0f - wh) * ForwardFromRight(in.ShoulderRight[i]);
        // Hips and shoulders turned more than 180 degrees apart cancel out: keep the last facing.
        yaw[i] = glm::length(f) > 1e-4f ? MotionMath::YawOf(f) : last;
        last = yaw[i];
    }
    MotionMath::Unwrap(yaw);
    return yaw;
}

RootTrack Generate(const RootSynthInput& in, const RootSynthParams& p) {
    RootTrack out;
    const size_t n = in.Pelvis.size();
    if (n == 0) return out;
    std::vector<glm::vec3> ground(n);
    for (size_t i = 0; i < n; ++i) ground[i] = glm::vec3(in.Pelvis[i].x, 0.0f, in.Pelvis[i].z);
    const long stride = std::max(2L, (long)std::lround(in.Fps)); // the edges' trend over a second: a stride
    out.Position = MotionMath::GaussianSmooth(ground, p.PosSigma * in.Fps, stride);
    for (glm::vec3& v : out.Position) v.y = 0.0f;
    out.Yaw = MotionMath::GaussianSmooth(RawYaw(in, p.HipsWeight), p.YawSigma * in.Fps, stride);
    return out;
}

glm::mat4 RootMatrix(const glm::vec3& position, float yaw, float restYaw) {
    return glm::rotate(glm::translate(glm::mat4(1.0f), position), yaw - restYaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

std::vector<char> Contacts(const std::vector<glm::vec3>& foot, float fps, float maxHeight, float maxSpeed) {
    const size_t n = foot.size();
    std::vector<char> c(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const size_t a = i > 0 ? i - 1 : i, b = i + 1 < n ? i + 1 : i;
        glm::vec3 d = foot[b] - foot[a];
        d.y = 0.0f;
        const float speed = b > a ? glm::length(d) * fps / (float)(b - a) : 0.0f;
        c[i] = foot[i].y < maxHeight && speed < maxSpeed ? 1 : 0;
    }
    // Fill gaps of up to 2 frames between contacts, then drop contacts of up to 2 frames.
    const auto runs = [&](char value, char fill) {
        size_t i = 0;
        while (i < n) {
            if (c[i] != value) { ++i; continue; }
            size_t j = i;
            while (j < n && c[j] == value) ++j;
            const bool inner = i > 0 && j < n;
            if (j - i <= 2 && (inner || value == 1)) std::fill(c.begin() + (long)i, c.begin() + (long)j, fill);
            i = j;
        }
    };
    runs(0, 1);
    runs(1, 0);
    return c;
}

RootValidation Validate(const RootTrack& root, float fps, const std::vector<glm::vec3>& hipsRight,
                        const std::vector<glm::vec3>* feet, const std::vector<char>* contacts) {
    RootValidation v;
    const size_t n = root.Size();
    if (n < 3) return v;
    constexpr float kDeg = 180.0f / MotionMath::kPi;
    // Jitter: the yaw rate's high-frequency part. A steady turn (turn in place, a circle) has a constant rate and a
    // smooth start or stop changes it slowly; a stride-by-stride wobble changes it every step. The rate minus its own
    // 0.25 s Gaussian average leaves the wobble; its RMS per second is the jitter (deg/s^2).
    std::vector<float> rate(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t a = i > 0 ? i - 1 : i, b = i + 1 < n ? i + 1 : i;
        rate[i] = (root.Yaw[b] - root.Yaw[a]) * fps / (float)(b - a);
        v.YawRateMax = std::max(v.YawRateMax, std::fabs(rate[i]) * kDeg);
    }
    const std::vector<float> slow = MotionMath::GaussianSmooth(rate, 0.25f * fps);
    double sq = 0.0;
    for (size_t i = 0; i < n; ++i) sq += (double)(rate[i] - slow[i]) * (rate[i] - slow[i]);
    // A wobble of amplitude A at frequency w has a rate error of ~A w and a yaw acceleration of ~A w^2: scale by the
    // stride frequency (~2 Hz, 4 pi rad/s) so the figure reads as an acceleration.
    v.YawJitter = (float)std::sqrt(sq / (double)n) * 4.0f * MotionMath::kPi * kDeg;

    {
        // A band-pass around the stride (~1 Hz): the path smoothed over 0.08 s minus the path smoothed over 0.3 s. Slow
        // curves and turnarounds cancel out; the side-to-side sway left in the root doesn't.
        const std::vector<glm::vec3> fine = MotionMath::GaussianSmooth(root.Position, 0.08f * fps);
        const std::vector<glm::vec3> avg = MotionMath::GaussianSmooth(root.Position, 0.3f * fps);
        double w = 0.0;
        for (size_t i = 0; i < n; ++i) {
            glm::vec3 d = fine[i] - avg[i];
            d.y = 0.0f;
            w += (double)glm::dot(d, d);
        }
        v.PathWobble = (float)std::sqrt(w / (double)n) * 100.0f;
    }

    std::vector<float> facing;
    for (size_t i = 0; i < n; ++i) {
        const size_t a = i > 0 ? i - 1 : i, b = i + 1 < n ? i + 1 : i;
        const float speed = glm::length(root.Position[b] - root.Position[a]) * fps / (float)(b - a);
        v.SpeedMax = std::max(v.SpeedMax, speed);
        if (i > 0) v.StepMax = std::max(v.StepMax, glm::length(root.Position[i] - root.Position[i - 1]));
        if (speed < 0.5f || i >= hipsRight.size()) continue;
        const glm::vec3 hf = ForwardFromRight(hipsRight[i]);
        if (glm::length(hf) < 0.5f) continue;
        facing.push_back(std::fabs(MotionMath::WrapPi(MotionMath::YawOf(hf) - root.Yaw[i])) * kDeg);
    }
    if (!facing.empty()) {
        std::sort(facing.begin(), facing.end());
        v.FacingP95 = facing[std::min(facing.size() - 1, (size_t)(0.95 * (double)facing.size()))];
        v.FacingMax = facing.back();
    }

    if (feet && contacts)
        for (int k = 0; k < 2; ++k) {
            const auto& f = feet[k];
            const auto& c = contacts[k];
            double dist = 0.0;
            int frames = 0;
            for (size_t i = 1; i < std::min(f.size(), c.size()); ++i)
                if (c[i] && c[i - 1]) {
                    glm::vec3 d = f[i] - f[i - 1];
                    d.y = 0.0f;
                    dist += glm::length(d);
                    ++frames;
                }
            v.ContactFrames[k] = frames;
            v.FootSlide[k] = frames ? (float)(dist / (double)frames) * fps * 100.0f : 0.0f;
        }
    return v;
}

} // namespace RootSynth
