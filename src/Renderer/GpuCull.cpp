#include "GpuCull.h"

#include "Core/Env.h"
#include "Physics/Frustum.h"
#include "SceneRenderer.h"
#include "SceneViewPass.h"
#include "GpuScene.h"
#include "GLStateCache.h"
#include "InstanceBatch.h"
#include "Shader.h"
#include "gl.h"
#include "World.h"
#include "Components.h"
#include "Core/Log.h"
#include "Core/Profiler.h"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

namespace GpuCull {

// The slot bits are the candidate cache's and SceneSlot's own (compared as numbers: they are different enums).
constexpr bool SameBit(std::uint32_t a, std::uint32_t b) { return a == b; }
static_assert(SameBit(kSlotInactive, kCandInactive) && SameBit(kSlotPoseSource, kCandPoseSource) &&
              SameBit(kSlotOwnerViewOnly, kCandOwnerViewOnly) && SameBit(kSlotHiddenFromOwner, kCandHiddenFromOwner) &&
              SameBit(kSlotViewModel, kCandViewModel) && SameBit(kSlotPlayerBody, kCandPlayerBody) &&
              SameBit(kSlotHiddenInScene, kCandHiddenInScene) && SameBit(kSlotPosed, kCandPosed),
              "slot bits = CandidateFlag bits");
static_assert(SameBit(kSlotCastOff, kSlotFlagCastOff) && SameBit(kSlotShadowsOnly, kSlotFlagShadowsOnly) &&
              SameBit(kSlotTransparent, kSlotFlagTransparent) && SameBit(kSlotDead, GpuScene::kDead),
              "slot bits = SceneSlot bits");

ViewQuery MakeQuery(const VisibilityQuery& q) {
    ViewQuery v;
    // GatherVisible's flag tests, in its order: inactive / pose source, the split pose's copy for this view, view
    // models, the player body, the editor's hide, Cast Shadows, transparent.
    v.RejectFlags = kSlotInactive | kSlotPoseSource | kSlotDead;
    v.RejectFlags |= (q.ShadowCasters || !q.OwnerView) ? kSlotOwnerViewOnly : kSlotHiddenFromOwner;
    if (q.SkipViewModels) v.RejectFlags |= kSlotViewModel;
    if (q.SkipPlayerBody) v.RejectFlags |= kSlotPlayerBody;
    if (q.EditorView) {
        v.RejectFlags |= kSlotHiddenInScene;
        v.Bits |= kQCheckLayer;
        v.LayerMask = q.LayerVisibleMask;
    }
    v.RejectFlags |= q.ShadowCasters ? kSlotCastOff : kSlotShadowsOnly;
    if (q.SkipTransparent) v.RejectFlags |= kSlotTransparent;
    if (q.Cull) {
        v.Bits |= kQHasFrustum;
        for (int i = 0; i < 6; ++i) v.Planes[i] = glm::vec4(q.Cull->Planes[i].Normal, q.Cull->Planes[i].D);
    }
    if (q.Within) {
        v.Bits |= kQHasWithin;
        v.WithinMin = glm::vec4(q.Within->Min, 0.0f);
        v.WithinMax = glm::vec4(q.Within->Max, 0.0f);
    }
    if (q.SizeCullEye && q.MinScreenHeight > 0.0f && q.SizeCullProjScale > 0.0f) { // TooSmallOnScreen's early out
        v.Bits |= kQHasSizeCull;
        v.SizeEye = glm::vec4(*q.SizeCullEye, q.SizeCullProjScale);
        v.MinScreenHeight = q.MinScreenHeight;
    }
    return v;
}

namespace {
std::uint32_t AsUint(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}
} // namespace

bool PassesStageA(const glm::vec4 bounds[2], const ViewQuery& q, bool lodCulled) {
    const std::uint32_t bits = AsUint(bounds[1].w);
    if (bits & q.RejectFlags) return false;
    if (lodCulled) return false;
    const std::uint32_t layer = bits >> 16;
    if ((q.Bits & kQCheckLayer) && layer < 32u && !((q.LayerMask >> layer) & 1u)) return false;
    if (bounds[0].w == 0.0f) return true; // unbounded: drawn without a frustum test
    const glm::vec3 mn(bounds[0]), mx(bounds[1]);
    if (q.Bits & kQHasFrustum) {
        // Frustum::Intersects: the box's corner furthest along each plane's normal must not be behind it.
        for (const glm::vec4& pl : q.Planes) {
            const glm::vec3 p(pl.x >= 0.0f ? mx.x : mn.x, pl.y >= 0.0f ? mx.y : mn.y, pl.z >= 0.0f ? mx.z : mn.z);
            if (glm::dot(glm::vec3(pl), p) + pl.w < 0.0f) return false;
        }
    }
    if (q.Bits & kQHasWithin) {
        const glm::vec3 wmin(q.WithinMin), wmax(q.WithinMax);
        if (!(wmin.x <= mx.x && wmax.x >= mn.x && wmin.y <= mx.y && wmax.y >= mn.y && wmin.z <= mx.z && wmax.z >= mn.z))
            return false;
    }
    if (q.Bits & kQHasSizeCull) {
        // TooSmallOnScreen, op for op (MakeQuery only sets the bit when it can cull).
        const glm::vec3 centre = (mn + mx) * 0.5f;
        const float radius = glm::length(mx - mn) * 0.5f;
        const float dist = glm::length(centre - glm::vec3(q.SizeEye));
        if (dist > radius && radius * q.SizeEye.w / dist < q.MinScreenHeight) return false;
    }
    return true;
}

bool VerifyEnabled() {
    static const bool on = Env::Flag(Env::Var::GpuCullVerify);
    return on;
}

bool Enabled() {
    static const bool off = Env::Flag(Env::Var::NoGpuCull);
    // G2-a: nothing draws from the GPU lists yet; the cull only runs for the verify mode.
    return !off && VerifyEnabled();
}

// Stage A + the ordered compaction: one workgroup per group. Inputs mirror GpuScene's Bounds and ViewQuery above.
// Bindings: the HiZ compute's 22-25 per dispatch (no new bindings, W2's G-1 rule), remap 28, commands 29.
const char* ComputeSource() {
    return R"(#version 460 core
layout(local_size_x = 64) in;
// GpuScene's Bounds: per slot, world min (w = 1 bounded, 0 not) then world max (w = the bits of a uint: flags | layer << 16).
layout(std430, binding = 22) readonly buffer BoundsBlock { vec4 uBounds[]; };
layout(std430, binding = 23) readonly buffer GroupBlock { uvec4 uGroups[]; };    // slot base, count, cmd list at, cmd count
// The group runs' scene slots, then each group's command indices (where .z says): commands of a group need not be
// consecutive (the multi-draw batches order them by how they bind).
layout(std430, binding = 24) readonly buffer GroupSlotBlock { uint uGroupSlots[]; };
layout(std430, binding = 25) readonly buffer LodBlock { uint uLodCulled[]; };     // 1 bit per scene slot
layout(std430, binding = 28) writeonly buffer RemapBlock { uint uRemap[]; };
layout(std430, binding = 29) buffer CommandBlock { uint uCommands[]; };           // 5 uints per command
uniform vec4 uPlanes[6];
uniform vec4 uWithinMin, uWithinMax, uSizeEye;
uniform uint uRejectFlags, uLayerMask, uBits, uGroupOffset;
uniform float uMinScreenHeight;
shared uint sScan[64];
// The GPU's float rounding (fused multiply-adds, sqrt) can land a hair either side of the CPU's. These keep its
// misses on the "draw more" side: a box within 1 mm of a plane (distances are metres: the planes are normalised),
// or within 1e-5 of the size threshold, is kept. Verify mode reports what that adds as "extra".
const float kPlaneSlack = 1e-3;
const float kSizeSlackDist = 1.00001;
const float kSizeSlackHeight = 0.99999;

bool Visible(uint slot) {
    vec4 mn = uBounds[slot * 2u], mx = uBounds[slot * 2u + 1u];
    uint bits = floatBitsToUint(mx.w);
    if ((bits & uRejectFlags) != 0u) return false;
    if (((uLodCulled[slot >> 5u] >> (slot & 31u)) & 1u) != 0u) return false;
    uint layer = bits >> 16u;
    if ((uBits & 8u) != 0u && layer < 32u && ((uLayerMask >> layer) & 1u) == 0u) return false;
    if (mn.w == 0.0) return true;
    if ((uBits & 1u) != 0u) {
        for (int i = 0; i < 6; ++i) {
            vec4 pl = uPlanes[i];
            vec3 p = vec3(pl.x >= 0.0 ? mx.x : mn.x, pl.y >= 0.0 ? mx.y : mn.y, pl.z >= 0.0 ? mx.z : mn.z);
            if (dot(pl.xyz, p) + pl.w < -kPlaneSlack) return false;
        }
    }
    if ((uBits & 2u) != 0u && !(all(lessThanEqual(uWithinMin.xyz, mx.xyz)) && all(greaterThanEqual(uWithinMax.xyz, mn.xyz))))
        return false;
    if ((uBits & 4u) != 0u) {
        vec3 centre = (mn.xyz + mx.xyz) * 0.5;
        float radius = length(mx.xyz - mn.xyz) * 0.5;
        float dist = length(centre - uSizeEye.xyz);
        if (dist > radius * kSizeSlackDist && radius * uSizeEye.w / dist < uMinScreenHeight * kSizeSlackHeight) return false;
    }
    return true;
}

// One workgroup per group: test its slots 64 at a time and append the survivors in slot order (a prefix sum per
// chunk), then give the group's commands the count.
void main() {
    uvec4 g = uGroups[uGroupOffset + gl_WorkGroupID.x];
    uint lid = gl_LocalInvocationID.x;
    uint written = 0u;
    for (uint c = 0u; c < g.y; c += 64u) {
        uint k = c + lid;
        uint slot = k < g.y ? uGroupSlots[g.x + k] : 0u;
        bool v = k < g.y && Visible(slot);
        sScan[lid] = v ? 1u : 0u;
        barrier();
        for (uint off = 1u; off < 64u; off <<= 1u) {
            uint t = lid >= off ? sScan[lid - off] : 0u;
            barrier();
            sScan[lid] += t;
            barrier();
        }
        if (v) uRemap[g.x + written + sScan[lid] - 1u] = slot;
        uint total = sScan[63];
        barrier();
        written += total;
    }
    for (uint i = lid; i < g.w; i += 64u) uCommands[uGroupSlots[g.z + i] * 5u + 1u] = written;
}
)";
}

