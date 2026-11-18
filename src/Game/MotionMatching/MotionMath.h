#pragma once

// Small numeric helpers the motion matching bake and runtime share: Gaussian smoothing with mirrored edges, yaw
// unwrapping, and the smallest-three 16-bit quaternion packing the pose stream stores (MotionClipStream).
// Header-only and free of engine dependencies, so the unit tests exercise them on synthetic data.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace MotionMath {

inline constexpr float kPi = 3.14159265358979f;

// Facing angle of a horizontal direction: 0 = +Z, +90 degrees = +X (glm::rotate about +Y by the angle turns +Z into it).
inline float YawOf(const glm::vec3& dir) { return std::atan2(dir.x, dir.z); }
inline glm::vec3 YawDir(float yaw) { return glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)); }

// Wraps an angle into (-pi, pi].
inline float WrapPi(float a) {
    a = std::fmod(a + kPi, 2.0f * kPi);
    if (a < 0.0f) a += 2.0f * kPi;
    return a - kPi;
}

// Removes the 2-pi jumps from a sequence of angles, so it can be smoothed and differentiated.
inline void Unwrap(std::vector<float>& a) {
    for (size_t i = 1; i < a.size(); ++i) a[i] = a[i - 1] + WrapPi(a[i] - a[i - 1]);
}

// `v` padded by `pad` samples each side, point-mirrored through the edge's trend value: v[-k] = 2 E0 - v[k], where E0 is
// where the take's trend (its travel, its turn) stands at the edge, not the edge sample itself.
//   - A line stays a line across the edge, so a take that starts or ends mid-stride or mid-turn keeps its speed and
//     turn rate up to the edge (a plain mirror, v[-k] = v[k], flattens it: the root would slow down at every cut).
//   - The edge is smoothed like the middle: mirrored through the edge sample itself (E0 = v[0]) the smoothed edge would
//     be exactly the raw edge sample, sway and twist and all.
// E0 is the trend line through two consecutive `window`-sample means at the edge (the line through two means of a
// whole stride cancels the stride's sway; a least-squares line over less than a stride picks up the sway's own slope).
template <class T>
std::vector<T> MirrorPad(const std::vector<T>& v, long pad, long window) {
    const long n = (long)v.size();
    std::vector<T> out((size_t)(n + 2 * pad));
    for (long i = 0; i < n; ++i) out[(size_t)(i + pad)] = v[(size_t)i];
    const long w = std::max(1L, std::min(window, n / 2));
    const auto mean = [&](long from) {
        T m = v[(size_t)from] * 0.0f;
        for (long k = 0; k < w; ++k) m = m + v[(size_t)(from + k)] * (1.0f / (float)w);
        return m;
    };
    // Edge trend value: first-window mean, moved back half a window along the slope between the two window means.
    T e0 = v[0], e1 = v[(size_t)(n - 1)];
    if (n >= 2 * w && w >= 2) {
        const T a = mean(0), b = mean(w);
        e0 = a - (b - a) * (0.5f * (float)(w - 1) / (float)w);
        const T c = mean(n - w), d = mean(n - 2 * w);
        e1 = c + (c - d) * (0.5f * (float)(w - 1) / (float)w);
    }
    for (long k = 1; k <= pad; ++k) {
        out[(size_t)(pad - k)] = e0 * 2.0f - v[(size_t)std::min(k, n - 1)];
        out[(size_t)(pad + n - 1 + k)] = e1 * 2.0f - v[(size_t)std::max(0L, n - 1 - k)];
    }
    return out;
}

// Gaussian low-pass of `v` with a standard deviation of `sigmaSamples` (kernel out to 3 sigma), padded by MirrorPad
// with the trend measured over `trendWindow` samples (0: the kernel's width; RootSynth passes one second, a stride).
// sigma <= 0 returns the input.
template <class T>
std::vector<T> GaussianSmooth(const std::vector<T>& v, float sigmaSamples, long trendWindow = 0) {
    if (v.empty() || sigmaSamples <= 1e-4f) return v;
    const long radius = std::max(1L, (long)std::ceil(3.0f * sigmaSamples));
    std::vector<float> w((size_t)(2 * radius + 1));
    float sum = 0.0f;
    for (long k = -radius; k <= radius; ++k) {
        const float x = (float)k / sigmaSamples;
        w[(size_t)(k + radius)] = std::exp(-0.5f * x * x);
        sum += w[(size_t)(k + radius)];
    }
    for (float& x : w) x /= sum;
    const std::vector<T> p = MirrorPad(v, radius, trendWindow > 0 ? trendWindow : 2 * radius + 1);
    std::vector<T> out(v.size());
    for (long i = 0; i < (long)v.size(); ++i) {
        T acc = v[(size_t)i] * 0.0f;
        for (long k = -radius; k <= radius; ++k) acc = acc + p[(size_t)(i + radius + k)] * w[(size_t)(k + radius)];
        out[(size_t)i] = acc;
    }
    return out;
}

