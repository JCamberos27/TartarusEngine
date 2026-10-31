#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Occlusion culling for the camera views' opaque pass (Project Settings > Quality > Occlusion culling).
//
// The static world's depth pre-pass is the occluder. Each static object in the frustum is either drawn as before or,
// when its last test said it was hidden, a "suspect": left out of the pre-pass and of the instanced batches, box-tested
// against the pre-pass depth with an occlusion query, and its depth and colour draws gated on that query on the GPU
// (conditional rendering, waiting for the result). So whether a suspect draws is decided against this frame's own
// depth: one that comes into view draws on that very frame - nothing pops in, after a camera cut either. Predictions
// only decide who is tested; a wrong one costs speed, never a missing object.
//
// The visible objects are re-tested every few frames (their own depth is in the buffer then, but a box can only be
// hidden by what lies in front of the object, so that never makes a hidden object look visible), and every result is
// read back a frame or more later without waiting, to make the next predictions. A camera cut forgets them all.
//
// Shadow passes are not culled: a caster the camera can't see can still throw a shadow it can.
class OcclusionCuller {
public:
    struct Candidate {
        std::uint32_t Key;     // stable per object across frames (the entity)
        glm::vec3 Min, Max;    // world bounds
        bool Suspect;          // from Suspect(): tested this frame and gated on the result
    };

    // The setting, TARTARUS_OCCLUSION=0 (A/B benches) and the GL entry points.
    static bool Enabled();
    // One culler per camera view: the editor Scene view and the player's camera. Null for anything else.
    static OcclusionCuller* ForView(bool editorView, bool ownerView);

    // The view's frame begins: read back finished results, and forget every prediction on a camera cut or a new world.
    void BeginFrame(const void* world, const glm::vec3& eye, const glm::vec3& forward, float nearZ);
    // Predicted hidden. Never for a box the eye is in or near (the near plane would clip its query).
    bool Suspect(std::uint32_t key, const glm::vec3& wmin, const glm::vec3& wmax) const;
    // Issues the queries against the bound depth buffer (colour writes off): every suspect, plus the visible objects
    // due a re-test. `queries[i]` is candidate i's query when it is a suspect (gate its draws on it), else 0. Restores
    // the depth, cull and polygon-offset state it changes; leaves the caller's program and vertex array unbound.
    void Test(const std::vector<Candidate>& candidates, const glm::mat4& viewProj, std::vector<unsigned>& queries);

    // Results read back: suspects that came back visible, and all hidden ones, since the last BeginFrame.
    int RevealedLastFrame() const { return m_Revealed; }

    // Testable pieces (unit tests).
    static bool NearEye(const glm::vec3& wmin, const glm::vec3& wmax, const glm::vec3& eye, float margin);
    static bool IsCut(const glm::vec3& lastEye, const glm::vec3& lastFwd, const glm::vec3& eye, const glm::vec3& fwd);
    static void PadBounds(glm::vec3& wmin, glm::vec3& wmax);

private:
    struct Entry {
        std::uint32_t TestedFrame = 0; // when its last query was issued
        bool Hidden = false;           // the last result read back
    };
    struct Pending {
        unsigned Query;
        std::uint32_t Key;
        std::uint32_t Epoch;
        bool Suspect;
    };
    std::unordered_map<std::uint32_t, Entry> m_State;
    std::vector<Pending> m_Pending; // issue order; results come back in it
    std::size_t m_PendingHead = 0;
    std::vector<unsigned> m_FreeQueries;
    std::uint32_t m_Frame = 0, m_Epoch = 0;
    const void* m_World = nullptr;
    glm::vec3 m_LastEye{0.0f}, m_LastFwd{0.0f, 0.0f, -1.0f};
    bool m_HaveLast = false;
    float m_NearMargin = 0.5f;
    glm::vec3 m_Eye{0.0f};
    int m_Revealed = 0;

    unsigned TakeQuery();
};