void BuildDrawTable(std::uint32_t slotCount, DrawTable& out) {
    out.Groups.clear();
    out.Slots.clear();
    out.GroupOf.assign(slotCount, DrawTable::kNoGroup);
    static std::vector<std::uint32_t> batched;
    static std::vector<const RenderableComponent*> owner;
    batched.clear();
    owner.clear();
    SceneSlot s;
    for (std::uint32_t i = 0; i < slotCount; ++i) {
        if (!ReadSceneSlot(i, s) || !s.Alive || !s.Renderable || (s.Flags & kCandPosed)) continue;
        if (!s.Renderable->ModelRef) continue;
        batched.push_back(i);
        owner.push_back(s.Renderable);
    }
    static InstanceGroups groups;
    GroupInstances(
        batched.size(),
        [&](std::size_t k) {
            const RenderableComponent& r = *owner[k];
            return HashSlots(r.Materials, (std::uint64_t)(std::uintptr_t)r.ModelRef.get() * 0x9E3779B97F4A7C15ull);
        },
        [&](std::uint32_t a, std::uint32_t b) {
            return owner[a]->ModelRef.get() == owner[b]->ModelRef.get() && SameSlots(owner[a]->Materials, owner[b]->Materials);
        },
        groups);
    out.RunSlots = (std::uint32_t)batched.size();
    out.Slots.resize(batched.size() + groups.Groups.size());
    out.Groups.reserve(groups.Groups.size());
    for (std::size_t g = 0; g < groups.Groups.size(); ++g) {
        const InstanceGroups::Group& run = groups.Groups[g];
        // One command per group (command g), whose instanceCount is the survivor count; its index follows the runs.
        const std::uint32_t at = (std::uint32_t)(batched.size() + g);
        out.Slots[at] = (std::uint32_t)g;
        out.Groups.push_back({run.First, run.Count, at, 1});
        for (std::uint32_t k = 0; k < run.Count; ++k) {
            const std::uint32_t slot = batched[groups.Order[run.First + k]];
            out.Slots[run.First + k] = slot;
            out.GroupOf[slot] = (std::uint32_t)g;
        }
    }
}

