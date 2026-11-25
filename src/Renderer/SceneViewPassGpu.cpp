// G2-b: the camera view's batched static world culled and drawn from the GPU (GpuCull.h). The draw table holds every
// GPU scene slot that would always batch on the CPU path (static, unhidden, opaque, instancing programs, in the mesh
// arena), grouped and batched as BuildBatches would group and batch them. Per view, the GPU tests each member against
// the view (stage A), the depth pre-pass draws the survivors, Hi-Z culls them against that depth, and the opaque pass
// draws what is left - the CPU never walks them: GatherVisible skips them (VisibilityQuery::SkipSlots).
// What can change a member's draw from frame to frame (a LOD cross-fade, blood, a move that may change its probes or
// GI volumes) sends it down the CPU path for as long as it lasts: the GPU leaves it out and the gather takes it.
#include "SceneViewPass.h"
#include "GpuCull.h"
#include "GpuScene.h"
#include "OpaqueBatchKey.h"
#include "RenderFrameContext.h"
#include "HdrTarget.h"
#include "World.h"
#include "Components.h"
#include "Shader.h"
#include "ReflectionProbeArray.h"
#include "GIVolumes.h"
#include "MaterialAsset.h"
#include "ProjectSettings.h"
#include "BloodRenderer.h"
#include "FurRenderer.h"
#include "MeshArena.h"
#include "ShaderAsset.h"
#include "GLStateCache.h"
#include "gl.h"
#include "Core/Profiler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
bool PerspectiveProj(const glm::mat4& proj) { return std::abs(proj[3][3]) < 0.5f; }

void SetBit(std::vector<std::uint32_t>& bits, std::uint32_t i) { bits[i >> 5] |= 1u << (i & 31u); }
void ClearBit(std::vector<std::uint32_t>& bits, std::uint32_t i) { bits[i >> 5] &= ~(1u << (i & 31u)); }
bool TestBit(const std::vector<std::uint32_t>& bits, std::uint32_t i) {
    return (i >> 5) < bits.size() && ((bits[i >> 5] >> (i & 31u)) & 1u) != 0;
}
constexpr std::uint32_t kNone = ~0u;
} // namespace

bool SceneRenderer::ViewPass::GpuDrawsWanted() const {
    const RenderFrameContext& ctx = m_Ctx;
    const auto& q = ProjectSettings::Quality();
    // The table's batches are all pre-passed (GL_EQUAL, no depth writes), which is what makes their draw order free:
    // without a pre-pass, or where the CPU path draws differently (probe and GI bake captures), it stays off.
    return m_In.depthShader && q.DepthPrepass && q.GpuInstancing && !ctx.ProbeCapture && !ctx.GIBake &&
           ctx.DebugView == 0 && GpuCull::DrawsEnabled();
}

