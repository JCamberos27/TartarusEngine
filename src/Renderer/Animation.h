#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

struct PositionKey { glm::vec3 Value; float TimeTicks; };
struct RotationKey { glm::quat Value; float TimeTicks; };
struct ScaleKey { glm::vec3 Value; float TimeTicks; };

// Unity's WrapMode for a playing clip (#113 / #175). Once plays to the end and then stops (the
// model returns to its bind pose); ClampForever plays to the end and holds the last frame.
enum class AnimationWrapMode { Once = 0, Loop = 1, PingPong = 2, ClampForever = 3 };

// A local transform as translation / rotation / scale, the form clips are sampled and blended in.
struct LocalTRS {
    glm::vec3 T{0.0f};
    glm::quat R{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 S{1.0f};
    glm::mat4 ToMatrix() const;
    static LocalTRS Blend(const LocalTRS& a, const LocalTRS& b, float t); // lerp / slerp
};

// Per-node keyframe track within one animation clip.
struct BoneAnimChannel {
    std::string BoneName;
    std::vector<PositionKey> Positions;
    std::vector<RotationKey> Rotations;
    std::vector<ScaleKey> Scales;

    // Samples at `timeTicks`; a component with no keys keeps `bind`'s value (a channel that only
    // animates rotation leaves the node's authored translation / scale alone).
    LocalTRS Sample(float timeTicks, const LocalTRS& bind) const;
    glm::mat4 Interpolate(float timeTicks) const; // Sample() with identity defaults, as a matrix
};

// The imported node tree, flattened in depth-first order so a node's parent always comes first
// (#113: evaluated as one linear pass instead of a recursive walk with name lookups).
struct AnimNode {
    std::string Name;
    int Parent = -1;            // index into the flat list; -1 for the root
    glm::mat4 BindLocal{1.0f};  // the node's own authored transform
    LocalTRS BindTRS;           // the same, decomposed (for blending with sampled channels)
    int BoneId = -1;            // skinning palette slot, or -1 when no vertex is bound to it
    glm::mat4 BoneOffset{1.0f}; // mesh space -> bone space (the inverse bind matrix)
};

struct AnimationClip {
    std::string Name;
    float DurationTicks = 0.0f;
    float TicksPerSecond = 25.0f;
    // A trim (Model Import Settings > Clip Trims): only [StartTicks, EndTicks] of the source plays; EndTicks <= 0 = to the end.
    float StartTicks = 0.0f;
    float EndTicks = 0.0f;
    std::vector<BoneAnimChannel> Channels;
    // Channel index per AnimNode (-1 = the clip doesn't animate that node), resolved once at
    // import (#113: was a std::map<string> lookup per node per frame).
    std::vector<int> NodeChannel;

    // The ticks that play: the whole clip, or the trimmed range.
    float EffectiveTicks() const {
        const float end = EndTicks > 0.0f ? std::min(EndTicks, DurationTicks) : DurationTicks;
        return std::max(0.0f, end - std::min(StartTicks, end));
    }
    float LengthSeconds() const { return TicksPerSecond > 0.0f ? EffectiveTicks() / TicksPerSecond : 0.0f; }
};

// Clip-local time in ticks for a playback position of `seconds`, under `wrap`.
float WrappedClipTicks(const AnimationClip& clip, float seconds, AnimationWrapMode wrap);