namespace {
VerifyTotals g_Totals;

struct Resources {
    std::unique_ptr<Shader> Program;
    bool Failed = false;
    unsigned Groups = 0, Slots = 0, Lod = 0, Remap = 0, Commands = 0;
};
Resources& Res() {
    static Resources* r = new Resources(); // never destroyed: the GL context is gone by the time statics are
    return *r;
}
bool EnsureProgram() {
    Resources& r = Res();
    if (r.Program || r.Failed) return !r.Failed;
    try {
        r.Program = std::make_unique<Shader>(std::string(ComputeSource()), "GpuCull");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[GpuCull] compute program failed, GPU culling off: %s\n", e.what());
    }
    if (!r.Program || !r.Program->Program()) {
        r.Program.reset();
        r.Failed = true;
        return false;
    }
    glCreateBuffers(1, &r.Groups);
    glCreateBuffers(1, &r.Slots);
    glCreateBuffers(1, &r.Lod);
    glCreateBuffers(1, &r.Remap);
    glCreateBuffers(1, &r.Commands);
    return true;
}

void SetQueryUniforms(Shader& prog, const ViewQuery& vq) {
    static const char* const kPlaneNames[6] = {"uPlanes[0]", "uPlanes[1]", "uPlanes[2]",
                                               "uPlanes[3]", "uPlanes[4]", "uPlanes[5]"};
    for (int i = 0; i < 6; ++i) prog.SetVec4(kPlaneNames[i], vq.Planes[i]);
    prog.SetVec4("uWithinMin", vq.WithinMin);
    prog.SetVec4("uWithinMax", vq.WithinMax);
    prog.SetVec4("uSizeEye", vq.SizeEye);
    glUniform1ui(prog.Loc("uRejectFlags"), vq.RejectFlags);
    glUniform1ui(prog.Loc("uLayerMask"), vq.LayerMask);
    glUniform1ui(prog.Loc("uBits"), vq.Bits);
    prog.SetFloat("uMinScreenHeight", vq.MinScreenHeight);
}

void Dispatch(Shader& prog, std::uint32_t groupCount) {
    const int offsetLoc = prog.Loc("uGroupOffset");
    constexpr std::uint32_t kMaxGroupsPerDispatch = 65535; // GL's guaranteed minimum for one dimension
    for (std::uint32_t first = 0; first < groupCount; first += kMaxGroupsPerDispatch) {
        glUniform1ui(offsetLoc, first);
        prog.DispatchCompute(std::min(kMaxGroupsPerDispatch, groupCount - first), 1, 1);
    }
}

// A GPU-only store of at least `bytes` (grow-only; its old contents are not kept).
void EnsureStore(unsigned& buf, std::size_t& capacity, std::size_t bytes) {
    if (buf && bytes <= capacity) return;
    if (buf) glDeleteBuffers(1, &buf);
    capacity = std::max<std::size_t>(bytes + bytes / 2, 4096);
    glCreateBuffers(1, &buf);
    glNamedBufferStorage(buf, (GLsizeiptr)capacity, nullptr, 0);
}
std::size_t g_StageACap = 0, g_FinalCap = 0;
} // namespace

