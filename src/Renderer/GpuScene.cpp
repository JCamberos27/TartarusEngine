#include "GpuScene.h"
#include "SceneRenderer.h"
#include "GpuMemTrack.h"
#include "InstanceBatch.h"
#include "gl.h"
#include "World.h"
#include "Components.h"
#include "Model.h"
#include "Core/Env.h"
#include "Core/Log.h"
#include "ShaderLibrary.h"
#include <algorithm>
#include <cstring>
#include <glm/gtc/type_ptr.hpp>

namespace {
constexpr std::size_t kXformBytes = 2 * sizeof(glm::mat4);  // per slot
constexpr std::size_t kBoundsBytes = 2 * sizeof(glm::vec4); // per slot
static_assert(sizeof(GpuScene::ObjectState) == 80, "ModelFragment.glsl SlotState is 80 bytes (std430)");

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
// A skinned slot's pose (Model::PoseVersion; 0: no bones). Posed slots come with every Sync: a new pose is logged.
std::uint64_t PoseOf(const SceneSlot& s) {
    const Model* m = s.Renderable && s.Renderable->ModelRef ? s.Renderable->ModelRef.get() : nullptr;
    return m && m->HasBones() ? m->PoseVersion() : 0;
}
} // namespace

bool GpuScene::Enabled() {
    static const bool off = Env::Flag(Env::Var::NoGpuScene);
    return !off;
}

namespace {
int s_MaxFragmentBlocks = 0;
bool s_SlotStateSupported = true;
} // namespace

void GpuScene::InitSlotStateSupport() {
    GLint limit = 0;
    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &limit);
    s_MaxFragmentBlocks = std::min((int)limit, Env::Int(Env::Var::FragmentSsboLimit, (int)limit)); // a lower test limit only
    s_SlotStateSupported = SlotStateFits(s_MaxFragmentBlocks);
    if (!s_SlotStateSupported) {
        ShaderLibrary::SetGlobalDefines(ShaderLibrary::GlobalDefines() + "#define _NO_SLOT_STATE 1\n");
        Log::Warn("GpuScene: GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS = " + std::to_string(s_MaxFragmentBlocks) + " < " +
                  std::to_string(kSlotStateFragmentBlocks) + ": slot state off (probes + GI mask bound per batch)");
    }
}

bool GpuScene::SlotStateSupported() { return s_SlotStateSupported; }
int GpuScene::MaxFragmentBlocks() { return s_MaxFragmentBlocks; }

int GpuScene::FragmentBlocks(unsigned program) {
    GLint count = 0;
    glGetProgramInterfaceiv(program, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES, &count);
    int fragment = 0;
    const GLenum prop = GL_REFERENCED_BY_FRAGMENT_SHADER;
    for (GLint i = 0; i < count; ++i) {
        GLint used = 0;
        glGetProgramResourceiv(program, GL_SHADER_STORAGE_BLOCK, (GLuint)i, 1, &prop, 1, nullptr, &used);
        fragment += used ? 1 : 0;
    }
    return fragment;
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
    // The layer's low 16 bits as they are: a clamp sent a negative layer to 0, which a layer mask can hide while the
    // CPU (which tests 0..31 only) never does.
    bits |= (std::uint32_t)(std::uint16_t)s.Layer << 16;
    bounds[0] = glm::vec4(s.WorldMin, s.Bounded ? 1.0f : 0.0f);
    bounds[1] = glm::vec4(s.WorldMax, AsFloat(bits));
}

void GpuScene::WriteSlot(std::uint32_t slot, const SceneSlot& s) {
    Pack(s, &m_XformCpu[(std::size_t)slot * 2], &m_BoundsCpu[(std::size_t)slot * 2]);
}

