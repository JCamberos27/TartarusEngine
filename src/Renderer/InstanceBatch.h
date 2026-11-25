#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

class Model;
class Shader;
struct MaterialAsset;
class VisibleIndexBuffer;
class World;

// GPU instancing of repeated static models (Project Settings > Quality > GPU instancing). A pass groups the copies of
// one model that draw the same way, puts their transforms in the per-frame instance buffer, and draws each group once
// with an instance count: ModelVertex.glsl and ShadowDepth.vert.glsl read the transforms (uInstanced) at binding 27.

// Items grouped by key. Groups keep the order of their first item, and items keep their order within a group: Order
// lists every item, group by group, and Groups[g] is the run of Order that group g takes.
struct InstanceGroups {
    struct Group {
        std::uint32_t First, Count;
    };
    std::vector<std::uint32_t> Order;
    std::vector<Group> Groups;
};

// `hash(i)` and `same(a, b)` describe item i's key; an item whose hash is kNoBatch is a group of its own.
constexpr std::uint64_t kNoBatch = ~0ull;
template <class Hash, class Same>
void GroupInstances(std::size_t count, Hash hash, Same same, InstanceGroups& out) {
    // Runs per pass on every frame, so nothing here allocates once the vectors have grown: an open-addressing table
    // from hash to the first group with it, and the groups that share a hash chained through `next`.
    static thread_local std::vector<std::uint64_t> slotHash; // kNoBatch = empty
    static thread_local std::vector<std::uint32_t> slotGroup, groupOf, leader, sizes, next;
    std::size_t cap = 16;
    while (cap < count * 2) cap <<= 1;
    slotHash.assign(cap, kNoBatch);
    slotGroup.resize(cap);
    groupOf.resize(count);
    leader.clear();
    sizes.clear();
    next.clear();
    constexpr std::uint32_t kEnd = ~0u;
    const auto newGroup = [&](std::uint32_t i) {
        leader.push_back(i);
        sizes.push_back(0);
        next.push_back(kEnd);
        return (std::uint32_t)leader.size() - 1;
    };
    for (std::uint32_t i = 0; i < (std::uint32_t)count; ++i) {
        const std::uint64_t h = hash(i);
        std::uint32_t g = kEnd;
        if (h == kNoBatch) {
            g = newGroup(i);
        } else {
            std::size_t at = (std::size_t)((h ^ (h >> 29)) * 0xBF58476D1CE4E5B9ull) & (cap - 1);
            while (slotHash[at] != kNoBatch && slotHash[at] != h) at = (at + 1) & (cap - 1);
            if (slotHash[at] == kNoBatch) {
                g = newGroup(i);
                slotHash[at] = h;
                slotGroup[at] = g;
            } else {
                std::uint32_t c = slotGroup[at], last = c;
                for (; c != kEnd; last = c, c = next[c])
                    if (same(leader[c], i)) { g = c; break; }
                if (g == kEnd) { // the same hash, a different key
                    g = newGroup(i);
                    next[last] = g;
                }
            }
        }
        groupOf[i] = g;
        ++sizes[g];
    }
    out.Groups.resize(leader.size());
    std::uint32_t at = 0;
    for (std::size_t g = 0; g < leader.size(); ++g) {
        out.Groups[g] = {at, 0};
        at += sizes[g];
    }
    out.Order.resize(count);
    for (std::uint32_t i = 0; i < (std::uint32_t)count; ++i) {
        auto& grp = out.Groups[groupOf[i]];
        out.Order[grp.First + grp.Count++] = i;
    }
}

// Whether two material slot lists draw alike: slot for slot the same asset, or one with the same shader, queue and values.
bool SameSlots(const std::vector<std::shared_ptr<MaterialAsset>>& a, const std::vector<std::shared_ptr<MaterialAsset>>& b);
// A hash of the slot list's assets, for GroupInstances.
std::uint64_t HashSlots(const std::vector<std::shared_ptr<MaterialAsset>>& slots, std::uint64_t seed);