// --- Smallest-three quaternion packing (48 bits) ------------------------------------------------------------------
// The largest component is dropped (its sign folded positive: q and -q are one rotation) and rebuilt from the unit
// length; the other three lie in [-1/sqrt2, 1/sqrt2]. Each is stored in 15 bits, plus 16 for the last, with the
// dropped component's index in the low bit of the first two words. Worst-case error is about 1e-4 rad.
struct PackedQuat {
    std::uint16_t W[3] = {0, 0, 0};
};

inline constexpr float kSqrtHalf = 0.70710678118f;

inline PackedQuat PackQuat(glm::quat q) {
    float c[4] = {q.x, q.y, q.z, q.w};
    int big = 0;
    for (int i = 1; i < 4; ++i)
        if (std::fabs(c[i]) > std::fabs(c[big])) big = i;
    const float sign = c[big] < 0.0f ? -1.0f : 1.0f;
    float rest[3];
    for (int i = 0, k = 0; i < 4; ++i)
        if (i != big) rest[k++] = std::clamp(c[i] * sign, -kSqrtHalf, kSqrtHalf);
    const auto quant = [](float x, int bits) {
        const float t = (x + kSqrtHalf) / (2.0f * kSqrtHalf); // 0..1
        const int maxv = (1 << bits) - 1;
        return (std::uint32_t)std::lround(std::clamp(t, 0.0f, 1.0f) * (float)maxv);
    };
    PackedQuat p;
    p.W[0] = (std::uint16_t)((quant(rest[0], 15) << 1) | (std::uint32_t)(big & 1));
    p.W[1] = (std::uint16_t)((quant(rest[1], 15) << 1) | (std::uint32_t)((big >> 1) & 1));
    p.W[2] = (std::uint16_t)quant(rest[2], 16);
    return p;
}

inline glm::quat UnpackQuat(const PackedQuat& p) {
    const int big = (p.W[0] & 1) | ((p.W[1] & 1) << 1);
    const auto dequant = [](std::uint32_t v, int bits) {
        const int maxv = (1 << bits) - 1;
        return (float)v / (float)maxv * (2.0f * kSqrtHalf) - kSqrtHalf;
    };
    const float rest[3] = {dequant(p.W[0] >> 1, 15), dequant(p.W[1] >> 1, 15), dequant(p.W[2], 16)};
    float c[4];
    float sq = 0.0f;
    for (int i = 0, k = 0; i < 4; ++i)
        if (i != big) {
            c[i] = rest[k++];
            sq += c[i] * c[i];
        }
    c[big] = std::sqrt(std::max(0.0f, 1.0f - sq));
    return glm::normalize(glm::quat(c[3], c[0], c[1], c[2]));
}

// --- Smallest-three in 32 bits (the finger tracks) -------------------------------------------------------------------
// The same scheme in one word: the dropped component's index in the top 2 bits, the other three in 10 bits each. Worst-case
// error about 0.14 degrees (a step of 1.4/1023 per component) - under the fingers' 0.3 degree budget, at two thirds of the
// 48-bit size.
inline std::uint32_t PackQuat32(glm::quat q) {
    float c[4] = {q.x, q.y, q.z, q.w};
    int big = 0;
    for (int i = 1; i < 4; ++i)
        if (std::fabs(c[i]) > std::fabs(c[big])) big = i;
    const float sign = c[big] < 0.0f ? -1.0f : 1.0f;
    std::uint32_t out = (std::uint32_t)big << 30;
    for (int i = 0, k = 0; i < 4; ++i) {
        if (i == big) continue;
        const float t = (std::clamp(c[i] * sign, -kSqrtHalf, kSqrtHalf) + kSqrtHalf) / (2.0f * kSqrtHalf);
        out |= (std::uint32_t)std::lround(std::clamp(t, 0.0f, 1.0f) * 1023.0f) << (20 - 10 * k);
        ++k;
    }
    return out;
}

inline glm::quat UnpackQuat32(std::uint32_t p) {
    const int big = (int)(p >> 30);
    float c[4];
    float sq = 0.0f;
    for (int i = 0, k = 0; i < 4; ++i) {
        if (i == big) continue;
        c[i] = (float)((p >> (20 - 10 * k)) & 1023u) / 1023.0f * (2.0f * kSqrtHalf) - kSqrtHalf;
        sq += c[i] * c[i];
        ++k;
    }
    c[big] = std::sqrt(std::max(0.0f, 1.0f - sq));
    return glm::normalize(glm::quat(c[3], c[0], c[1], c[2]));
}

// The angle (radians, in (-pi, pi]) of q's twist about the unit `axis`: the swing-twist decomposition's twist part.
inline float TwistAngle(glm::quat q, const glm::vec3& axis) {
    if (q.w < 0.0f) q = -q;
    return 2.0f * std::atan2(glm::dot(glm::vec3(q.x, q.y, q.z), axis), q.w);
}

// The shortest-arc angle between two rotations (radians). From the relative rotation's axis part, not acos of the dot:
// acos near 1 can't resolve anything under ~7e-4 rad in float.
inline float QuatAngle(const glm::quat& a, const glm::quat& b) {
    const glm::quat d = glm::conjugate(glm::normalize(a)) * glm::normalize(b);
    return 2.0f * std::atan2(glm::length(glm::vec3(d.x, d.y, d.z)), std::fabs(d.w));
}

} // namespace MotionMath