// The table, rebuilt when what it was built from changed.
void SceneRenderer::ViewPass::PrepareGpuFrame(VisibilityQuery& inView) {
    SceneGpuTable& t = m_S.Gpu;
    GpuScene& scene = GpuScene::Get();
    scene.Sync(m_World);
    if (!scene.Ready()) {
        m_GpuDraws = false;
        return;
    }
    // Last frame's (or view's) temporary CPU-path members are table members again.
    for (std::uint32_t slot : t.Cleared) SetBit(t.Owned, slot);
    t.Cleared.clear();

    const ReflectionProbeArray* probes = m_In.probeArray;
    const GIVolumes* gi = m_In.giVolumes;
    const std::uint64_t probeHash = probes && probes->Count() > 0 ? probes->LayoutHash() : 0;
    const std::uint64_t giHash = gi ? gi->LayoutHash() : 0;
    const bool stale = !t.Built || t.Gen != SceneSlotTableGeneration() || t.SceneCount != scene.Count() ||
                       t.Probes != probes || t.GI != gi || t.ProbeHash != probeHash || t.GIHash != giHash ||
                       t.Variant != FrameVariantBits();
    if (stale) {
        BuildGpuTable();
    } else {
        // Members that moved: their probes or GI volumes may be others now. The CPU path draws them until the next build.
        TakeMovedSceneSlots(t.MovedNow);
        std::uint32_t moved = 0;
        for (std::uint32_t slot : t.MovedNow) {
            if (slot >= t.MemberEntity.size() || t.MemberEntity[slot] == kNone || TestBit(t.Moved, slot)) continue;
            SetBit(t.Moved, slot);
            ClearBit(t.Owned, slot);
            ++moved;
        }
        (void)moved;
    }
    if (!t.Members) {
        m_GpuDraws = false;
        return;
    }

    // This frame's: a LOD cross-fade (dithered, one by one) and blood on it (its splats bind per draw).
    const auto sendToCpu = [&](std::uint32_t integral) {
        const std::uint32_t index = (std::uint32_t)entt::to_entity(entt::entity{integral});
        if (index >= t.SlotOfEntity.size()) return;
        const std::uint32_t slot = t.SlotOfEntity[index];
        if (slot == kNone || t.MemberEntity[slot] != integral || !TestBit(t.Owned, slot)) return;
        ClearBit(t.Owned, slot);
        t.Cleared.push_back(slot);
    };
    for (entt::entity e : m_World.LodFadeCandidates())
        if (m_World.LodFadeOf(e) != 0.0f) sendToCpu((std::uint32_t)entt::to_integral(e));
    BloodRenderer::Get().ForEachSplatEntity([&](unsigned e) { sendToCpu((std::uint32_t)e); });
    inView.SkipSlots = t.Owned.data();
}

