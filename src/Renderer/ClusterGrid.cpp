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
    for (int i = 0; i < kOverflowRing; ++i) {
        if (m_OverflowFence[i]) glDeleteSync((GLsync)m_OverflowFence[i]);
        if (m_OverflowCopy[i]) glDeleteBuffers(1, &m_OverflowCopy[i]);
    }
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

    // Ring of 1-uint staging buffers for the deferred overflow-flag readback (PERF-203). Mapped
    // for read each time a fence signals; GL_STREAM_READ hints the GPU->CPU access pattern.
    for (int i = 0; i < kOverflowRing; ++i) {
        glCreateBuffers(1, &m_OverflowCopy[i]);
        glNamedBufferStorage(m_OverflowCopy[i], (GLsizeiptr)sizeof(unsigned int), nullptr, GL_MAP_READ_BIT);
    }
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
    // SHADER_STORAGE: the shading pass reads m_Counts / m_Indices as SSBOs. BUFFER_UPDATE: the
    // deferred overflow readback below pulls m_Overflow through glCopyNamedBufferSubData.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    // Deferred, non-blocking readback of just the overflow flag (PERF-203). A synchronous
    // glGetNamedBufferSubData here read data the cull dispatch had just written *this* frame,
    // forcing a CPU<-GPU sync twice per frame. Instead: copy the one flag uint (offset past the
    // global counter) into the next ring slot, fence it, and consume the OLDEST slot only once
    // its fence has signalled — the value is then a couple of frames stale, which is fine for the
    // Stats-panel warning and invisible everywhere else.
    glCopyNamedBufferSubData(m_Overflow, m_OverflowCopy[m_OverflowHead],
                             (GLintptr)sizeof(unsigned int), 0, (GLsizeiptr)sizeof(unsigned int));
    if (m_OverflowFence[m_OverflowHead]) glDeleteSync((GLsync)m_OverflowFence[m_OverflowHead]);
    m_OverflowFence[m_OverflowHead] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    m_OverflowSlotFilled[m_OverflowHead] = true;

    const int oldest = (m_OverflowHead + 1) % kOverflowRing;
    if (m_OverflowSlotFilled[oldest] && m_OverflowFence[oldest]) {
        // Non-blocking poll (zero timeout); FLUSH_COMMANDS_BIT guarantees the fence reaches the
        // GPU so it can eventually signal even on a frame that never otherwise flushes.
        GLenum w = glClientWaitSync((GLsync)m_OverflowFence[oldest], GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        if (w == GL_ALREADY_SIGNALED || w == GL_CONDITION_SATISFIED) {
            if (void* p = glMapNamedBuffer(m_OverflowCopy[oldest], GL_READ_ONLY)) {
                m_LastSaturated = *(const unsigned int*)p != 0;
                glUnmapNamedBuffer(m_OverflowCopy[oldest]);
            }
            glDeleteSync((GLsync)m_OverflowFence[oldest]);
            m_OverflowFence[oldest] = nullptr;
            m_OverflowSlotFilled[oldest] = false;
        }
    }
    m_OverflowHead = oldest;
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