bool DrawsEnabled() {
    static const bool off = Env::Flag(Env::Var::NoGpuCull);
    return !off && GpuScene::Enabled() && MultiDrawEnabled() && EnsureProgram();
}

void UploadTable(DrawBuffers& b, const std::vector<std::uint32_t>& slots, std::uint32_t runSlots) {
    if (!b.Slots) glCreateBuffers(1, &b.Slots);
    static const std::uint32_t none[4] = {};
    const std::size_t bytes = slots.size() * sizeof(std::uint32_t);
    glNamedBufferData(b.Slots, (GLsizeiptr)std::max<std::size_t>(bytes, 16), bytes ? (const void*)slots.data() : none,
                      GL_STATIC_DRAW);
    EnsureStore(b.StageA, g_StageACap, std::max<std::size_t>(runSlots, 1) * sizeof(std::uint32_t));
    EnsureStore(b.Final, g_FinalCap, std::max<std::size_t>(runSlots, 1) * sizeof(std::uint32_t));
    b.RunSlots = runSlots;
}

bool CullView(const DrawBuffers& b, const std::vector<Group>& groups, const ViewQuery& q,
              const std::vector<std::uint32_t>& reject) {
    GpuScene& scene = GpuScene::Get();
    if (groups.empty() || !b.Slots || !scene.Ready() || !scene.BoundsBuffer() || scene.Count() == 0 || !EnsureProgram())
        return false;
    PROFILE_GPU_SCOPE("GPU Cull");
    const std::uint32_t none = 0;
    const std::size_t rejectBytes = std::max<std::size_t>(reject.size(), 1) * sizeof(std::uint32_t);
    if (!InstanceBuffer::UploadRange(25, reject.empty() ? &none : reject.data(), rejectBytes)) {
        Resources& r = Res();
        glNamedBufferData(r.Lod, (GLsizeiptr)rejectBytes, reject.empty() ? &none : reject.data(), GL_STREAM_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 25, r.Lod);
    }
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 22, scene.BoundsBuffer(), 0,
                      (GLsizeiptr)scene.Count() * 2 * (GLsizeiptr)sizeof(glm::vec4));
    if (!InstanceBuffer::UploadRange(23, groups.data(), groups.size() * sizeof(Group))) {
        Resources& r = Res();
        glNamedBufferData(r.Groups, (GLsizeiptr)(groups.size() * sizeof(Group)), groups.data(), GL_STREAM_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 23, r.Groups);
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 24, b.Slots);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 28, b.StageA);
    Shader& prog = *Res().Program;
    prog.Bind();
    SetQueryUniforms(prog, q);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); // the GPU scene's uploads land first
    Dispatch(prog, (std::uint32_t)groups.size());
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    GLStateCache::Invalidate();
    return true;
}