namespace InstanceBuffer {
constexpr unsigned kXformBinding = 27; // ModelVertex.glsl / ShadowDepth.vert.glsl InstanceBlock
// The per-frame instance transforms, two mat4s per instance (the transform, then its normal matrix), bound at 27.
// Each Upload copies the staging into a fresh span of a persistently mapped ring and binds that range (the draws
// before it keep reading theirs).
std::vector<glm::mat4>& Staging();
// Appends one instance for a depth pass: its transform, then an identity in the normal-matrix slot (a depth pass reads
// no normal matrix, but the shader's layout is still two mat4s per instance).
void Append(std::vector<glm::mat4>& staging, const glm::mat4& xform);
void Upload();
// Binds the last Upload's transforms at 27 again (after something else, the GPU scene, was bound there).
void Rebind();
// Any per-frame data through the same ring: copies `bytes` into a fresh span and binds it as SSBO `binding` (a range).
// False when the ring can't take it (too big, or no ring): the caller uploads its own way.
bool UploadRange(unsigned binding, const void* data, std::size_t bytes);
// Closes this frame's ring span (reused kSafeFrames presented frames later). Once per frame, after SwapBuffers.
void EndFrame();
}

// Multi-draw indirect: one glMultiDrawElementsIndirect per run of sub-meshes that bind alike, however many models and
// copies they come from (their meshes share the MeshArena's pages). Each command's baseInstance is where its copies'
// transforms start in the instance buffer, which the vertex programs add to gl_InstanceID. Project Settings > Quality
// > GPU-driven draws (with GPU instancing); TARTARUS_NO_MDI=1 turns it off for A/B benches.
bool MultiDrawEnabled();
struct IndirectCommand { // GL's DrawElementsIndirectCommand
    std::uint32_t Count, InstanceCount, FirstIndex;
    std::int32_t BaseVertex;
    std::uint32_t BaseInstance;
};
static_assert(sizeof(IndirectCommand) == 20, "the GL command layout");
struct IndirectItem {
    std::uint64_t Key;    // items with equal keys draw in one batch (the caller packs page, state and material in)
    std::uint32_t Tag;    // the caller's: what the batch binds from (its first item's)
    IndirectCommand Cmd;
};
struct IndirectBatch {
    std::uint64_t Key;
    std::uint32_t Tag;           // the batch's first item's
    std::uint32_t First, Count;  // its commands
};
// Batches in the order their keys first appear; commands keep their item order within a batch.
void BuildIndirectBatches(const std::vector<IndirectItem>& items, std::vector<IndirectCommand>& commands,
                          std::vector<IndirectBatch>& batches);
namespace IndirectBuffer {
// Uploads the commands into a fresh span of the instance ring (or, when it can't take them, a fresh data store) and
// binds it as GL_DRAW_INDIRECT_BUFFER (draws issued before keep theirs).
void Upload(const std::vector<IndirectCommand>& commands);
unsigned Handle();     // the GL buffer the last Upload filled
std::size_t Base();    // ...and where in it the commands start
// The last Upload's commands as SSBO `binding` (the HiZ cull writes their instance counts).
void BindStorage(unsigned binding);
inline const void* Offset(std::uint32_t command) {
    return (const void*)(Base() + (std::uintptr_t)command * sizeof(IndirectCommand));
}
}

// One caster in a depth-only pass: the SSAO and opaque depth pre-passes, and the sun, spot and point shadow maps.
struct DepthCaster {
    Model* Ref;
    const std::vector<std::shared_ptr<MaterialAsset>>* Slots;
    const VisibleIndexBuffer* Visible; // an outfit piece's uncovered triangles (OutfitHideTag)
    const glm::mat4* Xform;
    bool TwoSided = false;  // drawn without back-face culling (Cast Shadows: Two Sided)
    int LayerFirst = 0;     // the sun's single-pass cascades: its first cascade and how many in a row
    int Layers = 1;
    std::uint32_t Slot = ~0u; // VisibleRenderable::Slot: its transform in the GPU scene (GpuScene.h), ~0u = none
};
// Draws the casters through a depth program (ShadowDepth.vert.glsl) whose uLightViewProj (or cascade matrices) the
// caller set. Copies of an unskinned, whole model with the same materials, sidedness and cascades draw as one
// instanced draw when GPU instancing is on; the rest one by one with uModel. `firstLoc`: the program's uCascadeFirst
// when drawing the sun's layered cascades, else -1. Returns the draw calls issued (one per model, not per mesh).
// `sceneWorld`: the world the casters were gathered from. With it, the multi-draw copies read their transforms from
// the persistent GPU scene by slot (synced first, GpuScene::SyncOnce) instead of being copied into the instance ring;
// copies without a slot draw the old way.
// `cache` (the sun's layered pass): when the casters are the ones it last saw, their grouping and indirect commands
// are reused instead of worked out again (DepthCasterCache).
class DepthCasterCache;
int DrawDepthCasters(Shader& shader, const std::vector<DepthCaster>& casters, int firstLoc = -1, World* sceneWorld = nullptr,
                     DepthCasterCache* cache = nullptr);

