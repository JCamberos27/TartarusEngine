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

template <typename Keys>
BoneAnimChannel::TrackInfo AnalyseTrack(const Keys& keys) {
    BoneAnimChannel::TrackInfo info;
    info.Count = (std::uint32_t)keys.size();
    if (keys.empty()) return info;
    info.Constant = true;
    for (const auto& k : keys)
        if (k.Value != keys.front().Value) { info.Constant = false; break; }
    const size_t n = keys.size();
    if (n >= 2) {
        const float t0 = keys.front().TimeTicks, step = (keys.back().TimeTicks - t0) / float(n - 1);
        if (step > 0.0f) {
            info.Uniform = true;
            for (size_t i = 0; i < n && info.Uniform; ++i)
                info.Uniform = std::abs(keys[i].TimeTicks - (t0 + step * float(i))) <= step * 1e-3f;
            info.T0 = t0;
            info.InvStep = 1.0f / step;
        }
    }
    return info;
}

// FindKeyIndex's answer, for evenly spaced keys by arithmetic (then nudged one key either way, so rounding
// can't pick a different segment than the search would).
template <typename Keys>
size_t KeyIndex(const Keys& keys, const BoneAnimChannel::TrackInfo& info, float timeTicks) {
    if (!info.Uniform || info.Count != keys.size()) return FindKeyIndex(keys, timeTicks);
    const size_t n = keys.size();
    const float f = (timeTicks - info.T0) * info.InvStep;
    size_t i = f <= 0.0f ? 0 : std::min((size_t)f, n - 1);
    if (i + 1 < n && keys[i + 1].TimeTicks <= timeTicks) ++i;
    else if (i > 0 && keys[i].TimeTicks > timeTicks) --i;
    return i;
}

float Factor(float lastTime, float nextTime, float timeTicks) {
    float delta = nextTime - lastTime;
    if (delta <= 0.0f) return 0.0f;
    return std::clamp((timeTicks - lastTime) / delta, 0.0f, 1.0f);
}

} // namespace

LocalTRS LocalTRS::Blend(const LocalTRS& a, const LocalTRS& b, float t) {
    LocalTRS out;
    out.T = glm::mix(a.T, b.T, t);
    out.R = glm::normalize(glm::slerp(a.R, b.R, t));
    out.S = glm::mix(a.S, b.S, t);
    return out;
}

void BoneAnimChannel::Finalize() {
    PositionInfo = AnalyseTrack(Positions);
    RotationInfo = AnalyseTrack(Rotations);
    ScaleInfo = AnalyseTrack(Scales);
}

LocalTRS BoneAnimChannel::Sample(float timeTicks, const LocalTRS& bind) const {
    LocalTRS out = bind;
    if (!Positions.empty()) {
        if (PositionInfo.Constant && PositionInfo.Count == Positions.size()) out.T = Positions.front().Value;
        else {
            size_t i = KeyIndex(Positions, PositionInfo, timeTicks);
            size_t j = std::min(i + 1, Positions.size() - 1);
            out.T = glm::mix(Positions[i].Value, Positions[j].Value, Factor(Positions[i].TimeTicks, Positions[j].TimeTicks, timeTicks));
        }
    }
    if (!Rotations.empty()) {
        if (RotationInfo.Constant && RotationInfo.Count == Rotations.size()) out.R = glm::normalize(Rotations.front().Value);
        else {
            size_t i = KeyIndex(Rotations, RotationInfo, timeTicks);
            size_t j = std::min(i + 1, Rotations.size() - 1);
            out.R = glm::normalize(glm::slerp(Rotations[i].Value, Rotations[j].Value,
                                              Factor(Rotations[i].TimeTicks, Rotations[j].TimeTicks, timeTicks)));
        }
    }
    if (!Scales.empty()) {
        if (ScaleInfo.Constant && ScaleInfo.Count == Scales.size()) out.S = Scales.front().Value;
        else {
            size_t i = KeyIndex(Scales, ScaleInfo, timeTicks);
            size_t j = std::min(i + 1, Scales.size() - 1);
            out.S = glm::mix(Scales[i].Value, Scales[j].Value, Factor(Scales[i].TimeTicks, Scales[j].TimeTicks, timeTicks));
        }
    }
    return out;
}

glm::mat4 BoneAnimChannel::Interpolate(float timeTicks) const {
    return Sample(timeTicks, LocalTRS{}).ToMatrix();
}

float WrappedClipTicks(const AnimationClip& clip, float seconds, AnimationWrapMode wrap) {
    const float d = clip.EffectiveTicks();
    if (d <= 0.0f) return 0.0f;
    const float t = seconds * clip.TicksPerSecond;
    const float from = clip.StartTicks; // 0 unless trimmed
    switch (wrap) {
    case AnimationWrapMode::Loop: {
        float m = std::fmod(t, d);
        return from + (m < 0.0f ? m + d : m);
    }
    case AnimationWrapMode::PingPong: {
        float m = std::fmod(std::fabs(t), 2.0f * d);
        return from + (m > d ? 2.0f * d - m : m);
    }
    case AnimationWrapMode::Once:
    case AnimationWrapMode::ClampForever:
    default:
        return from + std::clamp(t, 0.0f, d);
    }
}
