#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

class Shader;
class Texture;
class World;
struct Frustum;

// Octahedral impostors (ImpostorLibrary.h): draws the LOD groups World::ApplyLod left on their impostor - one instanced
// card per baked set, which picks the three atlas frames nearest the view, blends them, and offsets its depth to the
// blended surface (Impostor.vert.glsl, ImpostorSurface.glsl). They go through the same passes as the trees they stand
// in for: the depth pre-pass, the opaque pass (lit as foliage: sun and its shadow, lamps, sky, SSAO, fog) over that
// depth GL_EQUAL, and the sun's cascades.
class ImpostorRenderer {
public:
    static ImpostorRenderer& Get();

    // A view's impostors: those ApplyLod picked for this view, inside `frustum`, grouped by set and uploaded. False when
    // there are none (nothing to draw this view). Call after the view's ApplyLod; the draws below use this list.
    bool Gather(const World& world, const Frustum& frustum, const glm::vec3& eye);
    // Into the bound depth target (colour writes off), perspective. Writes depth with its own program; leaves it bound.
    void DrawDepth(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye);
    // Lit, over the pre-pass depth (`depthPrepassed`: GL_EQUAL, no depth writes; otherwise GL_LEQUAL with writes).
    // `viewDepth`: the bound target's multisampled depth texture (`samples` a pixel), or 0. A program that writes
    // gl_FragDepth is depth tested only after it runs, so a covered pixel hidden behind a nearer tree ran the whole
    // lighting before GL_EQUAL threw it away; reading the depth (writes are off) drops it first. `applyFrameState`
    // brings the lit program up to the view's frame state (SceneRenderer::ApplyFrameState).
    void DrawLit(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye, bool depthPrepassed,
                 unsigned viewDepth, int samples, const std::function<void(Shader&)>& applyFrameState);
    // The sun: each impostor into each cascade in `mask` whose frustum it is in and whose read slice its box can shadow
    // (`reaches`, CascadedShadowMap::CasterReachesSlice). `bindCascade` makes cascade c the target (Resume, its
    // scissor). `world`'s last ApplyLod must be the view's.
    void DrawSun(const World& world, const glm::mat4* cascadeViewProj, const Frustum* cascadeFrusta, int cascades,
                 unsigned mask, const glm::vec3& towardSun,
                 const std::function<bool(int, const glm::vec3&, const glm::vec3&)>& reaches,
                 const std::function<void(int)>& bindCascade);
    int DrawCalls() const { return m_DrawCalls; }

private:
    struct alignas(16) Instance {
        glm::vec4 CentreScale; // the sphere's world centre, the copy's scale
        glm::vec4 Rotation;    // object -> world quaternion (x, y, z, w)
        glm::vec4 OriginFade;  // the group's world origin, its LOD cross-fade
    };
    struct SetDraw {
        std::uint32_t First = 0, Count = 0; // in the uploaded span
    };
    struct Atlases {
        std::shared_ptr<Texture> Albedo, NormalDepth, Mask;
        bool Tried = false;
        bool Ok() const;
    };
    bool EnsurePrograms();
    bool EnsureAtlases(int set);
    // Gathers `world`'s impostors into `buckets` lists (the view: 1; the sun: one per cascade) - `keep` returns the
    // buckets an impostor goes in from its world box - and uploads them; false when none.
    // Each list is sorted near to far along `sortDir` from `sortFrom` (the view: from the eye, any direction - by
    // distance; the sun: along its rays), so early depth testing drops what nearer cards cover.
    bool Build(const World& world, int buckets,
               const std::function<unsigned(const glm::vec3& boxMin, const glm::vec3& boxMax)>& keep,
               const glm::vec3& sortFrom, const glm::vec3* sortDir);
    void SetCommon(Shader& prog, int set);
    void DrawSets(Shader& prog, int bucket);
    std::size_t TakeUpload(std::size_t bytes);

    std::unique_ptr<Shader> m_Depth, m_Lit, m_Shadow;
    bool m_ProgramsFailed = false;
    unsigned m_Vao = 0;
    std::vector<Atlases> m_Atlases;
    std::vector<std::vector<Instance>> m_Lists; // per bucket and set (bucket x sets + set), this gather
    std::vector<SetDraw> m_Draws;              // the same layout
    std::size_t m_Span = 0, m_SpanBytes = 0;    // the uploaded instances (TakeUpload offset, bytes)
    bool m_HaveList = false;
    int m_DrawCalls = 0;
    // The instance upload ring (persistently mapped, reused by frame age - never re-specified: a driver drain).
    unsigned m_UploadBuf = 0;
    unsigned char* m_UploadBase = nullptr;
    bool m_UploadFailed = false;
    std::size_t m_UploadHead = 0;
    struct UploadSpan { std::size_t Start; std::uint64_t Frame; };
    std::vector<UploadSpan> m_UploadSpans;
};
