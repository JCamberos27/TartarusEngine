#include "InstanceBatch.h"
#include "GpuMemTrack.h"
#include "GpuScene.h"
#include "gl.h"
#include "Model.h"
#include "Shader.h"
#include "MaterialAsset.h"
#include "ProjectSettings.h"
#include "GLStateCache.h"
#include "ShaderAsset.h"
#include "Core/Env.h"
#include "UploadStaging.h"
#include "RenderSettingsEpoch.h"
#include "TimeService.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>

namespace {
// Two slots draw alike when they are the same asset, or assets with the same shader, queue and values: a scene gives
// each lamp or window its own copy of an identical material (its emission is linked per object).
bool SameMaterial(const MaterialAsset* a, const MaterialAsset* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return a->Shader == b->Shader && a->RenderQueue == b->RenderQueue && a->QueueIndex == b->QueueIndex &&
           a->Opacity == b->Opacity && a->Mat.HashWithExtraProps() == b->Mat.HashWithExtraProps();
}
} // namespace

bool SameSlots(const std::vector<std::shared_ptr<MaterialAsset>>& a, const std::vector<std::shared_ptr<MaterialAsset>>& b) {
    if (&a == &b) return true;
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!SameMaterial(a[i].get(), b[i].get())) return false;
    return true;
}

