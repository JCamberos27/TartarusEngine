#include "InstanceBatch.h"
#include "gl.h"
#include "Model.h"
#include "Shader.h"
#include "MaterialAsset.h"
#include "ProjectSettings.h"
#include "GLStateCache.h"
#include "ShaderAsset.h"
#include <algorithm>
#include <cstdlib>
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
constexpr GLuint kBinding = 27; // ModelVertex.glsl / ShadowDepth.vert.glsl InstanceBlock
GLuint g_Buffer = 0;
std::vector<glm::mat4> g_Staging;
}

std::vector<glm::mat4>& Staging() { return g_Staging; }

void Upload() {
    if (g_Staging.empty()) return;
    if (!g_Buffer) glCreateBuffers(1, &g_Buffer);
    // A new data store each time: the draws already issued against the old one still read their own transforms.
    glNamedBufferData(g_Buffer, (GLsizeiptr)(g_Staging.size() * sizeof(glm::mat4)), g_Staging.data(), GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBinding, g_Buffer);
}
} // namespace InstanceBuffer

bool MultiDrawEnabled() {
    static const bool envOff = [] {
        char* e = nullptr;
        size_t n = 0;
        const bool off = _dupenv_s(&e, &n, "TARTARUS_NO_MDI") == 0 && e && e[0] == '1';
        std::free(e);
        return off;
    }();
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
GLuint g_Indirect = 0;
}
void Upload(const std::vector<IndirectCommand>& commands) {
    if (!g_Indirect) glCreateBuffers(1, &g_Indirect);
    glNamedBufferData(g_Indirect, (GLsizeiptr)(std::max<std::size_t>(commands.size(), 1) * sizeof(IndirectCommand)),
                      commands.empty() ? nullptr : commands.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_Indirect);
}
} // namespace IndirectBuffer

namespace {
// The depth passes' multi-draw path: every caster of an arena model (unskinned, drawn whole) goes out through the
// instance buffer, a group of copies at a time, and each of its sub-meshes becomes one indirect command in the batch
// of sub-meshes that bind alike (page, cascades, culling, cutout / sway). `rest` gets the casters it can't take.
int DrawDepthIndirect(Shader& shader, const std::vector<DepthCaster>& casters, const InstanceGroups& groups, int firstLoc,
                      std::vector<std::uint32_t>& rest) {
    struct Tag { std::uint32_t Group; std::uint16_t Mesh; };
    static std::vector<Tag> tags;
    static std::vector<IndirectItem> items;
    static std::vector<IndirectCommand> commands;
    static std::vector<IndirectBatch> batches;
    tags.clear();
    items.clear();
    std::vector<glm::mat4>& staging = InstanceBuffer::Staging();
    staging.clear();
    for (std::uint32_t g = 0; g < (std::uint32_t)groups.Groups.size(); ++g) {
        const auto& grp = groups.Groups[g];
        const DepthCaster& lead = casters[groups.Order[grp.First]];
        if (lead.Visible || !lead.Ref->InArena()) {
            for (std::uint32_t k = 0; k < grp.Count; ++k) rest.push_back(groups.Order[grp.First + k]);
            continue;
        }
        const std::uint32_t base = (std::uint32_t)(staging.size() / 2);
        for (std::uint32_t k = 0; k < grp.Count; ++k) {
            staging.push_back(*casters[groups.Order[grp.First + k]].Xform);
            staging.push_back(glm::mat4(1.0f));
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
    if (items.empty()) return 0;
    InstanceBuffer::Upload();
    BuildIndirectBatches(items, commands, batches);
    IndirectBuffer::Upload(commands);

    const bool passCull = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    GLint passFace = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &passFace);
    shader.SetInt(shader.Loc("uInstanced"), 1);
    shader.SetInt(shader.Loc("uInstanceBase"), 0);
    const int layersLoc = shader.Loc("uInstanceLayers");
    int firstNow = -1, layersNow = -1, cullNow = 0;
    for (const IndirectBatch& b : batches) {
        const Tag& t = tags[b.Tag];
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
    return (int)batches.size();
}
} // namespace

namespace {
bool g_DirectOnly = false; // drawing the casters the multi-draw path left
}

int DrawDepthCasters(Shader& shader, const std::vector<DepthCaster>& casters, int firstLoc) {
    if (casters.empty()) return 0;
    const bool instancing = ProjectSettings::Quality().GpuInstancing;
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

    // GPU-driven: the arena models go out as multi-draws; what's left (skinned, partly hidden) draws as below.
    if (instancing && !g_DirectOnly && MultiDrawEnabled()) {
        static std::vector<std::uint32_t> rest;
        static std::vector<DepthCaster> restCasters;
        rest.clear();
        const int indirectDraws = DrawDepthIndirect(shader, casters, groups, firstLoc, rest);
        if (rest.empty()) return indirectDraws;
        restCasters.clear();
        for (std::uint32_t i : rest) restCasters.push_back(casters[i]);
        g_DirectOnly = true;
        const int directDraws = DrawDepthCasters(shader, restCasters, firstLoc);
        g_DirectOnly = false;
        return indirectDraws + directDraws;
    }

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
            staging.push_back(*casters[groups.Order[grp.First + k]].Xform);
            staging.push_back(glm::mat4(1.0f));
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