void SceneRenderer::ViewPass::BuildGpuTable() {
    PROFILE_SCOPE("Scene: GPU Draw Table");
    SceneGpuTable& t = m_S.Gpu;
    GpuScene& scene = GpuScene::Get();
    const std::uint32_t n = scene.Count();
    const ReflectionProbeArray* probes = m_In.probeArray;
    const GIVolumes* gi = m_In.giVolumes;
    t.Built = true;
    t.Gen = SceneSlotTableGeneration();
    t.SceneCount = n;
    t.Probes = probes;
    t.GI = gi;
    t.AnyProbes = probes && probes->Count() > 0;
    t.ProbeHash = t.AnyProbes ? probes->LayoutHash() : 0;
    t.GIHash = gi ? gi->LayoutHash() : 0;
    t.Variant = FrameVariantBits();
    TakeMovedSceneSlots(t.MovedNow); // what moved before this build is read as it is now
    const std::size_t words = ((std::size_t)n + 31) / 32;
    t.Owned.assign(words, 0u);
    t.Moved.assign(words, 0u);
    t.MemberEntity.assign(n, kNone);
    t.SlotOfEntity.clear();
    t.Cleared.clear();
    t.Leads.clear();
    t.OpaqueTags.clear();
    t.OpaqueBatches.clear();
    t.DepthTags.clear();
    t.DepthBatches.clear();
    t.Commands.clear();
    t.OpaqueCommands = 0;
    t.Members = 0;

    // The members, as the classify step would make their draw items.
    auto& reg = m_World.Registry;
    static std::vector<DrawItem> items;
    static std::vector<std::uint32_t> itemSlot;
    items.clear();
    itemSlot.clear();
    SceneSlot s;
    for (std::uint32_t slot = 0; slot < n; ++slot) {
        if (!ReadSceneSlot(slot, s) || !s.Alive || !s.Renderable) continue;
        constexpr std::uint16_t kNotStatic = kCandPosed | kCandViewModel | kCandPlayerBody | kCandPoseSource |
                                             kCandOwnerViewOnly | kCandHiddenFromOwner | kCandInactive;
        if (s.Flags & kNotStatic) continue;
        const RenderableComponent& r = *s.Renderable;
        Model* m = r.ModelRef.get();
        if (!m || m->HasBones() || !m->InArena() || m->MeshCount() <= 0) continue;
        const entt::entity e = s.Entity;
        if (!reg.valid(e) || reg.any_of<RigidbodyComponent, OutfitHideTag, OutfitLayerTag, OutfitPieceComponent,
                                        OpenableComponent, PlayerBodyTag>(e))
            continue;
        const auto& slots = r.Materials;
        bool ok = true;
        std::uint32_t key = 0;
        for (int i = 0; i < m->MeshCount() && ok; ++i) {
            const MaterialAsset* ma = i < (int)slots.size() ? slots[i].get() : nullptr;
            if (ma && ma->RenderQueue == MaterialAsset::Queue::Transparent) ok = false;
            else if (!ProgramFor(ma, key)->Instancing()) ok = false;
        }
        if (!ok || FurRenderer::HasFur(*m, slots)) continue;

        const glm::mat4& model = s.Xform;
        const glm::vec3 centre = s.Bounded ? glm::vec3(model * glm::vec4((s.LocalMin + s.LocalMax) * 0.5f, 1.0f))
                                           : glm::vec3(model[3]);
        const std::uint64_t matKey = (!slots.empty() && slots[0]) ? slots[0]->Mat.Hash() : m->MaterialSortKey();
        std::uint32_t progKeyUnused = 0;
        const Shader* prog = ProgramFor(!slots.empty() ? slots[0].get() : nullptr, progKeyUnused);
        const int layerBit = (int)(1u << std::clamp(s.Layer, 0, 31));
        DrawItem item{model, m, &slots, matKey, prog, m->MeshCount(), (int)m->TriangleCount(), (int)m->VertexCount(),
                      2000, 0.0f, centre, Model::MeshPass::All, r.ReceiveShadows, layerBit};
        item.StaticReceiver = true; // nothing skinned or simulated
        item.GIMask = gi ? (s.Bounded ? gi->MaskFor(s.WorldMin, s.WorldMax) : gi->AllMask()) : 0;
        item.SortId = (std::uint32_t)entt::to_integral(e);
        item.WorldMin = s.WorldMin;
        item.WorldMax = s.WorldMax;
        item.Bounded = s.Bounded;
        item.Normal = s.Normal;
        item.Slot = slot;
        // No blood lookup for the batch's lead: a member with splats leaves the table for that frame.
        item.Entity = 0xFFFFFFFFu;
        items.push_back(item);
        itemSlot.push_back(slot);
    }

    // Groups, as BuildBatches groups (the members' constant fields - pass, sidedness, SSAO, flat L1 - are equal).
    static std::vector<std::uint32_t> probeKey;
    probeKey.assign(items.size(), 0u);
    if (t.AnyProbes)
        for (std::size_t i = 0; i < items.size(); ++i) probeKey[i] = probes->SelectionKey(items[i].Centre);
    static InstanceGroups groups;
    GroupInstances(
        items.size(),
        [&](std::uint32_t i) -> std::uint64_t {
            const DrawItem& it = items[i];
            std::uint64_t seed = (std::uint64_t)(std::uintptr_t)it.Ref->ImportId() * 0x9E3779B97F4A7C15ull;
            seed ^= (std::uint64_t)it.Pass | ((std::uint64_t)it.ReceiveShadows << 5) | ((std::uint64_t)(std::uint32_t)it.LayerBit << 8) |
                    ((std::uint64_t)probeKey[i] << 32) ^ ((std::uint64_t)it.GIMask * 0xC2B2AE3D27D4EB4Full);
            return HashSlots(*it.Slots, seed);
        },
        [&](std::uint32_t a, std::uint32_t b) {
            const DrawItem& x = items[a];
            const DrawItem& y = items[b];
            return x.Ref->ImportId() == y.Ref->ImportId() && x.ReceiveShadows == y.ReceiveShadows && x.LayerBit == y.LayerBit &&
                   probeKey[a] == probeKey[b] && x.GIMask == y.GIMask && SameSlots(*x.Slots, *y.Slots);
        },
        groups);

    // Runs (in slot order within a group), the opaque and depth items of each group's lead.
    static std::vector<std::uint32_t> runs;
    static std::vector<GpuCull::Group> stageGroups;
    static std::vector<float> sway;
    static std::vector<IndirectItem> opaqueItems, depthItems;
    runs.clear();
    stageGroups.clear();
    sway.clear();
    opaqueItems.clear();
    depthItems.clear();
    t.Leads.reserve(groups.Groups.size());
    std::uint32_t maxEntity = 0;
    for (std::size_t g = 0; g < groups.Groups.size(); ++g) {
        const InstanceGroups::Group& grp = groups.Groups[g];
        const std::uint32_t base = (std::uint32_t)runs.size();
        for (std::uint32_t k = 0; k < grp.Count; ++k) {
            const std::uint32_t i = groups.Order[grp.First + k];
            const std::uint32_t slot = itemSlot[i];
            runs.push_back(slot);
            SetBit(t.Owned, slot);
            t.MemberEntity[slot] = items[i].SortId;
            maxEntity = std::max(maxEntity, (std::uint32_t)entt::to_entity(entt::entity{items[i].SortId}));
        }
        const DrawItem& lead = items[groups.Order[grp.First]];
        t.Leads.push_back(lead);
        stageGroups.push_back({base, grp.Count, 0, 0});
        float sw = 0.0f; // foliage sways past its bounds (BuildBatches' Hi-Z boxes)
        for (int m = 0; m < lead.Ref->MeshCount(); ++m) {
            const MaterialAsset* ma = m < (int)lead.Slots->size() ? (*lead.Slots)[m].get() : nullptr;
            const Material& mat = ma ? ma->Mat : lead.Ref->MeshMaterial(m);
            if (mat.Foliage) sw = std::max(sw, HiZCuller::FoliageSway(std::abs(mat.WindStrength), Material::Wind().Strength));
        }
        sway.push_back(sw);

        OpaqueBatchKey::ObjectState objectState;
        objectState.ReceiveShadows = lead.ReceiveShadows;
        objectState.StaticReceiver = lead.StaticReceiver;
        objectState.NoSsao = false;
        objectState.DoubleSided = false;
        objectState.Prepassed = true;
        objectState.LayerBit = (std::uint32_t)lead.LayerBit;
        objectState.ProbeKey = probeKey[groups.Order[grp.First]];
        objectState.GIMask = lead.GIMask;
        objectState.GIFlatL1 = false;
        const std::uint64_t objectKey = OpaqueBatchKey::Object(objectState);
        for (int m = 0; m < lead.Ref->MeshCount(); ++m) {
            const MaterialAsset* ma = m < (int)lead.Slots->size() ? (*lead.Slots)[m].get() : nullptr;
            const MeshArena::Range& r = lead.Ref->Mesh(m).ArenaRange();
            if (!r.IndexCount) continue;
            // Opaque (BuildBatches' key).
            std::uint32_t variant = 0;
            const Shader* prog = ProgramFor(ma, variant);
            const Material& mat = ma ? ma->Mat : lead.Ref->MeshMaterial(m);
            std::uint64_t key = objectKey;
            const auto mix = [&key](std::uint64_t v) { key ^= v + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2); };
            mix((std::uint64_t)(std::uintptr_t)prog);
            const std::uint64_t state = lead.Ref->MeshStateKey(m, *lead.Slots, false);
            const bool alphaTest = mat.AlphaClip || (ma && ma->RenderQueue == MaterialAsset::Queue::AlphaTest);
            const std::uint64_t shaderId = ma ? (std::uint64_t)(std::uintptr_t)ma->Shader.get() : 1ull;
            std::uint64_t matKey = mat.HashWithExtraProps();
            matKey ^= shaderId * 0x9E3779B97F4A7C15ull;
            matKey ^= alphaTest ? 0x5bd1e995ull : 0ull;
            mix(mat.HashWithExtraProps());
            mix(shaderId);
            mix(state);
            mix(r.Page);
            mix(alphaTest ? 1ull : 0ull);
            t.OpaqueTags.push_back({(std::uint32_t)g, (std::uint16_t)m, true, prog, state, matKey, objectKey});
            opaqueItems.push_back({key, (std::uint32_t)t.OpaqueTags.size() - 1,
                                   {r.IndexCount, 0u, r.FirstIndex, (std::int32_t)r.BaseVertex, base}});
            // Depth (DrawDepthIndirect's key; one layer, not two-sided).
            int cull = 0;
            std::uint64_t depthState = 0;
            if (!lead.Ref->DepthMeshState(m, *lead.Slots, cull, depthState)) continue;
            std::uint64_t dkey = ((std::uint64_t)r.Page << 52) | ((std::uint64_t)(1 & 7) << 47) | ((std::uint64_t)(cull & 3) << 45);
            dkey |= (depthState * 0x9E3779B97F4A7C15ull) >> 20;
            t.DepthTags.push_back({(std::uint32_t)g, (std::uint16_t)m});
            depthItems.push_back({dkey, (std::uint32_t)t.DepthTags.size() - 1,
                                  {r.IndexCount, 0u, r.FirstIndex, (std::int32_t)r.BaseVertex, base}});
        }
    }
    t.Members = (std::uint32_t)runs.size();
    t.SlotOfEntity.assign((std::size_t)maxEntity + 1, kNone);
    for (std::uint32_t slot : runs) t.SlotOfEntity[(std::uint32_t)entt::to_entity(entt::entity{t.MemberEntity[slot]})] = slot;

    // Commands: the opaque batches', then the depth batches'. Each group's command list (by baseInstance = its base).
    static std::vector<IndirectCommand> opaqueCmds, depthCmds;
    BuildIndirectBatches(opaqueItems, opaqueCmds, t.OpaqueBatches);
    BuildIndirectBatches(depthItems, depthCmds, t.DepthBatches);
    t.OpaqueCommands = (std::uint32_t)opaqueCmds.size();
    for (IndirectBatch& b : t.DepthBatches) b.First += t.OpaqueCommands;
    t.Commands = opaqueCmds;
    t.Commands.insert(t.Commands.end(), depthCmds.begin(), depthCmds.end());

    static std::vector<std::uint32_t> groupOfBase, opaqueCount, totalCount;
    groupOfBase.assign(t.Members + 1, kNone);
    for (std::uint32_t g = 0; g < (std::uint32_t)stageGroups.size(); ++g) groupOfBase[stageGroups[g].Base] = g;
    opaqueCount.assign(stageGroups.size(), 0);
    totalCount.assign(stageGroups.size(), 0);
    for (std::uint32_t i = 0; i < (std::uint32_t)t.Commands.size(); ++i) {
        const std::uint32_t g = groupOfBase[t.Commands[i].BaseInstance];
        ++totalCount[g];
        if (i < t.OpaqueCommands) ++opaqueCount[g];
    }
    // The lists follow the runs: a group's opaque commands, then its depth ones.
    static std::vector<std::uint32_t> slotsBuf;
    slotsBuf = runs;
    std::uint32_t at = (std::uint32_t)slotsBuf.size();
    static std::vector<std::uint32_t> fill;
    fill.assign(stageGroups.size(), 0);
    for (std::uint32_t g = 0; g < (std::uint32_t)stageGroups.size(); ++g) {
        stageGroups[g].CmdFirst = at;
        stageGroups[g].CmdCount = totalCount[g];
        at += totalCount[g];
    }
    slotsBuf.resize(at);
    static std::vector<std::uint32_t> depthFill;
    depthFill.assign(stageGroups.size(), 0);
    for (std::uint32_t i = 0; i < (std::uint32_t)t.Commands.size(); ++i) {
        const std::uint32_t g = groupOfBase[t.Commands[i].BaseInstance];
        if (i < t.OpaqueCommands) slotsBuf[stageGroups[g].CmdFirst + fill[g]++] = i;
        else slotsBuf[stageGroups[g].CmdFirst + opaqueCount[g] + depthFill[g]++] = i;
    }
    static std::vector<glm::uvec4> hizGroups;
    hizGroups.clear();
    for (std::uint32_t g = 0; g < (std::uint32_t)stageGroups.size(); ++g) {
        std::uint32_t swayBits;
        std::memcpy(&swayBits, &sway[g], 4);
        hizGroups.push_back({stageGroups[g].Base, swayBits, stageGroups[g].CmdFirst, opaqueCount[g]});
    }
    GpuCull::UploadTable(t.Gl, stageGroups, hizGroups, slotsBuf, t.Members);
}