std::uint64_t HashSlots(const std::vector<std::shared_ptr<MaterialAsset>>& slots, std::uint64_t seed) {
    std::uint64_t h = seed ^ (slots.size() * 0x9E3779B97F4A7C15ull);
    for (const auto& s : slots) {
        const std::uint64_t v = s ? s->Mat.HashWithExtraProps() ^ (std::uint64_t)(std::uintptr_t)s->Shader.get() : 0ull;
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    return h == kNoBatch ? h - 1 : h;
}

namespace InstanceBuffer {
namespace {
constexpr GLuint kBinding = kXformBinding;
GLuint g_Buffer = 0; // the fallback: an upload the ring can't take (or no ring)
std::vector<glm::mat4> g_Staging;
GLuint g_LastBuffer = 0; // what the last Upload bound at kBinding (Rebind)
std::size_t g_LastAt = 0, g_LastBytes = 0;

// Each Upload used to re-specify one buffer (glNamedBufferData). On NVIDIA's threaded driver that waits for the driver
// thread to drain, and so does every fence query: either way the CPU stopped until the GPU had caught up, and the two
// ran one after the other instead of overlapping (the Forest's GPU sat idle ~5 ms a frame). The transforms now go
// into a persistently mapped ring, written with a memcpy and bound as a range, and a frame's span is reused only
// kSafeFrames presented frames later: SwapBuffers never lets the CPU run more than a few frames ahead of the GPU,
// so by then the GPU is long done with it - no fence, no query. The Forest uses under 1 MB a frame (~70 frames of
// ring); an upload that doesn't fit takes the old path.
constexpr std::size_t kRingBytes = 64ull << 20;
constexpr std::uint64_t kSafeFrames = 16;
struct FrameSpan {
    std::size_t Start;
    std::uint64_t Frame;
};
struct Ring {
    GLuint Buffer = 0;
    unsigned char* Base = nullptr;
    bool Failed = false;
    std::size_t Head = 0;
    std::size_t FrameStart = 0;
    bool FrameUsed = false;
    std::uint64_t Frame = 0;    // frames ended (EndFrame)
    std::deque<FrameSpan> Done; // past frames' spans, oldest first
};
Ring& R() {
    static Ring* r = new Ring(); // never destroyed: the GL context is gone by the time statics are
    return *r;
}

bool EnsureRing() {
    Ring& r = R();
    if (r.Base || r.Failed) return !r.Failed;
    GLint align = 0;
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
    if (align <= 0 || 256 % align != 0) { r.Failed = true; return false; } // RingFit places at multiples of 256
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glCreateBuffers(1, &r.Buffer);
    glNamedBufferStorage(r.Buffer, (GLsizeiptr)kRingBytes, nullptr, flags);
    r.Base = static_cast<unsigned char*>(glMapNamedBufferRange(r.Buffer, 0, (GLsizeiptr)kRingBytes, flags));
    if (!r.Base) {
        glDeleteBuffers(1, &r.Buffer);
        r.Buffer = 0;
        r.Failed = true;
    } else {
        GpuMemTrack::Set("InstanceBuffer ring (persistently mapped)", kRingBytes);
    }
    return !r.Failed;
}

// Where `size` bytes go in the ring, or ~0 (the fallback takes them): past frames' spans are reused once they are
// kSafeFrames old.
std::size_t Take(std::size_t size) {
    constexpr std::size_t kNone = ~std::size_t(0);
    if (size > kRingBytes / 4 || !EnsureRing()) return kNone;
    Ring& r = R();
    while (!r.Done.empty() && r.Frame - r.Done.front().Frame > kSafeFrames) r.Done.pop_front();
    const bool empty = r.Done.empty() && !r.FrameUsed;
    const std::size_t tail = !r.Done.empty() ? r.Done.front().Start : r.FrameStart;
    const std::size_t at = UploadStaging::RingFit(kRingBytes, r.Head, tail, empty, size);
    if (at == kNone) return kNone;
    if (!r.FrameUsed) r.FrameStart = at;
    r.FrameUsed = true;
    r.Head = at + size;
    return at;
}
}

void EndFrame() {
    Ring& r = R();
    if (r.FrameUsed) r.Done.push_back({r.FrameStart, r.Frame});
    r.FrameUsed = false;
    ++r.Frame;
}

std::vector<glm::mat4>& Staging() { return g_Staging; }
void Append(std::vector<glm::mat4>& staging, const glm::mat4& xform) {
    staging.push_back(xform);
    staging.push_back(glm::mat4(1.0f));
}

void Upload() {
    if (g_Staging.empty()) return;
    const std::size_t bytes = g_Staging.size() * sizeof(glm::mat4);
    const std::size_t at = Take(bytes);
    if (at != ~std::size_t(0)) {
        std::memcpy(R().Base + at, g_Staging.data(), bytes);
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, kBinding, R().Buffer, (GLintptr)at, (GLsizeiptr)bytes);
        g_LastBuffer = R().Buffer;
        g_LastAt = at;
        g_LastBytes = bytes;
        return;
    }
    if (!g_Buffer) glCreateBuffers(1, &g_Buffer);
    // A new data store each time: the draws already issued against the old one still read their own transforms.
    glNamedBufferData(g_Buffer, (GLsizeiptr)(g_Staging.size() * sizeof(glm::mat4)), g_Staging.data(), GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBinding, g_Buffer);
    g_LastBuffer = g_Buffer;
    g_LastAt = 0;
    g_LastBytes = 0;
}

void Rebind() {
    if (!g_LastBuffer) return;
    if (g_LastBytes) glBindBufferRange(GL_SHADER_STORAGE_BUFFER, kBinding, g_LastBuffer, (GLintptr)g_LastAt, (GLsizeiptr)g_LastBytes);
    else glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBinding, g_LastBuffer);
}

bool UploadRange(unsigned binding, const void* data, std::size_t bytes) {
    if (bytes == 0) return false;
    const std::size_t at = Take(bytes);
    if (at == ~std::size_t(0)) return false;
    std::memcpy(R().Base + at, data, bytes);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, binding, R().Buffer, (GLintptr)at, (GLsizeiptr)bytes);
    return true;
}
} // namespace InstanceBuffer

bool MultiDrawEnabled() {
    static const bool envOff = Env::Flag(Env::Var::NoMdi);
    const auto& q = ProjectSettings::Quality();
    return !envOff && q.GpuInstancing && q.GpuDrivenDraws && MeshArena::Enabled();
}

void BuildIndirectBatches(const std::vector<IndirectItem>& items, std::vector<IndirectCommand>& commands,
                          std::vector<IndirectBatch>& batches) {
    static thread_local std::unordered_map<std::uint64_t, std::uint32_t> batchOf;
    static thread_local std::vector<std::uint32_t> itemBatch;
    batchOf.clear();
    batches.clear();
    itemBatch.resize(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto [it, fresh] = batchOf.try_emplace(items[i].Key, (std::uint32_t)batches.size());
        if (fresh) batches.push_back({items[i].Key, items[i].Tag, 0, 0});
        itemBatch[i] = it->second;
        ++batches[it->second].Count;
    }
    std::uint32_t at = 0;
    for (IndirectBatch& b : batches) {
        b.First = at;
        at += b.Count;
        b.Count = 0;
    }
    commands.resize(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        IndirectBatch& b = batches[itemBatch[i]];
        commands[b.First + b.Count++] = items[i].Cmd;
    }
}

