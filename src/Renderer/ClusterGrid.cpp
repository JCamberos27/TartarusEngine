#include "ClusterGrid.h"
#include "Shader.h"
#include "gl.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

ClusterGrid::~ClusterGrid() {
    if (m_AABBs[0]) glDeleteBuffers(kBuildSlots, m_AABBs);
    if (m_Counts) glDeleteBuffers(1, &m_Counts);
    if (m_Indices) glDeleteBuffers(1, &m_Indices);
    if (m_Overflow) glDeleteBuffers(1, &m_Overflow);
    if (m_OverflowCopy) glDeleteBuffers(1, &m_OverflowCopy); // deleting unmaps it
}

void ClusterGrid::EnsureCreated() {
    if (m_AABBs[0]) return;
    glCreateBuffers(kBuildSlots, m_AABBs);
    for (unsigned int buf : m_AABBs)
        glNamedBufferStorage(buf, (GLsizeiptr)CLUSTERS * 2 * (GLsizeiptr)sizeof(glm::vec4), nullptr, 0);
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

    // The staging uint for the deferred overflow-flag readback (PERF-203), mapped once for good.
    const GLbitfield mapFlags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const unsigned int zero = 0;
    glCreateBuffers(1, &m_OverflowCopy);
    glNamedBufferStorage(m_OverflowCopy, (GLsizeiptr)sizeof(unsigned int), &zero, mapFlags);
    m_OverflowMapped = (const volatile unsigned int*)glMapNamedBufferRange(m_OverflowCopy, 0, (GLsizeiptr)sizeof(unsigned int), mapFlags);
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

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_Counts);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_Indices);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_Overflow);

    // Optimization (#205): skip the expensive build pass if projection state is unchanged.
    // The build pass computes all 3,456 froxel AABBs from uInvProj, uScreenSize, uZNear, uZFar,
    // which only change on resize/FOV/near-far edit. Cache comparison avoids ~6,912 dispatches
    // per typical gameplay frame (build runs once per view, view runs 2x: shadows+main).
    // #160: look for a slot already built for this projection; otherwise rebuild the least
    // recently used one.
    ++m_CullCounter;
    int slot = -1;
    for (int i = 0; i < kBuildSlots; ++i) {
        const BuildKey& k = m_BuildKeys[i];
        if (k.Proj == proj && k.ScreenW == screenW && k.ScreenH == screenH && k.NearZ == nearZ && k.FarZ == farZ) {
            slot = i;
            break;
        }
    }
    const bool buildNeeded = slot < 0;
    if (buildNeeded) {
        slot = 0;
        for (int i = 1; i < kBuildSlots; ++i)
            if (m_BuildKeys[i].LastUsed < m_BuildKeys[slot].LastUsed) slot = i;
    }
    m_BuildKeys[slot].LastUsed = m_CullCounter;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_AABBs[slot]);

    if (buildNeeded) {
        buildShader.Bind();
        buildShader.SetMat4("uInvProj", glm::inverse(proj));
        buildShader.SetVec2("uScreenSize", glm::vec2((float)screenW, (float)screenH));
        buildShader.SetFloat("uZNear", nearZ);
        buildShader.SetFloat("uZFar", farZ);
        buildShader.DispatchCompute(groups, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        BuildKey& k = m_BuildKeys[slot];
        k.Proj = proj;
        k.ScreenW = screenW;
        k.ScreenH = screenH;
        k.NearZ = nearZ;
        k.FarZ = farZ;
    }

    cullShader.Bind();
    cullShader.SetMat4("uView", view);
    cullShader.DispatchCompute(groups, 1, 1);
    // SHADER_STORAGE: the shading pass reads m_Counts / m_Indices as SSBOs. BUFFER_UPDATE: the
    // deferred overflow readback below pulls m_Overflow through glCopyNamedBufferSubData.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    // Deferred, non-blocking readback of just the overflow flag (PERF-203). A synchronous
    // glGetNamedBufferSubData here read data the cull dispatch had just written *this* frame,
    // forcing a CPU<-GPU sync twice per frame, and the fence-and-map ring that replaced it still
    // waited on a threaded driver's worker (NVIDIA's) at every poll and map, like a glGet. Now the
    // flag uint (past the global counter) is copied into the coherent mapped uint, which is read
    // here first: whatever an earlier cull's copy left in it (or 0 before one lands).
    if (m_OverflowMapped) {
        m_LastSaturated = *m_OverflowMapped != 0;
        glCopyNamedBufferSubData(m_Overflow, m_OverflowCopy, (GLintptr)sizeof(unsigned int), 0, (GLsizeiptr)sizeof(unsigned int));
    }
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
