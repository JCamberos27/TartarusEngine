#include "InstanceBatch.h"
#include "gl.h"
#include "Model.h"
#include "Shader.h"
#include "MaterialAsset.h"
#include "ProjectSettings.h"

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
