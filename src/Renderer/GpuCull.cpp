#include "GpuCull.h"

#include "Core/Env.h"
#include "Physics/Frustum.h"
#include "SceneRenderer.h"
#include "SceneViewPass.h"
#include "GpuScene.h"

#include <cmath>
#include <cstring>

namespace GpuCull {

static_assert(kSlotInactive == kCandInactive && kSlotPoseSource == kCandPoseSource &&
              kSlotOwnerViewOnly == kCandOwnerViewOnly && kSlotHiddenFromOwner == kCandHiddenFromOwner &&
              kSlotViewModel == kCandViewModel && kSlotPlayerBody == kCandPlayerBody &&
              kSlotHiddenInScene == kCandHiddenInScene && kSlotPosed == kCandPosed, "slot bits = CandidateFlag bits");
static_assert(kSlotCastOff == kSlotFlagCastOff && kSlotShadowsOnly == kSlotFlagShadowsOnly &&
              kSlotTransparent == kSlotFlagTransparent && kSlotDead == GpuScene::kDead, "slot bits = SceneSlot bits");

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

// Stage A + the ordered compaction: one workgroup per group. Inputs mirror SlotData / ViewQuery above.
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
uniform uint uRejectFlags, uLayerMask, uBits;
uniform float uMinScreenHeight;
shared uint sScan[64];

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
            if (dot(pl.xyz, p) + pl.w < 0.0) return false;
        }
    }
    if ((uBits & 2u) != 0u && !(all(lessThanEqual(uWithinMin.xyz, mx.xyz)) && all(greaterThanEqual(uWithinMax.xyz, mn.xyz))))
        return false;
    if ((uBits & 4u) != 0u) {
        vec3 centre = (mn.xyz + mx.xyz) * 0.5;
        float radius = length(mx.xyz - mn.xyz) * 0.5;
        float dist = length(centre - uSizeEye.xyz);
        if (dist > radius && radius * uSizeEye.w / dist < uMinScreenHeight) return false;
    }
    return true;
}

// One workgroup per group: test its slots 64 at a time and append the survivors in slot order (a prefix sum per
// chunk), then give the group's commands the count.
void main() {
    uvec4 g = uGroups[gl_WorkGroupID.x];
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
    for (uint i = lid; i < g.w; i += 64u) uCommands[(g.z + i) * 5u + 1u] = written;
}
)";
}

} // namespace GpuCull
