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
layout(std430, binding = 23) readonly buffer GroupBlock { uvec4 uGroups[]; };    // slot base, count, cmd first, cmd count
layout(std430, binding = 24) readonly buffer GroupSlotBlock { uint uGroupSlots[]; }; // the group runs' scene slots
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

#ifdef CASTERS
// G4-a caster mode: per cascade, CascadedShadowMap::CasterSlice + its frustum planes (GpuCull::SunCascade).
struct SunCascade { mat4 vp; vec4 rect; vec4 params; vec4 axes[8]; vec4 planes[6]; };
layout(std140, binding = 5) uniform SunCasterBlock { SunCascade uCascade[4]; uvec4 uSunInfo; }; // x render mask, y count
// The footprint's light clip-space slack: a tenth of a texel at 2048, far above the float error near a slice (where
// the coordinates are within a few units), so a box the CPU keeps is never dropped.
const float kNdcSlack = 1e-4;

// The cascades (bit c) a slot that passed the flag tests goes into: castsInto (ShadowFrame::DrawSunCasters).
uint CasterMask(vec4 mn, vec4 mx) {
    uint mask = 0u;
    uint n = min(uSunInfo.y, 4u);
    for (uint c = 0u; c < n; ++c) {
        if (((uSunInfo.x >> c) & 1u) == 0u) continue; // held from last frame
        if (mn.w == 0.0) { mask |= 1u << c; continue; } // unbounded: every cascade drawn
        bool inside = true;
        for (int i = 0; i < 6 && inside; ++i) {
            vec4 pl = uCascade[c].planes[i];
            vec3 p = vec3(pl.x >= 0.0 ? mx.x : mn.x, pl.y >= 0.0 ? mx.y : mn.y, pl.z >= 0.0 ? mx.z : mn.z);
            inside = dot(pl.xyz, p) + pl.w >= -kPlaneSlack;
        }
        if (!inside) continue;
        vec2 lo = vec2(3.4e38), hi = vec2(-3.4e38);
        for (int k = 0; k < 8; ++k) {
            vec3 p = vec3((k & 1) != 0 ? mx.x : mn.x, (k & 2) != 0 ? mx.y : mn.y, (k & 4) != 0 ? mx.z : mn.z);
            vec4 q = uCascade[c].vp * vec4(p, 1.0);
            vec2 xy = q.xy / q.w;
            lo = min(lo, xy);
            hi = max(hi, xy);
        }
        vec4 r = uCascade[c].rect, pr = uCascade[c].params;
        float m = (pr.z != 0.0 ? 0.0 : pr.x) + kNdcSlack;
        lo -= vec2(m);
        hi += vec2(m);
        if (r.z < lo.x || r.x > hi.x || r.w < lo.y || r.y > hi.y) continue;
        if (pr.z == 0.0) { // cascades 1-3: the slice hull's separating axes
            vec2 mid = (lo + hi) * 0.5, hh = (hi - lo) * 0.5;
            bool apart = false;
            for (int a = 0; a < int(pr.y) && !apart; ++a) {
                vec4 ax = uCascade[c].axes[a];
                float centre = dot(ax.xy, mid), radius = abs(ax.x) * hh.x + abs(ax.y) * hh.y;
                apart = ax.w < centre - radius || centre + radius < ax.z;
            }
            if (apart) continue;
        }
        mask |= 1u << c;
    }
    return mask;
}
#endif

// One workgroup per group: test its slots 64 at a time and append the survivors in slot order (a prefix sum per
// chunk), then give the group's commands the count. Caster mode appends one entry per (slot, cascade) instead, into a
// run 4x the group's.
void main() {
    uvec4 g = uGroups[uGroupOffset + gl_WorkGroupID.x];
    uint lid = gl_LocalInvocationID.x;
    uint written = 0u;
    for (uint c = 0u; c < g.y; c += 64u) {
        uint k = c + lid;
        uint slot = k < g.y ? uGroupSlots[g.x + k] : 0u;
#ifdef CASTERS
        uint mask = k < g.y && Visible(slot) ? CasterMask(uBounds[slot * 2u], uBounds[slot * 2u + 1u]) : 0u;
        uint n = uint(bitCount(mask));
#else
        uint n = k < g.y && Visible(slot) ? 1u : 0u;
#endif
        sScan[lid] = n;
        barrier();
        for (uint off = 1u; off < 64u; off <<= 1u) {
            uint t = lid >= off ? sScan[lid - off] : 0u;
            barrier();
            sScan[lid] += t;
            barrier();
        }
#ifdef CASTERS
        uint at = g.x * 4u + written + sScan[lid] - n;
        for (uint cc = 0u; cc < 4u; ++cc)
            if (((mask >> cc) & 1u) != 0u) uRemap[at++] = slot | (cc << 30u);
#else
        if (n != 0u) uRemap[g.x + written + sScan[lid] - 1u] = slot;
#endif
        uint total = sScan[63];
        barrier();
        written += total;
    }
    for (uint i = lid; i < g.w; i += 64u) uCommands[(g.z + i) * 5u + 1u] = written;
}
)";
}