void SceneRenderer::ViewPass::CullGpuView(const VisibilityQuery& framed) {
    SceneGpuTable& t = m_S.Gpu;
    m_GpuCulled = false;
    if (!m_GpuDraws || !t.Members) return;
    // The reject bits: moved members, this frame's CPU-path ones, and what this view's LOD levels leave out.
    t.Reject = t.Moved;
    for (std::uint32_t slot : t.Cleared) SetBit(t.Reject, slot);
    for (entt::entity e : m_World.Registry.storage<LodCulledTag>()) {
        const std::uint32_t integral = (std::uint32_t)entt::to_integral(e);
        const std::uint32_t index = (std::uint32_t)entt::to_entity(e);
        if (index >= t.SlotOfEntity.size()) continue;
        const std::uint32_t slot = t.SlotOfEntity[index];
        if (slot != kNone && t.MemberEntity[slot] == integral) SetBit(t.Reject, slot);
    }
    // This view's counts go into its own copy of the commands (views draw in turn from one table).
    IndirectBuffer::Upload(t.Commands);
    m_GpuCmdBuffer = IndirectBuffer::Handle();
    m_GpuCmdBase = IndirectBuffer::Base();
    IndirectBuffer::BindStorage(29);
    m_GpuCulled = GpuCull::CullView(t.Gl, GpuCull::MakeQuery(framed), t.Reject);
    m_GpuRemap = t.Gl.StageA;
    m_ModelShader.Bind();
}

