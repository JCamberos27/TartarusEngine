#pragma once
#include <string>
#include <vector>
#include <map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

struct AiSceneWrapper; // fwd, not used outside Model.cpp

struct PositionKey { glm::vec3 Value; float TimeTicks; };
struct RotationKey { glm::quat Value; float TimeTicks; };
struct ScaleKey { glm::vec3 Value; float TimeTicks; };

// Per-bone keyframe track within one animation clip.
struct BoneAnimChannel {
    std::string BoneName;
    std::vector<PositionKey> Positions;
    std::vector<RotationKey> Rotations;
    std::vector<ScaleKey> Scales;

    glm::mat4 Interpolate(float timeTicks) const;
};

// Mirrors the imported scene's node tree (needed to walk hierarchy during animation).
struct AssimpNodeData {
    glm::mat4 Transform{1.0f};
    std::string Name;
    std::vector<AssimpNodeData> Children;
};

struct AnimationClip {
    std::string Name;
    float DurationTicks = 0.0f;
    float TicksPerSecond = 25.0f;
    std::map<std::string, BoneAnimChannel> Channels; // keyed by bone name
};
