#include "ClusterGrid.h"
#include "Shader.h"
#include "gl.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

ClusterGrid::~ClusterGrid() {
    if (m_AABBs) glDeleteBuffers(1, &m_AABBs);
    if (m_Counts) glDeleteBuffers(1, &m_Counts);
    if (m_Indices) glDeleteBuffers(1, &m_Indices);
    if (m_Overflow) glDeleteBuffers(1, &m_Overflow);
}

void ClusterGrid::EnsureCreated() {
    if (m_AABBs) return;
    glCreateBuffers(1, &m_AABBs);
    glNamedBufferStorage(m_AABBs, (GLsizeiptr)CLUSTERS * 2 * (GLsizeiptr)sizeof(glm::vec4), nullptr, 0);
    glCreateBuffers(1, &m_Counts);
    // Per-cluster {offset, count} pair (#208) instead of a bare count at a fixed slot.
    glNamedBufferStorage(m_Counts, (GLsizeiptr)CLUSTERS * 2 * (GLsizeiptr)sizeof(unsigned int), nullptr, 0);
    glCreateBuffers(1, &m_Indices);
    // One global compacted list every cluster appends its lights into (#208), sized well below
    // the old worst-case fixed-stride buffer (see GLOBAL_INDEX_CAPACITY).
    glNamedBufferStorage(m_Indices,
        (GLsizeiptr)GLOBAL_INDEX_CAPACITY * (GLsizeiptr)sizeof(unsigned int), nullptr, 0);
    glCreateBuffers(1, &m_Overflow);
    // {globalCounter, overflowFlag} — see field comment in ClusterGrid.h.
    glNamedBufferStorage(m_Overflow, (GLsizeiptr)2 * (GLsizeiptr)sizeof(unsigned int), nullptr, GL_DYNAMIC_STORAGE_BIT);
}

void ClusterGrid::Cull(Shader& buildShader, Shader& cullShader, const glm::mat4& view, const glm::mat4& proj,
                       float nearZ, float farZ, int screenW, int screenH) {
    EnsureCreated();
    const unsigned int groups = (CLUSTERS + 63) / 64;

    // Cleared before the cull pass so kCullLightsCompute's atomicAdd/atomicOr accumulate fresh
    // state for just this frame's dispatch: the global append cursor restarts at 0, and the
    // overflow flag restarts clear (#204, adapted for #208's global list).
    const unsigned int zero2[2] = {0, 0};
    glNamedBufferSubData(m_Overflow, 0, sizeof(zero2), zero2);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_AABBs);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_Counts);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_Indices);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_Overflow);

    // Optimization (#205): skip the expensive build pass if projection state is unchanged.
    // The build pass computes all 3,456 froxel AABBs from uInvProj, uScreenSize, uZNear, uZFar,
    // which only change on resize/FOV/near-far edit. Cache comparison avoids ~6,912 dispatches
    // per typical gameplay frame (build runs once per view, view runs 2x: shadows+main).
    bool buildNeeded = (proj != m_LastProj || screenW != m_LastScreenW || screenH != m_LastScreenH ||
                        nearZ != m_LastNearZ || farZ != m_LastFarZ);

    if (buildNeeded) {
        buildShader.Bind();
        buildShader.SetMat4("uInvProj", glm::inverse(proj));
        buildShader.SetVec2("uScreenSize", glm::vec2((float)screenW, (float)screenH));
        buildShader.SetFloat("uZNear", nearZ);
        buildShader.SetFloat("uZFar", farZ);
        buildShader.DispatchCompute(groups, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        m_LastProj = proj;
        m_LastScreenW = screenW;
        m_LastScreenH = screenH;
        m_LastNearZ = nearZ;
        m_LastFarZ = farZ;
    }

    cullShader.Bind();
    cullShader.SetMat4("uView", view);
    cullShader.DispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Single-uint synchronous readback of just the overflow flag (offset past the global
    // counter): negligible cost, and only feeds the Stats panel warning (#204) — nothing on the
    // render path depends on this frame's value.
    unsigned int flag = 0;
    glGetNamedBufferSubData(m_Overflow, sizeof(unsigned int), sizeof(unsigned int), &flag);
    m_LastSaturated = flag != 0;
}

void ClusterGrid::BindForShading() const {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_Counts);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_Indices);
}

glm::vec4 ClusterGrid::ZParams(float nearZ, float farZ) {
    float lg = std::log(farZ / nearZ);
    return glm::vec4(nearZ, farZ,
                     (float)GRID_Z / lg,
                     -(float)GRID_Z * std::log(nearZ) / lg);
}