const char* CasterComputeSource() {
    static const std::string source = [] {
        std::string s = ComputeSource();
        const std::size_t eol = s.find('\n');
        return s.substr(0, eol + 1) + "#define CASTERS 1\n" + s.substr(eol + 1);
    }();
    return source.c_str();
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
    out.Slots.resize(batched.size());
    out.Groups.reserve(groups.Groups.size());
    for (std::size_t g = 0; g < groups.Groups.size(); ++g) {
        const InstanceGroups::Group& run = groups.Groups[g];
        // G2-a: one command per group, whose instanceCount is the survivor count (G2-b gives each mesh its own).
        out.Groups.push_back({run.First, run.Count, (std::uint32_t)g, 1});
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
    std::unique_ptr<Shader> CasterProgram; // G4-a caster mode
    bool CasterFailed = false;
    unsigned SunBlock = 0;
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

bool EnsureCasterProgram() {
    Resources& r = Res();
    if (!EnsureProgram()) return false; // the buffers
    if (r.CasterProgram || r.CasterFailed) return !r.CasterFailed;
    try {
        r.CasterProgram = std::make_unique<Shader>(std::string(CasterComputeSource()), "GpuCull (casters)");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[GpuCull] caster compute program failed: %s\n", e.what());
    }
    if (!r.CasterProgram || !r.CasterProgram->Program()) {
        r.CasterProgram.reset();
        r.CasterFailed = true;
        return false;
    }
    glCreateBuffers(1, &r.SunBlock);
    return true;
}

// The view's LOD bitset (1 bit per scene slot, from LOD Select's tags) over the draw table's slots.
void LodBits(World& world, const DrawTable& table, std::uint32_t slotCount, std::vector<std::uint32_t>& lod) {
    lod.assign((slotCount + 31) / 32, 0u);
    if (world.Registry.storage<LodCulledTag>().empty()) return;
    SceneSlot s;
    for (std::uint32_t slot : table.Slots)
        if (ReadSceneSlot(slot, s) && world.IsLodCulled(s.Entity)) lod[slot >> 5] |= 1u << (slot & 31u);
}

// Uploads the draw table and the LOD bits, runs `prog` (already bound, its uniforms set) over every group, and reads
// the remap (`remapScale` entries per table slot) and the commands back. Waits on the GPU.
void DispatchAndRead(Shader& prog, const GpuScene& scene, const DrawTable& table, const std::vector<std::uint32_t>& lod,
                     std::uint32_t remapScale, std::vector<std::uint32_t>& remap, std::vector<IndirectCommand>& commands) {
    Resources& r = Res();
    const std::uint32_t slotCount = scene.Count();
    glNamedBufferData(r.Groups, (GLsizeiptr)(table.Groups.size() * sizeof(Group)), table.Groups.data(), GL_STREAM_DRAW);
    glNamedBufferData(r.Slots, (GLsizeiptr)(table.Slots.size() * sizeof(std::uint32_t)), table.Slots.data(), GL_STREAM_DRAW);
    glNamedBufferData(r.Lod, (GLsizeiptr)(lod.size() * sizeof(std::uint32_t)), lod.data(), GL_STREAM_DRAW);
    remap.resize(table.Slots.size() * remapScale);
    glNamedBufferData(r.Remap, (GLsizeiptr)(remap.size() * sizeof(std::uint32_t)), nullptr, GL_STREAM_READ);
    commands.assign(table.Groups.size(), IndirectCommand{0, 0, 0, 0, 0});
    glNamedBufferData(r.Commands, (GLsizeiptr)(commands.size() * sizeof(IndirectCommand)), commands.data(), GL_STREAM_READ);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 22, scene.BoundsBuffer(), 0,
                      (GLsizeiptr)slotCount * 2 * (GLsizeiptr)sizeof(glm::vec4));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 23, r.Groups);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 24, r.Slots);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 25, r.Lod);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 28, r.Remap);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 29, r.Commands);
    const int offsetLoc = prog.Loc("uGroupOffset");
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); // the GPU scene's uploads land first
    constexpr std::uint32_t kMaxGroupsPerDispatch = 65535; // GL's guaranteed minimum for one dimension
    const std::uint32_t groupCount = (std::uint32_t)table.Groups.size();
    for (std::uint32_t first = 0; first < groupCount; first += kMaxGroupsPerDispatch) {
        glUniform1ui(offsetLoc, first);
        prog.DispatchCompute(std::min(kMaxGroupsPerDispatch, groupCount - first), 1, 1);
    }
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    glGetNamedBufferSubData(r.Remap, 0, (GLsizeiptr)(remap.size() * sizeof(std::uint32_t)), remap.data());
    glGetNamedBufferSubData(r.Commands, 0, (GLsizeiptr)(commands.size() * sizeof(IndirectCommand)), commands.data());
    GLStateCache::Invalidate();
}

