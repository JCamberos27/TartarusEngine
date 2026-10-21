#pragma once
#include <glm/glm.hpp>
#include <cstdint>

// Shared constants and the GPU record for clustered decals (docs/DECALS.md). The CPU side
// (DecalSystem), the cull compute (ClusterDecalCull.comp.glsl) and the model fragment shader
// (ModelDecals.glsl) all agree on this layout; keep the GLSL copies in sync with it.
namespace Decals {

// Budget (interview 2026-10-21): 4096 visible decals, 256 per cluster.
constexpr int MAX_VISIBLE = 4096;
constexpr int MAX_PER_CLUSTER = 256;
constexpr int INDEX_CAPACITY = 131072; // global compacted decal-index list, shared by all clusters

// SSBO bindings (11-14 were free; 0-10, 15, 20, 21, 25-27 are taken).
constexpr int BIND_DECALS = 11;  // GpuDecal[]
constexpr int BIND_BOUNDS = 12;  // GpuDecalBounds[] (cull only)
constexpr int BIND_RANGES = 13;  // per-cluster {offset, count}
constexpr int BIND_INDICES = 14; // uint[2] header {cursor, overflow} + uint[INDEX_CAPACITY]

// Texture units reserved for the decal arrays. Materials are clamped to units < 22.
constexpr int UNIT_ALBEDO = 22;  // sampler2DArray, sRGB BC3: rgb colour, a opacity
constexpr int UNIT_NORMAL = 23;  // sampler2DArray, BC5: tangent-space xy
constexpr int UNIT_SURFACE = 24; // sampler2DArray, BC3 linear: r rough, g metal, b AO, a height
constexpr int UNIT_AUX = 25;     // sampler2DArray, BC4: emissive / peel mask
constexpr int UNIT_GRUNGE = 21;  // material Grunge map (triplanar), Standard shaders only

constexpr int PAGE_SIZE = 4096;  // every array layer is a 4096^2 page, quadtree-subdivided

// Channel bits in GpuDecal::Ids.x >> 16.
enum Channel : uint32_t {
    CH_ALBEDO = 1u << 0,
    CH_NORMAL = 1u << 1,
    CH_ROUGHNESS = 1u << 2,
    CH_METAL = 1u << 3,
    CH_AO = 1u << 4,
    CH_EMISSIVE = 1u << 5,
    CH_HEIGHT_BLEND = 1u << 6, // paint sinks into the receiver's cracks/cavities
    CH_PAINT = 1u << 7,        // paint material response + weathering terms
};

// Flag bits in GpuDecal::Ids.z.
enum Flag : uint32_t {
    FLAG_STATIC_RECEIVERS = 1u << 0,
    FLAG_DYNAMIC_RECEIVERS = 1u << 1,
    FLAG_UV_PAINT = 1u << 2,       // emissive channel only glows under UV light / "blacklight"
    FLAG_HAS_NORMAL_TEX = 1u << 3,
    FLAG_HAS_SURFACE_TEX = 1u << 4,
    FLAG_HAS_AUX_TEX = 1u << 5,
    FLAG_LUMA_MASK = 1u << 6,      // opacity comes from albedo luminance (white-on-black alphas)
    FLAG_INVERT_MASK = 1u << 7,    // opacity inverted (DecalAsset::InvertMask: dark marks on light)
};
// GpuDecal::Ids.z also carries the height-blend amount (DecalAsset::HeightBlend) as unorm8 in its
// top byte: every half in H0-H2 is taken, and CH_HEIGHT_BLEND alone can't say how much.
constexpr uint32_t HEIGHT_BLEND_SHIFT = 24;

// 128 bytes, std430. Rows of WorldToBox map world space into the unit box [-0.5, 0.5]^3;
// +Z is the projection axis pointing out of the surface. Rotation/scale/flip are folded in.
struct GpuDecal {
    glm::vec4 WorldToBox[3];
    glm::vec4 Rect;     // xy = slot UV offset in the layer, zw = slot UV size (atlas frame folded in)
    glm::uvec4 Ids;     // x: layer | maxLod << 8 | channelMask << 16; y: receiver layer mask;
                        // z: Flag bits | heightBlend << HEIGHT_BLEND_SHIFT; w: sort key
    glm::uvec4 H0;      // packHalf2x16: (tint.r, tint.g) (tint.b, albedoOp) (normalOp, roughOp) (metalOp, aoOp)
    glm::uvec4 H1;      // (emissive.r, emissive.g) (emissive.b, emissiveOp) (angleCosStart, angleCosEnd) (edgeFade, heightContrast)
    glm::uvec4 H2;      // (paintRough, paintMetal) (relief, wear) (fade, bleach) (peel, buff)
};
static_assert(sizeof(GpuDecal) == 128, "GpuDecal must stay 128 bytes (std430)");

// World-space oriented box for the cull compute: centre + three half-extent axes.
struct GpuDecalBounds {
    glm::vec4 Centre;
    glm::vec4 AxisX, AxisY, AxisZ;
};
static_assert(sizeof(GpuDecalBounds) == 64, "GpuDecalBounds must stay 64 bytes");

} // namespace Decals
