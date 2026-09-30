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
    // translate(T) * mat4_cast(R) * scale(S), built directly: the terms the three-matrix product
    // adds are exact zeros, so the values are the same, at a fraction of the work. Every node of
    // every posed skeleton goes through this, several times a frame.
    glm::mat4 ToMatrix() const {
        const glm::mat3 r = glm::mat3_cast(R);
        return glm::mat4(glm::vec4(r[0] * S.x, 0.0f), glm::vec4(r[1] * S.y, 0.0f), glm::vec4(r[2] * S.z, 0.0f), glm::vec4(T, 1.0f));
    }
    static LocalTRS Blend(const LocalTRS& a, const LocalTRS& b, float t); // lerp / slerp
};

// a * b for two affine transforms (bottom row 0 0 0 1), such as a node's parent global and its
// ToMatrix() local. glm's full product only adds the exact-zero terms this skips, so the result is
// the same; the pose hot loops (each node, each skeleton, each IK pass) use it.
inline glm::mat4 AffineMul(const glm::mat4& a, const glm::mat4& b) {
    glm::mat4 r;
    for (int c = 0; c < 3; ++c) r[c] = a[0] * b[c][0] + a[1] * b[c][1] + a[2] * b[c][2];
    r[3] = a[0] * b[3][0] + a[1] * b[3][1] + a[2] * b[3][2] + a[3];
    return r;
}

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