// The query's uniforms (stage A's flag tests and, outside caster mode, its frustum / Within / size cull).
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

} // namespace

const VerifyTotals& Totals() { return g_Totals; }

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

    // The view's LOD bitset and each batched entity's slot.
    static std::vector<std::uint32_t> lod;
    LodBits(world, table, slotCount, lod);
    static std::vector<std::uint32_t> slotOf; // entity index -> scene slot
    std::fill(slotOf.begin(), slotOf.end(), DrawTable::kNoGroup);
    {
        SceneSlot s;
        for (std::uint32_t slot : table.Slots) {
            if (!ReadSceneSlot(slot, s)) continue;
            const std::size_t e = (std::size_t)entt::to_entity(s.Entity);
            if (e >= slotOf.size()) slotOf.resize(e + 1, DrawTable::kNoGroup);
            slotOf[e] = slot;
        }
    }

    Shader& prog = *Res().Program;
    prog.Bind();
    SetQueryUniforms(prog, MakeQuery(q));
    static std::vector<std::uint32_t> remap;
    static std::vector<IndirectCommand> commands;
    DispatchAndRead(prog, scene, table, lod, 1, remap, commands);

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
        std::fflush(stdout);
    }
}

// ---- G4-a: the sun's shadow casters ----

static_assert(sizeof(CascadedShadowMap::CasterSlice) == 14 * sizeof(glm::vec4), "CasterSlice: std140 SunCascade's first members");
static_assert(sizeof(SunCascade) == 20 * sizeof(glm::vec4), "SunCascade = the GLSL struct (std140)");
static_assert(sizeof(SunCasterBlock) == (4 * 20 + 1) * sizeof(glm::vec4), "SunCasterBlock = the GLSL block (std140)");

SunCasterBlock MakeSunCasterBlock(const CascadedShadowMap& map, const Frustum* cells, unsigned renderMask,
                                  float kernelTexels, float normalBias) {
    SunCasterBlock b;
    const int n = std::min(map.Count(), CascadedShadowMap::kMaxCascades);
    for (int c = 0; c < n; ++c) {
        b.Cascades[c].Slice = map.CasterSliceOf(c, kernelTexels, normalBias);
        for (int i = 0; i < 6; ++i) b.Cascades[c].Planes[i] = glm::vec4(cells[c].Planes[i].Normal, cells[c].Planes[i].D);
    }
    b.Info = glm::uvec4(renderMask & ((1u << n) - 1u), (unsigned)n, 0u, 0u);
    return b;
}

unsigned CasterCascades(const glm::vec4 bounds[2], const SunCasterBlock& b) {
    unsigned mask = 0;
    const glm::vec3 mn(bounds[0]), mx(bounds[1]);
    for (unsigned c = 0; c < std::min(b.Info.y, 4u); ++c) {
        if (!((b.Info.x >> c) & 1u)) continue; // held from last frame
        if (bounds[0].w == 0.0f) { mask |= 1u << c; continue; } // unbounded
        const SunCascade& sc = b.Cascades[c];
        bool inside = true; // Frustum::Intersects
        for (int i = 0; i < 6 && inside; ++i) {
            const glm::vec4& pl = sc.Planes[i];
            const glm::vec3 p(pl.x >= 0.0f ? mx.x : mn.x, pl.y >= 0.0f ? mx.y : mn.y, pl.z >= 0.0f ? mx.z : mn.z);
            inside = glm::dot(glm::vec3(pl), p) + pl.w >= 0.0f;
        }
        if (inside && CascadedShadowMap::ReachesSlice(sc.Slice, mn, mx)) mask |= 1u << c;
    }
    return mask;
}