void SceneRenderer::ViewPass::DrawGpuDepth(Shader& depth) {
    const SceneGpuTable& t = m_S.Gpu;
    if (!m_GpuCulled || t.DepthBatches.empty()) return;
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_GpuCmdBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 28, m_GpuRemap);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, InstanceBuffer::kXformBinding, GpuScene::Get().XformBuffer());
    depth.SetInt(depth.Loc("uInstanced"), 2);
    depth.SetInt(depth.Loc("uInstanceBase"), 0);
    depth.SetInt(depth.Loc("uInstanceLayers"), 1);
    const bool passCull = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    GLint passFace = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &passFace);
    int cullNow = 0;
    for (const IndirectBatch& b : t.DepthBatches) {
        const SceneGpuTable::DepthTag& tag = t.DepthTags[b.Tag];
        const DrawItem& lead = t.Leads[tag.Group];
        const int cull = (int)((b.Key >> 45) & 3);
        if (cull != cullNow) {
            if (cull == (int)ShaderRenderState::CullMode::Off) glDisable(GL_CULL_FACE);
            else if (cull == 0) { passCull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE); glCullFace((GLenum)passFace); }
            else { glEnable(GL_CULL_FACE); glCullFace(cull == (int)ShaderRenderState::CullMode::Front ? GL_FRONT : GL_BACK); }
            cullNow = cull;
        }
        lead.Ref->BindDepthMesh(depth, tag.Mesh, *lead.Slots);
        GLStateCache::BindVertexArray(MeshArena::PageVao((std::uint32_t)(b.Key >> 52)));
        glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                                    (const void*)(m_GpuCmdBase + (std::size_t)b.First * sizeof(IndirectCommand)),
                                    (GLsizei)b.Count, 0);
        ++m_Stats.DrawCalls;
    }
    if (cullNow != 0) { passCull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE); glCullFace((GLenum)passFace); }
    depth.SetInt(depth.Loc("uInstanced"), 0);
    InstanceBuffer::Rebind(); // 27 back to the ring
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, IndirectBuffer::Handle());
}

