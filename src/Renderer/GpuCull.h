#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

struct VisibilityQuery;
class World;

// GPU-driven culling (G-2): the per-view visibility test of GatherVisible, run on the GPU over the persistent GPU scene
// (GpuScene: one slot per candidate), packing each multi-draw group's visible slots IN SLOT ORDER into its remap run
// and setting its commands' instance counts, so the batched draws come out exactly as the CPU path would order them.
//
// Exactness (never a missing object): stage A is the same test as GatherVisible's, on the same data. Where the GPU's
// float order can differ (frustum planes) it may only draw MORE; the size cull is the CPU formula op for op.
// TARTARUS_GPU_CULL_VERIFY=1 reads the result back every frame (debug only: it waits for the GPU) and compares it with
// GatherVisible: a slot the CPU keeps and the GPU drops is "missing" (a bug), one the GPU keeps extra is counted.
//
// Off with TARTARUS_NO_GPU_CULL=1, without the GPU scene or multi-draw, or when the compute program fails to build.
namespace GpuCull {

// The flag bits of a GPU scene slot (GpuScene's Bounds max.w, low 16 bits) stage A reads. Bits 0-7 are the candidate
// cache's CandidateFlag bits (SceneViewPass.h); 8-10 are the renderable's own settings (SceneSlot, SceneRenderer.h);
// 15 is GpuScene::kDead. The layer is the high 16 bits.
enum SlotFlag : std::uint32_t {
    kSlotInactive = 1u << 0,        // kCandInactive
    kSlotPoseSource = 1u << 1,      // first-person arms rig: animated only, never drawn
    kSlotOwnerViewOnly = 1u << 2,   // split pose: the first-person copy
    kSlotHiddenFromOwner = 1u << 3, // split pose: the world copy
    kSlotViewModel = 1u << 4,
    kSlotPlayerBody = 1u << 5,
    kSlotHiddenInScene = 1u << 6,   // editor Scene view's per-entity hide
    kSlotPosed = 1u << 7,           // bounds follow the pose (read live; stays on the CPU path)
    kSlotCastOff = 1u << 8,         // Cast Shadows: Off
    kSlotShadowsOnly = 1u << 9,     // Cast Shadows: Shadows Only
    kSlotTransparent = 1u << 10,    // first material slot is in the Transparent queue
    kSlotDead = 1u << 15,           // GpuScene::kDead: no renderable (left, no model, or inactive)
};

// A VisibilityQuery flattened into what the compute's uniforms hold.
struct ViewQuery {
    glm::vec4 Planes[6] = {};      // xyz normal, w = D; unused when !HasFrustum
    glm::vec4 WithinMin{0.0f}, WithinMax{0.0f};
    glm::vec4 SizeEye{0.0f};       // xyz eye, w = projScaleY
    std::uint32_t RejectFlags = 0; // any of these set -> left out
    std::uint32_t LayerMask = ~0u; // editor view only (CheckLayer)
    float MinScreenHeight = 0.0f;
    std::uint32_t Bits = 0;        // kQ* below
};
enum QueryBit : std::uint32_t { kQHasFrustum = 1u, kQHasWithin = 2u, kQHasSizeCull = 4u, kQCheckLayer = 8u };

// The query's settings as a ViewQuery (which flags reject, which tests run). CellCull is not carried: it is a coarse
// superset test for callers that test each object again later, so leaving it out only keeps more.
ViewQuery MakeQuery(const VisibilityQuery& q);

// Stage A on the CPU, op for op what the compute does: the reference the unit test and the verify mode hold the GPU to.
// `bounds` is one GPU scene slot (GpuScene::BoundsMirror()[2 * slot], [2 * slot + 1]); `lodCulled` is the slot's bit
// in the view's LOD bitset.
bool PassesStageA(const glm::vec4 bounds[2], const ViewQuery& q, bool lodCulled);

// A multi-draw group: its run of slots (in draw order) and its commands (same layout as HiZCuller::Group).
struct Group {
    std::uint32_t Base, Count;
    std::uint32_t CmdFirst, CmdCount;
};

// The setting, the env switch, the GPU scene and the compute program.
bool Enabled();
bool VerifyEnabled();

// The GLSL of stage A + the ordered compaction (exposed for the shader-compile unit test).
const char* ComputeSource();

// The draw table: the GPU scene's batchable slots (alive, not posed, with a model) grouped by model + material set
// (InstanceBatch's key), groups in the order of their first slot and slots in slot order within a group.
struct DrawTable {
    static constexpr std::uint32_t kNoGroup = ~0u;
    std::vector<Group> Groups;
    std::vector<std::uint32_t> Slots;   // the groups' runs, back to back (Group::Base indexes this)
    std::vector<std::uint32_t> GroupOf; // scene slot -> group (kNoGroup: not batched)
};
// Built from the candidate list as it is now (ReadSceneSlot); the GPU scene must be synced this frame.
void BuildDrawTable(std::uint32_t slotCount, DrawTable& out);

// Verify mode totals since start (TARTARUS_GPU_CULL_VERIFY=1).
struct VerifyTotals {
    std::uint64_t Views = 0;    // queries compared
    std::uint64_t Expected = 0; // batched slots GatherVisible kept
    std::uint64_t Missing = 0;  // ...that the GPU dropped: a bug
    std::uint64_t Extra = 0;    // batched slots the GPU kept and GatherVisible didn't (allowed)
    std::uint64_t OrderBad = 0; // groups whose survivors came out in another order than GatherVisible's
    std::uint64_t Skipped = 0;  // views not compared (no GPU scene buffers, or the program failed)
};
const VerifyTotals& Totals();

// Runs stage A + the compaction on the GPU for `q` over the draw table, reads the result back (waits on the GPU) and
// compares it group by group with GatherVisible(q). Puts back the storage bindings it took. G2-a: nothing draws from
// it yet.
void VerifyView(World& world, const VisibilityQuery& q);

} // namespace GpuCull
