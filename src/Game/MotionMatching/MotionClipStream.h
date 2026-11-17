#pragma once

// MotionClipStream: the motion matching pose stream (.mmanim), written by MotionBake (--bake-motion) and played by the
// runtime (Phase 3+) and by `--npc-test mmview`.
//
// One file holds every baked take back to back at one frame rate (60 fps). Per frame:
//   - the generated root (RootSynth): ground position + facing yaw, in the target rig's model space (metres, Y up)
//   - the pelvis translation, relative to the rig's `root` node, re-expressed under the generated root
//   - one smallest-three 16-bit rotation per stream bone (MotionMath::PackQuat), the bone's local rotation
// plus, once for the whole file, a static local rotation for the bones it doesn't stream (the fingers' relaxed pose).
//
// Playing it on a model: Bind() maps the stream's bones to the model's nodes by name; SamplePose() writes a local pose
// (nodes the stream doesn't know keep what the caller put there, normally the bind pose); the model's entity sits at
// placement * RootMatrix(). The pelvis translation is scaled by the model's pelvis height over the source's.
//
// File layout (little endian), version 1:
//   char[4] "TMMA", u32 version, f32 fps, u32 frames, u32 bones, u32 statics, u32 clips,
//   f32 sourcePelvisHeight, f32 restYaw,
//   bones x { str name }, statics x { str name, f32 x y z w }, clips x { str name, str tags, u32 first, u32 count,
//   f32 sourceStartSeconds },
//   frames x { f32 rootX rootZ yaw }, frames x { f32 pelvis x y z }, frames x bones x { u16 w0 w1 w2 }
// (str = u16 length + bytes.)

#include "MotionMath.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

class Model;
struct LocalTRS;

class MotionClipStream {
public:
    static constexpr std::uint32_t kVersion = 1;

    struct Clip {
        std::string Name;
        std::string Tags;          // ';'-separated (tools/mm/takes.csv)
        std::uint32_t First = 0;   // global frame index of the clip's first frame
        std::uint32_t Count = 0;
        float SourceStart = 0.0f;  // seconds into the source take where the clip starts (its trim)
    };
    struct StaticBone {
        std::string Name;
        glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
    struct RootSample {
        glm::vec3 Position{0.0f};
        float Yaw = 0.0f;
    };

    // --- Building (MotionBake, tests) ---
    void Reset(float fps, const std::vector<std::string>& bones);
    // Appends a clip: `rotations` is frames x bones, `pelvis` and `root` one per frame.
    void AddClip(const Clip& info, const std::vector<RootSample>& root, const std::vector<glm::vec3>& pelvis,
                 const std::vector<glm::quat>& rotations);
    std::vector<StaticBone> Statics;
    float SourcePelvisHeight = 1.0f; // the source rig's pelvis height over the ground at rest (m)
    float RestYaw = 0.0f;            // the target rig's rest facing (RootSynth::RootMatrix)

    bool Save(const std::string& path, std::string* error = nullptr) const;
    bool Load(const std::string& path, std::string* error = nullptr);

    // --- Reading ---
    float Fps() const { return m_Fps; }
    std::uint32_t FrameCount() const { return (std::uint32_t)m_Root.size(); }
    const std::vector<std::string>& Bones() const { return m_Bones; }
    int BoneIndex(const std::string& name) const;
    const std::vector<Clip>& Clips() const { return m_Clips; }
    int FindClip(const std::string& name) const;
    float ClipSeconds(int clip) const;
    // Bytes the per-frame data takes in memory (what the budget counts).
    size_t PoseBytes() const;

    const RootSample& Root(std::uint32_t frame) const { return m_Root[frame]; }
    const glm::vec3& Pelvis(std::uint32_t frame) const { return m_Pelvis[frame]; }
    glm::quat Rotation(std::uint32_t frame, int bone) const {
        return MotionMath::UnpackQuat(m_Rot[(size_t)frame * m_Bones.size() + (size_t)bone]);
    }
    // The root at `seconds` into `clip` (clamped to the clip), interpolated between frames.
    RootSample RootAt(int clip, float seconds) const;
    glm::mat4 RootMatrixAt(int clip, float seconds) const;

    // A model's node index per stream bone (-1 where the model lacks it), per static bone, and its pelvis.
    struct Binding {
        std::vector<int> Nodes;
        std::vector<int> StaticNodes;
        int Pelvis = -1;
        float PelvisScale = 1.0f;
        int Bound = 0; // stream bones found on the model
    };
    // `pelvisHeight`: the model's pelvis height at rest (m), for the translation scale; <= 0 keeps the source's.
    Binding Bind(const Model& model, float pelvisHeight) const;
    Binding BindNames(const std::vector<std::string>& nodeNames, int pelvisNode, float pelvisHeight) const;
    // Writes `clip` at `seconds` (clamped) into `pose` (one LocalTRS per model node) through `binding`: the bones'
    // rotations (nlerp between frames), the statics, the pelvis translation. Translation and scale of the other bones
    // are left as `pose` has them.
    void SamplePose(int clip, float seconds, const Binding& binding, std::vector<LocalTRS>& pose) const;
    // The same at a whole global frame (no interpolation).
    void FramePose(std::uint32_t frame, const Binding& binding, std::vector<LocalTRS>& pose) const;

private:
    float m_Fps = 60.0f;
    std::vector<std::string> m_Bones;
    std::vector<Clip> m_Clips;
    std::vector<RootSample> m_Root;
    std::vector<glm::vec3> m_Pelvis;
    std::vector<MotionMath::PackedQuat> m_Rot;
};