void SceneRenderer::ViewPass::CullGpuHiz() {
    SceneGpuTable& t = m_S.Gpu;
    if (!m_GpuCulled || !m_HizDepth || !m_Ctx.TxHdr) return;
    HiZCuller::Chained c;
    c.DepthTex = m_HizDepth;
    c.W = m_Ctx.TxHdr->Width();
    c.H = m_Ctx.TxHdr->Height();
    c.PyramidBuilt = m_HizCulled; // the CPU path's cull built this view's pyramid
    c.Viewport = glm::vec4(m_Fs.vp[0], m_Fs.vp[1], m_Fs.vp[2], m_Fs.vp[3]);
    c.ViewProj = m_Ctx.Proj * m_Ctx.View;
    c.Eye = m_Ctx.ViewPos;
    c.Groups = t.Gl.HizGroups;
    c.GroupCount = t.Gl.GroupCount;
    c.Bounds = GpuScene::Get().BoundsBuffer();
    c.BoundsBytes = (std::size_t)GpuScene::Get().Count() * 2 * sizeof(glm::vec4);
    c.CmdList = t.Gl.Slots;
    c.InSlots = t.Gl.StageA;
    c.OutRemap = t.Gl.Final;
    c.Commands = m_GpuCmdBuffer;
    c.CommandsOffset = m_GpuCmdBase;
    c.CommandsBytes = t.Commands.size() * sizeof(IndirectCommand);
    if (HiZCuller::CullChained(c)) {
        m_GpuRemap = t.Gl.Final;
    }
    m_ModelShader.Bind();
}

