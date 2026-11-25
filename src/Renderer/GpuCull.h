#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "CascadedShadowMap.h"

struct VisibilityQuery;
struct Frustum;
class World;

// GPU-driven culling (G-2): the per-view visibility test of GatherVisible, run on the GPU over the persistent GPU scene
// (GpuScene: one slot per candidate), packing each multi-draw group's visible slots IN SLOT ORDER into its remap run
// and setting its commands' instance counts, so the batched draws come out exactly as the CPU path would order them.
//
// Exactness (never a missing object): stage A is the same test as GatherVisible's, on the same data. Where the GPU's
// float rounding can differ (frustum planes, the size cull's sqrt) it errs by a hair on the kept side, so it may only
// draw MORE (ComputeSource's slack constants).
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

// The GLSL of stage A + the ordered compaction (exposed for the shader-compile unit test). CasterComputeSource is the
// same source in caster mode (G4-a, below).
const char* ComputeSource();
const char* CasterComputeSource();

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
// compares it group by group with GatherVisible(q). Like HiZCuller::Cull it leaves storage bindings 22-25, 28 and 29
// as it used them: their users bind their own before they draw. G2-a: nothing draws from it yet.
void VerifyView(World& world, const VisibilityQuery& q);

// ---- G4-a: the sun's shadow casters (caster mode) ----
// Stage A with the caster query's flags (MakeQuery of a ShadowCasters query), then per cascade c the cascade's frustum
// (inclusive, as Frustum::Intersects) and CascadedShadowMap::CasterReachesSlice, gated by bit c of the render mask
// (held cascades draw nothing). Each group's survivors come out as one entry per (slot, cascade): the slot in the low
// 30 bits, the cascade in the top 2, slots in slot order and each slot's cascades in ascending order. A group's run in
// the remap is 4x its slot run (Group::Base * 4), and its commands' instance count is the entry count.
// G4-a: verify only (nothing draws from it).

// The per-frame block (std140, uniform binding kSunCasterBinding): each cascade's CasterReachesSlice inputs and planes.
struct SunCascade {
    CascadedShadowMap::CasterSlice Slice;
    glm::vec4 Planes[6] = {}; // Frustum::FromViewProj(LightViewProj(c)): xyz normal, w = D
};
struct SunCasterBlock {
    SunCascade Cascades[CascadedShadowMap::kMaxCascades];
    glm::uvec4 Info{0u}; // x = render mask (bit c: cascade c draws this frame), y = cascade count
};
constexpr int kSunCasterBinding = 5; // uniform block (2 sky, 3 clouds, 4 GI are taken)
constexpr std::uint32_t kCascadeShift = 30;  // remap entry: slot | cascade << 30
constexpr std::uint32_t kSlotMask = (1u << kCascadeShift) - 1u;

// The block for this frame's fit: `cells` are the cascades' frustums (ShadowFrame's SunPass::cells).
SunCasterBlock MakeSunCasterBlock(const CascadedShadowMap& map, const Frustum* cells, unsigned renderMask,
                                  float kernelTexels, float normalBias);
// The per-cascade test on the CPU, op for op what the compute does without its slack: the cascades (bit c) a slot
// that passed the flag tests goes into. An unbounded slot goes into every cascade in the render mask.
unsigned CasterCascades(const glm::vec4 bounds[2], const SunCasterBlock& b);

// Caster-mode verify totals since start.
struct SunVerifyTotals {
    std::uint64_t Frames = 0;   // passes compared
    std::uint64_t Expected = 0; // batched (slot, cascade) pairs the CPU drew
    std::uint64_t Missing = 0;  // ...that the GPU left out: a bug (hard gate in smoke)
    std::uint64_t Extra = 0;    // pairs the GPU kept and the CPU didn't (allowed: the slack)
    std::uint64_t Skipped = 0;
};
const SunVerifyTotals& SunTotals();
// Whether this sun pass should be verified: TARTARUS_GPU_CULL_VERIFY, or a one-shot RequestSunVerify (the smoke check).
bool SunVerifyDue();
void RequestSunVerify();
// One (slot, cascade) pair the CPU drew: its layered draws' runs expanded per cascade (ShadowFrame's m_Draws).
struct SunPair {
    std::uint32_t Slot;
    std::uint32_t Cascade;
};
// Runs the caster mode on the GPU for this frame's block, reads it back (waits on the GPU) and compares the batched
// slots' per-cascade sets with `cpu`. Order doesn't matter (a depth-only pass). Same bindings note as VerifyView.
void VerifySunCasters(World& world, const SunCasterBlock& block, const std::vector<SunPair>& cpu);

} // namespace GpuCull
