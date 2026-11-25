#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

class World;
struct SceneSlot; // SceneRenderer.h

// The persistent GPU scene (G-1): every renderable GatherVisible knows, in GPU buffers at a stable slot (SceneRenderer.h
// SceneSlot: the candidate list's index), written only when it changes instead of re-copied by every pass.
//
//   Xforms: two mat4 per slot, the world transform then its normal matrix - the layout ModelVertex.glsl and
//           ShadowDepth.vert.glsl already read from the instance buffer (binding 27).
//   Bounds: two vec4 per slot, world min (w = 1 bounded, 0 not: HiZCuller::Boxes' layout) and world max (w = the bits of
//           a uint: the candidate flags, kDead when the slot holds nothing, the layer << 16).
//
// Sync once a frame, after World::RebuildWorldTransformCache and before the first pass. A frame that moved a few objects
// writes just their slots (merged into runs); a full candidate rebuild, or a patch past half the list, writes them all.
// The CPU keeps a mirror of both buffers: the runs are uploaded from it, and the unit tests read it without GL.
// One consumer only: TakeSceneSlotChanges hands each change out once (the engine's is Get()).
// TARTARUS_NO_GPU_SCENE=1 turns it off (nothing is synced or allocated; the passes keep the per-pass instance copies).
class GpuScene {
public:
    static constexpr std::uint32_t kDead = 1u << 15; // in Bounds' max.w: the slot holds no renderable now

    static bool Enabled();
    static GpuScene& Get();

    // Brings the mirror and (with `upload`) the GPU buffers up to the world's renderables. Unit tests pass false: no GL.
    void Sync(World& world, bool upload = true);
    // Sync, unless nothing the candidate list is built from changed since the last one for this world (its transform
    // cache stamp and renderable count: the list's own fast path). For passes that draw from the scene many times a
    // frame (every shadow map). Returns Ready().
    bool SyncOnce(World& world);
    void Release(); // GL buffers freed (the mirror kept); the next Sync re-creates and fills them

    std::uint32_t Count() const { return m_Count; }
    // The GL buffers hold the mirror (the last Sync uploaded): the passes may read them.
    bool Ready() const { return m_Xforms && !m_GpuStale; }
    // A pass's slot list (one uint per drawn copy) bound as SSBO `binding`: a span of the instance ring, or this
    // scene's own buffer when the ring can't take it. GL.
    void BindSlotList(unsigned binding, const std::vector<std::uint32_t>& slots);
    unsigned XformBuffer() const { return m_Xforms; }
    unsigned BoundsBuffer() const { return m_Bounds; }
    const std::vector<glm::mat4>& XformMirror() const { return m_XformCpu; } // 2 per slot
    const std::vector<glm::vec4>& BoundsMirror() const { return m_BoundsCpu; } // 2 per slot

    struct SyncStats {
        bool Full = false;           // every slot written
        std::uint32_t Slots = 0;     // slots written to the mirror
        std::uint32_t Runs = 0;      // uploads issued (0 without upload)
        std::uint64_t Bytes = 0;     // bytes uploaded
    };
    const SyncStats& LastSync() const { return m_Last; }

    // GL: reads both buffers back and compares them slot by slot with the candidate list (SceneRenderer.h ReadSceneSlot).
    // False with the first mismatch in `why`. For the smoke run; it stalls the pipeline.
    bool SelfTest(World& world, std::string& why);

    // One slot's packing (both buffers), for the unit tests.
    static void Pack(const SceneSlot& s, glm::mat4 xf[2], glm::vec4 bounds[2]);

private:
    void WriteSlot(std::uint32_t slot, const SceneSlot& s);
    bool EnsureCapacity(std::uint32_t count); // GL; false: the buffers could not be made
    void UploadRuns(const std::vector<std::uint32_t>& sortedSlots);

    std::uint32_t m_Count = 0;
    std::uint32_t m_Capacity = 0; // slots the GL buffers hold
    unsigned m_Xforms = 0, m_Bounds = 0;
    unsigned m_SlotFallback = 0; // BindSlotList's store when the instance ring is full
    bool m_GpuStale = true;       // the GL buffers don't hold the mirror (new, grown or released): upload it all
    std::vector<glm::mat4> m_XformCpu;
    std::vector<glm::vec4> m_BoundsCpu;
    std::vector<std::uint32_t> m_Sorted;
    SyncStats m_Last;
    const World* m_OnceWorld = nullptr; // SyncOnce's last key
    std::uint64_t m_OnceStamp = 0;
    std::size_t m_OnceRenderables = 0;
};
