#include "Animation.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace {

// Index of the key segment containing timeTicks (the last key with TimeTicks <= timeTicks).
// Keys are sorted by time, so this is a binary search rather than the old per-call linear scan
// over the whole track — which ran 3x per bone per frame (#111).
template <typename Keys>
size_t FindKeyIndex(const Keys& keys, float timeTicks) {
    if (keys.size() <= 1) return 0;
    auto it = std::upper_bound(keys.begin(), keys.end(), timeTicks,
        [](float t, const auto& k) { return t < k.TimeTicks; });
    return it == keys.begin() ? 0 : size_t((it - keys.begin()) - 1);
}

float Factor(float lastTime, float nextTime, float timeTicks) {
    float delta = nextTime - lastTime;
    if (delta <= 0.0f) return 0.0f;
    return std::clamp((timeTicks - lastTime) / delta, 0.0f, 1.0f);
}

} // namespace

glm::mat4 LocalTRS::ToMatrix() const {
    return glm::translate(glm::mat4(1.0f), T) * glm::mat4_cast(R) * glm::scale(glm::mat4(1.0f), S);
}

LocalTRS LocalTRS::Blend(const LocalTRS& a, const LocalTRS& b, float t) {
    LocalTRS out;
    out.T = glm::mix(a.T, b.T, t);
    out.R = glm::normalize(glm::slerp(a.R, b.R, t));
    out.S = glm::mix(a.S, b.S, t);
    return out;
}

LocalTRS BoneAnimChannel::Sample(float timeTicks, const LocalTRS& bind) const {
    LocalTRS out = bind;
    if (!Positions.empty()) {
        size_t i = FindKeyIndex(Positions, timeTicks);
        size_t j = std::min(i + 1, Positions.size() - 1);
        out.T = glm::mix(Positions[i].Value, Positions[j].Value, Factor(Positions[i].TimeTicks, Positions[j].TimeTicks, timeTicks));
    }
    if (!Rotations.empty()) {
        size_t i = FindKeyIndex(Rotations, timeTicks);
        size_t j = std::min(i + 1, Rotations.size() - 1);
        out.R = glm::normalize(glm::slerp(Rotations[i].Value, Rotations[j].Value,
                                          Factor(Rotations[i].TimeTicks, Rotations[j].TimeTicks, timeTicks)));
    }
    if (!Scales.empty()) {
        size_t i = FindKeyIndex(Scales, timeTicks);
        size_t j = std::min(i + 1, Scales.size() - 1);
        out.S = glm::mix(Scales[i].Value, Scales[j].Value, Factor(Scales[i].TimeTicks, Scales[j].TimeTicks, timeTicks));
    }
    return out;
}

glm::mat4 BoneAnimChannel::Interpolate(float timeTicks) const {
    return Sample(timeTicks, LocalTRS{}).ToMatrix();
}

float WrappedClipTicks(const AnimationClip& clip, float seconds, AnimationWrapMode wrap) {
    const float d = clip.DurationTicks;
    if (d <= 0.0f) return 0.0f;
    const float t = seconds * clip.TicksPerSecond;
    switch (wrap) {
    case AnimationWrapMode::Loop: {
        float m = std::fmod(t, d);
        return m < 0.0f ? m + d : m;
    }
    case AnimationWrapMode::PingPong: {
        float m = std::fmod(std::fabs(t), 2.0f * d);
        return m > d ? 2.0f * d - m : m;
    }
    case AnimationWrapMode::Once:
    case AnimationWrapMode::ClampForever:
    default:
        return std::clamp(t, 0.0f, d);
    }
}