namespace IndirectBuffer {
namespace {
GLuint g_Indirect = 0; // the fallback store
GLuint g_Current = 0;  // what the last Upload filled: the ring's buffer or the fallback
std::size_t g_Base = 0, g_Bytes = 0;
}
// A depth pass and the opaque pass each upload their commands: re-specifying one buffer each time (glNamedBufferData)
// waited on NVIDIA's threaded driver like the instance uploads did, so the commands share the instance ring.
void Upload(const std::vector<IndirectCommand>& commands) {
    const std::size_t bytes = std::max<std::size_t>(commands.size(), 1) * sizeof(IndirectCommand);
    const std::size_t at = commands.empty() ? ~std::size_t(0) : InstanceBuffer::Take(bytes);
    if (at != ~std::size_t(0)) {
        std::memcpy(InstanceBuffer::R().Base + at, commands.data(), bytes);
        g_Current = InstanceBuffer::R().Buffer;
        g_Base = at;
    } else {
        if (!g_Indirect) glCreateBuffers(1, &g_Indirect);
        glNamedBufferData(g_Indirect, (GLsizeiptr)bytes, commands.empty() ? nullptr : commands.data(), GL_STREAM_DRAW);
        g_Current = g_Indirect;
        g_Base = 0;
    }
    g_Bytes = bytes;
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_Current);
}
unsigned Handle() { return g_Current; }
std::size_t Base() { return g_Base; }
void BindStorage(unsigned binding) {
    if (g_Current) glBindBufferRange(GL_SHADER_STORAGE_BUFFER, binding, g_Current, (GLintptr)g_Base, (GLsizeiptr)g_Bytes);
}
} // namespace IndirectBuffer