// What the depth multi-draw works out from a caster list before it draws: each indirect command's group and sub-mesh,
// the commands in batches, the copies' GPU scene slots, and the casters it can't take (they draw directly).
struct DepthIndirectBuild {
    struct Tag {
        std::uint32_t Group;
        std::uint16_t Mesh;
    };
    std::vector<Tag> Tags;
    std::vector<IndirectItem> Items; // scratch
    std::vector<IndirectCommand> Commands;
    std::vector<IndirectBatch> Batches;
    std::vector<std::uint32_t> Slots;
    std::vector<std::uint32_t> Rest;
};

// The sun's casters barely change from frame to frame (a forest: ~2.5k casters, the same list every frame while
// nothing moves), yet grouping them and building their commands cost ~0.35 ms a frame. This keeps both while the list
// is the same - caster for caster its model import, material slots, outfit mask, GPU scene slot, sidedness and cascade
// run - and so are the render settings epoch (material depth fields, render queues: NoteRenderSettingsChanged) and the
// GPU scene's slot count. Transforms aren't in it: the copies read theirs from the GPU scene by slot. A change nothing
// reports (a streamed cutout texture landing on a caster no other pass draws, a model's meshes reaching the arena) is
// picked up by the rebuild once a build is kMaxAgeFrames old.
// The sun keeps one per set of cascades it draws (CascadedShadowMap::PlanRender: the far ones on their own turns), so
// the frames between two turns of a far cascade don't throw each other's builds away.
class DepthCasterCache {
public:
    static constexpr std::uint64_t kMaxAgeFrames = 16; // a rebuild at least every 16 frames (~0.25 s at 60 fps)
    struct Epochs {
        std::uint32_t RenderSettings = 0;
        std::uint32_t SceneCount = 0;
        bool operator==(const Epochs& o) const { return RenderSettings == o.RenderSettings && SceneCount == o.SceneCount; }
    };
    // True: Groups and Build hold for these casters as they are on `frame`. False: the caller rebuilds both, then
    // calls Built(frame).
    bool Reuse(const std::vector<DepthCaster>& casters, const Epochs& epochs, std::uint64_t frame);
    void Built(std::uint64_t frame) { m_Valid = true; m_BuiltFrame = frame; }
    void Invalidate() { m_Valid = false; }
    InstanceGroups Groups;
    DepthIndirectBuild Build;
    std::uint64_t Reused() const { return m_Reused; }   // calls that reused (unit tests)
    std::uint64_t Rebuilt() const { return m_Rebuilt; } // calls that rebuilt
    // Every cache's calls since startup (--perf-bench prints a phase's share reused).
    struct Totals { std::uint64_t Reused = 0, Rebuilt = 0; };
    static Totals AllTotals();

private:
    struct Entry { // 48 bytes, no padding: compared with memcmp
        const void* Ref;
        std::uint64_t Import;  // Model::ImportStamp: a re-import is a new list even at a reused address
        const void* SlotList;
        std::uint64_t SlotsHash; // the slot list's material pointers
        const void* Visible;
        std::uint32_t Slot;
        std::int32_t Run; // two-sided, first cascade, cascade count
    };
    static_assert(sizeof(Entry) == 48, "DepthCasterCache::Entry is compared with memcmp");
    std::vector<Entry> m_Key, m_Next;
    Epochs m_Epochs;
    bool m_Valid = false;
    std::uint64_t m_BuiltFrame = 0;
    std::uint64_t m_Reused = 0, m_Rebuilt = 0;
};
