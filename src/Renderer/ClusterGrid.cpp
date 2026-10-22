#include "ClusterGrid.h"
#include "Shader.h"
#include "DecalTypes.h"
#include "gl.h"
#include "Core/Profiler.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

static_assert(ClusterGrid::kDecalIndexCapacity == ClusterGrid::CLUSTERS * Decals::MAX_PER_CLUSTER,
              "the decal index list holds every cluster's worst case");
static_assert(Decals::MAX_VISIBLE <= 4096, "ClusterDecalCull.comp.glsl's hit bitmask holds 4096 decals");

ClusterGrid::~ClusterGrid() {
    if (m_AABBs[0]) glDeleteBuffers(kBuildSlots, m_AABBs);
    if (m_Counts) glDeleteBuffers(1, &m_Counts);
    if (m_Indices) glDeleteBuffers(1, &m_Indices);
    if (m_Overflow) glDeleteBuffers(1, &m_Overflow);
    if (m_OverflowCopy) glDeleteBuffers(1, &m_OverflowCopy); // deleting unmaps it
    if (m_DecalRanges) glDeleteBuffers(1, &m_DecalRanges);
    if (m_DecalIndices) glDeleteBuffers(1, &m_DecalIndices);
    if (m_DecalViewBoxes) glDeleteBuffers(1, &m_DecalViewBoxes);
    if (m_DecalCullState) glDeleteBuffers(1, &m_DecalCullState);
    if (m_DecalReadback) glDeleteBuffers(1, &m_DecalReadback);
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
    // {globalCounter, overflowFlag} — see field comment in ClusterGrid.h. Cleared on the GPU each cull.
    glNamedBufferStorage(m_Overflow, (GLsizeiptr)2 * (GLsizeiptr)sizeof(unsigned int), nullptr, 0);

    // The staging uint for the deferred overflow-flag readback (PERF-203), mapped once for good.
    const GLbitfield mapFlags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const unsigned int zero = 0;
    glCreateBuffers(1, &m_OverflowCopy);
    glNamedBufferStorage(m_OverflowCopy, (GLsizeiptr)sizeof(unsigned int), &zero, mapFlags);
    m_OverflowMapped = (const volatile unsigned int*)glMapNamedBufferRange(m_OverflowCopy, 0, (GLsizeiptr)sizeof(unsigned int), mapFlags);

    // Decal lists (ClusterDecalCull.comp.glsl): ranges like the lights', one index list with its
    // {entries, overflow} header in front and a MAX_PER_CLUSTER block per cluster, and the cull's scratch.
    glCreateBuffers(1, &m_DecalRanges);
    glNamedBufferStorage(m_DecalRanges, (GLsizeiptr)CLUSTERS * 2 * (GLsizeiptr)sizeof(unsigned int), nullptr, 0);
    glCreateBuffers(1, &m_DecalIndices);
    glNamedBufferStorage(m_DecalIndices, (GLsizeiptr)(2 + kDecalIndexCapacity) * (GLsizeiptr)sizeof(unsigned int), nullptr, 0);
    glCreateBuffers(1, &m_DecalViewBoxes);
    glNamedBufferStorage(m_DecalViewBoxes,
                         (GLsizeiptr)Decals::MAX_VISIBLE * kDecalViewBoxVec4s * (GLsizeiptr)sizeof(glm::vec4), nullptr, 0);
    // The cull's finished-group counter, and the readback ring it writes {entries, overflow, serial, -} into.
    glCreateBuffers(1, &m_DecalCullState);
    glNamedBufferStorage(m_DecalCullState, (GLsizeiptr)sizeof(unsigned int), nullptr, 0);
    const unsigned int zeros[kDecalReadbackSlots * 4] = {};
    glCreateBuffers(1, &m_DecalReadback);
    glNamedBufferStorage(m_DecalReadback, (GLsizeiptr)sizeof(zeros), zeros, mapFlags);
    m_DecalReadbackMapped =
        (const volatile unsigned int*)glMapNamedBufferRange(m_DecalReadback, 0, (GLsizeiptr)sizeof(zeros), mapFlags);
}

// Takes every decal readback that has landed (its slot carries the serial its cull was given), oldest
// first, and stops at the first that hasn't. Reads mapped memory only: no GL call, nothing waits.
void ClusterGrid::ReadDecalOverflow() const {
    if (!m_DecalReadbackMapped) return;
    while (m_DecalReadbackCount > 0) {
        const DecalReadback& r = m_DecalPending[m_DecalReadbackHead];
        const volatile unsigned int* slot = m_DecalReadbackMapped + m_DecalReadbackHead * 4;
        if (slot[2] != r.Serial) break;
        m_DecalOverflowBySlot[r.BuildSlot] = slot[1];
        m_DecalReadbackHead = (m_DecalReadbackHead + 1) % kDecalReadbackSlots;
        --m_DecalReadbackCount;
    }
}

unsigned ClusterGrid::DecalOverflow() const {
    ReadDecalOverflow();
    unsigned bits = 0;
    for (unsigned v : m_DecalOverflowBySlot) bits |= v & 0xFFu;
    return bits;
}

unsigned ClusterGrid::DecalOverflowClusters(int buildSlot) const {
    ReadDecalOverflow();
    if (buildSlot >= 0 && buildSlot < kBuildSlots) return m_DecalOverflowBySlot[buildSlot] >> 8;
    unsigned most = 0;
    for (unsigned v : m_DecalOverflowBySlot) most = std::max(most, v >> 8);
    return most;
}

