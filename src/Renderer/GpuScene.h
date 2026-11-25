#pragma once

#include "ReflectionProbeArray.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <utility>
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
    // The uploads for some changed slots (ascending): runs [first, last], joined across gaps of up to kMergeGap
    // unchanged slots (one call costs more than a few hundred bytes), and one span over all of them past kMaxRuns.
    using Run = std::pair<std::uint32_t, std::uint32_t>;
    static constexpr std::uint32_t kMergeGap = 16;
    static constexpr std::size_t kMaxRuns = 256;
    static void BuildRuns(const std::vector<std::uint32_t>& sortedSlots, std::vector<Run>& runs);

    static bool Enabled();
    static GpuScene& Get();

    // Brings the mirror and (with `upload`) the GPU buffers up to the world's renderables. Unit tests pass false: no GL.
    void Sync(World& world, bool upload = true);
    // Sync, unless nothing the candidate list is built from changed since the last one for this world (its transform
    // cache stamp and renderable count: the list's own fast path). For passes that draw from the scene many times a
    // frame (every shadow map). Returns Ready().
    bool SyncOnce(World& world);
    // SyncOnce's fast path: the last Sync (any caller's) was for this world, and its transform cache stamp and
    // renderable count haven't moved since. No GL: the unit tests check it directly.
    bool UpToDateFor(World& world) const;
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

    // G-MB: each slot's per-object shading state for the opaque multi-draw, so copies in different rooms share a batch
    // instead of splitting it on their probes and GI volumes (ModelFragment.glsl SlotStateBlock, binding 38). 80 bytes,
    // std430: Head = GI volume mask low/high bits, probe count, 0; Probe[2j] = probe j's centre + layer,
    // Probe[2j + 1] = its half size + blend weight (the values GIVolumes::SetMask and ReflectionProbeArray::Bind set).
    // Written by the view that draws the slot (SceneRenderer::ViewPass::BuildBatches), never by Sync: a slot's state
    // is only read by the draws of the view that just wrote it.
    struct ObjectState {
        std::uint32_t Head[4] = {0, 0, 0, 0};
        glm::vec4 Probe[4] = {glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)};
    };
    static constexpr unsigned kObjectStateBinding = 38;
    // The model fragment program's full variant (decals + local lights) has 9 SSBO blocks with SlotStateBlock, and
    // GL 4.3 only promises 8 per fragment stage (GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS). On a GPU below 9 the run
    // compiles every program without the block (_NO_SLOT_STATE, a ShaderLibrary global define) and the multi-draw keeps
    // binding probes and GI masks per batch, as TARTARUS_NO_SLOT_STATE does.
    static constexpr int kSlotStateFragmentBlocks = 9;
    static bool SlotStateFits(int maxFragmentBlocks) { return maxFragmentBlocks >= kSlotStateFragmentBlocks; }
    // GL, once at startup after ShaderLibrary::Init and before the model programs compile: reads the limit and, below
    // kSlotStateFragmentBlocks, sets the define. Never called (unit tests): supported.
    static void InitSlotStateSupport();
    static bool SlotStateSupported();
    static int MaxFragmentBlocks(); // the queried limit (0 before InitSlotStateSupport)
    // GL: how many SSBO blocks `program`'s fragment stage references (the startup log and its check).
    static int FragmentBlocks(unsigned program);
    static ObjectState MakeObjectState(std::uint64_t giMask, const ReflectionProbeArray::Selection& probes);
    // The mirror's entry for `slot` (< Count()) becomes `s`; a change goes up with the next UploadObjectState.
    void SetObjectState(std::uint32_t slot, const ObjectState& s);
    // GL: uploads the changed states and binds the buffer at kObjectStateBinding. False: no buffer.
    bool UploadObjectState();
    const std::vector<ObjectState>& ObjectStateMirror() const { return m_StateCpu; }
    // Slots changed since the last UploadObjectState (unit tests).
    std::uint32_t PendingObjectStates() const { return (std::uint32_t)m_StateDirty.size(); }

    struct SyncStats {
        bool Full = false;           // every slot written
        std::uint32_t Slots = 0;     // slots written to the mirror
        std::uint32_t Runs = 0;      // uploads issued (0 without upload)
        std::uint64_t Bytes = 0;     // bytes uploaded
    };
    const SyncStats& LastSync() const { return m_Last; }
    // The last Sync's uploads as planned (made even with upload false): slot runs [first, last].
    const std::vector<Run>& LastRuns() const { return m_Runs; }

    // G4-c: what changed for a cached shadow map since the last TakeChangeLog, across every Sync in between. A slot is
    // logged when its packed transform or bounds changed, or a skinned slot's pose did (posed slots come with every
    // Sync), with the box it held before and holds now (Bounded false: either had none, so it reaches everything).
    // All: anything may have changed (the first take, a full rebuild, a Sync for another world, or too many to list).
    struct SlotChange {
        std::uint32_t Slot = 0;
        glm::vec3 Min{0.0f}, Max{0.0f};
        bool Bounded = false;
    };
    struct ChangeLog {
        bool All = true;
        std::vector<SlotChange> Changes;
    };
    static constexpr std::size_t kMaxLoggedChanges = 4096; // past this, All (a rebuild's worth of work anyway)
    // Hands out the log for `world` and starts a new one. The first call turns logging on (and is All).
    void TakeChangeLog(const World& world, ChangeLog& out);

    // GL: reads both buffers back and compares them slot by slot with the candidate list (SceneRenderer.h ReadSceneSlot).
    // False with the first mismatch in `why`. For the smoke run; it stalls the pipeline.
    bool SelfTest(World& world, std::string& why);

    // One slot's packing (both buffers), for the unit tests.
    static void Pack(const SceneSlot& s, glm::mat4 xf[2], glm::vec4 bounds[2]);


private:
    void WriteSlot(std::uint32_t slot, const SceneSlot& s);
    bool EnsureCapacity(std::uint32_t count); // GL; false: the buffers could not be made
    void UploadRuns(const std::vector<Run>& runs);
    void LogChange(std::uint32_t slot, const glm::vec4 before[2], bool hadBefore, const SceneSlot& now);
    void LogAll();

    std::uint32_t m_Count = 0;
    std::uint32_t m_Capacity = 0; // slots the GL buffers hold
    unsigned m_Xforms = 0, m_Bounds = 0;
    unsigned m_SlotFallback = 0; // BindSlotList's store when the instance ring is full
    bool m_GpuStale = true;       // the GL buffers don't hold the mirror (new, grown or released): upload it all
    std::vector<glm::mat4> m_XformCpu;
    std::vector<glm::vec4> m_BoundsCpu;
    std::vector<std::uint32_t> m_Sorted;
    std::vector<Run> m_Runs;
    SyncStats m_Last;
    const World* m_OnceWorld = nullptr; // the last Sync's world and what it saw (UpToDateFor)
    std::uint64_t m_OnceStamp = 0;
    std::size_t m_OnceRenderables = 0;
    // TakeChangeLog's log: on once asked; the world its slots belong to; each skinned slot's pose when last logged.
    bool m_LogOn = false;
    const World* m_LogWorld = nullptr;
    ChangeLog m_Log;
    std::vector<std::uint64_t> m_Pose; // by slot (0: no bones)
    // G-MB object state: the mirror (by slot), the slots changed since the last upload, and its GL buffer.
    std::vector<ObjectState> m_StateCpu;
    std::vector<std::uint32_t> m_StateDirty;
    std::vector<Run> m_StateRuns;
    unsigned m_StateBuffer = 0;
    std::uint32_t m_StateCapacity = 0;
};
