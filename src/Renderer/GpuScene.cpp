#include "GpuScene.h"
#include "SceneRenderer.h"
#include "GpuMemTrack.h"
#include "InstanceBatch.h"
#include "gl.h"
#include "Core/Env.h"
#include <algorithm>
#include <cstring>
#include <glm/gtc/type_ptr.hpp>

namespace {
constexpr std::size_t kXformBytes = 2 * sizeof(glm::mat4);  // per slot
constexpr std::size_t kBoundsBytes = 2 * sizeof(glm::vec4); // per slot
// Slots between two changed ones that are uploaded anyway to merge the runs: one upload call costs more than a few
// hundred unchanged bytes.
constexpr std::uint32_t kMergeGap = 16;
// More runs than this and one upload of the span they cover is cheaper.
constexpr std::size_t kMaxRuns = 256;

std::uint32_t AsUint(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}
float AsFloat(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}
} // namespace

bool GpuScene::Enabled() {
    static const bool off = Env::Flag(Env::Var::NoGpuScene);
    return !off;
}

GpuScene& GpuScene::Get() {
    static GpuScene* s = new GpuScene(); // never destroyed: the GL context is gone by the time statics are
    return *s;
}

void GpuScene::Pack(const SceneSlot& s, glm::mat4 xf[2], glm::vec4 bounds[2]) {
    xf[0] = s.Xform;
    xf[1] = glm::mat4(s.Normal);
    std::uint32_t bits = (std::uint32_t)s.Flags & 0x7FFFu;
    if (!s.Alive) bits |= kDead;
    bits |= (std::uint32_t)std::clamp(s.Layer, 0, 0xFFFF) << 16;
    bounds[0] = glm::vec4(s.WorldMin, s.Bounded ? 1.0f : 0.0f);
    bounds[1] = glm::vec4(s.WorldMax, AsFloat(bits));
}

void GpuScene::WriteSlot(std::uint32_t slot, const SceneSlot& s) {
    Pack(s, &m_XformCpu[(std::size_t)slot * 2], &m_BoundsCpu[(std::size_t)slot * 2]);
}

void GpuScene::Sync(World& world, bool upload) {
    m_Last = {};
    if (!Enabled()) return;
    static SceneSlotChanges changes;
    TakeSceneSlotChanges(world, changes);
    m_Count = changes.Count;
    m_XformCpu.resize((std::size_t)m_Count * 2);
    m_BoundsCpu.resize((std::size_t)m_Count * 2);
    SceneSlot s;
    m_Sorted.clear();
    if (changes.Full) {
        for (std::uint32_t i = 0; i < m_Count; ++i)
            if (ReadSceneSlot(i, s)) WriteSlot(i, s);
        m_Last.Full = true;
        m_Last.Slots = m_Count;
    } else {
        for (std::uint32_t i : changes.Slots) {
            if (i >= m_Count || !ReadSceneSlot(i, s)) continue;
            WriteSlot(i, s);
            m_Sorted.push_back(i);
        }
        m_Last.Slots = (std::uint32_t)m_Sorted.size();
    }
    if (!upload || !EnsureCapacity(m_Count)) {
        m_GpuStale = true; // the GL buffers missed this change: everything goes up next time
        return;
    }
    if (m_Last.Full || m_GpuStale) {
        m_Sorted.clear();
        if (m_Count) {
            m_Sorted.push_back(0);
            m_Sorted.push_back(m_Count - 1); // one run over everything (the gap merge joins them)
        }
        m_GpuStale = false;
    } else {
        std::sort(m_Sorted.begin(), m_Sorted.end());
    }
    UploadRuns(m_Sorted);
}