void ClusterGrid::Cull(Shader& buildShader, Shader& cullShader, const glm::mat4& view, const glm::mat4& proj,
                       float nearZ, float farZ, int screenW, int screenH, Shader* decalCullShader, int decalCount) {
    EnsureCreated();
    const unsigned int groups = (CLUSTERS + 63) / 64;

    // Cleared before the cull pass so kCullLightsCompute's atomicAdd/atomicOr accumulate fresh
    // state for just this frame's dispatch: the global append cursor restarts at 0, and the
    // overflow flag restarts clear (#204, adapted for #208's global list). A GPU-side clear: an
    // upload would write a buffer the previous view's dispatch may still be using.
    glClearNamedBufferSubData(m_Overflow, GL_R32UI, 0, 2 * sizeof(unsigned int), GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);

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

    // Decals into the same froxels, independent of the lights' buffers, so one barrier covers both.
    const int decalsIn = std::min(decalCount, Decals::MAX_VISIBLE);
    const bool decals = decalCullShader && decalsIn > 0;
    if (decals) {
        PROFILE_GPU_SCOPE("Decal Cull");
        // The header readback (see the field comment): landed slots taken first, a ring still full after
        // that drops its oldest sample, then the cull writes into the next one itself. Nothing waits.
        int at = -1;
        if (m_DecalReadbackMapped) {
            ReadDecalOverflow();
            if (m_DecalReadbackCount == kDecalReadbackSlots) {
                m_DecalReadbackHead = (m_DecalReadbackHead + 1) % kDecalReadbackSlots;
                --m_DecalReadbackCount;
            }
            at = (m_DecalReadbackHead + m_DecalReadbackCount) % kDecalReadbackSlots;
            m_DecalSerial = m_DecalSerial % 0x7FFFFFFFu + 1u; // 1..2^31-1: a fresh slot holds 0
            m_DecalPending[at].Serial = m_DecalSerial;
            m_DecalPending[at].BuildSlot = slot;
            ++m_DecalReadbackCount;
        }
        glClearNamedBufferSubData(m_DecalIndices, GL_R32UI, 0, 2 * sizeof(unsigned int), GL_RED_INTEGER, GL_UNSIGNED_INT,
                                  nullptr);
        glClearNamedBufferSubData(m_DecalCullState, GL_R32UI, 0, sizeof(unsigned int), GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, Decals::BIND_RANGES, m_DecalRanges);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, Decals::BIND_INDICES, m_DecalIndices);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindDecalViewBoxes, m_DecalViewBoxes);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindDecalCullState, m_DecalCullState);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindDecalReadback, m_DecalReadback);
        decalCullShader->Bind();
        decalCullShader->SetMat4("uView", view);
        decalCullShader->SetInt("uDecalCountI", decalsIn);
        // Slot 0 with serial 0 when there's no ring: it rewrites the zeros already there.
        decalCullShader->SetInt("uReadbackSlot", at < 0 ? 0 : at);
        decalCullShader->SetInt("uSerial", at < 0 ? 0 : (int)m_DecalSerial);
        // Pass 0: every decal into view space once. Pass 1: one workgroup per cluster.
        decalCullShader->SetInt("uPass", 0);
        decalCullShader->DispatchCompute((unsigned)(decalsIn + 63) / 64, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        decalCullShader->SetInt("uPass", 1);
        decalCullShader->DispatchCompute(CLUSTERS, 1, 1);
        m_LastDecalSlot = slot;
        m_LastDecalCount = decalsIn;
    }
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
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, Decals::BIND_RANGES, m_DecalRanges);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, Decals::BIND_INDICES, m_DecalIndices);
}

bool ClusterGrid::ReadDecalSnapshot(DecalSnapshot& out) const {
    if (!m_DecalIndices || m_LastDecalSlot < 0) return false;
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    out.DecalCount = m_LastDecalCount;
    out.BuildSlot = m_LastDecalSlot;
    out.Clusters.resize((size_t)CLUSTERS * 2);
    glGetNamedBufferSubData(m_AABBs[m_LastDecalSlot], 0, (GLsizeiptr)(out.Clusters.size() * sizeof(glm::vec4)), out.Clusters.data());
    out.ViewBoxes.resize((size_t)m_LastDecalCount * kDecalViewBoxVec4s);
    glGetNamedBufferSubData(m_DecalViewBoxes, 0, (GLsizeiptr)(out.ViewBoxes.size() * sizeof(glm::vec4)), out.ViewBoxes.data());
    out.Ranges.resize((size_t)CLUSTERS * 2);
    glGetNamedBufferSubData(m_DecalRanges, 0, (GLsizeiptr)(out.Ranges.size() * sizeof(uint32_t)), out.Ranges.data());
    out.Indices.resize(2 + (size_t)kDecalIndexCapacity);
    glGetNamedBufferSubData(m_DecalIndices, 0, (GLsizeiptr)(out.Indices.size() * sizeof(uint32_t)), out.Indices.data());
    return true;
}

bool ClusterGrid::DecalBoxOverlaps(const glm::vec4* box, const glm::vec3& mn, const glm::vec3& mx, float slack) {
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

glm::vec4 ClusterGrid::ZParams(float nearZ, float farZ) {
    float lg = std::log(farZ / nearZ);
    return glm::vec4(nearZ, farZ,
                     (float)GRID_Z / lg,
                     -(float)GRID_Z * std::log(nearZ) / lg);
}