void GpuScene::Sync(World& world, bool upload) {
    m_Last = {};
    if (!Enabled()) return;
    if (m_LogOn && m_LogWorld != &world) LogAll(); // the slots now mean another world's renderables
    m_LogWorld = &world;
    // Every Sync keys SyncOnce, so a Sync for another world in between (a bake, a preview) makes the next SyncOnce
    // for this one sync again instead of reading the other world's transforms.
    m_OnceWorld = &world;
    m_OnceStamp = world.TransformCacheStamp();
    m_OnceRenderables = world.Registry.storage<RenderableComponent>().size();
    static SceneSlotChanges changes;
    TakeSceneSlotChanges(world, changes);
    const std::uint32_t had = m_Count; // slots the mirror held before this Sync
    m_Count = changes.Count;
    m_XformCpu.resize((std::size_t)m_Count * 2);
    m_BoundsCpu.resize((std::size_t)m_Count * 2);
    m_StateCpu.resize(m_Count); // a view writes the state of every slot it draws before drawing it
    if (m_LogOn) m_Pose.resize(m_Count, 0);
    SceneSlot s;
    m_Sorted.clear();
    if (changes.Full) {
        for (std::uint32_t i = 0; i < m_Count; ++i) {
            if (!ReadSceneSlot(i, s)) continue;
            WriteSlot(i, s);
            if (m_LogOn) m_Pose[i] = PoseOf(s);
        }
        if (m_LogOn) LogAll();
        m_Last.Full = true;
        m_Last.Slots = m_Count;
    } else {
        for (std::uint32_t i : changes.Slots) {
            if (i >= m_Count || !ReadSceneSlot(i, s)) continue;
            if (m_LogOn) {
                const std::size_t at = (std::size_t)i * 2;
                const glm::vec4 before[2] = {m_BoundsCpu[at], m_BoundsCpu[at + 1]};
                const glm::mat4 xfBefore = m_XformCpu[at];
                WriteSlot(i, s);
                const std::uint64_t pose = PoseOf(s);
                const bool changed = i >= had || pose != m_Pose[i] ||
                                     std::memcmp(&xfBefore, &m_XformCpu[at], sizeof xfBefore) != 0 ||
                                     std::memcmp(before, &m_BoundsCpu[at], sizeof before) != 0;
                m_Pose[i] = pose;
                if (changed) LogChange(i, before, i < had, s);
            } else {
                WriteSlot(i, s);
            }
            m_Sorted.push_back(i);
        }
        m_Last.Slots = (std::uint32_t)m_Sorted.size();
    }
    // What goes up: every slot in one span when the list was rebuilt or the GL buffers don't hold the mirror (new,
    // grown, or a sync skipped the upload), else the changed slots' runs. Planned before the upload, so the unit
    // tests (no GL) see it.
    m_Runs.clear();
    const bool whole = m_Last.Full || m_GpuStale || (upload && m_Count > m_Capacity);
    if (whole) {
        if (m_Count) m_Runs.push_back({0, m_Count - 1});
    } else {
        std::sort(m_Sorted.begin(), m_Sorted.end());
        BuildRuns(m_Sorted, m_Runs);
    }
    if (!upload || !EnsureCapacity(m_Count)) {
        m_GpuStale = true; // the GL buffers missed this change: everything goes up next time
        return;
    }
    m_GpuStale = false;
    UploadRuns(m_Runs);
}

void GpuScene::LogAll() {
    m_Log.All = true;
    m_Log.Changes.clear();
}

void GpuScene::LogChange(std::uint32_t slot, const glm::vec4 before[2], bool hadBefore, const SceneSlot& now) {
    if (m_Log.All) return; // everything is already in
    if (m_Log.Changes.size() >= kMaxLoggedChanges) {
        LogAll();
        return;
    }
    SlotChange& c = m_Log.Changes.emplace_back();
    c.Slot = slot;
    c.Min = now.WorldMin;
    c.Max = now.WorldMax;
    c.Bounded = now.Bounded;
    if (hadBefore) { // where it was counts too: a caster that left a light's reach changes that light's map
        c.Bounded = c.Bounded && before[0].w == 1.0f;
        c.Min = glm::min(c.Min, glm::vec3(before[0]));
        c.Max = glm::max(c.Max, glm::vec3(before[1]));
    }
}

void GpuScene::TakeChangeLog(const World& world, ChangeLog& out) {
    out.Changes.clear();
    if (!m_LogOn || !Enabled()) { // logging starts now: nothing before it is known
        m_LogOn = Enabled();
        m_LogWorld = m_OnceWorld;
        m_Log.All = false;
        m_Log.Changes.clear();
        out.All = true;
        return;
    }
    out.All = m_Log.All || m_LogWorld != &world;
    if (!out.All) out.Changes.swap(m_Log.Changes);
    m_Log.Changes.clear();
    m_Log.All = false;
}

