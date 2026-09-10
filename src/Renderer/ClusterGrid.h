#pragma once
#include <glm/glm.hpp>

class Shader;

// Clustered-forward light culling (#120). Owns three SSBOs — per-cluster view-space AABBs
// (binding 2, compute-internal), a per-cluster (offset, count) range into the global index
// list (binding 3), and one global compacted light-index list that every cluster's lights are
// appended into (binding 4) — and drives the two compute passes that fill them (see
// ClusterShaderSource.h). The model fragment shader reads bindings 3 and 4.
//
// The grid is a fixed GRID_X x GRID_Y screen tiling by GRID_Z exponential depth slices, so a
// fragment maps to its cluster with cheap arithmetic on gl_FragCoord + view depth.
//
// Compacted layout (#208): earlier versions gave every cluster a fixed MAX_LIGHTS_PER_CLUSTER
// slots regardless of how many lights actually touched it (3,456 * 100 * 4 bytes = 1.38 MB,
// scattered reads). Now the cull pass reserves a right-sized, contiguous block per cluster in
// one global index list (binding 4) via an atomic counter, and each cluster just records where
// its block starts and how long it is (binding 3). Total memory scales with actual light
// coverage instead of worst case, and the fragment shader's per-cluster loop reads contiguously.
//
// Optimization (#205): The build pass (AABB computation) only needs to run when the projection
// matrix or screen size changes (expensive operation: 3,456 froxel AABBs + glMemoryBarrier).
// The cull pass (light binning) runs every frame regardless, since lights move. We cache the
// last (proj, screenW, screenH, nearZ, farZ) and skip the build dispatch when unchanged.
class ClusterGrid {
public:
    static constexpr int GRID_X = 16;
    static constexpr int GRID_Y = 9;
    static constexpr int GRID_Z = 24;
    static constexpr int CLUSTERS = GRID_X * GRID_Y * GRID_Z; // 3456
    // Total capacity of the global compacted light-index list (#208), shared across all
    // clusters instead of each reserving its own fixed slot range. Generously sized relative to
    // typical light coverage (16x the old average of ~4 lights/cluster) while still being ~5.6x
    // smaller than the old fixed-stride buffer (65536 * 4 bytes = 256 KB vs. 1.38 MB). Must
    // match kCullLightsCompute's GLOBAL_CAPACITY.
    static constexpr int GLOBAL_INDEX_CAPACITY = 65536;

    ~ClusterGrid();

    // Build cluster AABBs for this view and assign every point/spot light to the clusters it
    // touches. The LightBuffer SSBO must already be bound at binding 0 (LightBuffer::Bind(0)).
    // nearZ / farZ are positive view-space distances (camera near/far planes).
    // The build pass is cached and skipped if proj/screenW/screenH/nearZ/farZ are unchanged;
    // the cull pass always runs since lights move.
    void Cull(Shader& buildShader, Shader& cullShader, const glm::mat4& view, const glm::mat4& proj,
              float nearZ, float farZ, int screenW, int screenH);

    // Bind the range (3) and index (4) SSBOs for the model fragment shader's per-cluster loop.
    void BindForShading() const;

    // True if the global index list (binding 4) would have overflowed its GLOBAL_INDEX_CAPACITY
    // during a recent Cull() — i.e. at least one cluster's reserved block was truncated and some
    // lights that should shade it were dropped (#204, adapted for #208's global list: the failure
    // mode moved from "one cluster's 100-light cap" to "the shared list is full"). Sourced from a
    // fenced ring readback that lands a couple of frames late (PERF-203), so a scene that only
    // momentarily saturates still trips the Stats-panel warning within a frame or two; nothing on
    // the render path reads this.
    bool Saturated() const { return m_LastSaturated; }

    // (nearZ, farZ, GRID_Z / ln(far/near), -GRID_Z * ln(near) / ln(far/near)) — lets the
    // fragment shader turn a view-space depth into a slice index with one log + fma.
    static glm::vec4 ZParams(float nearZ, float farZ);

private:
    void EnsureCreated();
    unsigned int m_AABBs = 0;   // binding 2
    unsigned int m_Counts = 0;  // binding 3: per-cluster {offset, count} pairs (#208)
    unsigned int m_Indices = 0; // binding 4: global compacted light-index list (#208)
    // binding 5: {globalCounter, overflowFlag} — globalCounter is the atomic append cursor into
    // m_Indices (reset to 0 before every cull dispatch); overflowFlag is atomicOr'd by
    // kCullLightsCompute when a cluster's reserved block ran past GLOBAL_INDEX_CAPACITY (#204,
    // adapted for #208).
    unsigned int m_Overflow = 0;
    bool m_LastSaturated = false;

    // Deferred readback of the overflow flag (PERF-203). Each Cull() copies the one flag uint into
    // the next ring slot and fences it, then consumes the oldest slot only once its fence has
    // signalled — so the CPU never blocks on data the same dispatch just produced. Cull() runs
    // ~2x/frame (Scene + Game views), so 4 slots is ~2 frames of latency. Fences are opaque GLsync
    // stored as void* to keep gl.h out of this header.
    static constexpr int kOverflowRing = 4;
    unsigned int m_OverflowCopy[kOverflowRing] = {}; // 1-uint staging buffers
    void* m_OverflowFence[kOverflowRing] = {};       // GLsync per slot (null = none pending)
    bool m_OverflowSlotFilled[kOverflowRing] = {};   // slot has a copy+fence not yet consumed
    int m_OverflowHead = 0;                           // slot the next Cull() writes into

    // Cached projection state for build-pass optimization (#205)
    glm::mat4 m_LastProj = glm::mat4(0.0f);
    int m_LastScreenW = -1;
    int m_LastScreenH = -1;
    float m_LastNearZ = -1.0f;
    float m_LastFarZ = -1.0f;
};