void SceneRenderer::ViewPass::DrawGpuOpaque(Model::DrawPassState& passState, unsigned char& depthWrites) {
    const SceneGpuTable& t = m_S.Gpu;
    if (!m_GpuCulled || t.OpaqueBatches.empty()) return;
    if (depthWrites != GL_FALSE) { // pre-passed: tested GL_EQUAL against the pre-pass, writes off
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_EQUAL);
        depthWrites = GL_FALSE;
        passState.DepthMask = false;
        passState.DepthFunc = GL_EQUAL;
    }
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_GpuCmdBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 28, m_GpuRemap);
    const auto floatBits = [](float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; };
    // Grouped by program, render state and material, as the CPU path's pre-passed batches draw.
    static std::vector<std::uint32_t> order;
    order.resize(t.OpaqueBatches.size());
    for (std::uint32_t i = 0; i < (std::uint32_t)order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        const SceneOpaqueTag& x = t.OpaqueTags[t.OpaqueBatches[a].Tag];
        const SceneOpaqueTag& y = t.OpaqueTags[t.OpaqueBatches[b].Tag];
        if (x.Prog != y.Prog) return x.Prog < y.Prog;
        if (x.State != y.State) return x.State < y.State;
        if (x.Mat != y.Mat) return x.Mat < y.Mat;
        if (x.Object != y.Object) return x.Object < y.Object;
        return a < b;
    });
    for (std::uint32_t i : order) {
        const IndirectBatch& b = t.OpaqueBatches[i];
        const SceneOpaqueTag& tag = t.OpaqueTags[b.Tag];
        const DrawItem& lead = t.Leads[tag.Group];
        m_ProbeItem = &lead;
        m_InstanceBase = 0;
        m_InstanceMode = 2;
        BindInstanceXforms(true);
        m_PerDrawKey = tag.Object ^ ((std::uint64_t)m_InstanceMode << 32) ^ ((std::uint64_t)floatBits(lead.NearHideWidth) << 13) ^
                       ((std::uint64_t)lead.GIMask * 0x9E3779B97F4A7C15ull);
        m_PerDrawKeyed = true;
        if (lead.Ref->DrawMeshIndirect(m_ModelShader, tag.Mesh, *lead.Slots, m_Select, m_PerDraw, false,
                                       (const void*)(m_GpuCmdBase + (std::size_t)b.First * sizeof(IndirectCommand)),
                                       (int)b.Count, &passState))
            ++m_Stats.DrawCalls;
        m_PerDrawKeyed = false;
    }
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, IndirectBuffer::Handle());
}
