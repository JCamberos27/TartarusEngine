#pragma once
#include <glm/glm.hpp>

constexpr int MAX_BONE_INFLUENCE = 4;

struct ModelVertex {
    glm::vec3 Position{0.0f};
    glm::vec3 Normal{0.0f};
    glm::vec2 UV{0.0f};
    glm::vec3 Tangent{0.0f};
    float TangentSign = 1.0f; // +1/-1: handedness, needed to get bitangent right on mirrored UV islands
    int BoneIDs[MAX_BONE_INFLUENCE] = {-1, -1, -1, -1};
    float Weights[MAX_BONE_INFLUENCE] = {0, 0, 0, 0};
};
