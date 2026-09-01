#include "Animation.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

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

glm::mat4 BoneAnimChannel::Interpolate(float timeTicks) const {
    glm::vec3 pos(0.0f);
    if (!Positions.empty()) {
        size_t i = FindKeyIndex(Positions, timeTicks);
        size_t j = std::min(i + 1, Positions.size() - 1);
        float f = Factor(Positions[i].TimeTicks, Positions[j].TimeTicks, timeTicks);
        pos = glm::mix(Positions[i].Value, Positions[j].Value, f);
    }

    glm::quat rot(1, 0, 0, 0);
    if (!Rotations.empty()) {
        size_t i = FindKeyIndex(Rotations, timeTicks);
        size_t j = std::min(i + 1, Rotations.size() - 1);
        float f = Factor(Rotations[i].TimeTicks, Rotations[j].TimeTicks, timeTicks);
        rot = glm::slerp(Rotations[i].Value, Rotations[j].Value, f);
        rot = glm::normalize(rot);
    }

    glm::vec3 scale(1.0f);
    if (!Scales.empty()) {
        size_t i = FindKeyIndex(Scales, timeTicks);
        size_t j = std::min(i + 1, Scales.size() - 1);
        float f = Factor(Scales[i].TimeTicks, Scales[j].TimeTicks, timeTicks);
        scale = glm::mix(Scales[i].Value, Scales[j].Value, f);
    }

    glm::mat4 T = glm::translate(glm::mat4(1.0f), pos);
    glm::mat4 R = glm::mat4_cast(rot);
    glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
    return T * R * S;
}