const VerifyTotals& Totals() { return g_Totals; }

void NoteBatches(std::uint64_t drawn, const std::uint64_t (&empty)[VerifyTotals::kEmptyCauses]) {
    g_Totals.BatchesDrawn += drawn;
    for (int i = 0; i < VerifyTotals::kEmptyCauses; ++i) g_Totals.BatchesEmpty[i] += empty[i];
}

void NoteGroupSkips(std::uint64_t checked, std::uint64_t missing) {
    g_Totals.GroupSkipChecked += checked;
    g_Totals.GroupSkipMissing += missing;
    if (missing) Log::Error("[GpuCull] verify: " + std::to_string(missing) + " members of skipped groups would be drawn");
}

void VerifyView(World& world, const VisibilityQuery& q) {
    GpuScene& scene = GpuScene::Get();
    scene.Sync(world);
    if (!GpuScene::Enabled() || !scene.BoundsBuffer() || scene.Count() == 0 || !EnsureProgram()) {
        ++g_Totals.Skipped;
        return;
    }
    static DrawTable table;
    BuildDrawTable(scene.Count(), table); // verify only: rebuilt every call (G2-b keys it on the candidate stamp)
    if (table.Groups.empty()) { // nothing batched: nothing to differ
        ++g_Totals.Views;
        return;
    }
    const std::uint32_t slotCount = scene.Count();

    // The view's LOD bitset (1 bit per scene slot, from LOD Select's tags) and each batched entity's slot.
    static std::vector<std::uint32_t> lod;
    lod.assign((slotCount + 31) / 32, 0u);
    static std::vector<std::uint32_t> slotOf; // entity index -> scene slot
    std::fill(slotOf.begin(), slotOf.end(), DrawTable::kNoGroup);
    const bool anyLod = !world.Registry.storage<LodCulledTag>().empty();
    {
        SceneSlot s;
        for (std::uint32_t k = 0; k < table.RunSlots; ++k) {
            const std::uint32_t slot = table.Slots[k];
            if (!ReadSceneSlot(slot, s)) continue;
            if (anyLod && world.IsLodCulled(s.Entity)) lod[slot >> 5] |= 1u << (slot & 31u);
            const std::size_t e = (std::size_t)entt::to_entity(s.Entity);
            if (e >= slotOf.size()) slotOf.resize(e + 1, DrawTable::kNoGroup);
            slotOf[e] = slot;
        }
    }

    Resources& r = Res();
    glNamedBufferData(r.Groups, (GLsizeiptr)(table.Groups.size() * sizeof(Group)), table.Groups.data(), GL_STREAM_DRAW);
    glNamedBufferData(r.Slots, (GLsizeiptr)(table.Slots.size() * sizeof(std::uint32_t)), table.Slots.data(), GL_STREAM_DRAW);
    glNamedBufferData(r.Lod, (GLsizeiptr)(lod.size() * sizeof(std::uint32_t)), lod.data(), GL_STREAM_DRAW);
    glNamedBufferData(r.Remap, (GLsizeiptr)(table.Slots.size() * sizeof(std::uint32_t)), nullptr, GL_STREAM_READ);
    static std::vector<IndirectCommand> commands;
    commands.assign(table.Groups.size(), IndirectCommand{0, 0, 0, 0, 0});
    glNamedBufferData(r.Commands, (GLsizeiptr)(commands.size() * sizeof(IndirectCommand)), commands.data(), GL_STREAM_READ);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 22, scene.BoundsBuffer(), 0,
                      (GLsizeiptr)slotCount * 2 * (GLsizeiptr)sizeof(glm::vec4));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 23, r.Groups);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 24, r.Slots);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 25, r.Lod);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 28, r.Remap);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 29, r.Commands);

    const ViewQuery vq = MakeQuery(q);
    Shader& prog = *r.Program;
    prog.Bind();
    SetQueryUniforms(prog, vq);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); // the GPU scene's uploads land first
    Dispatch(prog, (std::uint32_t)table.Groups.size());
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    static std::vector<std::uint32_t> remap;
    remap.resize(table.Slots.size());
    glGetNamedBufferSubData(r.Remap, 0, (GLsizeiptr)(remap.size() * sizeof(std::uint32_t)), remap.data());
    glGetNamedBufferSubData(r.Commands, 0, (GLsizeiptr)(commands.size() * sizeof(IndirectCommand)), commands.data());
    GLStateCache::Invalidate();

    // The CPU's answer for the same query: each group's slots in GatherVisible's order.
    static std::vector<VisibleRenderable> visible;
    GatherVisible(world, q, visible);
    static std::vector<std::vector<std::uint32_t>> expected;
    if (expected.size() < table.Groups.size()) expected.resize(table.Groups.size());
    for (std::size_t g = 0; g < table.Groups.size(); ++g) expected[g].clear();
    for (const VisibleRenderable& v : visible) {
        const std::size_t e = (std::size_t)entt::to_entity(v.Entity);
        if (e >= slotOf.size() || slotOf[e] == DrawTable::kNoGroup) continue; // not batched (posed, ...)
        expected[table.GroupOf[slotOf[e]]].push_back(slotOf[e]);
    }
    static std::vector<std::uint8_t> mark;
    mark.assign(slotCount, 0);
    std::uint64_t missing = 0, orderBad = 0, cpuKept = 0, gpuKept = 0;
    for (std::size_t g = 0; g < table.Groups.size(); ++g) {
        const Group& grp = table.Groups[g];
        const std::uint32_t n = std::min(commands[g].InstanceCount, grp.Count);
        const std::uint32_t* got = remap.data() + grp.Base;
        for (std::uint32_t k = 0; k < n; ++k)
            if (got[k] < slotCount) mark[got[k]] = 1;
        // GatherVisible's survivors must all be there, in its order (the GPU's extras may sit between them).
        std::uint32_t at = 0;
        bool ordered = true;
        for (std::uint32_t slot : expected[g]) {
            if (!mark[slot]) {
                ++missing;
                continue;
            }
            while (at < n && got[at] != slot) ++at;
            if (at == n) ordered = false;
            else ++at;
        }
        orderBad += !ordered;
        cpuKept += expected[g].size();
        gpuKept += n;
        for (std::uint32_t k = 0; k < n; ++k)
            if (got[k] < slotCount) mark[got[k]] = 0;
    }
    const std::uint64_t extra = gpuKept - (cpuKept - missing); // what the GPU kept beyond the expected ones it kept
    ++g_Totals.Views;
    g_Totals.Expected += cpuKept;
    g_Totals.Missing += missing;
    g_Totals.Extra += extra;
    g_Totals.OrderBad += orderBad;
    const int kind = q.Cull ? VerifyTotals::kFramed : (q.CellCull && q.CellCullCount > 0) ? VerifyTotals::kCellOnly
                                                                                          : VerifyTotals::kUnculled;
    ++g_Totals.KindViews[kind];
    g_Totals.KindExpected[kind] += cpuKept;
    g_Totals.KindExtra[kind] += extra;
    if (missing || orderBad)
        Log::Error("[GpuCull] verify: " + std::to_string(missing) + " missing, " + std::to_string(orderBad) +
                   " groups out of order (" + std::to_string(cpuKept) + " expected in " +
                   std::to_string(table.Groups.size()) + " groups)");
    static std::uint64_t reported = 0;
    if (g_Totals.Views == 1 || g_Totals.Views - reported >= 120) {
        reported = g_Totals.Views;
        std::printf("[GpuCull] verify: %llu views, %llu expected, %llu missing, %llu extra, %llu out of order, %llu skipped\n",
                    (unsigned long long)g_Totals.Views, (unsigned long long)g_Totals.Expected,
                    (unsigned long long)g_Totals.Missing, (unsigned long long)g_Totals.Extra,
                    (unsigned long long)g_Totals.OrderBad, (unsigned long long)g_Totals.Skipped);
        std::printf("[GpuCull] verify   group skips: %llu members checked, %llu missing\n",
                    (unsigned long long)g_Totals.GroupSkipChecked, (unsigned long long)g_Totals.GroupSkipMissing);
        std::printf("[GpuCull] verify   table batches drawn: %llu, empty: %llu LOD, %llu size, %llu frustum, %llu other\n",
                    (unsigned long long)g_Totals.BatchesDrawn, (unsigned long long)g_Totals.BatchesEmpty[0],
                    (unsigned long long)g_Totals.BatchesEmpty[1], (unsigned long long)g_Totals.BatchesEmpty[2],
                    (unsigned long long)g_Totals.BatchesEmpty[3]);
        static const char* const kKindNames[VerifyTotals::kKinds] = {"framed", "cell-only", "unculled"};
        for (int k = 0; k < VerifyTotals::kKinds; ++k)
            if (g_Totals.KindViews[k])
                std::printf("[GpuCull] verify   %s: %llu views, %llu expected, %llu extra\n", kKindNames[k],
                            (unsigned long long)g_Totals.KindViews[k], (unsigned long long)g_Totals.KindExpected[k],
                            (unsigned long long)g_Totals.KindExtra[k]);
        std::fflush(stdout);
    }
}

} // namespace GpuCull
