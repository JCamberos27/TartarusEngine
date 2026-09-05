#pragma once
#include <glm/glm.hpp>

class Shader;

// Clustered-forward light culling (#120). Owns three SSBOs — per-cluster view-space AABBs
// (binding 2, compute-internal), per-cluster light count (binding 3), and a fixed-stride
// per-cluster light-index list (binding 4) — and drives the two compute passes that fill
// them (see ClusterShaderSource.h). The model fragment shader reads bindings 3 and 4.
//
// The grid is a fixed GRID_X x GRID_Y screen tiling by GRID_Z exponential depth slices, so a
// fragment maps to its cluster with cheap arithmetic on gl_FragCoord + view depth. Rebuilt
// every rendered view every frame (a few thousand trivial compute invocations) rather than
// tracking projection/viewport dirtiness.
class ClusterGrid {
public:
    static constexpr int GRID_X = 16;
    static constexpr int GRID_Y = 9;
    static constexpr int GRID_Z = 24;
    static constexpr int CLUSTERS = GRID_X * GRID_Y * GRID_Z; // 3456
    static constexpr int MAX_LIGHTS_PER_CLUSTER = 100;        // must match kCullLightsCompute MAXL

    ~ClusterGrid();

    // Build cluster AABBs for this view and assign every point/spot light to the clusters it
    // touches. The LightBuffer SSBO must already be bound at binding 0 (LightBuffer::Bind(0)).
    // nearZ / farZ are positive view-space distances (camera near/far planes).
    void Cull(Shader& buildShader, Shader& cullShader, const glm::mat4& view, const glm::mat4& proj,
              float nearZ, float farZ, int screenW, int screenH);

    // Bind the count (3) and index (4) SSBOs for the model fragment shader's per-cluster loop.
    void BindForShading() const;

    // True if any cluster hit MAX_LIGHTS_PER_CLUSTER during the most recent Cull() — i.e. that
    // cluster's light list was capped and may be missing lights that should shade it (#204). A
    // single-uint readback taken once per Cull() call; cheap enough to always do, not something
    // to poll every fragment. Drives the Stats panel warning, nothing render-critical.
    bool Saturated() const { return m_LastSaturated; }

    // (nearZ, farZ, GRID_Z / ln(far/near), -GRID_Z * ln(near) / ln(far/near)) — lets the
    // fragment shader turn a view-space depth into a slice index with one log + fma.
    static glm::vec4 ZParams(float nearZ, float farZ);

private:
    void EnsureCreated();
    unsigned int m_AABBs = 0;    // binding 2
    unsigned int m_Counts = 0;   // binding 3
    unsigned int m_Indices = 0;  // binding 4
    unsigned int m_Overflow = 0; // binding 5: single uint, atomicOr'd by kCullLightsCompute (#204)
    bool m_LastSaturated = false;
};
