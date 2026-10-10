#pragma once
#include <glm/glm.hpp>

// Up to eight bones skin a vertex: the Quantum clothing is weighted to as many as 15, and at four a shoulder or a
// hood lost up to 40% of its weight (the rest renormalized onto too few bones). Eight keeps all but a sliver.
constexpr int MAX_BONE_INFLUENCE = 8;

struct ModelVertex {
    glm::vec3 Position{0.0f};
    glm::vec3 Normal{0.0f};
    glm::vec2 UV{0.0f};
    glm::vec3 Tangent{0.0f};
    float TangentSign = 1.0f; // +1/-1: handedness, needed to get bitangent right on mirrored UV islands
    int BoneIDs[MAX_BONE_INFLUENCE] = {-1, -1, -1, -1, -1, -1, -1, -1};
    float Weights[MAX_BONE_INFLUENCE] = {0, 0, 0, 0, 0, 0, 0, 0};
    glm::vec4 Color{1.0f}; // #113 — vertex colour (shader location 7); white when the mesh has none
};
