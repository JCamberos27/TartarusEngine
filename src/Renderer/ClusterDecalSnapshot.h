#pragma once
#include "ClusterGrid.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

// Tests only (smoke_decals_stress): the last decal cull's inputs and outputs, read back synchronously
// (four buffer reads that stall the GPU), and the cull's overlap test on the CPU. Kept out of ClusterGrid,
// whose render path never reads any of it.
struct ClusterDecalSnapshot {
    int DecalCount = 0;
    int BuildSlot = -1;
    std::vector<glm::vec4> Clusters;  // CLUSTERS * 2: view-space min, max
    std::vector<glm::vec4> ViewBoxes; // DecalCount * ClusterGrid::kDecalViewBoxVec4s
    std::vector<uint32_t> Ranges;     // CLUSTERS * 2: offset, count
    std::vector<uint32_t> Indices;    // the {entries in use, overflow} header, then the whole list
};

// False before the grid has culled any decal.
bool ReadClusterDecalSnapshot(const ClusterGrid& grid, ClusterDecalSnapshot& out);

// The cull's overlap test on one ViewBox (ClusterDecalCull.comp.glsl's Overlaps), CPU side. `slack`
// widens (> 0) or narrows (< 0) every separating-axis test by that much, to bracket rounding.
bool ClusterDecalBoxOverlaps(const glm::vec4* box, const glm::vec3& mn, const glm::vec3& mx, float slack);