bool GpuScene::EnsureCapacity(std::uint32_t count) {
    if (count <= m_Capacity && m_Xforms && m_Bounds) return true;
    std::uint32_t cap = std::max<std::uint32_t>(m_Capacity, 1024);
    while (cap < count) cap *= 2;
    unsigned buffers[2] = {0, 0};
    glCreateBuffers(2, buffers);
    if (!buffers[0] || !buffers[1]) {
        glDeleteBuffers(2, buffers);
        return false;
    }
    glNamedBufferStorage(buffers[0], (GLsizeiptr)(cap * kXformBytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glNamedBufferStorage(buffers[1], (GLsizeiptr)(cap * kBoundsBytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
    if (m_Xforms || m_Bounds) {
        const unsigned old[2] = {m_Xforms, m_Bounds};
        glDeleteBuffers(2, old);
    }
    m_Xforms = buffers[0];
    m_Bounds = buffers[1];
    m_Capacity = cap;
    m_GpuStale = true; // a new store: everything goes up
    GpuMemTrack::Set("GpuScene xforms + bounds", (std::size_t)cap * (kXformBytes + kBoundsBytes));
    return true;
}

void GpuScene::UploadRuns(const std::vector<std::uint32_t>& slots) {
    if (slots.empty()) return;
    // Runs of slots [first, last], merged across gaps of up to kMergeGap.
    static std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
    runs.clear();
    for (std::uint32_t i : slots) {
        if (!runs.empty() && i <= runs.back().second + kMergeGap + 1) runs.back().second = std::max(runs.back().second, i);
        else runs.push_back({i, i});
    }
    if (runs.size() > kMaxRuns) {
        const std::pair<std::uint32_t, std::uint32_t> all{runs.front().first, runs.back().second};
        runs.assign(1, all);
    }
    for (const auto& [first, last] : runs) {
        const std::size_t n = (std::size_t)(last - first) + 1;
        glNamedBufferSubData(m_Xforms, (GLintptr)(first * kXformBytes), (GLsizeiptr)(n * kXformBytes),
                             glm::value_ptr(m_XformCpu[(std::size_t)first * 2]));
        glNamedBufferSubData(m_Bounds, (GLintptr)(first * kBoundsBytes), (GLsizeiptr)(n * kBoundsBytes),
                             glm::value_ptr(m_BoundsCpu[(std::size_t)first * 2]));
        m_Last.Runs++;
        m_Last.Bytes += n * (kXformBytes + kBoundsBytes);
    }
}

void GpuScene::BindSlotList(unsigned binding, const std::vector<std::uint32_t>& slots) {
    const std::uint32_t none = 0;
    const void* data = slots.empty() ? (const void*)&none : (const void*)slots.data();
    const std::size_t bytes = std::max<std::size_t>(slots.size(), 1) * sizeof(std::uint32_t);
    if (InstanceBuffer::UploadRange(binding, data, bytes)) return;
    if (!m_SlotFallback) glCreateBuffers(1, &m_SlotFallback);
    // A new data store each time: the draws already issued against the old one still read their own slots.
    glNamedBufferData(m_SlotFallback, (GLsizeiptr)bytes, data, GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, m_SlotFallback);
}

void GpuScene::Release() {
    if (m_Xforms || m_Bounds) {
        const unsigned old[2] = {m_Xforms, m_Bounds};
        glDeleteBuffers(2, old);
        GpuMemTrack::Set("GpuScene xforms + bounds", 0);
    }
    if (m_SlotFallback) glDeleteBuffers(1, &m_SlotFallback);
    m_SlotFallback = 0;
    m_Xforms = m_Bounds = 0;
    m_Capacity = 0;
    m_GpuStale = true;
}

bool GpuScene::SelfTest(World& world, std::string& why) {
    if (!Enabled()) { why = "off (TARTARUS_NO_GPU_SCENE)"; return true; }
    Sync(world);
    if (!m_Xforms || !m_Bounds) { why = "no GL buffers"; return false; }
    std::vector<glm::mat4> xf((std::size_t)m_Count * 2);
    std::vector<glm::vec4> bounds((std::size_t)m_Count * 2);
    if (m_Count) {
        glGetNamedBufferSubData(m_Xforms, 0, (GLsizeiptr)(m_Count * kXformBytes), xf.data());
        glGetNamedBufferSubData(m_Bounds, 0, (GLsizeiptr)(m_Count * kBoundsBytes), bounds.data());
    }
    SceneSlot s;
    glm::mat4 wantXf[2];
    glm::vec4 wantB[2];
    for (std::uint32_t i = 0; i < m_Count; ++i) {
        if (!ReadSceneSlot(i, s)) { why = "slot " + std::to_string(i) + " not in the candidate list"; return false; }
        Pack(s, wantXf, wantB);
        const bool same = std::memcmp(&xf[(std::size_t)i * 2], wantXf, kXformBytes) == 0 &&
                          std::memcmp(&bounds[(std::size_t)i * 2], wantB, kBoundsBytes) == 0;
        if (!same) {
            why = "slot " + std::to_string(i) + " of " + std::to_string(m_Count) + " differs from the candidate list (bits " +
                  std::to_string(AsUint(bounds[(std::size_t)i * 2 + 1].w)) + " vs " + std::to_string(AsUint(wantB[1].w)) + ")";
            return false;
        }
    }
    return true;
}
