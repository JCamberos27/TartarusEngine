#include "ClusterDecalSnapshot.h"
#include "gl.h"
#include <cmath>

bool ReadClusterDecalSnapshot(const ClusterGrid& grid, ClusterDecalSnapshot& out) {
    if (!grid.m_DecalIndices || grid.m_LastDecalSlot < 0) return false;
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    out.DecalCount = grid.m_LastDecalCount;
    out.BuildSlot = grid.m_LastDecalSlot;
    const auto read = [](unsigned int buffer, auto& v) {
        glGetNamedBufferSubData(buffer, 0, (GLsizeiptr)(v.size() * sizeof(v[0])), v.data());
    };
    out.Clusters.resize((size_t)ClusterGrid::CLUSTERS * 2);
    read(grid.m_AABBs[grid.m_LastDecalSlot], out.Clusters);
    out.ViewBoxes.resize((size_t)grid.m_LastDecalCount * ClusterGrid::kDecalViewBoxVec4s);
    read(grid.m_DecalViewBoxes, out.ViewBoxes);
    out.Ranges.resize((size_t)ClusterGrid::CLUSTERS * 2);
    read(grid.m_DecalRanges, out.Ranges);
    out.Indices.resize(2 + (size_t)ClusterGrid::kDecalIndexCapacity);
    read(grid.m_DecalIndices, out.Indices);
    return true;
}

bool ClusterDecalBoxOverlaps(const glm::vec4* box, const glm::vec3& mn, const glm::vec3& mx, float slack) {
    const glm::vec3 bc = 0.5f * (mn + mx), bh = 0.5f * (mx - mn);
    const glm::vec3 d = glm::vec3(box[0]) - bc;
    const glm::vec3 reach = glm::vec3(box[1]) + bh + glm::vec3(slack);
    if (std::abs(d.x) > reach.x || std::abs(d.y) > reach.y || std::abs(d.z) > reach.z) return false;
    for (int k = 0; k < 3; ++k) {
        const glm::vec4 a = box[2 + k];
        if (a.w == 0.0f) continue;
        const glm::vec3 n(a);
        if (std::abs(glm::dot(d, n)) > a.w + glm::dot(bh, glm::abs(n)) + slack) return false;
    }
    return true;
}