bool GpuScene::SyncOnce(World& world) {
    if (!Enabled()) return false;
    if (Ready() && UpToDateFor(world)) return true;
    Sync(world);
    return Ready();
}

bool GpuScene::UpToDateFor(World& world) const {
    const std::uint64_t stamp = world.TransformCacheStamp();
    return stamp != 0 && m_OnceWorld == &world && m_OnceStamp == stamp &&
           m_OnceRenderables == world.Registry.storage<RenderableComponent>().size();
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

void GpuScene::BuildRuns(const std::vector<std::uint32_t>& slots, std::vector<Run>& runs) {
    runs.clear();
    for (std::uint32_t i : slots) {
        if (!runs.empty() && i <= runs.back().second + kMergeGap + 1) runs.back().second = std::max(runs.back().second, i);
        else runs.push_back({i, i});
    }
    if (runs.size() > kMaxRuns) {
        const Run all{runs.front().first, runs.back().second};
        runs.assign(1, all);
    }
}

void GpuScene::UploadRuns(const std::vector<Run>& runs) {
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

GpuScene::ObjectState GpuScene::MakeObjectState(std::uint64_t giMask, const ReflectionProbeArray::Selection& probes) {
    ObjectState s;
    s.Head[0] = (std::uint32_t)giMask;
    s.Head[1] = (std::uint32_t)(giMask >> 32);
    s.Head[2] = (std::uint32_t)probes.Count;
    for (int j = 0; j < probes.Count && j < ReflectionProbeArray::kMaxDraw; ++j) {
        s.Probe[2 * j] = glm::vec4(probes.Center[j], (float)probes.Layer[j]);
        s.Probe[2 * j + 1] = glm::vec4(probes.HalfSize[j], probes.Blend[j]);
    }
    return s;
}

void GpuScene::SetObjectState(std::uint32_t slot, const ObjectState& s) {
    if (slot >= m_StateCpu.size()) return;
    ObjectState& at = m_StateCpu[slot];
    if (std::memcmp(&at, &s, sizeof s) == 0) return;
    at = s;
    m_StateDirty.push_back(slot);
}

bool GpuScene::UploadObjectState() {
    const std::uint32_t count = (std::uint32_t)m_StateCpu.size();
    bool whole = false;
    if (!m_StateBuffer || count > m_StateCapacity) {
        std::uint32_t cap = std::max<std::uint32_t>(m_StateCapacity, 1024);
        while (cap < count) cap *= 2;
        unsigned buffer = 0;
        glCreateBuffers(1, &buffer);
        if (!buffer) return false;
        glNamedBufferStorage(buffer, (GLsizeiptr)(cap * sizeof(ObjectState)), nullptr, GL_DYNAMIC_STORAGE_BIT);
        if (m_StateBuffer) glDeleteBuffers(1, &m_StateBuffer);
        m_StateBuffer = buffer;
        m_StateCapacity = cap;
        whole = true; // a new store: everything goes up
        GpuMemTrack::Set("GpuScene object state", (std::size_t)cap * sizeof(ObjectState));
    }
    m_StateRuns.clear();
    if (whole) {
        if (count) m_StateRuns.push_back({0, count - 1});
    } else if (!m_StateDirty.empty()) {
        std::sort(m_StateDirty.begin(), m_StateDirty.end());
        BuildRuns(m_StateDirty, m_StateRuns);
    }
    m_StateDirty.clear();
    for (const auto& [first, last] : m_StateRuns) {
        if (first >= count) continue; // the list shrank since (a Sync between the write and this upload)
        const std::size_t n = (std::size_t)(std::min(last, count - 1) - first) + 1;
        glNamedBufferSubData(m_StateBuffer, (GLintptr)(first * sizeof(ObjectState)), (GLsizeiptr)(n * sizeof(ObjectState)),
                             &m_StateCpu[first]);
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kObjectStateBinding, m_StateBuffer);
    return true;
}

void GpuScene::Release() {
    if (m_StateBuffer) {
        glDeleteBuffers(1, &m_StateBuffer);
        GpuMemTrack::Set("GpuScene object state", 0);
    }
    m_StateBuffer = 0;
    m_StateCapacity = 0;
    m_StateDirty.clear();
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
