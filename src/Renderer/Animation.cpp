#include "Animation.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace {

template <typename Key, typename Value>
size_t FindKeyIndex(const std::vector<Key>& keys, float timeTicks) {
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (timeTicks < keys[i + 1].TimeTicks) return i;
    }
    return keys.empty() ? 0 : keys.size() - 1;
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
        size_t i = FindKeyIndex<PositionKey, glm::vec3>(Positions, timeTicks);
        size_t j = std::min(i + 1, Positions.size() - 1);
        float f = Factor(Positions[i].TimeTicks, Positions[j].TimeTicks, timeTicks);
        pos = glm::mix(Positions[i].Value, Positions[j].Value, f);
    }

    glm::quat rot(1, 0, 0, 0);
    if (!Rotations.empty()) {
        size_t i = FindKeyIndex<RotationKey, glm::quat>(Rotations, timeTicks);
        size_t j = std::min(i + 1, Rotations.size() - 1);
        float f = Factor(Rotations[i].TimeTicks, Rotations[j].TimeTicks, timeTicks);
        rot = glm::slerp(Rotations[i].Value, Rotations[j].Value, f);
        rot = glm::normalize(rot);
    }

    glm::vec3 scale(1.0f);
    if (!Scales.empty()) {
        size_t i = FindKeyIndex<ScaleKey, glm::vec3>(Scales, timeTicks);
        size_t j = std::min(i + 1, Scales.size() - 1);
        float f = Factor(Scales[i].TimeTicks, Scales[j].TimeTicks, timeTicks);
        scale = glm::mix(Scales[i].Value, Scales[j].Value, f);
    }

    glm::mat4 T = glm::translate(glm::mat4(1.0f), pos);
    glm::mat4 R = glm::mat4_cast(rot);
    glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
    return T * R * S;
}
