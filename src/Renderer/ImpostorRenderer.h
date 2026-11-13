#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <glm/glm.hpp>

class Shader;
class World;
struct Frustum;
struct TextureCpuData;

// Octahedral impostors (ImpostorLibrary.h): draws the LOD groups World::ApplyLod left on their impostor - a card per
// tree, which picks the three atlas frames nearest the view, blends them, and offsets its depth to the blended surface
// (Impostor.vert.glsl, ImpostorSurface.glsl). Every set's atlases sit in three texture arrays (a layer per set), so
// each pass is one instanced draw, its cards sorted near to far. They go through the same passes as the trees they
// stand in for: the depth pre-pass, the opaque pass (lit as foliage: sun and its shadow, lamps, sky, SSAO, fog) over
// that depth GL_EQUAL, and the sun's cascades.
class ImpostorRenderer {
public:
    static ImpostorRenderer& Get();
    ~ImpostorRenderer();

    // A view's impostors: those ApplyLod picked for this view, inside `frustum`, sorted from `eye` and uploaded. False when
    // there are none (nothing to draw this view). Call after the view's ApplyLod; the draws below use this list. Also
    // gets the atlases ready the first time impostors are wanted: decoded on a worker, uploaded a few layers a frame,
    // then ImpostorLibrary::SetDrawable - until then the trees stay on their levels.
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
        glm::vec4 Set;         // x the set
    };
    struct Range {
        std::uint32_t First = 0, Count = 0; // in the uploaded span
    };
    bool EnsurePrograms();
    // The atlas arrays and the per-set buffers: true once they're up. Starts the load the first time, and uploads
    // what the worker has decoded a few layers a frame after that.
    bool EnsureAtlases();
    // Gathers `world`'s impostors into `buckets` lists (the view: 1; the sun: one per cascade) - `keep` returns the
    // buckets an impostor goes in from its world box - each sorted near to far along `sortDir` from `sortFrom` (or by
    // distance from it), and uploads them; false when none.
    bool Build(const World& world, int buckets,
               const std::function<unsigned(const glm::vec3& boxMin, const glm::vec3& boxMax)>& keep,
               const glm::vec3& sortFrom, const glm::vec3* sortDir);
    void DrawBucket(Shader& prog, int bucket);
    std::size_t TakeUpload(std::size_t bytes);

    std::unique_ptr<Shader> m_Depth, m_Lit, m_Shadow;
    bool m_ProgramsFailed = false;
    unsigned m_Vao = 0;
    // The atlases: a texture array per kind (albedo, normal + depth, mask), a layer per set, BC3 (forced, so every set
    // packs alike). m_Decoded: the worker's output, set x 3 + kind; m_Uploaded counts what's in the arrays.
    unsigned m_Arrays[3] = {};
    unsigned m_SetBuf = 0, m_ExtentBuf = 0;
    std::vector<TextureCpuData> m_Decoded;
    std::thread m_Loader;
    std::atomic<bool> m_LoaderDone{false};
    bool m_LoadStarted = false, m_AtlasesReady = false, m_AtlasesFailed = false;
    std::size_t m_Uploaded = 0;
    std::vector<std::vector<Instance>> m_Lists; // per bucket, this gather
    std::vector<Range> m_Draws;                 // per bucket, in the uploaded span
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