namespace {
// The depth passes' multi-draw path: every caster of an arena model (unskinned, drawn whole) goes out through the
// instance buffer, a group of copies at a time, and each of its sub-meshes becomes one indirect command in the batch
// of sub-meshes that bind alike (page, cascades, culling, cutout / sway). `rest` gets the casters it can't take.
// `sceneSlots`: the copies stage their GPU scene slot (GpuScene.h) instead of their transform - binding 27 holds the
// scene's transforms and 28 the slots while the batches draw (ShadowDepth.vert.glsl uInstanced 2); a group with a copy
// that has no slot goes to `rest`.
// BuildDepthIndirect works the commands out (no GL; without `sceneSlots` it stages the copies' transforms), and
// SubmitDepthIndirect uploads and draws them - every frame, also when DepthCasterCache kept the build.
void BuildDepthIndirect(const std::vector<DepthCaster>& casters, const InstanceGroups& groups, bool sceneSlots,
                        DepthIndirectBuild& out) {
    using Tag = DepthIndirectBuild::Tag;
    std::vector<Tag>& tags = out.Tags;
    std::vector<IndirectItem>& items = out.Items;
    std::vector<std::uint32_t>& slots = out.Slots;
    std::vector<std::uint32_t>& rest = out.Rest;
    tags.clear();
    items.clear();
    slots.clear();
    rest.clear();
    out.Commands.clear();
    out.Batches.clear();
    const std::uint32_t sceneCount = sceneSlots ? GpuScene::Get().Count() : 0;
    std::vector<glm::mat4>& staging = InstanceBuffer::Staging();
    staging.clear();
    for (std::uint32_t g = 0; g < (std::uint32_t)groups.Groups.size(); ++g) {
        const auto& grp = groups.Groups[g];
        const DepthCaster& lead = casters[groups.Order[grp.First]];
        bool slotless = false;
        for (std::uint32_t k = 0; sceneSlots && k < grp.Count && !slotless; ++k)
            slotless = casters[groups.Order[grp.First + k]].Slot >= sceneCount;
        if (lead.Visible || !lead.Ref->InArena() || slotless) {
            for (std::uint32_t k = 0; k < grp.Count; ++k) rest.push_back(groups.Order[grp.First + k]);
            continue;
        }
        const std::uint32_t base = (std::uint32_t)(sceneSlots ? slots.size() : staging.size() / 2);
        for (std::uint32_t k = 0; k < grp.Count; ++k) {
            const DepthCaster& c = casters[groups.Order[grp.First + k]];
            if (sceneSlots) slots.push_back(c.Slot);
            else InstanceBuffer::Append(staging, *c.Xform);
        }
        for (int m = 0; m < lead.Ref->MeshCount(); ++m) {
            int cull = 0;
            std::uint64_t state = 0;
            if (!lead.Ref->DepthMeshState(m, *lead.Slots, cull, state)) continue;
            const MeshArena::Range& r = lead.Ref->Mesh(m).ArenaRange();
            if (!r.IndexCount) continue;
            if (lead.TwoSided) cull = (int)ShaderRenderState::CullMode::Off;
            std::uint64_t key = ((std::uint64_t)r.Page << 52) | ((std::uint64_t)(lead.LayerFirst & 3) << 50) |
                                ((std::uint64_t)(lead.Layers & 7) << 47) | ((std::uint64_t)(cull & 3) << 45);
            key |= (state * 0x9E3779B97F4A7C15ull) >> 20; // the low 44 bits
            tags.push_back({g, (std::uint16_t)m});
            items.push_back({key, (std::uint32_t)tags.size() - 1,
                             {r.IndexCount, grp.Count * (std::uint32_t)std::max(lead.Layers, 1), r.FirstIndex,
                              (std::int32_t)r.BaseVertex, base}});
        }
    }
    if (!items.empty()) BuildIndirectBatches(items, out.Commands, out.Batches);
}

int SubmitDepthIndirect(Shader& shader, const std::vector<DepthCaster>& casters, const InstanceGroups& groups,
                        const DepthIndirectBuild& build, int firstLoc, bool sceneSlots) {
    const std::vector<DepthIndirectBuild::Tag>& tags = build.Tags;
    const std::vector<IndirectBatch>& batches = build.Batches;
    if (batches.empty()) return 0;
    if (sceneSlots) {
        GpuScene::Get().BindSlotList(28, build.Slots);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, InstanceBuffer::kXformBinding, GpuScene::Get().XformBuffer());
    } else {
        InstanceBuffer::Upload();
    }
    IndirectBuffer::Upload(build.Commands);

    const bool passCull = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    GLint passFace = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &passFace);
    shader.SetInt(shader.Loc("uInstanced"), sceneSlots ? 2 : 1);
    shader.SetInt(shader.Loc("uInstanceBase"), 0);
    const int layersLoc = shader.Loc("uInstanceLayers");
    int firstNow = -1, layersNow = -1, cullNow = 0;
    for (const IndirectBatch& b : batches) {
        const DepthIndirectBuild::Tag& t = tags[b.Tag];
        const DepthCaster& lead = casters[groups.Order[groups.Groups[t.Group].First]];
        if (firstLoc >= 0 && lead.LayerFirst != firstNow) shader.SetInt(firstLoc, firstNow = lead.LayerFirst);
        if (lead.Layers != layersNow) shader.SetInt(layersLoc, layersNow = lead.Layers);
        const int cull = (int)((b.Key >> 45) & 3);
        if (cull != cullNow) {
            if (cull == (int)ShaderRenderState::CullMode::Off) glDisable(GL_CULL_FACE);
            else if (cull == 0) { passCull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE); glCullFace((GLenum)passFace); }
            else { glEnable(GL_CULL_FACE); glCullFace(cull == (int)ShaderRenderState::CullMode::Front ? GL_FRONT : GL_BACK); }
            cullNow = cull;
        }
        lead.Ref->BindDepthMesh(shader, t.Mesh, *lead.Slots);
        GLStateCache::BindVertexArray(MeshArena::PageVao((std::uint32_t)(b.Key >> 52)));
        glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, IndirectBuffer::Offset(b.First), (GLsizei)b.Count, 0);
    }
    if (cullNow != 0) { passCull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE); glCullFace((GLenum)passFace); }
    shader.SetInt(shader.Loc("uInstanced"), 0);
    if (sceneSlots) InstanceBuffer::Rebind(); // 27 back to the ring, for whatever reads it next
    return (int)batches.size();
}
} // namespace