namespace {
SunVerifyTotals g_SunTotals;
bool g_SunVerifyRequested = false;
} // namespace

const SunVerifyTotals& SunTotals() { return g_SunTotals; }
void RequestSunVerify() { g_SunVerifyRequested = true; }
bool SunVerifyDue() {
    static const bool off = Env::Flag(Env::Var::NoGpuCull);
    return !off && (VerifyEnabled() || g_SunVerifyRequested);
}

void VerifySunCasters(World& world, const SunCasterBlock& block, const std::vector<SunPair>& cpu) {
    g_SunVerifyRequested = false;
    GpuScene& scene = GpuScene::Get();
    scene.Sync(world);
    if (!GpuScene::Enabled() || !scene.BoundsBuffer() || scene.Count() == 0 || !EnsureCasterProgram()) {
        ++g_SunTotals.Skipped;
        return;
    }
    const std::uint32_t slotCount = scene.Count();
    if (slotCount > kSlotMask) { // the remap word's cascade bits would overlap the slot
        ++g_SunTotals.Skipped;
        return;
    }
    static DrawTable table;
    BuildDrawTable(slotCount, table); // verify only: rebuilt every call
    static std::vector<std::uint32_t> lod;
    LodBits(world, table, slotCount, lod);

    Resources& r = Res();
    Shader& prog = *r.CasterProgram;
    prog.Bind();
    VisibilityQuery casts; // ShadowFrame::GatherSunCasters' query: the flag tests (its CellCull is a coarse superset)
    casts.ShadowCasters = true;
    SetQueryUniforms(prog, MakeQuery(casts));
    glNamedBufferData(r.SunBlock, (GLsizeiptr)sizeof(SunCasterBlock), &block, GL_STREAM_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, kSunCasterBinding, r.SunBlock);
    static std::vector<std::uint32_t> remap;
    static std::vector<IndirectCommand> commands;
    if (!table.Groups.empty()) DispatchAndRead(prog, scene, table, lod, 4, remap, commands);
    glBindBufferBase(GL_UNIFORM_BUFFER, kSunCasterBinding, 0);

    // Per (slot, cascade): bit c of mark[slot] = the GPU put the slot in cascade c; the CPU's pairs clear theirs.
    static std::vector<std::uint8_t> mark;
    mark.assign(slotCount, 0);
    std::uint64_t gpuKept = 0;
    for (std::size_t g = 0; g < table.Groups.size(); ++g) {
        const Group& grp = table.Groups[g];
        const std::uint32_t n = std::min(commands[g].InstanceCount, grp.Count * 4u);
        const std::uint32_t* got = remap.data() + (std::size_t)grp.Base * 4u;
        for (std::uint32_t k = 0; k < n; ++k) {
            const std::uint32_t slot = got[k] & kSlotMask;
            if (slot < slotCount) mark[slot] |= (std::uint8_t)(1u << (got[k] >> kCascadeShift));
        }
        gpuKept += n;
    }
    std::uint64_t expected = 0, missing = 0;
    for (const SunPair& p : cpu) {
        if (p.Slot >= slotCount || table.GroupOf[p.Slot] == DrawTable::kNoGroup) continue; // not batched (posed, ...)
        ++expected;
        const std::uint8_t bit = (std::uint8_t)(1u << p.Cascade);
        if (mark[p.Slot] & bit) mark[p.Slot] &= (std::uint8_t)~bit;
        else ++missing;
    }
    ++g_SunTotals.Frames;
    g_SunTotals.Expected += expected;
    g_SunTotals.Missing += missing;
    g_SunTotals.Extra += gpuKept - (expected - missing);
    if (missing)
        Log::Error("[GpuCull] sun casters verify: " + std::to_string(missing) + " missing of " + std::to_string(expected) +
                   " (slot, cascade) pairs");
    static std::uint64_t reported = 0;
    if (g_SunTotals.Frames == 1 || g_SunTotals.Frames - reported >= 120) {
        reported = g_SunTotals.Frames;
        std::printf("[GpuCull] sun casters verify: %llu passes, %llu expected, %llu missing, %llu extra, %llu skipped\n",
                    (unsigned long long)g_SunTotals.Frames, (unsigned long long)g_SunTotals.Expected,
                    (unsigned long long)g_SunTotals.Missing, (unsigned long long)g_SunTotals.Extra,
                    (unsigned long long)g_SunTotals.Skipped);
        std::fflush(stdout);
    }
}

} // namespace GpuCull
