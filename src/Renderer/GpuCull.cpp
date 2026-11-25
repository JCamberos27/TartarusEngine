#include "GpuCull.h"

#include "Core/Env.h"
#include "Physics/Frustum.h"
#include "SceneRenderer.h"

#include <cmath>

namespace GpuCull {

ViewQuery MakeQuery(const VisibilityQuery& q) {
    ViewQuery v;
    // GatherVisible's flag tests, in its order: inactive / pose source, the split pose's copy for this view, view
    // models, the player body, the editor's hide, Cast Shadows, transparent.
    v.RejectFlags = kSlotInactive | kSlotPoseSource | kSlotNoModel;
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
    if (q.SizeCullEye) {
        v.Bits |= kQHasSizeCull;
        v.SizeEye = glm::vec4(*q.SizeCullEye, q.SizeCullProjScale);
        v.MinScreenHeight = q.MinScreenHeight;
    }
    return v;
}

bool PassesStageA(const SlotData& s, const ViewQuery& q, bool lodCulled) {
    if (s.Flags & q.RejectFlags) return false;
    if (lodCulled) return false;
    if ((q.Bits & kQCheckLayer) && s.Layer >= 0 && s.Layer < 32 && !((q.LayerMask >> s.Layer) & 1u)) return false;
    if (!(s.Flags & kSlotBounded)) return true;
    if (q.Bits & kQHasFrustum) {
        // Frustum::Intersects: the box's corner furthest along each plane's normal must not be behind it.
        for (const glm::vec4& pl : q.Planes) {
            const glm::vec3 p(pl.x >= 0.0f ? s.Max.x : s.Min.x, pl.y >= 0.0f ? s.Max.y : s.Min.y,
                              pl.z >= 0.0f ? s.Max.z : s.Min.z);
            if (glm::dot(glm::vec3(pl), p) + pl.w < 0.0f) return false;
        }
    }
    if (q.Bits & kQHasWithin) {
        const glm::vec3 wmin(q.WithinMin), wmax(q.WithinMax);
        if (!(wmin.x <= s.Max.x && wmax.x >= s.Min.x && wmin.y <= s.Max.y && wmax.y >= s.Min.y && wmin.z <= s.Max.z &&
              wmax.z >= s.Min.z))
            return false;
    }
    if (q.Bits & kQHasSizeCull) {
        // TooSmallOnScreen, op for op.
        const glm::vec3 centre = (s.Min + s.Max) * 0.5f;
        const float radius = glm::length(s.Max - s.Min) * 0.5f;
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
// TODO(G2-a, after G1-a lands): bind GpuScene's slot buffer + this file's group/command/LOD buffers; binding numbers
// follow W2's G-1 rule (no new bindings: reuse the HiZ compute's 22-24 per dispatch, remap 28, commands 29).
const char* ComputeSource() {
    return R"(#version 460 core
layout(local_size_x = 64) in;
struct Slot { vec3 mn; uint flags; vec3 mx; int layer; };
layout(std430, binding = 22) readonly buffer SlotBlock { Slot uSlots[]; };
layout(std430, binding = 23) readonly buffer GroupBlock { uvec4 uGroups[]; };
layout(std430, binding = 24) readonly buffer CmdListBlock { uint uCmdList[]; };
layout(std430, binding = 29) buffer CommandBlock { uint uCommands[]; };
layout(std430, binding = 28) writeonly buffer RemapBlock { int uRemap[]; };
layout(std430, binding = 25) readonly buffer LodBlock { uint uLodCulled[]; }; // 1 bit per slot
uniform vec4 uPlanes[6];
uniform vec4 uWithinMin, uWithinMax, uSizeEye;
uniform uint uRejectFlags, uLayerMask, uBits;
uniform float uMinScreenHeight;
shared uint sScan[64];

const uint kBounded = 1024u;

bool Visible(uint slot) {
    Slot s = uSlots[slot];
    if ((s.flags & uRejectFlags) != 0u) return false;
    if (((uLodCulled[slot >> 5u] >> (slot & 31u)) & 1u) != 0u) return false;
    if ((uBits & 8u) != 0u && s.layer >= 0 && s.layer < 32 && ((uLayerMask >> uint(s.layer)) & 1u) == 0u) return false;
    if ((s.flags & kBounded) == 0u) return true;
    if ((uBits & 1u) != 0u) {
        for (int i = 0; i < 6; ++i) {
            vec4 pl = uPlanes[i];
            vec3 p = vec3(pl.x >= 0.0 ? s.mx.x : s.mn.x, pl.y >= 0.0 ? s.mx.y : s.mn.y, pl.z >= 0.0 ? s.mx.z : s.mn.z);
            if (dot(pl.xyz, p) + pl.w < 0.0) return false;
        }
    }
    if ((uBits & 2u) != 0u && !all(lessThanEqual(uWithinMin.xyz, s.mx)) ) return false;
    if ((uBits & 2u) != 0u && !all(greaterThanEqual(uWithinMax.xyz, s.mn))) return false;
    if ((uBits & 4u) != 0u) {
        vec3 centre = (s.mn + s.mx) * 0.5;
        float radius = length(s.mx - s.mn) * 0.5;
        float dist = length(centre - uSizeEye.xyz);
        if (dist > radius && radius * uSizeEye.w / dist < uMinScreenHeight) return false;
    }
    return true;
}

void main() {
    uvec4 g = uGroups[gl_WorkGroupID.x];
    uint lid = gl_LocalInvocationID.x;
    uint written = 0u;
    for (uint c = 0u; c < g.y; c += 64u) {
        uint k = c + lid;
        bool v = k < g.y && Visible(g.x + k);
        sScan[lid] = v ? 1u : 0u;
        barrier();
        for (uint off = 1u; off < 64u; off <<= 1u) {
            uint t = lid >= off ? sScan[lid - off] : 0u;
            barrier();
            sScan[lid] += t;
            barrier();
        }
        if (v) uRemap[g.x + written + sScan[lid] - 1u] = int(g.x + k);
        uint total = sScan[63];
        barrier();
        written += total;
    }
    for (uint i = lid; i < g.w; i += 64u) uCommands[uCmdList[g.z + i] * 5u + 1u] = written;
}
)";
}

} // namespace GpuCull