namespace {
// Groups the casters for the depth passes (see GroupInstances); the result lives until the next call.
const InstanceGroups& GroupCasters(const std::vector<DepthCaster>& casters, bool instancing) {
    static InstanceGroups groups;
    GroupInstances(
        casters.size(),
        [&](std::uint32_t i) -> std::uint64_t {
            const DepthCaster& c = casters[i];
            // Skinned models pose per entity, and a partly hidden outfit piece draws its own index list.
            if (!instancing || c.Visible || c.Ref->HasBones()) return kNoBatch;
            std::uint64_t seed = (std::uint64_t)(std::uintptr_t)c.Ref->ImportId() * 0x9E3779B97F4A7C15ull;
            seed ^= (std::uint64_t)c.TwoSided | ((std::uint64_t)c.LayerFirst << 8) | ((std::uint64_t)c.Layers << 16);
            return HashSlots(*c.Slots, seed);
        },
        [&](std::uint32_t a, std::uint32_t b) {
            const DepthCaster& x = casters[a];
            const DepthCaster& y = casters[b];
            return x.Ref->ImportId() == y.Ref->ImportId() && x.TwoSided == y.TwoSided && x.LayerFirst == y.LayerFirst && x.Layers == y.Layers &&
                   SameSlots(*x.Slots, *y.Slots);
        },
        groups);
    return groups;
}

// One draw per caster, or per group of two or more copies through the instance buffer.
int DrawDepthCastersDirect(Shader& shader, const std::vector<DepthCaster>& casters, int firstLoc) {
    if (casters.empty()) return 0;
    const bool instancing = ProjectSettings::Quality().GpuInstancing;
    const InstanceGroups& groups = GroupCasters(casters, instancing);

    // Every group of two or more: its transforms, back to back (a depth pass reads no normal matrix).
    static std::vector<int> base;
    base.assign(groups.Groups.size(), -1);
    std::vector<glm::mat4>& staging = InstanceBuffer::Staging();
    staging.clear();
    for (std::size_t g = 0; g < groups.Groups.size(); ++g) {
        const auto& grp = groups.Groups[g];
        if (grp.Count < 2) continue;
        base[g] = (int)(staging.size() / 2);
        for (std::uint32_t k = 0; k < grp.Count; ++k) {
            InstanceBuffer::Append(staging, *casters[groups.Order[grp.First + k]].Xform);
        }
    }
    InstanceBuffer::Upload();

    const int modelLoc = shader.Loc("uModel");
    const int instancedLoc = shader.Loc("uInstanced");
    const int baseLoc = shader.Loc("uInstanceBase");
    const int layersLoc = shader.Loc("uInstanceLayers");
    int instancedNow = -1, firstNow = -1, draws = 0;
    for (std::size_t g = 0; g < groups.Groups.size(); ++g) {
        const auto& grp = groups.Groups[g];
        const DepthCaster& lead = casters[groups.Order[grp.First]];
        if (firstLoc >= 0 && lead.LayerFirst != firstNow) {
            shader.SetInt(firstLoc, lead.LayerFirst);
            firstNow = lead.LayerFirst;
        }
        const int instanced = base[g] >= 0 ? 1 : 0;
        if (instanced != instancedNow) {
            shader.SetInt(instancedLoc, instanced);
            instancedNow = instanced;
        }
        // Cull is on for the whole pass (DrawDepthOnly's per-mesh overrides put it back), so Two Sided just turns it
        // off around its own draw.
        if (lead.TwoSided) glDisable(GL_CULL_FACE);
        if (instanced) {
            shader.SetInt(baseLoc, base[g]);
            shader.SetInt(layersLoc, lead.Layers);
            lead.Ref->DrawDepthOnly(shader, *lead.Slots, (int)grp.Count * lead.Layers, nullptr);
        } else {
            shader.SetMat4(modelLoc, *lead.Xform);
            lead.Ref->DrawDepthOnly(shader, *lead.Slots, lead.Layers, lead.Visible);
        }
        if (lead.TwoSided) glEnable(GL_CULL_FACE);
        ++draws;
    }
    if (instancedNow != 0) shader.SetInt(instancedLoc, 0);
    return draws;
}
} // namespace

namespace {
DepthCasterCache::Totals g_DepthCacheTotals;
}
DepthCasterCache::Totals DepthCasterCache::AllTotals() { return g_DepthCacheTotals; }

bool DepthCasterCache::Reuse(const std::vector<DepthCaster>& casters, const Epochs& epochs, std::uint64_t frame) {
    m_Next.resize(casters.size());
    for (std::size_t i = 0; i < casters.size(); ++i) {
        const DepthCaster& c = casters[i];
        Entry& e = m_Next[i];
        std::memset(&e, 0, sizeof e);
        e.Ref = c.Ref;
        e.Import = c.Ref ? c.Ref->ImportStamp() : 0;
        e.SlotList = c.Slots;
        std::uint64_t h = 0xcbf29ce484222325ull;
        if (c.Slots)
            for (const auto& m : *c.Slots) h = (h ^ (std::uint64_t)(std::uintptr_t)m.get()) * 0x100000001b3ull;
        e.SlotsHash = h;
        e.Visible = c.Visible;
        e.Slot = c.Slot;
        e.Run = (c.TwoSided ? 1 : 0) | (c.LayerFirst << 1) | (c.Layers << 8);
    }
    const bool same = m_Valid && frame >= m_BuiltFrame && frame - m_BuiltFrame < kMaxAgeFrames && epochs == m_Epochs &&
                      m_Next.size() == m_Key.size() &&
                      (m_Key.empty() || std::memcmp(m_Next.data(), m_Key.data(), m_Key.size() * sizeof(Entry)) == 0);
    if (same) {
        ++m_Reused;
        ++g_DepthCacheTotals.Reused;
        return true;
    }
    m_Key.swap(m_Next);
    m_Epochs = epochs;
    m_Valid = false;
    ++m_Rebuilt;
    ++g_DepthCacheTotals.Rebuilt;
    return false;
}

int DrawDepthCasters(Shader& shader, const std::vector<DepthCaster>& casters, int firstLoc, World* sceneWorld,
                     DepthCasterCache* cache) {
    if (casters.empty()) return 0;
    const bool instancing = ProjectSettings::Quality().GpuInstancing;
    if (!instancing || !MultiDrawEnabled()) return DrawDepthCastersDirect(shader, casters, firstLoc);
    const bool sceneSlots = sceneWorld && GpuScene::Get().SyncOnce(*sceneWorld);

    // GPU-driven: the arena models go out as multi-draws; what's left (skinned, partly hidden) draws directly.
    // A cache only keeps a slot build: a transform build stages this frame's matrices.
    static DepthIndirectBuild scratch;
    const InstanceGroups* useGroups = nullptr;
    const DepthIndirectBuild* build = &scratch;
    if (cache && sceneSlots) {
        const std::uint64_t frame = Time::FrameCount();
        if (!cache->Reuse(casters, {RenderSettingsEpoch(), GpuScene::Get().Count()}, frame)) {
            cache->Groups = GroupCasters(casters, instancing);
            BuildDepthIndirect(casters, cache->Groups, sceneSlots, cache->Build);
            cache->Built(frame);
        }
        useGroups = &cache->Groups;
        build = &cache->Build;
    } else {
        if (cache) cache->Invalidate();
        useGroups = &GroupCasters(casters, instancing); // valid until the next GroupCasters (the rest's, after the draws)
        BuildDepthIndirect(casters, *useGroups, sceneSlots, scratch);
    }
    const int indirectDraws = SubmitDepthIndirect(shader, casters, *useGroups, *build, firstLoc, sceneSlots);
    const std::vector<std::uint32_t>& rest = build->Rest;
    static std::vector<DepthCaster> restCasters;
    if (rest.empty()) return indirectDraws;
    restCasters.clear();
    for (std::uint32_t i : rest) restCasters.push_back(casters[i]);
    return indirectDraws + DrawDepthCastersDirect(shader, restCasters, firstLoc);
}
